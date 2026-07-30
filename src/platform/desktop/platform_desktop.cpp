#include "platform/platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>     // cosf/sinf/sqrtf for the naive desktop DFT (audioFft)
#include <numbers>   // std::numbers::pi_v — same DFT
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>   // HostBus frame buffers — the memory-backed parallel bus
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cerrno>

#ifdef _WIN32
// Winsock + Win32 socket APIs. SOCKET is an unsigned handle (INVALID_SOCKET = ~0),
// but `fd_` stays `int` in the cross-platform header — the narrowing is well-defined
// for handle values in the practical range and is the standard Win32 pattern.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>     // _fileno, _commit (POSIX fileno/fsync equivalents)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>      // getaddrinfo — hostname resolution for TcpConnection::connect
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>   // mmap/munmap for allocExec (executable pages)
#include <net/if.h>     // if_nametoindex / ifreq — naming the NIC for raw L2 send
#ifdef __linux__
#include <netpacket/packet.h>   // sockaddr_ll — AF_PACKET raw frames (ethSendRaw)
#include <net/ethernet.h>       // ETH_P_ALL
#endif
#ifdef __APPLE__
#include <pthread.h>    // pthread_jit_write_protect_np — macOS arm64 W^X JIT toggle
#include <sys/ioctl.h>  // BIOCSETIF — binding a BPF device to an interface (ethSendRaw)
#include <net/bpf.h>
#endif
#endif

namespace mm::platform {

namespace {
// Tiny portability shims so each call site reads as plain code, not `#ifdef` noise.
// POSIX uses int FDs + errno + read/write/close; Winsock uses SOCKET handles +
// WSAGetLastError + recv/send/closesocket. Map to a small common surface.
#ifdef _WIN32
// SOCKET is unsigned (UINT_PTR). `sock(fd)` casts to it at API boundaries so
// /W4 doesn't warn about signed→unsigned at every call site.
inline SOCKET sock(int fd) { return static_cast<SOCKET>(fd); }
inline int close_sock(int fd) { return ::closesocket(sock(fd)); }
// WSAEWOULDBLOCK: non-blocking call had no buffer/data. WSAETIMEDOUT: blocking
// recv hit SO_RCVTIMEO without data. Both translate to POSIX EAGAIN semantics
// (the read/write path returns -1 / WouldBlock and the caller retries).
inline bool sockWouldBlock() {
    int err = ::WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
}
inline int open_sock(int domain, int type, int protocol) {
    SOCKET s = ::socket(domain, type, protocol);
    return (s == INVALID_SOCKET) ? -1 : static_cast<int>(s);
}
inline int make_nonblocking(int fd) {
    u_long mode = 1;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
inline int make_blocking(int fd) {
    u_long mode = 0;
    return ::ioctlsocket(sock(fd), FIONBIO, &mode);
}
#else
inline int sock(int fd) { return fd; }
inline int close_sock(int fd) { return ::close(fd); }
inline bool sockWouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
inline int open_sock(int domain, int type, int protocol) {
    return ::socket(domain, type, protocol);
}
inline int make_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
inline int make_blocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}
#endif
#ifdef _WIN32
// Winsock 2.2 must be initialized once per process before any socket call.
// A static RAII guard runs at library load (covers both the app and the test
// binaries, which have their own main() but link mm_platform). WSAStartup is
// reference-counted so this is safe alongside any future caller-side init.
struct WinsockInit {
    WinsockInit() {
        WSADATA d;
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~WinsockInit() { ::WSACleanup(); }
};
static WinsockInit g_winsockInit;
#endif

}  // namespace

static auto startTime = std::chrono::steady_clock::now();
// Test-only override for millis(); 0 means "use the real clock". std::atomic so
// a test can set it from one thread while a tested module reads from another.
static std::atomic<uint32_t> testNowMs{0};

void setTestNowMs(uint32_t ms) { testNowMs.store(ms, std::memory_order_relaxed); }

// steady_clock::now() is a vDSO clock_gettime read — no allocation, no lock, no syscall on
// any platform we build for. libc++ does not annotate it, so -Wfunction-effects has to assume
// the worst; this is the standard-library gap, not ours. Scoped to the two clock readers, and
// desktop-only (the ESP32 millis/micros call esp_timer_get_time directly).
// Ask the compiler whether it HAS the warning, rather than inferring it from a version number.
// `#pragma clang ...` is an unknown pragma to GCC, and `-Wfunction-effects` is an unknown warning
// group to older clangs — both are errors under -Werror. A `__clang_major__ >= 20` test does NOT
// work here: Apple Clang carries its own version line, so the macos-14 runner reports a major
// >= 20 while predating the warning, which is exactly how this reached main.
#if defined(__clang__) && defined(__has_warning)
#  if __has_warning("-Wfunction-effects")
#    define MM_SUPPRESS_FUNCTION_EFFECTS 1
#  endif
#endif
#ifdef MM_SUPPRESS_FUNCTION_EFFECTS
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfunction-effects"
#endif
uint32_t millis() MM_NONBLOCKING {
    uint32_t override_ = testNowMs.load(std::memory_order_relaxed);
    if (override_) return override_;
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count()
    );
}

uint32_t micros() MM_NONBLOCKING {
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count()
    );
}
#ifdef MM_SUPPRESS_FUNCTION_EFFECTS
#pragma clang diagnostic pop
#endif

void* alloc(size_t bytes) {
    return std::malloc(bytes);
}

bool ptrIsPsram(const void* /*p*/) { return false; }   // desktop has no PSRAM

void* allocInternal(size_t bytes) {
    return std::malloc(bytes);   // desktop has one flat RAM — internal == ordinary
}

void free(void* ptr) {
    std::free(ptr);
}

// Executable memory for MoonLive's emitted code. macOS on Apple Silicon enforces W^X
// (a page is writable OR executable, never both at once) and demands MAP_JIT for any
// JIT page; the write happens later in writeExec, bracketed by a per-thread
// write-protect toggle. Linux/Windows allow a plain RWX page. Returns nullptr on
// failure so the caller degrades.
void* allocExec(size_t bytes) {
    if (bytes == 0) return nullptr;
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return p;   // VirtualAlloc returns nullptr on failure
#elif defined(__APPLE__)
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#else
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void freeExec(void* ptr, size_t bytes) {
    if (!ptr) return;
#ifdef _WIN32
    (void)bytes;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    ::munmap(ptr, bytes);
#endif
}

void writeExec(void* dst, const void* src, size_t len) {
    if (!dst || !src || !len) return;
#if defined(_WIN32)
    // Windows: the VirtualAlloc page is RWX; memcpy suffices, then FlushInstructionCache
    // (MSVC has no __builtin___clear_cache).
    std::memcpy(dst, src, len);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
#elif defined(__APPLE__)
    // macOS arm64 W^X: flip this thread's MAP_JIT pages to writable, copy, flip back to
    // executable, then sync the I-cache (required on arm64 for freshly-written code).
    pthread_jit_write_protect_np(0);
    std::memcpy(dst, src, len);
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache(static_cast<char*>(dst), static_cast<char*>(dst) + len);
#else
    // Linux: the RWX page is plain memory; memcpy suffices. arm64 Linux still wants an
    // I-cache sync; on x86-64 __builtin___clear_cache is a harmless no-op.
    std::memcpy(dst, src, len);
    __builtin___clear_cache(static_cast<char*>(dst), static_cast<char*>(dst) + len);
#endif
}

void yield() {
    // Hand the CPU to another runnable thread — the desktop twin of the ESP32's vTaskDelay(1).
    // It must actually yield, not no-op: the multicore split's frame boundary polls this while it
    // waits for the encode worker, and a no-op turns that into a busy-spin that pins a core and
    // starves the very worker it is waiting for. std::this_thread::yield() is the portable form.
    std::this_thread::yield();
}

void delayMs(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void delayUs(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

size_t freeHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

size_t freeInternalHeap() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

// Test-only cap on the reported largest-free block; 0 = unlimited (the real
// desktop default). Lets a test force MappingLUT's paged fallback (which only
// triggers when no single contiguous block fits) without an actual fragmented
// heap. std::atomic to match setTestNowMs's cross-thread contract.
static std::atomic<size_t> testMaxBlock{0};
void setTestMaxAllocBlock(size_t bytes) { testMaxBlock.store(bytes, std::memory_order_relaxed); }

size_t maxAllocBlock() {
    return testMaxBlock.load(std::memory_order_relaxed); // 0 = unlimited
}

size_t maxInternalAllocBlock() {
    return 0; // Not meaningful on desktop (0 = unlimited)
}

// No RTOS on desktop — the TasksModule shows only its MoonModule cost table here.
// Test seam: a unit test can inject a canned task snapshot + render-task name so TasksModule's
// row/detail JSON + the nesting predicate are exercised on the host (no RTOS here otherwise). Empty
// by default → the real "desktop shows no tasks" behaviour. Declared in platform.h under a test guard.
static const TaskInfo* g_testTasks = nullptr;
static size_t g_testTaskCount = 0;
static const char* g_testRenderTask = "";
void setTestTaskSnapshot(const TaskInfo* tasks, size_t count, const char* renderTask) {
    g_testTasks = tasks; g_testTaskCount = count; g_testRenderTask = renderTask ? renderTask : "";
}
size_t taskSnapshot(TaskInfo* out, size_t maxTasks) {
    if (!g_testTasks || !out) return 0;
    const size_t n = g_testTaskCount < maxTasks ? g_testTaskCount : maxTasks;
    for (size_t i = 0; i < n; i++) out[i] = g_testTasks[i];
    return n;
}
void currentTaskOnCore(int, char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
const char* renderTaskName() { return g_testRenderTask; }

// Worker-task seam — std::thread + condition_variable backing. The `core` pin is ignored (the host
// has no core-affinity story; the core-split is ESP32-only), but the spawn/notify/wait/stop handoff
// is real, so the render↔encode invariants are host-testable on an actual second thread. The wake is
// a single-slot latch (`pending`): notifyTask sets it, waitNotify consumes it — matching the FreeRTOS
// direct-to-task notification's "one pending count" semantics so a host test sees the same behavior.
namespace {
struct DesktopWorker {
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    bool pending = false;   // a notify is waiting to be consumed (the single-slot latch)
    bool stop = false;
};
}  // namespace

bool spawnPinnedTask(WorkerTask& t, const char* /*name*/, WorkerFn fn, void* user,
                     size_t /*stackBytes*/, uint8_t /*priority*/, int /*core*/) {
    auto* w = new (std::nothrow) DesktopWorker();
    if (!w) return false;
    t.impl = w;
    w->thread = std::thread([fn, user] { fn(user); });   // the fn owns its loop until stop
    return true;
}

void notifyTask(WorkerTask& t) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return;
    { std::scoped_lock<std::mutex> lk(w->mtx); w->pending = true; }
    w->cv.notify_one();
}

bool waitNotify(WorkerTask& t, uint32_t timeoutMs) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return false;
    std::unique_lock<std::mutex> lk(w->mtx);
    const bool got = w->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                    [w] { return w->pending || w->stop; });
    if (!got) return false;         // timed out with no notify/stop
    w->pending = false;             // consume the single-slot latch
    return true;                    // woken by a notify OR stop; the fn re-checks its stop flag
}

void stopPinnedTask(WorkerTask& t) {
    auto* w = static_cast<DesktopWorker*>(t.impl);
    if (!w) return;
    { std::scoped_lock<std::mutex> lk(w->mtx); w->stop = true; }
    w->cv.notify_one();
    if (w->thread.joinable()) w->thread.join();
    delete w;
    t.impl = nullptr;
}

void taskWdtSubscribe() {}     // no watchdog on the host
void taskWdtUnsubscribe() {}   // no watchdog on the host
void taskWdtReset() {}         // no watchdog on the host


// A host build has no real GPIOs to protect — every pin is valid, output-capable, and free of
// straps/reserved roles. So the pin map on desktop flags nothing (which is correct: there's no
// silicon to corrupt). The ESP32 build fills the real capability in platform_esp32_gpio.cpp.
// A test can override one pin's capability (setTestGpioCapability) to exercise PinsModule's severity
// derivation on the host; a small fixed table (no heap) holds the overrides.
namespace {
struct GpioCapOverride { uint8_t gpio; GpioCapability cap; bool set; };
GpioCapOverride g_gpioCapOverrides[16] = {};
}  // namespace
GpioCapability gpioCapability(uint8_t gpio) {
    for (const auto& o : g_gpioCapOverrides)
        if (o.set && o.gpio == gpio) return o.cap;
    return GpioCapability{};
}
void setTestGpioCapability(uint8_t gpio, GpioCapability cap) {
    for (auto& o : g_gpioCapOverrides)
        if (!o.set || o.gpio == gpio) { o = {gpio, cap, true}; return; }
}
void clearTestGpioCapability() {
    for (auto& o : g_gpioCapOverrides) o.set = false;
}

// Live state — desktop has no real pins, so valid=false (the map omits the live columns) unless a
// test injects one. Same small fixed override table as the capability stub.
namespace {
struct GpioLiveOverride { uint8_t gpio; GpioLiveState state; bool set; };
GpioLiveOverride g_gpioLiveOverrides[16] = {};
}  // namespace
GpioLiveState gpioLiveState(uint8_t gpio) {
    for (const auto& o : g_gpioLiveOverrides)
        if (o.set && o.gpio == gpio) return o.state;
    return GpioLiveState{};   // valid=false
}
void setTestGpioLiveState(uint8_t gpio, GpioLiveState state) {
    for (auto& o : g_gpioLiveOverrides)
        if (!o.set || o.gpio == gpio) { o = {gpio, state, true}; return; }
}
void clearTestGpioLiveState() {
    for (auto& o : g_gpioLiveOverrides) o.set = false;
}

size_t totalHeap() {
    return 0; // Not meaningful on desktop
}

size_t totalInternalHeap() {
    return 0; // Not meaningful on desktop
}

void getMacAddress(uint8_t mac[6]) {
    // Stable fake MAC for desktop (consistent deviceName across runs)
    mac[0] = 0xDE; mac[1] = 0xAD; mac[2] = 0xBE;
    mac[3] = 0xEF; mac[4] = 0xCA; mac[5] = 0xFE;
}

const char* macString() {
    static char buf[18] = {};
    if (buf[0] == 0) {
        uint8_t mac[6];
        getMacAddress(mac);
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return buf;
}

const char* chipModel() {
    return "desktop";
}

uint8_t currentCore() { return 0; }

uint32_t cycleCount() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* cpuInfo() {
    // Cores only: the host's clock speed has no portable query (and boosts dynamically anyway).
    static char buf[16] = {};
    if (!buf[0])
        std::snprintf(buf, sizeof(buf), "%u cores", std::thread::hardware_concurrency());
    return buf;
}

const char* hostIp() {
    // Resolve the outbound-interface address. A UDP socket connect() sends no
    // packet — it just selects the route — so getsockname() then yields this
    // host's LAN IP. Cached after the first call. "" if offline.
    static char ip[INET_ADDRSTRLEN] = {};
    if (ip[0]) return ip;
    int fd = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);
    if (::connect(sock(fd), reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_t len = sizeof(local);
        if (::getsockname(sock(fd), reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, ip, sizeof(ip));
        }
    }
    close_sock(fd);
    return ip;
}

const char* sdkVersion() {
#ifdef __clang__
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown";
#endif
}

const char* coprocessorWifi() {
    return "";   // desktop has no WiFi co-processor
}

const char* psramType() {
    return "";   // desktop has no PSRAM
}

const char* resetReason() {
    // Desktop has no reset-reason concept; report a benign value the UI treats as "not crashed".
    return "OK";
}

void setLogLevel(LogLevel) {
    // Desktop logs to the terminal unconditionally; the KPI-line gate reads the level directly,
    // so there is nothing to apply to a platform logger here.
}

size_t firmwareSize() { return 0; }
size_t firmwarePartition() { return 0; }
size_t flashChipSize() { return 0; }

// Filesystem — std::filesystem rooted at fsRoot_ (default "build", overridable via fsSetRoot).
// A leading '/' in the API path maps to root-relative. Default lives under build/ so the
// desktop-created .config/ is gitignored (along with the rest of build/) and doesn't clutter
// the repo root. Tests override this to a tmpdir via fsSetRoot for isolation.

namespace {
std::filesystem::path fsRoot_{"build"};

// Map "/.config/foo.json" → "<root>/.config/foo.json". Strips leading '/'s, normalizes
// the result, and rejects paths that escape fsRoot_ (e.g. "../../etc/passwd"). Returns
// an empty path on rejection; callers already treat empty/nonexistent as failure.
std::filesystem::path toFsPath(const char* path) {
    if (!path) return {};
    while (*path == '/') path++;  // strip any number of leading slashes
    std::filesystem::path candidate = (fsRoot_ / path).lexically_normal();
    std::filesystem::path rootNormal = fsRoot_.lexically_normal();
    // Prefix check on the normalized string: candidate must start with rootNormal followed
    // by either end-of-string or a separator. Iterator comparison is more robust against
    // trailing-slash quirks; mismatched_first signals an escape.
    auto [r, c] = std::mismatch(rootNormal.begin(), rootNormal.end(),
                                candidate.begin(), candidate.end());
    if (r != rootNormal.end()) return {};  // candidate diverges before consuming all of rootNormal
    return candidate;
}
}

void fsSetRoot(const char* path) {
    fsRoot_ = (path && *path) ? std::filesystem::path(path) : std::filesystem::path("build");
}

bool fsMount() {
    // No mount needed on desktop; OS handles it.
    return true;
}

void fsUnmount() {}

bool fsMkdir(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(toFsPath(path), ec);
    return !ec;
}

bool fsExists(const char* path) {
    std::error_code ec;
    return std::filesystem::exists(toFsPath(path), ec);
}

bool fsRemove(const char* path) {
    std::error_code ec;
    return std::filesystem::remove(toFsPath(path), ec);
}

int fsRead(const char* path, char* buf, size_t maxLen) {
    if (!buf || maxLen == 0) return -1;
    // path::c_str() returns wchar_t* on Windows; std::fopen needs char*. Go via
    // .string() so the call compiles on both. Costs one std::string allocation
    // per read — acceptable for /.config/*.json reads (rare, small).
    FILE* f = std::fopen(toFsPath(path).string().c_str(), "rb");
    if (!f) return -1;
    size_t n = std::fread(buf, 1, maxLen - 1, f);
    std::fclose(f);
    buf[n] = 0;
    return static_cast<int>(n);
}

// Open a temp file for atomic-write, owner-only (0600) where the OS has file modes.
//
// std::fopen creates with 0666 & ~umask, so on a typical umask 022 the file lands world-readable
// — and these are /.config/*.json, which hold WiFi PSKs and MQTT passwords. Nothing here is
// multi-user on ESP32 (LittleFS has no modes at all, so the platform layer's ESP32 half is
// unaffected), but the desktop build runs on real machines with real other users.
//
// POSIX gets O_CREAT|O_EXCL with an explicit 0600 — EXCL because a pre-existing temp file is
// either a crashed run's leftover or someone else's, and inheriting its mode would defeat the
// point. Windows has no mode_t; its files inherit the parent directory's ACL, which is the
// platform's own answer to the same question, so it keeps plain fopen.
static FILE* openTempOwnerOnly(const char* path) {
#ifdef _WIN32
    return std::fopen(path, "wb");
#else
    ::unlink(path);                       // clear a leftover so O_EXCL cannot fail on our own temp
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600);
    if (fd < 0) return nullptr;
    FILE* f = ::fdopen(fd, "wb");
    if (!f) ::close(fd);                  // fdopen failure leaves the descriptor ours to release
    return f;
#endif
}

bool fsWriteAtomic(const char* path, const char* data, size_t len) {
    auto target = toFsPath(path);
    auto tmp = target;
    tmp += ".tmp";

    FILE* f = openTempOwnerOnly(tmp.string().c_str());
    if (!f) return false;
    size_t written = std::fwrite(data, 1, len, f);
    if (written != len) {
        std::fclose(f);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::fflush(f);
#ifdef _WIN32
    int fd = ::_fileno(f);
    if (fd >= 0) ::_commit(fd);  // Windows equivalent of fsync
#else
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
#endif
    std::fclose(f);

    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

long fsSize(const char* path) {
    std::error_code ec;
    auto p = toFsPath(path);
    if (!std::filesystem::is_regular_file(p, ec)) return -1;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec ? -1 : static_cast<long>(sz);
}

int fsReadAt(const char* path, long offset, char* buf, size_t len) {
    if (!buf) return -1;
    FILE* f = std::fopen(toFsPath(path).string().c_str(), "rb");
    if (!f) return -1;
    if (std::fseek(f, offset, SEEK_SET) != 0) { std::fclose(f); return -1; }
    const size_t n = std::fread(buf, 1, len, f);
    std::fclose(f);
    return static_cast<int>(n);   // 0 at EOF
}

bool fsWriteStream(const char* path, FsWriteSrc src, void* user) {
    if (!src) return false;
    auto target = toFsPath(path);
    auto tmp = target;
    tmp += ".tmp";

    FILE* f = openTempOwnerOnly(tmp.string().c_str());
    if (!f) return false;
    // Pull chunks from the source and write each straight through — fixed buffer, any file size.
    // `abort` set by the source (a short/timed-out upload) means the data is incomplete → discard.
    char chunk[1024];
    bool ok = true, abort = false;
    for (;;) {
        const size_t got = src(chunk, sizeof(chunk), user, &abort);
        if (abort) { ok = false; break; }
        if (got == 0) break;                                    // clean end of stream
        if (std::fwrite(chunk, 1, got, f) != got) { ok = false; break; }
    }
    std::fflush(f);
#ifdef _WIN32
    int fd = ::_fileno(f);
    if (fd >= 0) ::_commit(fd);
#else
    int fd = ::fileno(f);
    if (fd >= 0) ::fsync(fd);
#endif
    std::fclose(f);

    std::error_code ec;
    if (!ok) { std::filesystem::remove(tmp, ec); return false; }
    std::filesystem::rename(tmp, target, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

void fsList(const char* dir, FsListCb cb, void* user) {
    if (!cb) return;
    std::error_code ec;
    auto p = toFsPath(dir);
    if (!std::filesystem::exists(p, ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(p, ec)) {
        if (ec) break;
        // path::filename().c_str() returns wchar_t* on Windows; the callback
        // wants char*. Round-trip through .string() to get a portable view.
        std::string name = entry.path().filename().string();
        const bool isDir = entry.is_directory(ec);
        std::error_code sizeEc;
        const auto sz = isDir ? 0u : static_cast<uint32_t>(std::filesystem::file_size(entry.path(), sizeEc));
        cb(name.c_str(), isDir, sizeEc ? 0u : sz, user);
    }
}

size_t filesystemUsed() {
    // Sum of file sizes under ./.config/
    std::error_code ec;
    auto root = toFsPath("/.config");
    if (!std::filesystem::exists(root, ec)) return 0;
    size_t total = 0;
    for (auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

size_t filesystemTotal() {
    // Desktop has no fixed quota; report a notional 384 KB to match the 4MB ESP32 partition.
    return 384 * 1024;
}

// Network stubs (desktop has no WiFi/Ethernet hardware)

void setEthConfig(const EthPinConfig&) {}   // no eth on desktop; ethInit stubs false
void ethStop() {}                           // no eth on desktop
bool ethInit() { return false; }
bool ethLinkUp() MM_NONBLOCKING { return false; }
bool ethConnected() MM_NONBLOCKING { return false; }

// Raw-frame capture: the desktop half of the ethSendRaw seam. Sending a real L2 frame from a host
// process needs a raw socket and root, which no test should ask for — so the host RECORDS what the
// driver emitted instead. That is what lets PanelCardDriver and its tests build and run everywhere
// (the desktop-runs-everything rule), with the packet bytes pinned on the host and only the wire
// itself left to the bench.
//
// Fixed capacity, no allocation: a test asserts over the first few frames of a render tick, and an
// unbounded recorder would turn a long run into unbounded memory. Frames past the cap are counted
// but not stored, so an overrun shows up as a count the test can see.
namespace {
// Sized for the largest frame sequence a test asserts over: a 128-row wall is 2 brightness + 128
// rows + 2 sync = 132.
//
// Allocated on FIRST CAPTURE, not from boot: this is a test seam, and as a plain static array it
// cost ~195 KB of BSS in the shipped desktop/Pi binary — a deployment that binds a real interface
// never records a frame and would have paid for it anyway. Same lazy-allocation reasoning as the
// task-snapshot scratch in the ESP32 platform layer. Never freed: freeing would put the allocation
// back on a path that runs per frame, and one buffer per process is the point.
constexpr size_t kEthTestMaxFrames = 132;
uint8_t (*ethTestFrames_)[kEthTestFrameMax] = nullptr;
size_t  ethTestLens_[kEthTestMaxFrames] = {};
size_t  ethTestCount_ = 0;
bool    ethTestSendFails_ = false;
uint16_t ethTestLinkSpeed_ = 1000;   // desktop reports gigabit unless a test says otherwise
int      ethRawClaims_ = 0;          // drivers holding the link for direct L2 use
uint32_t ethSendFails_ = 0;          // consecutive ethSendRaw failures
// The bound raw socket, or -1 for capture mode (the default, and all any test sees).
int      ethRawFd_ = -1;
unsigned ethRawIfIndex_ = 0;         // Linux AF_PACKET needs the index; BPF binds by name
}  // namespace

// Open a raw L2 socket on `ifName` so a host build drives panels for real — the deployment a Pi or
// a mini-PC covers, and the same code path the ESP32 takes. Linux uses AF_PACKET, macOS BPF; both
// need root (or CAP_NET_RAW), so an ordinary test run simply stays in capture mode.
bool ethBindRawInterface(const char* ifName) {
#ifdef _WIN32
    // No raw-L2 send without a third-party driver (WinPcap/Npcap) on Windows; capture mode only.
    (void)ifName;
    return false;
#else
    if (ethRawFd_ >= 0) { ::close(ethRawFd_); ethRawFd_ = -1; }
    ethRawIfIndex_ = 0;
    if (!ifName || !ifName[0]) return true;   // explicit return to capture mode

#if defined(__linux__)
    const int fd = ::socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) return false;
    const unsigned idx = if_nametoindex(ifName);
    if (idx == 0) { ::close(fd); return false; }
    ethRawFd_ = fd;
    ethRawIfIndex_ = idx;
    return true;
#elif defined(__APPLE__)
    // BPF has no single device: open the first free /dev/bpfN, then bind it to the interface.
    for (int i = 0; i < 99; i++) {
        char dev[24];
        std::snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
        const int fd = ::open(dev, O_RDWR);
        if (fd < 0) continue;              // busy or no permission — try the next
        ifreq ifr = {};
        std::snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifName);
        if (::ioctl(fd, BIOCSETIF, &ifr) < 0) { ::close(fd); return false; }
        // Write whole frames as given. Without this BPF supplies its OWN source MAC, overwriting
        // the fixed one the cards filter on — the frames would go out well-formed and be ignored,
        // which is the hardest kind of failure to diagnose. So a failure here fails the bind.
        unsigned hdrComplete = 1;
        if (::ioctl(fd, BIOCSHDRCMPLT, &hdrComplete) < 0) { ::close(fd); return false; }
        ethRawFd_ = fd;
        return true;
    }
    return false;
#else
    (void)ifName;
    return false;   // no raw-L2 path on this host OS
#endif
#endif  // _WIN32
}

// Send the frame on the bound interface, or record it when none is bound. The capture branch is
// what every unit test exercises; the raw branch is what makes a host a panel controller.
bool ethSendRaw(const uint8_t* frame, size_t len) MM_NONBLOCKING {
    if (!frame || len == 0) return false;
    if (ethTestSendFails_) { ethSendFails_++; return false; }   // simulated link-down / full TX ring

#ifndef _WIN32
    if (ethRawFd_ >= 0) {
#if defined(__linux__)
        sockaddr_ll dst = {};
        dst.sll_family = AF_PACKET;
        dst.sll_ifindex = static_cast<int>(ethRawIfIndex_);
        dst.sll_halen = 6;
        std::memcpy(dst.sll_addr, frame, 6);   // destination MAC is the frame's first 6 bytes
        const ssize_t n = ::sendto(ethRawFd_, frame, len, 0,
                                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
#else
        const ssize_t n = ::write(ethRawFd_, frame, len);
#endif
        // Track failures on the REAL send path too, not just the capture path: a bound host is
        // where frames actually reach a wire, so a streak here is the one that matters.
        if (n != static_cast<ssize_t>(len)) { ethSendFails_++; return false; }
        ethSendFails_ = 0;
        return true;
    }
#endif

    if (ethTestCount_ < kEthTestMaxFrames) {
        if (!ethTestFrames_) {
            ethTestFrames_ = static_cast<uint8_t(*)[kEthTestFrameMax]>(
                std::calloc(kEthTestMaxFrames, kEthTestFrameMax));
        }
        if (ethTestFrames_) {
            // Record the TRUE length even when the copy is clipped, so an oversized frame is visible
            // as a length no reader expected rather than as silently short data.
            const size_t copy = len < kEthTestFrameMax ? len : kEthTestFrameMax;
            std::memcpy(ethTestFrames_[ethTestCount_], frame, copy);
            ethTestLens_[ethTestCount_] = len;
        }
    }
    ethTestCount_++;
    ethSendFails_ = 0;
    return true;
}

uint32_t ethSendFailStreak() MM_NONBLOCKING { return ethSendFails_; }

// See platform.h: a claim stated by the driver, reference-counted.
void ethClaimRawL2(bool claim) {
    if (claim) ethRawClaims_++;
    else if (ethRawClaims_ > 0) ethRawClaims_--;
}

bool ethRawL2Claimed() MM_NONBLOCKING { return ethRawClaims_ > 0; }

// The host has no negotiated link. Report gigabit so the driver's speed check passes on desktop and
// its tests exercise the send path rather than the too-slow branch (which has its own test via
// setTestEthLinkSpeed).
uint16_t ethLinkSpeedMbps() MM_NONBLOCKING { return ethTestLinkSpeed_; }

size_t ethTestFrameCount() { return ethTestCount_; }
size_t ethTestFrameLength(size_t i) { return i < kEthTestMaxFrames ? ethTestLens_[i] : 0; }
const uint8_t* ethTestFrameData(size_t i) {
    return (ethTestFrames_ && i < kEthTestMaxFrames) ? ethTestFrames_[i] : nullptr;
}
void ethTestClearFrames() { ethTestCount_ = 0; ethSendFails_ = 0; }
void setTestEthSendFails(bool fail) { ethTestSendFails_ = fail; }
void setTestEthLinkSpeed(uint16_t mbps) { ethTestLinkSpeed_ = mbps; }
void ethGetIPv4(uint8_t out[4]) MM_NONBLOCKING {
    // Desktop has no real interface state, but DevicesModule needs the host's LAN
    // IP to scan from (otherwise a desktop projectMM instance reports "no network" and
    // never sweeps). hostIp() resolves it via the outbound-route trick; report it
    // as the "ethernet" IP so DevicesModule's localIp() (eth-first) picks it up.
    out[0] = out[1] = out[2] = out[3] = 0;
    const char* ip = hostIp();
    if (ip && ip[0]) {
        // Parse the dotted-quad to octets with inet_pton (already used in this file)
        // — the platform layer doesn't include core/Control.h's parseDottedQuad.
        in_addr a{};
        if (inet_pton(AF_INET, ip, &a) == 1) {
            uint32_t n = a.s_addr;   // network byte order: octet 0 is the low byte
            out[0] = static_cast<uint8_t>(n & 0xff);
            out[1] = static_cast<uint8_t>((n >> 8) & 0xff);
            out[2] = static_cast<uint8_t>((n >> 16) & 0xff);
            out[3] = static_cast<uint8_t>((n >> 24) & 0xff);
        }
    }
}

// Test seam: the host has no STA radio, so wifiStaInit() reports "no STA" — unless a test fakes
// one to drive NetworkModule's WaitingSta path. Cross-thread atomic, the setTestNowMs contract.
static std::atomic<bool> testWifiStaAvailable{false};
void setTestWifiStaAvailable(bool available) { testWifiStaAvailable.store(available, std::memory_order_relaxed); }
bool wifiStaInit(const char* /*ssid*/, const char* /*password*/) {
    return testWifiStaAvailable.load(std::memory_order_relaxed);
}
bool wifiStaConnected() MM_NONBLOCKING { return false; }
void wifiStaGetIPv4(uint8_t out[4]) { out[0] = out[1] = out[2] = out[3] = 0; }
// Addressing is OS-managed on desktop; the static/DHCP setters are inert (no netif to reconfigure).
// The per-interface apply counter is the observable a host test pins the static-addressing path on.
static std::atomic<uint32_t> testStaticApplies[2] = {};   // indexed by NetIface
void netSetStaticIPv4(NetIface iface, const uint8_t[4], const uint8_t[4],
                      const uint8_t[4], const uint8_t[4]) {
    testStaticApplies[static_cast<uint8_t>(iface)].fetch_add(1, std::memory_order_relaxed);
}
uint32_t testNetStaticApplyCount(NetIface iface) {
    return testStaticApplies[static_cast<uint8_t>(iface)].load(std::memory_order_relaxed);
}
void netSetDhcp(NetIface /*iface*/) {}
void setHostname(const char* /*name*/) {}   // no DHCP client on desktop
void wifiStaStop() {}
int wifiStaRssi() { return 0; }
void wifiStaBssid(uint8_t out[6]) { std::memset(out, 0, 6); }
int wifiStaChannel() { return 0; }

bool wifiApInit(const char* /*apName*/, const char* /*ip*/) { return false; }
bool wifiApConnected() { return false; }
void wifiApStop() {}
uint32_t wifiApClientCount() { return 0; }

// Host sockets work regardless of the (stubbed) link predicates above, and there is
// no lwip-style init race — always socket-safe.
bool networkReady() { return true; }
int wifiTxPower() { return 0; }
// Match the API contract: 0 is a successful no-op (matches ESP-IDF
// MM_NO_WIFI stub semantics). Any non-zero value returns false since
// there's no radio to set on the desktop. The 0-as-success branch
// matters because NetworkModule's syncTxPower passes the ESP-IDF
// "no override" sentinel (80 quarter-dBm → full power, which maps to
// txPowerSetting_==0 in user-facing dBm) through this setter to lift
// any prior cap; on desktop the radio doesn't exist so "the cap is
// lifted" is trivially true.
bool wifiSetTxPower(int8_t quarterDbm) { return quarterDbm == 0; }

bool mdnsInit(const char* /*deviceName*/) { return false; }
void mdnsStop() {}
void mdnsShutdown() {}
// mDNS advertise is a device-only concern, so these are host stubs. Discovery is UDP
// presence (DevicesModule + WledPacket) over UdpSocket, which runs on desktop too — so the
// discovery path is unit-testable on the host with real loopback datagrams (a bound socket
// or DevicesModule::injectPacketForTest).

// OTA — no-op on desktop (no OTA partition). The /api/firmware/url route
// guards with `if constexpr (mm::platform::hasOta)` and returns 501 here,
// so this stub exists for compile coverage only.
bool http_fetch_to_ota(const char* /*url*/,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    if (bytesReadOut) *bytesReadOut = 0;
    if (bytesTotalOut) *bytesTotalOut = 0;
    return false;
}

bool otaWriteStream(FsWriteSrc /*src*/, void* /*user*/, size_t /*contentLen*/,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut) {
    // No OTA partition on desktop — call sites guard with `if constexpr (mm::platform::hasOta)`.
    if (statusBuf && statusBufLen > 0) std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    if (bytesReadOut) *bytesReadOut = 0;
    return false;
}

// Outbound HTTP request (plain HTTP, LAN, no TLS) — see platform.h. Blocking, bounded by a
// receive/send timeout. Builds the request into a stack buffer, connects, sends, reads the
// response, and returns the status code + the body (after the \r\n\r\n). Used by HueDriver
// off the render path.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen) {
    if (body && bodyLen) body[0] = '\0';
    if (!method || !host || !path) return 0;

    // One shared budget for the whole request: connect, send, and recv each consume from the same
    // timeoutMs rather than each getting a fresh one (which let the total reach ~3× timeoutMs).
    // `remainingMs()` is the time left, floored at 1ms so a phase never gets a 0 timeout (which
    // means "block forever" for SO_*TIMEO). Tracked as elapsed-since-start (now - start), which is
    // unsigned-wrap-safe across the 32-bit millis() rollover; an absolute `start + timeoutMs`
    // deadline compared with `now >=` would mis-fire when only one side has wrapped.
    const uint32_t start = millis();
    auto remainingMs = [&]() -> uint32_t {
        const uint32_t elapsed = millis() - start;
        return elapsed >= timeoutMs ? 1u : (timeoutMs - elapsed);
    };

    int fd = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct CloseGuard { int f; ~CloseGuard() { close_sock(f); } } guard{fd};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return 0;

    // Bound the CONNECT by timeoutMs: a blocking connect to an unreachable host hangs for the OS
    // default (tens of seconds) — and this runs on the driver's tick1s (shared with the render
    // loop), so it must not stall. Connect non-blocking, wait writable via select() up to
    // timeoutMs, then restore blocking for the bounded send/recv (which use SO_*TIMEO below).
    if (make_nonblocking(fd) != 0) return 0;
    int cr = ::connect(sock(fd), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    // A non-blocking connect that didn't complete immediately reports "in progress":
    // EINPROGRESS on POSIX, WSAEWOULDBLOCK on Winsock. Anything else is a hard failure.
#ifdef _WIN32
    const bool inProgress = (cr != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool inProgress = (cr != 0 && errno == EINPROGRESS);
#endif
    if (cr != 0 && !inProgress) return 0;          // immediate hard failure
    if (cr != 0) {                                 // connect in progress — wait for writable
        fd_set wf; FD_ZERO(&wf); FD_SET(sock(fd), &wf);
        const uint32_t cms = remainingMs();
        timeval ctv{};
        ctv.tv_sec = static_cast<time_t>(cms / 1000);
        // decltype the field, not suseconds_t: tv_usec is `long` on Winsock's timeval (no suseconds_t
        // on Windows) and suseconds_t on POSIX — decltype resolves to the right type on every platform.
        ctv.tv_usec = static_cast<decltype(ctv.tv_usec)>((cms % 1000) * 1000);
        if (::select(static_cast<int>(sock(fd)) + 1, nullptr, &wf, nullptr, &ctv) <= 0) return 0;  // timeout / error
        int soerr = 0; socklen_t len = sizeof(soerr);
        ::getsockopt(sock(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
        if (soerr != 0) return 0;                  // connect failed
    }
    if (make_blocking(fd) != 0) return 0;          // back to blocking for the bounded send/recv

    // Bound the request send + response recv with SO_RCVTIMEO/SO_SNDTIMEO, using the time LEFT on
    // the shared deadline (not a fresh timeoutMs) so connect + send + recv together stay within the
    // caller's budget.
    const uint32_t sms = remainingMs();
#ifdef _WIN32
    DWORD tv = sms;
    ::setsockopt(sock(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(sock(fd), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(sms / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((sms % 1000) * 1000);
    ::setsockopt(sock(fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(sock(fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    char req[1024];
    const size_t blen = reqBody ? std::strlen(reqBody) : 0;
    int n = blen
        ? std::snprintf(req, sizeof(req),
              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
              "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
              method, path, host, blen, reqBody)
        : std::snprintf(req, sizeof(req),
              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
              method, path, host);
    if (n <= 0 || n >= static_cast<int>(sizeof(req))) return 0;
    // Send the whole request — a blocking send can return short under backpressure, so loop
    // until all n bytes are out (retry on a positive partial, fail only on 0 / error).
    for (int off = 0; off < n;) {
        auto w = ::send(sock(fd), req + off, n - off, 0);
        if (w > 0) off += static_cast<int>(w);
        else return 0;
    }

    // Read the response. When the caller wants the body, read into THEIR buffer (so they size it
    // — a Hue /lights body runs several KB) and shift the body to the front. When they don't
    // (body==null, e.g. a fire-and-forget PUT), read into a small local scratch just far enough
    // to get the status line — the request still executes. The status line + headers sit at the
    // front of whatever we read.
    char scratch[256];
    char* buf = body ? body : scratch;
    const size_t cap = body ? bodyLen : sizeof(scratch);
    if (cap < 16) return 0;
    int total = 0;
    while (total < static_cast<int>(cap - 1)) {
        auto r = ::recv(sock(fd), buf + total, cap - 1 - total, 0);
        if (r > 0) total += static_cast<int>(r);
        else break;   // closed or timeout
    }
    buf[total] = '\0';
    if (total < 12 || std::strncmp(buf, "HTTP/1.", 7) != 0) { if (body) body[0] = '\0'; return 0; }
    int status = std::atoi(buf + 9);   // "HTTP/1.1 NNN ..."
    if (body) {
        char* b = std::strstr(body, "\r\n\r\n");
        if (b) std::memmove(body, b + 4, std::strlen(b + 4) + 1);   // drop headers, keep just the body
        else body[0] = '\0';
    }
    return status;
}


// Improv WiFi — no USB-serial path on desktop. The module gates with
// `if constexpr (mm::platform::hasImprov)` and never calls this on desktop;
// the stub exists for compile coverage.
bool improvProvisioningInit(const ImprovDeviceInfo& /*info*/,
                            char* /*ssidOut*/, size_t /*ssidOutLen*/,
                            char* /*passwordOut*/, size_t /*passwordOutLen*/,
                            std::atomic<bool>* /*ready*/,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* /*txPowerOut*/,
                            std::atomic<bool>* /*txPowerReady*/,
                            char* /*opOut*/, size_t /*opOutLen*/,
                            std::atomic<bool>* /*opReady*/) {
    if (statusBuf && statusBufLen > 0) {
        std::snprintf(statusBuf, statusBufLen, "unsupported on desktop");
    }
    return false;
}

void reboot() {
    // Desktop: the device is the host process. Exit cleanly; the OS user / supervisor
    // can restart it. Matches the "device disappeared from the network" semantics the
    // browser-side WS reconnect logic expects.
    std::printf("platform::reboot() — exiting\n");
    std::fflush(stdout);
    // Exiting the process IS the desktop reboot — there is no firmware to restart into. The
    // mt-unsafe warning is about exit() racing other threads' atexit handlers, which is exactly
    // the abrupt teardown a reboot models.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    std::exit(0);
}

// UdpSocket

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open() {
    if (fd_ >= 0) return true;
    fd_ = open_sock(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return false;
    // Allow sends to a broadcast address (e.g. 255.255.255.255 for an Art-Net /
    // E1.31 spray to every device on the LAN). Without SO_BROADCAST the OS rejects
    // such a send with EACCES; it has no effect on unicast/multicast sends.
    const int on = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&on), sizeof(on));
    return true;
}

bool UdpSocket::connect(const char* ip, uint16_t port) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return false;
    return ::connect(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
}

bool UdpSocket::sendTo(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    return ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0) >= 0;
}

// Test override (see platform.h): forces bind() to fail so a test can drive the failure path without
// relying on the OS to refuse a port — which is not portable (Linux permits the overlapping UDP bind).
static std::atomic<bool> testBindFails{false};
void setTestBindFails(bool fail) { testBindFails.store(fail, std::memory_order_relaxed); }

bool UdpSocket::bind(uint16_t port) {
    if (fd_ < 0) return false;
    if (testBindFails.load(std::memory_order_relaxed)) return false;
    // SO_REUSEADDR semantic split: on POSIX it lets a fresh socket claim a port left in
    // TIME_WAIT (never allows two live binds to overlap). On Winsock its meaning is the
    // opposite of POSIX — two live sockets can bind the same port, so a second bind()
    // returns success instead of the EADDRINUSE the audio-sync retry-backoff logic reads
    // as "port owned by someone else" (unit_AudioService_sync's hog-then-module scenario
    // exercises exactly that). Windows' equivalent-to-POSIX behaviour is the *default*,
    // so on Windows we skip the setsockopt and let a second bind fail naturally.
    //
    // NOTE the outcome is NOT the same on every platform, contrary to what this comment used to
    // claim: on LINUX, SO_REUSEADDR on a UDP socket bound to INADDR_ANY permits an overlapping bind,
    // so a second bind SUCCEEDS. A test that needs a bind to fail must use setTestBindFails(), not a
    // port hog.
#ifndef _WIN32
    int reuse = 1;
    ::setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock(fd_), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    // Non-blocking so the render loop's drain never stalls waiting for a packet.
    return make_nonblocking(fd_) == 0;
}

int UdpSocket::recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4]) {
    if (fd_ < 0) return -1;
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    auto n = ::recvfrom(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0,
                        reinterpret_cast<sockaddr*>(&src), &srcLen);
    // 0-byte datagrams and would-block both mean "nothing usable pending".
    if (n <= 0) return -1;
    if (srcIp) std::memcpy(srcIp, &src.sin_addr.s_addr, 4);   // network order = octets
    return static_cast<int>(n);
}

bool UdpSocket::sendToAddr(const uint8_t ip[4], uint16_t port,
                           const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::memcpy(&addr.sin_addr.s_addr, ip, 4);
    return ::sendto(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                    reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) >= 0;
}

void UdpSocket::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpConnection

TcpConnection::~TcpConnection() {
    close();
}

int TcpConnection::read(uint8_t* buf, size_t maxLen) {
    if (fd_ < 0) return -1;
    // recv() works the same on POSIX and Winsock — the socket is blocking with
    // SO_RCVTIMEO set in TcpServer::accept (Windows takes DWORD ms, POSIX takes
    // struct timeval). After the timeout, recv returns -1 with EAGAIN/EWOULDBLOCK
    // (POSIX) or WSAEWOULDBLOCK (Windows); we translate both to -1 for the caller.
    auto n = ::recv(sock(fd_), reinterpret_cast<char*>(buf), static_cast<int>(maxLen), 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0; // peer closed
    if (sockWouldBlock()) return -1; // read timed out, nothing available
    return 0; // error → treat as closed
}

bool TcpConnection::write(const uint8_t* data, size_t len) {
    if (fd_ < 0) return false;
    // Send ALL bytes (blocking retry on a full buffer) — an HTTP response / WS frame must arrive complete.
    // A healthy interface drains in microseconds so the retry rarely spins. Bounded by a wall-clock
    // deadline (mirrors the ESP32 impl): this runs on the render thread, and a stalled peer whose TCP
    // receive window is full would otherwise make send() block forever and hang the loop. On timeout,
    // return false so the caller closes that client instead of wedging the device.
    constexpr uint32_t kWriteDeadlineMs = 2000;
    const uint32_t start = millis();
    size_t sent = 0;
    while (sent < len) {
        auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data + sent),
                        static_cast<int>(len - sent), 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (sockWouldBlock()) {
            if (millis() - start >= kWriteDeadlineMs) return false;   // stalled peer — don't hang the loop
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#ifndef _WIN32
        } else if (errno == EINTR) {
            continue; // interrupted by signal, retry
#endif
        } else {
            return false;
        }
    }
    return true;
}

int TcpConnection::writeSome(const uint8_t* data, size_t len) {
    if (fd_ < 0) return -1;
    if (len == 0) return 0;
    // The accept()ed socket is persistently non-blocking (set in TcpServer::accept), so a
    // plain ::send() never blocks — no toggle needed. A full kernel send buffer surfaces as
    // EWOULDBLOCK, which we report as 0 ("try later"); the caller advances its own offset.
    auto n = ::send(sock(fd_), reinterpret_cast<const char*>(data), static_cast<int>(len), 0);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (sockWouldBlock()) return 0;         // buffer full — try later
#ifndef _WIN32
    if (errno == EINTR) return 0;           // interrupted — try later
#endif
    return -1;                              // real socket error
}


bool TcpConnection::connectStart(const char* host, uint16_t port) {
    if (!host || !host[0]) return false;
    close();

    // One bounded DNS lookup (getaddrinfo) up front — resolving is synchronous, but it's the one
    // unavoidable blocking bit; the CONNECT itself then proceeds non-blocking and is polled.
    char portStr[6];
    std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, portStr, &hints, &res) != 0 || !res) return false;
    struct AiGuard { addrinfo* p; ~AiGuard() { if (p) ::freeaddrinfo(p); } } aiGuard{res};

    int fd = open_sock(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) return false;
    if (make_nonblocking(fd) != 0) { close_sock(fd); return false; }
    int cr = ::connect(sock(fd), res->ai_addr, static_cast<int>(res->ai_addrlen));
#ifdef _WIN32
    const bool inProgress = (cr != 0 && ::WSAGetLastError() == WSAEWOULDBLOCK);
#else
    const bool inProgress = (cr != 0 && errno == EINPROGRESS);
#endif
    if (cr != 0 && !inProgress) { close_sock(fd); return false; }   // immediate hard failure
    fd_ = fd;   // in flight (or already connected) — connectPoll() resolves which
    return true;
}

TcpConnection::ConnectResult TcpConnection::connectPoll() {
    if (fd_ < 0) return ConnectResult::Failed;
    // Zero-timeout select: is the socket writable yet? Never blocks.
    // Watch BOTH writability and the exception set: a completed connect signals writable on POSIX,
    // but a FAILED (refused) non-blocking connect signals via the exception set on Winsock — checking
    // only writefds there leaves a refused connect reading Pending until the caller's timeout.
    fd_set wf; FD_ZERO(&wf); FD_SET(sock(fd_), &wf);
    fd_set ef; FD_ZERO(&ef); FD_SET(sock(fd_), &ef);
    timeval zero{};   // 0s / 0us
    const int r = ::select(static_cast<int>(sock(fd_)) + 1, nullptr, &wf, &ef, &zero);
    if (r == 0) return ConnectResult::Pending;                       // neither writable nor errored yet
    if (r < 0)  { close(); return ConnectResult::Failed; }
    // SO_ERROR distinguishes a real connect from an errored one on both platforms.
    int soerr = 0; socklen_t len = sizeof(soerr);
    ::getsockopt(sock(fd_), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
    if (soerr != 0) { close(); return ConnectResult::Failed; }
    return ConnectResult::Connected;                                 // socket stays non-blocking
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// TcpServer

TcpServer::~TcpServer() {
    close();
}

bool TcpServer::open(uint16_t port) {
    if (fd_ >= 0) return true;
    fd_ = open_sock(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    int opt = 1;
    setsockopt(sock(fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(sock(fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    if (::listen(sock(fd_), 8) < 0) {
        close_sock(fd_);
        fd_ = -1;
        return false;
    }

    make_nonblocking(fd_);

    return true;
}

TcpConnection TcpServer::accept() {
    if (fd_ < 0) return TcpConnection();
#ifdef _WIN32
    SOCKET client = ::accept(sock(fd_), nullptr, nullptr);
    if (client == INVALID_SOCKET) return TcpConnection();
    int clientFd = static_cast<int>(client);
    // NON-BLOCKING (see the POSIX branch below for the full rationale): a blocking recv on
    // the single-loop server stalls the whole render loop. make_nonblocking → recv returns
    // WSAEWOULDBLOCK → read() reports -1 ("nothing yet") immediately, never blocking.
    make_nonblocking(clientFd);
#else
    int clientFd = ::accept(fd_, nullptr, nullptr);
    if (clientFd < 0) return TcpConnection();
    // NON-BLOCKING client socket. The HTTP server is serviced from the single render loop,
    // so a blocking recv()'s timeout (we used 2 s) froze the WHOLE loop whenever a request's
    // bytes hadn't landed the instant accept() returned — UI to a crawl. Non-blocking makes
    // read() return -1 ("nothing yet") immediately, so the loop never stalls; the request
    // (which lands within ~1 ms on localhost/LAN) is read across a few rapid retries in
    // handleConnection. recv returns EWOULDBLOCK → -1, matching read()'s contract.
    make_nonblocking(clientFd);
#endif
    return TcpConnection(clientFd);
}

void TcpServer::close() {
    if (fd_ >= 0) {
        close_sock(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// RMT WS2812 on the host: accepted and counted, not refused.
//
// Same rule as the parallel buses above (architecture.md § Platform abstraction). Refusing here
// made RmtLedDriver inert off device, so nothing in it could be tested on a host.
//
// RMT is symbol-based rather than buffer-based, so there is nothing to hand back: the driver owns
// the symbol array and this seam only has to accept it. The resolution is echoed so the driver's
// timing arithmetic (which divides by it) works on real numbers instead of zero.
// ---------------------------------------------------------------------------
namespace {
struct HostRmt { uint32_t resolutionHz = 0; };
HostRmt* hostRmt(void*& impl) {
    if (!impl) impl = new HostRmt();
    return static_cast<HostRmt*>(impl);
}
}  // namespace

bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t /*gpio*/, uint32_t resolutionHz,
                   bool /*invert*/) {
    // A zero resolution would make the driver divide by zero when it converts nanoseconds to
    // ticks — refuse it here rather than hand back a channel that cannot be used.
    if (resolutionHz == 0) return false;
    hostRmt(h.impl)->resolutionHz = resolutionHz;
    return true;
}
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h) MM_NONBLOCKING {
    return h.impl ? static_cast<HostRmt*>(h.impl)->resolutionHz : 0;
}
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint32_t* symbols,
                       size_t symbolCount) {
    if (!h.impl || !symbols || symbolCount == 0) return false;
    return true;
}
void rmtWs2812Wait(RmtWs2812Handle& /*h*/, uint32_t /*timeoutMs*/) {}
void rmtWs2812Deinit(RmtWs2812Handle& h) {
    delete static_cast<HostRmt*>(h.impl);
    h.impl = nullptr;
}
size_t rmtWs2812RxCapture(uint8_t /*gpio*/, uint32_t /*resolutionHz*/,
                          uint32_t* /*outSymbols*/, size_t /*maxSymbols*/,
                          uint32_t /*timeoutMs*/) {
    return 0;
}
RmtLoopbackResult rmtWs2812Loopback(uint8_t /*txGpio*/, uint8_t /*rxGpio*/) {
    return {};   // not supported off ESP32
}
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t /*txGpio*/, uint8_t /*rxGpio*/,
                                         uint16_t /*lights*/, uint8_t /*channels*/) {
    return {};   // not supported off ESP32
}
RmtLoopbackResult ws2812LoopbackRide(uint16_t /*rxGpio*/, const uint8_t* /*sent*/, uint8_t /*sentLen*/,
                                     size_t /*dataBytes*/, uint8_t /*rowBits*/,
                                     uint8_t /*clockMultiplier*/) {
    return {};   // no RMT-RX capture off ESP32
}

// ---------------------------------------------------------------------------
// Parallel-WS2812 buses on desktop: REAL MEMORY, no silicon.
//
// The repo's rule is that everything runs on the desktop build — the platform layer simply has
// no hardware behind the call. These used to return false/nullptr, which made every parallel
// backend report failure, so ParallelLedDriver's ~2500-line body never executed off-device: not
// runnable, not unit-testable, and invisible to every AST-based check.
//
// So the bus is implemented against a heap buffer. `init` allocates and zeroes, `Buffer` hands
// back writable memory, `Transmit` records the byte count, `Wait` returns immediately. Everything
// ABOVE the seam is then the same code that runs on hardware — the driver encodes real WS2812 bit
// patterns into a real buffer — and only the DMA hand-off is absent.
//
// What is deliberately NOT modelled: timing, wire protocol, pin state, and loopback capture.
// Those need silicon, and faking them would make the driver's self-test lie about hardware it
// never touched.
// ---------------------------------------------------------------------------
namespace {

/// One memory-backed parallel bus. Shared by the i80, MoonI80 and Parlio seams below — they are
/// three DMA peripherals for the same job, and off-device the job is "hold a frame".
struct HostBus {
    std::vector<uint8_t> buf[2];
    size_t capacity = 0;

    bool init(size_t bytes, bool wantSecond) {
        if (bytes == 0) return false;
        capacity = bytes;
        buf[0].assign(bytes, 0);
        if (wantSecond) buf[1].assign(bytes, 0);
        else            buf[1].clear();
        return true;
    }
    uint8_t* buffer(uint8_t i) {
        if (i > 1 || buf[i].empty()) return nullptr;
        return buf[i].data();
    }
    bool transmit(uint8_t i, size_t bytes) {
        if (i > 1 || buf[i].empty() || bytes > capacity) return false;
        return true;
    }
};

HostBus* hostBus(void*& impl) {
    if (!impl) impl = new HostBus();
    return static_cast<HostBus*>(impl);
}
void freeHostBus(void*& impl) {
    delete static_cast<HostBus*>(impl);
    impl = nullptr;
}

}  // namespace

bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* /*dataPins*/,
                   uint8_t /*laneCount*/, uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                   size_t bufferBytes, bool wantSecondBuffer,
                   uint8_t /*clockMultiplier*/) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the RMT seam does
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
// True, not false: the driver reads a false as "the previous frame never completed" and holds
// the next one back, which would stall the render path on a bus that is never busy.
bool i80Ws2812Wait(I80Ws2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& /*h*/) { return 0; }
void i80Ws2812Deinit(I80Ws2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult i80Ws2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                    uint16_t /*wrGpio*/, uint16_t /*dcGpio*/,
                                    uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                    size_t /*frameBytes*/, size_t /*dataBytes*/,
                                    uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/) {
    return {};   // not supported off the S3
}

// MoonI80 (our own LCD_CAM DMA driver, ADR-0014) — the same memory-backed bus as the esp_lcd
// family above. The RING path stays inert: it is a GDMA construct with no host equivalent, so a
// driver that would stream on device runs whole-frame here (busInitRing returns false and the
// orchestrator falls back, exactly as its contract specifies).
bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* /*dataPins*/,
                       uint8_t /*laneCount*/, uint16_t /*wrGpio*/,
                       size_t bufferBytes, bool wantSecondBuffer,
                       uint8_t /*clockMultiplier*/) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the other seams do
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
// Ring mode is a GDMA construct with no host equivalent — inert here, bench-verified on the S3, exactly
// like the whole-frame path above. A driver that would pick the ring on device stays whole-frame on host.
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& /*h*/, const uint16_t* /*dataPins*/,
                           uint8_t /*laneCount*/, uint16_t /*wrGpio*/, size_t /*rowBytes*/,
                           uint32_t /*totalRows*/, uint32_t /*rowsPerBuf*/, uint8_t /*ringBufs*/,
                           uint8_t /*padUs*/, uint8_t /*clockMultiplier*/, MoonI80EncodeFn /*encode*/,
                           void* /*user*/) {
    return false;
}
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& /*h*/) { return false; }
void moonI80SetShiftClockDiv(uint8_t /*div*/) {}
void moonI80Ws2812PrimeRange(MoonI80Ws2812Handle& /*h*/, uint8_t /*bufLo*/, uint8_t /*bufHi*/) {}
bool moonI80Ws2812ArmRing(MoonI80Ws2812Handle& /*h*/) { return false; }
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& /*h*/) { return false; }
bool moonI80Ws2812InternalFits(size_t /*bytes*/) { return false; }
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
bool moonI80Ws2812Wait(MoonI80Ws2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& /*h*/) { return 0; }
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& /*h*/) { return {}; }
void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                        uint16_t /*wrGpio*/,
                                        uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                        size_t /*frameBytes*/, size_t /*dataBytes*/,
                                        uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/,
                                        uint32_t /*ringRows*/, uint32_t /*ringBufs*/,
                                        bool /*useRing*/) {
    return {};   // not supported off LCD_CAM
}
RmtLoopbackResult moonI80Ws2812LoopbackRide(uint16_t /*rxGpio*/, const uint8_t* /*sent*/,
                                            uint8_t /*sentLen*/, size_t /*dataBytes*/,
                                            uint8_t /*rowBits*/, uint8_t /*clockMultiplier*/) {
    return {};   // not supported off LCD_CAM
}

// Parlio WS2812 — the same memory-backed bus. No Parlio silicon here, but the driver runs and
// its sizing/slicing is host-pinned by the driver tests.
bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* /*dataPins*/,
                      uint8_t /*laneCount*/, uint32_t /*pclkHz*/, size_t bufferBytes,
                      bool wantSecondBuffer) {
    if (bufferBytes == 0) return false;   // refuse before allocating, as the other seams do
    return hostBus(h.impl)->init(bufferBytes, wantSecondBuffer);
}
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer) {
    return h.impl ? static_cast<HostBus*>(h.impl)->buffer(buffer) : nullptr;
}
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h) {
    return h.impl ? static_cast<HostBus*>(h.impl)->capacity : 0;
}
bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes) {
    return h.impl && static_cast<HostBus*>(h.impl)->transmit(buffer, bytes);
}
bool parlioWs2812Wait(ParlioWs2812Handle& /*h*/, uint8_t /*buffer*/, uint32_t /*timeoutMs*/) { return true; }
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& /*h*/) { return 0; }
void parlioWs2812Deinit(ParlioWs2812Handle& h) { freeHostBus(h.impl); }
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* /*dataPins*/, uint8_t /*laneCount*/,
                                       uint16_t /*rxGpio*/, const uint8_t* /*frame*/,
                                       size_t /*frameBytes*/, size_t /*dataBytes*/,
                                       uint8_t /*rowBits*/) {
    return {};   // not supported off the P4
}

// Audio codec — desktop has no codec, so init is a no-op that succeeds (there's
// nothing to bring up); the inert audioMicInit below is what keeps capture off.
bool audioCodecInit(CodecType /*type*/, const AudioCodecPins& /*pins*/, uint32_t /*sampleRate*/) {
    return true;
}
void audioCodecDeinit() {}

// I2S microphone — no capture on desktop (hasI2sMic == false, AudioService inert),
// so init fails and read returns nothing.
bool audioMicInit(AudioMicHandle& /*h*/, uint16_t /*wsPin*/, uint16_t /*sdPin*/,
                  uint16_t /*sckPin*/, int16_t /*mclkPin*/, uint32_t /*sampleRate*/) {
    return false;
}
size_t audioMicRead(AudioMicHandle& /*h*/, int32_t* /*out*/, size_t /*maxSamples*/) {
    return 0;
}
void audioMicDeinit(AudioMicHandle& /*h*/) {}

// FFT kernel — a real but naive O(n^2) DFT. NOT the production kernel (the ESP32
// uses esp-dsp's fast radix-2), but a correct reference so the host tests run the
// genuine magnitude->band path on synthesized signals. n must be a power of two;
// fills outMag[0..n/2) with the bin magnitudes.
void audioFft(const float* windowed, size_t n, float* outMag) {
    if (!windowed || !outMag || n == 0) return;
    const float twoPiOverN = -2.0f * std::numbers::pi_v<float> / static_cast<float>(n);
    for (size_t k = 0; k < n / 2; k++) {
        float re = 0.0f, im = 0.0f;
        for (size_t t = 0; t < n; t++) {
            const float a = twoPiOverN * static_cast<float>(k) * static_cast<float>(t);
            re += windowed[t] * std::cos(a);
            im += windowed[t] * std::sin(a);
        }
        outMag[k] = std::sqrt(re * re + im * im);
    }
}

// No I2C bus on the desktop host — report it as unavailable (the sentinel), the same as
// an I2C-less ESP32 target, so the module shows "bus unavailable" rather than a misleading
// "0 devices found" (which means "scanned a real bus, nothing ACKed").
size_t i2cScan(uint16_t /*sda*/, uint16_t /*scl*/, uint8_t* /*out*/, size_t /*maxOut*/) {
    return kI2cBusUnavailable;
}

// No IR receiver on the host — the seam is a no-op so IrService runs (its buttons still work
// through Scheduler::setControl); reception is ESP32-only.
bool irRead(uint16_t /*pin*/, uint32_t& /*codeOut*/) { return false; }
void irStop() {}   // no IR hardware on desktop
bool irChannelReady(uint16_t /*pin*/) { return true; }   // no channel to fail on desktop

} // namespace mm::platform
