#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include "platform_config.h"  // hasOta / hasPsram / … — flags this header's contract refers to

namespace mm::platform {

uint32_t millis();
uint32_t micros();

// Test-only override: when set to non-zero, millis() returns this value instead
// of reading the platform clock. Production code never calls this; tests use it
// to drive virtual time deterministically (replaces the wall-clock delayMs in
// animation tests). Pass 0 to restore real-clock behaviour — tests must reset
// in release so cases stay independent. ESP32 honours the override too so a
// scenario-tests run on real hardware can still freeze time if needed.
void setTestNowMs(uint32_t ms);

// Force the next UdpSocket::bind() calls to fail, so a test can exercise a bind-failure path without
// depending on the OS to refuse a port. Production code never calls this. The alternative — hog the
// port with a second socket — is NOT portable: on Linux, SO_REUSEADDR on a UDP socket bound to
// INADDR_ANY *permits* the overlapping bind, so the hog succeeds and the failure never happens (this
// silently broke unit_AudioService_sync on Linux for as long as it existed; nothing caught it because
// CI did not compile the C++ tests until the sanitizer job). Nor is a privileged port reliable —
// modern macOS lets a non-root process bind port 80. Pass false to restore; tests must reset in
// release so cases stay independent, same contract as setTestNowMs.
void setTestBindFails(bool fail);

void* alloc(size_t bytes);
void free(void* ptr);

// Internal-RAM-only allocation — the mirror of alloc()'s PSRAM-first policy, for buffers a hot ISR READS.
// alloc() prefers PSRAM because most large buffers are touched from tasks where PSRAM latency amortizes;
// a buffer read per-byte inside an interrupt (the streaming ring's encode source) pays that latency
// hundreds of times per invocation and blows its deadline (measured: ~595 µs per slice refill with a
// PSRAM-resident source, against a 151 µs drain budget). Returns nullptr when internal RAM can't supply it —
// the CALLER decides the fallback (typically plain alloc(), accepting the slower PSRAM read over failing).
// Free with the ordinary free().
void* allocInternal(size_t bytes);

// Executable memory for JIT-emitted native code (MoonLive). Distinct from alloc()
// because code must live in memory the CPU can FETCH from, not just read/write:
// IRAM on ESP32 (MALLOC_CAP_EXEC), an mmap'd PROT_EXEC page on desktop. Returns
// nullptr when exec memory is exhausted — the caller degrades (status, runs dark),
// never crashes. freeExec takes the same size so a backend that needs it (munmap)
// has it; ESP32 ignores the size.
void* allocExec(size_t bytes);
void  freeExec(void* ptr, size_t bytes);

// Copy emitted code INTO an allocExec block, then make it executable. Separate from a
// plain memcpy because ESP32 IRAM is fetch-only on the instruction bus and writable
// only via 32-bit-aligned data-bus stores (a byte memcpy faults), and after writing,
// the instruction cache must be synced so the core fetches the fresh bytes, not stale
// cache. Both quirks live here, behind the platform line; the engine just hands over
// (dst-from-allocExec, src-bytes, len). On desktop this is a plain memcpy. `len` need
// not be a multiple of 4 — the ESP32 path pads the final partial word.
void  writeExec(void* dst, const void* src, size_t len);

void yield();
void delayMs(uint32_t ms);  // blocking sleep; only use outside the hot path
void delayUs(uint32_t us);  // blocking busy-wait for sub-ms protocol gaps (e.g.
                            // the WS2812 inter-frame latch); fine for a few
                            // hundred µs, not a general-purpose sleep
size_t freeHeap();          // total free (internal + PSRAM if present)
size_t freeInternalHeap();  // internal RAM only (for stack/HTTP/WiFi reserve check)
size_t maxAllocBlock();     // largest contiguous block (any memory type — incl PSRAM)
size_t maxInternalAllocBlock(); // largest contiguous block in INTERNAL RAM only

// --- RTOS task introspection (TasksModule) --------------------------------------------------
// A fixed-size, allocation-free snapshot of the OS tasks, filled by the platform layer so no
// FreeRTOS type escapes src/platform/ (the platform-boundary rule). ESP32 fills it from
// uxTaskGetSystemState (the textbook RTOS-introspection call, needs CONFIG_FREERTOS_USE_TRACE_
// FACILITY); it uses a fixed static scratch (no heap) but briefly suspends the scheduler while it
// walks the task list — call it off the per-frame path (tick1s, once a second), not from tick().
// Desktop returns 0 (no RTOS). cpuPermille is 0..1000, or kTaskCpuUnmeasured when run-time-stats are
// compiled out (the cheap default) — the caller shows a CPU% column only when it's a real number.
enum class TaskState : uint8_t { Running, Ready, Blocked, Suspended, Deleted, Invalid, Unknown };
constexpr uint32_t kTaskCpuUnmeasured = 0xFFFFFFFFu;
struct TaskInfo {
    char      name[16] = {};
    TaskState state = TaskState::Unknown;
    int8_t    core = -1;               // 0, 1, or -1 (no affinity)
    uint8_t   priority = 0;
    uint32_t  stackFreeBytes = 0;      // high-water-mark: min free stack ever seen for this task
    uint32_t  cpuPermille = kTaskCpuUnmeasured;  // 0..1000, or kTaskCpuUnmeasured (stats off)
};
// Fill `out` with up to `maxTasks` task rows; returns the count written. 0 on a target without
// an RTOS (desktop) or without the trace facility compiled in.
size_t taskSnapshot(TaskInfo* out, size_t maxTasks);
// Name of the task currently running on `core` (0 or 1); empty string if unavailable/single-core.
void currentTaskOnCore(int core, char* out, size_t cap);
// Name of the RTOS task that runs the render loop (Scheduler::tick) — the task every MoonModule
// executes inside today. TasksModule nests the module rows under the matching `tasks` entry rather
// than hardcoding a task name. Empty on a target with no distinct render task (desktop).
const char* renderTaskName();
// Test-only (desktop): inject a canned task snapshot + render-task name so TasksModule's row/detail
// JSON + nesting predicate are testable on the host (no RTOS otherwise). `tasks` must outlive the use.
void setTestTaskSnapshot(const TaskInfo* tasks, size_t count, const char* renderTask);

// --- Pinned worker task + wake notification (render/encode multicore split) ------------------
// A minimal own-a-thread seam: spawn one function on a named task pinned to `core`, plus a
// single-slot wake notification. This is FreeRTOS's textbook lock-free pairing —
// xTaskCreatePinnedToCore + a direct-to-task notification (xTaskNotifyGive / ulTaskNotifyTake),
// which the RTOS documents as the lightweight replacement for a binary semaphore in a
// single-producer/single-consumer wake. The multicore pipeline (Drivers render↔encode split)
// is the first user; the async-ArtNet send wants the same primitive, so it lives in core.
// No FreeRTOS type escapes the header (opaque handle, same rule as RmtWs2812Handle). Desktop
// backs it with std::thread + a condition_variable so the handoff invariants are host-testable,
// even though the core-split itself is an ESP32-only capability.
struct WorkerTask { void* impl = nullptr; };
using WorkerFn = void(*)(void* user);
// Spawn `fn(user)` on a task named `name` with `stackBytes` stack, at `priority`, pinned to
// `core` (0 or 1; -1 = no affinity). Returns false if the task couldn't be created — the caller
// then runs the work inline (the allocate-and-degrade fallback). The spawned fn owns its loop and
// returns only after stopPinnedTask signals it.
bool spawnPinnedTask(WorkerTask& t, const char* name, WorkerFn fn, void* user,
                     size_t stackBytes, uint8_t priority, int core);
// Wake the task blocked in waitNotify (producer side; safe from any task on any core).
void notifyTask(WorkerTask& t);
// Block the spawned task until notifyTask fires or `timeoutMs` elapses; false on timeout (so the
// worker can service its own watchdog and re-check its stop flag). Called ONLY from inside the fn.
bool waitNotify(WorkerTask& t, uint32_t timeoutMs);
// Signal stop + wake, then block until the worker fn has returned and the task is torn down.
void stopPinnedTask(WorkerTask& t);
// Reset THIS task's watchdog (esp_task_wdt_reset) from inside a worker doing a long encode. No-op
// on desktop. Keeps the vTaskDelay(1)/WDT discipline the single render loop has today.
void taskWdtReset();

// --- GPIO capability introspection (PinsModule) ---------------------------------------------
// Static per-pin capability for one GPIO, so the pin ownership map can flag a claim that lands on
// an unsafe pin — an output role driven onto an input-only pin or a boot strap, or any role on a
// reserved (flash/PSRAM/USB) pin. Domain-neutral; no chip API escapes src/platform/. ESP32 fills
// `validGpio`/`outputCapable`/`rtc` from the IDF's own GPIO_IS_VALID_GPIO / GPIO_IS_VALID_OUTPUT_
// GPIO / rtc_gpio_is_valid_gpio (the textbook, always-correct SDK queries), and overlays `strap` /
// `reserved` from a small per-chip table sourced from docs/reference/gpio-usage.md (the SDK has no
// "is this a strap / flash pin" query — that's board/datasheet knowledge). Desktop returns
// "all valid, nothing reserved" (a host build has no real GPIOs to protect). Pure lookup, no state.
struct GpioCapability {
    bool validGpio = true;      // a real, usable GPIO on this chip (false = out of range / not bonded)
    bool outputCapable = true;  // has an output driver (false = input-only, e.g. classic ESP32 34-39)
    bool rtc = false;           // an RTC/low-power-domain pin (usable for deep-sleep wake / RTC I/O)
    bool strap = false;         // a boot-strapping pin — driving it at reset can change boot mode
    bool reserved = false;      // wired to flash / PSRAM / native USB — routing I/O here corrupts the device
};
GpioCapability gpioCapability(uint8_t gpio);
// Test-only (desktop): make gpioCapability(gpio) return `cap` for one specific gpio, so PinsModule's
// severity derivation (reserved→error, driven-role-on-strap/input-only→warn) is testable on the host
// where every real pin is otherwise "safe". Call per gpio under test; reset in release.
void setTestGpioCapability(uint8_t gpio, GpioCapability cap);
void clearTestGpioCapability();

// Live electrical state of one GPIO — the pin map's second axis (what a pin is *doing now*, vs.
// gpioCapability's static "what it *is*"). The see-the-wire HAL check: gpio_get_level reads the pad on
// ANY pin, even one a peripheral drives, so a driver's output must toggle when it renders and a mic
// clock must toggle when the mic runs. Sampled on tick1s (off the hot path), not per frame. Desktop
// returns valid=false (no real pins) so the UI omits the live columns there.
struct GpioLiveState {
    bool valid = false;    // pin is readable (false = out of range, or desktop → columns omitted)
    bool level = false;    // current pad level: true = HIGH, false = LOW (gpio_get_level)
    bool output = false;   // the pad's output driver is enabled RIGHT NOW (gpio_get_io_config .oe)
    bool input = false;    // the pad's input buffer is enabled RIGHT NOW (.ie) — a pin can be both
    uint8_t driveCap = 0;  // output drive strength 0..3 = WEAK / MEDIUM / STRONG / STRONGEST
};
GpioLiveState gpioLiveState(uint8_t gpio);
// Test-only (desktop): inject a live state for one gpio so PinsModule's level/drive columns are
// host-testable. Same shape as setTestGpioCapability.
void setTestGpioLiveState(uint8_t gpio, GpioLiveState state);
void clearTestGpioLiveState();

// Test-only cap on the value maxAllocBlock() reports; 0 = no cap (real value).
// Lets a test force MappingLUT's paged-destinations fallback without an actual
// fragmented heap. Production never calls this; reset to 0 in release.
void setTestMaxAllocBlock(size_t bytes);
                                // (scarce; use this as the memory-pressure KPI).
                                // PSRAM blocks dominate on S3/S2 boards and make
                                // maxAllocBlock useless as a stress signal —
                                // it'll report ~8 MB even when DRAM is exhausted.
size_t totalHeap();         // total heap capacity (internal + PSRAM)
size_t totalInternalHeap(); // total internal heap capacity

// Heap to keep free for stack, HTTP, WiFi, and overhead when sizing buffers —
// a platform memory constraint, not a domain one (it guards core subsystems).
// Any allocator checks free heap against this reserve before committing.
constexpr size_t HEAP_RESERVE = 32768;

void getMacAddress(uint8_t mac[6]);
// The MAC as canonical "XX:XX:XX:XX:XX:XX", formatted once into a static buffer — a stable per-chip
// identity string a caller can point at without keeping its own copy. (chipModel/sdkVersion likewise
// return static strings; a ReadOnly control binds straight to these, storing nothing per-module.)
const char* macString();
const char* chipModel();
const char* sdkVersion();

// CPU frequency + core count as one short static string ("240 MHz, 2 cores"), read from the RUNNING
// hardware, not a config macro — so a stale sdkconfig or a PM downclock is visible in the UI (finding
// the chip silently at 160 MHz is exactly what this control exists to catch). Desktop reports cores
// only (host clock speed has no portable query). Static-buffer contract as macString above.
const char* cpuInfo();

// PSRAM interface type as a short static string: "quad" (1-line SPI, classic ESP32 / WROVER) or
// "octal" (8-line, the S3/S2 -R8 parts). Derived from the compile-time CONFIG_SPIRAM_MODE (there is no
// runtime IDF query for the mode), so it reflects how the firmware drives the PSRAM. Empty "" when
// PSRAM is not enabled in this build. SystemModule shows it beside the psram usage so the type is
// visible (an S3 board reads "octal", a WROVER "quad"). Desktop returns "".
const char* psramType();

// WiFi co-processor status, for boards whose radio lives on a separate chip (the
// ESP32-P4 + on-board ESP32-C6 over esp_hosted). Returns a short status string:
// the detected co-processor firmware version when the link is up (e.g.
// "C6 fw 2.12.9"), "not detected" when the slave never completed its handshake or
// reports 0.0.0 (the tell for absent / incompatible C6 slave firmware), or "" on
// targets with a native radio (no co-processor). Lets SystemModule prove the C6
// firmware state instead of guessing. Empty string => render nothing.
const char* coprocessorWifi();

// This host's LAN IPv4 address as a dotted string, or "" if unavailable.
// Desktop: the outbound interface address. ESP32: empty — the device IP is
// owned by NetworkModule (WiFi/Ethernet), not the platform layer.
const char* hostIp();

// Human-readable reset reason: "POWERON", "SW", "PANIC", "INT_WDT", "TASK_WDT",
// "BROWNOUT", "DEEPSLEEP", or "UNKNOWN". On desktop always returns "OK". UI uses
// this to flag a "crashed" prior boot (PANIC / INT_WDT / TASK_WDT / BROWNOUT).
const char* resetReason();
size_t firmwareSize();        // firmware image bytes
size_t firmwarePartition();   // app partition size (firmware capacity)
size_t flashChipSize();       // total flash chip capacity
size_t filesystemUsed();      // filesystem used bytes
size_t filesystemTotal();     // filesystem total bytes

// Filesystem — LittleFS on ESP32, std::filesystem on desktop (rooted at ./.config/'s parent).
// Paths are absolute-looking (start with '/'); desktop strips the leading '/' so
// "/.config/System.json" maps to "<root>/.config/System.json".
//
// fsSetRoot redirects the desktop root from CWD to an absolute path. Used by unit tests
// to give each TEST_CASE an isolated working directory without chdir. No-op on ESP32
// (LittleFS is mounted at a fixed partition). Must be called BEFORE fsMount; defaults
// to ".".
void fsSetRoot(const char* path);
bool fsMount();                                              // idempotent; safe to call multiple times
void fsUnmount();
bool fsMkdir(const char* path);                              // mkdir -p; no error if exists
bool fsExists(const char* path);
bool fsRemove(const char* path);                             // file or empty dir
int  fsRead(const char* path, char* buf, size_t maxLen);     // bytes read; -1 on error; null-terminated on success
long fsSize(const char* path);                               // file size in bytes; -1 if missing/not a file
int  fsReadAt(const char* path, long offset, char* buf, size_t len);
                                                             // pread-style: read up to `len` bytes at `offset`; bytes read, 0 at EOF, -1 on error. NOT null-terminated. Lets a caller stream a file in fixed chunks.
bool fsWriteAtomic(const char* path, const char* data, size_t len);
                                                              // writes <path>.tmp, fsync, rename. Caller ensures parent dir exists.
// Streamed atomic write: open <path>.tmp, repeatedly call src(buf, cap, user, &abort) to pull up to
// `cap` bytes, fwrite each chunk, then fsync + rename into place. The source returns the byte count;
// 0 means *end*, BUT it distinguishes a clean end from a failed/incomplete one via `*abort`: a 0
// with `*abort == false` is a clean EOF (commit), a 0 (or any return) with `*abort == true` is an
// error (a short/timed-out upload) → the temp file is DISCARDED, not renamed. Returns false on abort,
// a write failure, or a rename failure. Lets the HTTP layer stream an upload of any size with a fixed
// small buffer — the device never holds the whole file in RAM. Caller ensures the parent dir exists.
using FsWriteSrc = size_t(*)(char* buf, size_t cap, void* user, bool* abort);
bool fsWriteStream(const char* path, FsWriteSrc src, void* user);
// Per-entry callback for fsList: name, whether it's a directory, and its size in bytes
// (0 for a directory). One level, one call per child.
using FsListCb = void(*)(const char* name, bool isDir, uint32_t sizeBytes, void* user);
void fsList(const char* dir, FsListCb cb, void* user);       // single-level listing

// Network (ESP32 only, stubs on desktop)
// setEthConfig overrides the per-chip default eth pin/PHY map (ethConfigDefault)
// with a board's runtime config before ethInit — NetworkModule pushes the values
// it got from deviceModels.json. Call before ethInit(); takes effect on the next init.
void setEthConfig(const EthPinConfig& cfg);
bool ethInit();
// Tear down a running Ethernet driver so ethInit() can re-init with new config —
// the live reconfigure path (used for W5500/SPI, which tears down cleanly; RMII
// keeps apply-on-next-init). Safe to call when nothing is running. Desktop: no-op.
void ethStop();
bool ethLinkUp();       // PHY link detected (cable plugged, fast check)
bool ethConnected();    // IP assigned (DHCP complete)
// Current IP as raw octets — out[0..3]. All-zero (0.0.0.0) means "no IP yet".
// Octets, not a string: the IP's canonical form is uint8_t[4] (matching the
// static-IP controls and formatDottedQuad); callers that need text format at
// their own boundary, callers that need bytes (ArtNet) use them directly.
void ethGetIPv4(uint8_t out[4]);

bool wifiStaInit(const char* ssid, const char* password);
bool wifiStaConnected();
void wifiStaGetIPv4(uint8_t out[4]);   // see ethGetIPv4 — same octet contract
void wifiStaStop();

// STA-side RSSI in dBm (negative, e.g. -58). Returns 0 when the STA isn't
// associated or the call fails — NetworkModule only surfaces this control
// while state_ == ConnectedSta so a 0 is effectively unreachable.
int wifiStaRssi();

// STA-side AP info for the WLED /json `wifi` block: the associated AP's BSSID
// (6 octets into `out`) and the WiFi channel. Both zeroed (all-zero BSSID /
// channel 0) when the STA isn't associated or the call fails.
void wifiStaBssid(uint8_t out[6]);
int  wifiStaChannel();

bool wifiApInit(const char* apName, const char* ip);
bool wifiApConnected();
void wifiApStop();
// Stations currently associated with our SoftAP. 0 when the AP is down or nobody is on it.
// NetworkModule's AP fallback uses this to hold off its periodic STA retry while somebody is using
// the portal: bringing STA up switches the radio to STA mode, which drops the AP under them.
uint32_t wifiApClientCount();

// True when it is safe to open/use a socket: the TCP/IP stack is initialised and
// an interface has an IP. On ESP32 that means Ethernet or WiFi (STA/AP) is up —
// calling any lwip socket API before then asserts (the core mutex is still null).
// Desktop: always true (host sockets work regardless of link state). Callers that
// open sockets at boot (before NetworkModule brings an interface up) must gate on
// this and open lazily from the tick path once it turns true.
bool networkReady();

// Current WiFi transmit power, in dBm (ESP-IDF reports quarter-dBm internally
// and we round to whole). Returns 0 when WiFi isn't initialised or the call
// fails. Same value for STA and AP — WiFi has one radio at one TX power.
int wifiTxPower();

// Cap the WiFi transmit power. `quarterDbm` is in ESP-IDF's quarter-dBm units
// (valid range 8..84 → 2..21 dBm); pass 0 to skip the override and let the
// stack use its default. Used by NetworkModule for the weak-power / brown-out
// WiFi cap: some boards / WiFi modules (a thin on-module LDO, a marginal USB
// supply — e.g. various S2/S3 mini-class boards) brown out at full TX power,
// dropping WiFi during association. Capping to 8 dBm (32 quarter-dBm, the value
// `deviceModels.json` injects for brown-out-prone entries) keeps them stable. Returns
// true on success or when called with 0 (no-op).
// Call after esp_wifi_start() — earlier calls are silently ignored by ESP-IDF.
bool wifiSetTxPower(int8_t quarterDbm);

// mDNS is advertise-only: `mdnsInit` brings the stack up and advertises this device as
// `_http._tcp` (with an `mm=1` TXT) and `_wled._tcp` (with a `mac=` TXT), so the native
// WLED app + Home Assistant discover it. Peer discovery is UDP presence (DevicesModule +
// WledPacket) — the platform exposes advertise here, discovery lives in the module.
bool mdnsInit(const char* deviceName);
// Stop advertising: remove both services and clear the hostname, keeping the stack up so
// a later mdnsInit re-advertises without a full re-init (the mDNS toggle uses this).
void mdnsStop();
// Full mdns_free — call at release.
void mdnsShutdown();

// Store the DHCP hostname (DHCP option 12) the next eth/wifi bring-up advertises.
// Routers populate their client list from the DHCP request, not mDNS, so without
// this a provisioned device shows as "Unknown" there. Call before ethInit() /
// wifiStaInit() — the netif applies it before the DHCP client starts, so it lands
// in the first DISCOVER. NetworkModule pushes deviceName (default MM-XXXX), keeping
// the router name, the mDNS .local name and the SoftAP name one identity. A later
// rename takes effect on the next DHCP renewal/reconnect. Desktop: no-op.
void setHostname(const char* name);

// OTA — fetch a firmware image from `url` and flash it to the next OTA partition.
// ESP32: spawns a one-shot FreeRTOS task (the call returns immediately; the task
// runs to completion or error). The task uses `esp_https_ota`, which rolls the
// download + partition write + boot-pointer flip into one API; on success it
// calls platform::reboot() so the device boots the new image.
// Desktop: returns false (no OTA partition to write to); call sites guard with
// `if constexpr (mm::platform::hasOta)`.
//
// `statusBuf` is updated in place by the task with a short progress string
// (e.g. "downloading", "flashing", "error: HTTP 404"). `bytesReadOut` /
// `bytesTotalOut` advance as the download proceeds — the UI renders them as
// "X KB / Y KB". `bytesTotalOut` is 0 until esp_https_ota reports the image
// size (just after the HTTPS handshake), then holds the real value for the
// rest of the task's lifetime. FirmwareUpdateModule polls all three at 1 Hz
// and copies into its Control buffers so the WS state push surfaces progress
// to the UI without extra wiring.
bool http_fetch_to_ota(const char* url,
                       char* statusBuf, size_t statusBufLen,
                       uint32_t* bytesReadOut, uint32_t* bytesTotalOut);

// OTA — flash a firmware image STREAMED from `src` (no URL fetch; the caller pulls the bytes,
// e.g. straight off an HTTP upload body). Same producer callback shape as fsWriteStream: `src`
// fills up to `cap` bytes, returns the count (0 = clean EOF), and sets `*abort` to fail the OTA
// (an incomplete/timed-out upload). Runs esp_ota_begin → esp_ota_write per chunk → esp_ota_end +
// set_boot_partition, then RETURNS true (it does NOT reboot — the caller sends its HTTP 200 first,
// then reboots into the flashed image, the same order /api/reboot uses). SYNCHRONOUS (unlike
// http_fetch_to_ota, which runs on its own task): the caller is the HTTP request handler, which runs
// on the tick20ms tick INSIDE Scheduler::tick — so this blocks rendering for the flash duration. That
// is the accepted trade-off (a firmware upload is user-initiated and reboots the device on success),
// bounded by the same upload idle/hard limits; the caller needs the result to reply.
// `statusBuf` / `bytesReadOut` are updated in place (bytesTotal is the caller-supplied
// Content-Length, so no separate out-param). Desktop: returns false (no OTA partition); guard with
// `if constexpr (mm::platform::hasOta)`. Returns true iff the image flashed + boot pointer flipped.
bool otaWriteStream(FsWriteSrc src, void* user, size_t contentLen,
                    char* statusBuf, size_t statusBufLen, uint32_t* bytesReadOut);

// Synchronous outbound HTTP request to a LAN host — plain HTTP, no TLS (the Philips Hue v1
// API, which HueDriver drives, allows it). Connects to `host:port`, sends `method path`
// with `reqBody` (NUL-terminated; "" for none — a Content-Length + JSON content-type are
// added when non-empty), and copies the RESPONSE BODY into `body` (NUL-terminated, truncated
// to bodyLen-1). Returns the HTTP status code, or 0 on connect/timeout/error. Blocks up to
// `timeoutMs`. Caller runs this OFF the render hot path (HueDriver on tick1s, like the OTA
// fetch / the old mDNS browse). Built on raw sockets, same primitives as the HTTP server.
int httpRequest(const char* method, const char* host, uint16_t port, const char* path,
                const char* reqBody, uint32_t timeoutMs, char* body, size_t bodyLen);

// Improv WiFi provisioning over UART0.
// ESP32 only; desktop stub returns false. Spawns a FreeRTOS task that installs
// a UART driver on UART_NUM_0 (the same channel ESP-IDF logging writes to;
// they coexist because logging uses direct register writes, not the driver).
// The task feeds inbound bytes into the `improv/improv` parser.
//
// On a provision request the task validates state: if wifiStaConnected() is
// true it emits Improv's wrong-state error frame. Otherwise it copies the
// credentials into the caller-owned buffers `ssidOut` / `passwordOut` (sized
// to hold 33 + 64 bytes, matching NetworkModule's storage) and sets `*ready`
// to true. The caller's tick1s() polls `ready`, copies the buffers onward,
// and clears the flag.
//
// `statusBuf` mirrors http_fetch_to_ota's pattern: the task writes short
// strings ("listening", "received credentials", "connecting",
// "connected: <ssid>", "error: <reason>"). ImprovProvisioningModule polls
// it into a read-only Control.
//
// `info` is borrowed; the task copies the strings on init (pass static
// storage like `kVersion` and SystemModule::deviceName()).
struct ImprovDeviceInfo {
    const char* name;            // device hostname, e.g. "MM-3A7F"
    const char* chipFamily;      // "ESP32" / "ESP32-S3" / ...
    const char* firmwareVersion; // e.g. "1.0.0-rc2"
};
// SET_TX_POWER RPC (command 0xFD) — when set, the Improv task validates the
// 1-byte dBm payload (0..21), writes it to txPowerOut, and publishes via
// txPowerReady's release-store. This is the pre-association escape hatch for
// boards whose LDO browns out at full TX power (weak-powered boards): their
// catalog cap normally arrives over HTTP *after* the device is online,
// which such a board can never reach — proven on the bench 2026-06-10. It stays
// a dedicated RPC (not an APPLY_OP) precisely because it must land BEFORE the
// radio associates, whereas APPLY_OP ops apply once the device is up.
// opOut/opOutLen/opReady carry the APPLY_OP vendor RPC (0xFC) — one REST operation
// as JSON, pushed over serial during provisioning ("Improv = REST over serial").
// Chunks reassemble into opOut; on the last chunk opReady's release-store publishes
// it and ImprovProvisioningModule applies the op on the main loop. This is how the
// deviceModel and every other catalog default arrive: a `set`/`add` op routed through
// the apply-core + per-control validators, the same path the HTTP API uses.
// Opt out by leaving null (desktop stub does).
bool improvProvisioningInit(const ImprovDeviceInfo& info,
                            char* ssidOut, size_t ssidOutLen,
                            char* passwordOut, size_t passwordOutLen,
                            std::atomic<bool>* ready,
                            char* statusBuf, size_t statusBufLen,
                            uint8_t* txPowerOut = nullptr,
                            std::atomic<bool>* txPowerReady = nullptr,
                            char* opOut = nullptr, size_t opOutLen = 0,
                            std::atomic<bool>* opReady = nullptr);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open();
    // Bind a fixed destination so each sendTo() skips the per-packet address
    // parse + route lookup. Returns false on a bad IP.
    bool connect(const char* ip, uint16_t port);
    bool sendTo(const uint8_t* data, size_t len);  // uses the connect()ed destination
    // Receiver side (ArtNet in): listen on `port` on any interface
    // (SO_REUSEADDR) and flip the socket non-blocking — note that flips the
    // whole socket, sendTo() included. Returns false when the port is taken.
    bool bind(uint16_t port);
    // Non-blocking receive of one datagram: >0 = bytes copied into buf, -1 =
    // nothing pending. Mirrors TcpConnection::read's contract minus the
    // peer-closed 0 case (UDP has no connection to close). A datagram longer
    // than maxLen is truncated. Pass `srcIp` to also get the sender's IPv4
    // octets (ArtNet discovery replies go back to the poller's address).
    int recvFrom(uint8_t* buf, size_t maxLen, uint8_t srcIp[4] = nullptr);
    // One-shot send to an explicit address — for replying on a bound,
    // unconnected receive socket (e.g. ArtPollReply to the poller). connect()ed
    // send sockets keep using sendTo().
    bool sendToAddr(const uint8_t ip[4], uint16_t port, const uint8_t* data, size_t len);
    void close();

private:
    int fd_ = -1;
};

class TcpConnection {
public:
    TcpConnection() = default;
    explicit TcpConnection(int fd) : fd_(fd) {}
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    TcpConnection(TcpConnection&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    TcpConnection& operator=(TcpConnection&& other) noexcept {
        if (this != &other) { close(); fd_ = other.fd_; other.fd_ = -1; }
        return *this;
    }

    // Non-blocking outbound connect to host:port, for a client that must NOT stall the render loop
    // (MQTT runs on tick1s inside Scheduler::tick). `connectStart` resolves `host` (a hostname via
    // getaddrinfo — one bounded DNS lookup — or a dotted-quad IP) and kicks off a non-blocking
    // connect, returning immediately; `connectPoll` checks the in-flight connect WITHOUT blocking and
    // returns Pending / Connected / Failed. The caller polls across ticks and enforces its own overall
    // timeout, then reads/writes via the non-blocking read()/writeSome(). Caller gates on
    // networkReady(). Desktop + ESP32 (lwip); a fresh TcpConnection (no fd yet) is the receiver.
    enum class ConnectResult : uint8_t { Pending, Connected, Failed };
    bool connectStart(const char* host, uint16_t port);   // false = immediate failure (DNS/socket)
    ConnectResult connectPoll();                           // non-blocking; call after connectStart

    bool valid() const { return fd_ >= 0; }
    int read(uint8_t* buf, size_t maxLen);   // non-blocking: >0 data, 0 closed, -1 nothing
    bool write(const uint8_t* data, size_t len);  // blocking — sends all bytes (HTTP responses must complete)
    // Non-blocking partial write: send as many of `len` bytes as the socket accepts right
    // now, return the count actually written (0..len). -1 = socket error (caller closes);
    // 0 = WouldBlock (buffer full, try later) or len==0. The caller advances its own offset
    // and re-calls — used by the preview drain to stream a frame across ticks without ever
    // blocking the render task. Never spins, never yields. (int mirrors read()'s contract.)
    int writeSome(const uint8_t* data, size_t len);

    void close();

private:
    int fd_ = -1;
};

class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool open(uint16_t port);
    TcpConnection accept();  // non-blocking, returns invalid if none pending
    void close();

private:
    int fd_ = -1;
};

// Restart the device. On ESP32: hardware reset (esp_restart). On desktop: process exit.
// Does not return.
[[noreturn]] void reboot();

// ---------------------------------------------------------------------------
// RMT WS2812 LED output (classic ESP32 + S3 + P4). The driver (src/light/drivers/
// RmtLedDriver.h) does the symbol encode in domain code and may run several
// channels at once (one per pin); the platform owns only the peripheral. All
// no-ops on targets without RMT, so the driver compiles everywhere behind
// `if constexpr (platform::rmtTxChannels > 0)` (see platform_config.h) and is
// simply inert off the chips that have RMT.
// ---------------------------------------------------------------------------

// Opaque handle to one configured RMT TX channel. `impl` is set by the platform
// (a heap struct holding the channel + encoder); the driver never inspects it.
struct RmtWs2812Handle { void* impl = nullptr; };

// Allocate + configure one RMT TX channel on `gpio`. `resolutionHz` is the tick
// clock the caller expresses symbol durations in; `invert` flips output polarity
// for inverting level-shifters. Returns false on failure (and on non-ESP32).
bool rmtWs2812Init(RmtWs2812Handle& h, uint8_t gpio, uint32_t resolutionHz, bool invert);

// The tick resolution the platform actually granted (may differ from requested).
// The driver converts its ns timings to ticks with this. 0 if not initialised.
uint32_t rmtWs2812Resolution(const RmtWs2812Handle& h);

// Start transmitting `symbolCount` pre-encoded WS2812 RMT symbols and return
// immediately — channels started back-to-back clock out concurrently. Pair with
// rmtWs2812Wait; the caller owns the inter-frame latch (delayUs) after the last
// wait. The symbol buffer must stay valid until the wait returns. Returns false
// when the channel isn't initialised (and on targets without RMT).
bool rmtWs2812Transmit(RmtWs2812Handle& h, const uint32_t* symbols, size_t symbolCount);

// Block until the channel's in-flight transmission finishes, bounded by
// `timeoutMs` so a wedged peripheral can't hang the render tick forever — a
// timed-out frame is simply dropped and re-encoded next tick (self-heals). With
// N channels waited sequentially the worst case is N×timeoutMs; acceptable for
// the same self-healing reason.
void rmtWs2812Wait(RmtWs2812Handle& h, uint32_t timeoutMs);

void rmtWs2812Deinit(RmtWs2812Handle& h);

// RX loopback capture, on-device test only (no-op stub off ESP32). Capture up to
// `maxSymbols` pulse-duration symbols on `gpio` (jumpered from the TX pin) within
// `timeoutMs`. Returns the number captured. Used only by the loopback self-test.
size_t rmtWs2812RxCapture(uint8_t gpio, uint32_t resolutionHz,
                          uint32_t* outSymbols, size_t maxSymbols, uint32_t timeoutMs);


// Self-contained RMT loopback self-test, runnable from the running firmware (the
// RmtLedDriver's loopbackTest control). Drives a known WS2812 pattern out `txGpio`
// and captures it back on `rxGpio` (the user jumpers them), proving the GPIO emits
// correct bytes on real silicon. All hardware (RMT TX/RX, the GPIO continuity
// pre-check) lives here so src/light/ stays platform-free. No-op returning a
// "not supported" result off ESP32.
struct RmtLoopbackResult {
    bool jumperDetected = false;  // plain-GPIO continuity pre-check (tx high→rx high, low→low)
    bool pass = false;            // captured bytes == sent bytes (or whole frame bit-exact)
    uint8_t sent[3] = {};         // the per-light test pattern transmitted
    uint8_t got[3] = {};          // the light holding the first mismatch (light 0 when clean)
    uint32_t bitsChecked = 0;     // total WS2812 bits verified (frame mode); 24 for the short test
    uint32_t firstBadBit = 0;     // index of the first wrong bit, or bitsChecked when all pass
    // Capture diagnostics (frame mode). The verdict must say WHY it failed, not only that it did:
    // an empty capture (dead wiring, idle line, stalled transfer) is a different fault class from a
    // full capture that decodes wrong (waveform/threshold) — without these numbers both collapse
    // into the same "bad bit 0/0" and the instrument can't isolate anything.
    uint32_t capturedSymbols = 0; // RMT RX symbols actually captured (target: >= bitsChecked)
    int8_t rxIdleLevel = -1;      // RX GPIO level sampled after the capture window (-1 = unknown)
    uint32_t txWallUs = 0;        // wall time of the first (timed) transmit
    uint32_t txExpectUs = 0;      // expected wire time (byte count / configured clock) — a wall time
                                  // far above this is a stalled/underrun transfer, measured directly
};
RmtLoopbackResult rmtWs2812Loopback(uint8_t txGpio, uint8_t rxGpio);

// Whole-FRAME RMT loopback: instead of a 24-bit synthetic burst, transmit a
// real `lights`-light WS2812 frame (the per-light pattern 0xA5/0x00/0xFF
// repeated, `channels` per light) back to back like the render loop, capture
// the WHOLE frame on rxGpio and bit-verify every WS2812 bit. This is what
// catches frame-rate / sustained-transfer corruption and RF interference on
// the data line that a 24-bit burst can't — a single flipped bit anywhere in
// the frame fails the test and reports its position. No-op off ESP32.
RmtLoopbackResult rmtWs2812LoopbackFrame(uint8_t txGpio, uint8_t rxGpio,
                                         uint16_t lights, uint8_t channels);

// ---------------------------------------------------------------------------
// i80-bus parallel WS2812 output — the LCD_CAM peripheral on the ESP32-S3/P4, the
// I2S peripheral on the classic ESP32 (IDF's esp_lcd i80 API picks the backend per
// chip). The driver (src/light/drivers/MultiPinLedDriver.h) pre-encodes the WHOLE frame into one
// DMA buffer (3-slot encode in ParallelSlots.h, domain code); the platform owns
// only the i80 bus/peripheral AND the DMA buffer itself — the buffer must be
// DMA-capable internal RAM (platform::alloc prefers PSRAM, which the
// peripheral can't stream from at full rate), so the platform allocates it at
// init and exposes the pointer for the driver's zero-copy encode. All inert
// on targets without the i80 LCD peripheral, guarded by
// `if constexpr (platform::lcdLanes == 0)` in the driver.
// ---------------------------------------------------------------------------

// Opaque handle to one configured i80 bus + IO device + one or TWO DMA frame buffers.
struct I80Ws2812Handle { void* impl = nullptr; };

// Create the 8-lane bus on `dataPins[0..laneCount)` plus the two peripheral-
// mandated lines WS2812 strands ignore: `wrGpio` (the pixel clock) and
// `dcGpio` (data/command). Allocates buffer 0 (a zeroed DMA-capable frame of
// `bufferBytes`). When `wantSecondBuffer` is true (the async double-buffer is
// on), it also TRIES a second identical buffer and, if it fits, arms
// double-buffer mode; if it won't fit (memory-tight board), buffer 1 stays null
// and the driver runs single-buffer (allocate-and-degrade — the double-buffer
// is never *required*). When `wantSecondBuffer` is false (default), NO second
// buffer is allocated at all — the off path costs exactly one buffer. Returns
// false only when buffer 0 (or the bus) can't be created (bad pins, DMA pressure).
// `clockMultiplier` (1 = direct, 8 = a 74HCT595 shift-register expander on every data pin) scales
// the pixel clock: a '595 is serial-in, so each WS2812 slot is shifted out over that many bus
// words, and the bus must clock proportionally faster to keep the slot's duration on the wire.
// The backend picks the exact rate its clock tree can divide to EXACTLY (an inexact rate is not an
// error in esp_lcd — it silently rounds the prescale, which would emit a wrong waveform). A
// multiplier > 1 is rejected on a backend that cannot DMA the resulting frame from PSRAM (the
// classic-ESP32 I2S i80 path), rather than driving a frame the hardware can't sustain.
bool i80Ws2812Init(I80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                   uint16_t wrGpio, uint16_t dcGpio, size_t bufferBytes,
                   bool wantSecondBuffer, uint8_t clockMultiplier = 1);

// DMA frame buffer `buffer` (0 or 1) the driver encodes into (zero-copy).
// Buffer 0 always exists once init succeeded; buffer 1 is null when the second
// allocation didn't fit — the driver reads that null as "run single-buffer".
// `i80Ws2812BufferCapacity` is the shared per-buffer capacity (both buffers are
// the same size) — the driver's grow-only check. nullptr / 0 when not initialised.
uint8_t* i80Ws2812Buffer(const I80Ws2812Handle& h, uint8_t buffer);
size_t i80Ws2812BufferCapacity(const I80Ws2812Handle& h);

// Start the autonomous DMA transfer of buffer `buffer`'s first `bytes` and
// return; pair with i80Ws2812Wait on the SAME buffer. Once started no CPU work
// remains — there is no refill deadline for WiFi to miss (the design difference
// vs the ISR-refilled rings in the hpwit/FastLED lineage). The deferred-wait
// tick encodes into the other buffer while this one clocks out.
bool i80Ws2812Transmit(I80Ws2812Handle& h, uint8_t buffer, size_t bytes);

// Block until buffer `buffer`'s in-flight transfer finishes, bounded by `timeoutMs`.
// Returns TRUE only when the transfer actually completed. FALSE on timeout — the DMA may still be
// reading that buffer, so the caller must NOT re-encode into it (see ParallelLedDriver::busWaitIfBusy,
// which keeps it marked in-flight and re-waits next tick rather than corrupting a live transfer).
bool i80Ws2812Wait(I80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

// Duration in microseconds of the most recent completed DMA transfer — measured start-of-transmit
// to done-callback, so it is the PURE wire/DMA time (independent of CPU / render load), i.e. the
// hard WS2812 output floor (256 lights × 30 µs ≈ 7680 µs → the 130 fps ceiling). The driver surfaces
// it as a read-only KPI so the actual output rate is visible as the pipeline improves (and if a
// future build overclocks the slot rate, this reflects it directly). 0 until the first transfer completes.
uint32_t i80Ws2812LastTransmitUs(const I80Ws2812Handle& h);

void i80Ws2812Deinit(I80Ws2812Handle& h);

// LCD loopback self-test: build a private FULL-WIDTH bus on the driver's
// real pins (the i80 peripheral configures all 8 data lines — a partial bus
// is rejected by the hardware layer) and transmit the caller's REAL encoded
// frame (`frame`/`frameBytes`, lane 0 = dataPins[0]) back to back, exactly
// like the render loop, while an RMT RX channel captures the whole frame off
// `rxGpio` and verifies every bit (RMT receive is transmitter-agnostic — the
// increment-1 rig reused). `dataBytes` is the slot-carrying prefix of the
// frame (before the latch pad); `rowBits` the bits per light row, so the
// expected pattern repeats per row. Testing the genuine frame matters: a
// short synthetic burst misses exactly the real-transfer failures (DMA
// descriptor boundaries, sustained-rate stalls). Same result shape as the
// RMT test; got[] holds the first mismatching row. No-op off the S3.
// `clockMultiplier` > 1 = a 74HCT595 expander is fitted: the private bus is built at the
// shift-mode pclk, and the GPIO CONTINUITY pre-check is SKIPPED. That pre-check drives the TX pin
// and expects the RX pin to follow directly — true for a bare jumper, false through a shift
// register (driving the serial input high does not raise an output; that takes 8 clocks + a latch),
// so it would report "jumper not detected" on perfectly good wiring. The captured signal is the
// real post-'595 WS2812 waveform, so the bit-verify itself is unchanged.
RmtLoopbackResult i80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                    uint16_t wrGpio, uint16_t dcGpio, uint16_t rxGpio,
                                    const uint8_t* frame, size_t frameBytes,
                                    size_t dataBytes, uint8_t rowBits,
                                    uint8_t clockMultiplier = 1);

// ---------------------------------------------------------------------------
// MoonI80 — the same i80 output, on OUR OWN DMA driver instead of IDF's esp_lcd.
//
// **Why a second implementation exists.** esp_lcd re-arms the peripheral on every
// transaction — `lcd_start_transaction()` does `lcd_ll_reset()` + `lcd_ll_fifo_reset()` +
// a hard-coded 4 µs busy-wait before each one. An LCD panel does not care; WS2812 is one
// unbroken self-clocked bit stream, so a mid-frame reset garbles everything after it.
// That makes a frame split across several esp_lcd transactions impossible to send gaplessly,
// at any chunk size — which in turn forces the whole frame into ONE transaction, and THAT is
// what caps the driver: the DMA must stream the entire frame from one contiguous, DMA-
// reachable block (hence ~96 lights/strand through the '595 expander on an S3, and no PSRAM
// at all on the classic ESP32).
//
// The hardware never demanded this. The LCD peripheral has no data-length register —
// `lcd_ll_set_phase_cycles()` only sets `lcd_dout` as a boolean enable, and IDF's own comment
// reads "Number of data phase cycles are controlled by DMA buffer length". So the peripheral
// clocks out exactly what the DMA feeds it and stops when the chain ends: ONE gdma_start() over
// an arbitrarily long descriptor chain + ONE lcd_ll_start() is a single gapless stream across as
// many buffers as we like. This backend takes that, built on IDF's HAL + GDMA link-list APIs
// (one level below esp_lcd — not raw registers; IDF's own drivers use the same APIs).
//
// **Both implementations ship.** The esp_lcd one above is the REFERENCE: correct, capped, and
// what this is measured against. Selecting between them is a module swap in the UI (two
// registered driver types), so the A/B needs no reflash. See docs/adr/0014.
//
// Identical contract to the i80Ws2812* family above, function for function — the domain driver
// (src/light/drivers/MoonLedDriver.h) is the same CRTP sibling with its forwards re-pointed.
// Inert on chips without LCD_CAM.
// ---------------------------------------------------------------------------

struct MoonI80Ws2812Handle { void* impl = nullptr; };

// **The streaming ring — how MoonI80 drives a frame too big to hold.**
//
// The whole-frame path above needs the entire encoded frame in one DMA-reachable block, and that is
// what caps the 74HCT595 expander: the encoder emits ~1,152 bytes per light in shift mode, so 96
// lights per strand is already 108 KB — the internal-DMA-RAM edge. Above that the frame can only live
// in PSRAM, and the S3's GDMA cannot sustain a PSRAM read at the expander's 10× pixel clock (measured:
// a PSRAM frame drives fine at 2.67 MHz and never completes at 26.67 MHz, at any size). Moving that
// read to the CPU does not help — same memory, same bus, and the CPU is not faster at bulk reads.
//
// So the frame is never materialised at all. The DMA loops a small ring of INTERNAL buffers, and as
// each one drains, the CPU encodes the next slice straight into it — reading the Layer buffer, which
// is internal and ~24× smaller than the encoded output (3 bytes/light vs 1,152). PSRAM leaves the path
// entirely. Espressif's RGB-LCD driver calls the same trick "bounce buffers"; hpwit's LED driver
// arrived at it independently.
//
// The deadline is comfortable, and the expander is *why*: the DMA takes ~345 µs to drain one 16-row
// buffer while the CPU encodes those rows in ~96 µs (measured on an S3) — the 8× output inflation buys
// far more DMA time than it costs CPU, a ~3.6× margin.
//
// **The refill runs INLINE IN THE GDMA EOF ISR** — IDF's own continuous-gapless pattern (the RGB-LCD
// bounce buffers, esp_lcd_panel_rgb.c: the refill runs synchronously in the EOF handler). As each buffer
// drains, the ISR encodes the next slice into it at interrupt priority, so the refill always finishes
// before the DMA laps back into that buffer `kRingBufs` slices later. That is the reuse-race guarantee: a
// lower-priority task (the original design) could lose the buffer-reuse race to task-wake latency at >8
// slices (≥192 lights/strand), stalling the frame; an ISR cannot. The domain encode it calls lives in
// FLASH, which is safe here because the channel does NOT set `isr_cache_safe` — the interrupt is not
// `ESP_INTR_FLAG_IRAM`, so a flash-resident callback is permitted and only faults if the ISR fires while
// the flash cache is disabled (a SPI-flash write: OTA/NVS), which never overlaps rendering. This is
// exactly how esp_lcd_panel_rgb.c behaves by default (its ISR-refill is forced into IRAM only under the
// opt-in CONFIG_LCD_RGB_ISR_IRAM_SAFE). Full-flash-write hardening (isr_cache_safe + an IRAM encode via a
// neutral MM_HOT macro) is a later increment, not needed for the reuse-race fix.
//
// `MoonI80EncodeFn` is the seam: the platform owns the ring, the descriptors and the completion; the
// domain owns the encode. The callback runs from the EOF ISR (and once from the priming call).
//
// `needsPrefill` is the platform's buffer-lifecycle fact the encode's biggest saving hangs on: a ring
// buffer's CONSTANT words (the shift waveform frame prefillShiftRows lays) survive recycling — a data-only
// refill of a recycled buffer is byte-identical to a full one — so the encoder may skip the prefill except
// when the platform says the buffer's constants are gone: its FIRST use since the pool was built, or after
// any platform-side memset (the short-last-slice tail zero, the past-frame zero-fill). Only the platform
// knows those events, so it computes the flag; the domain decides what "prefill" means (and may still
// prefill unconditionally when its lane masks vary per row — ragged strands). Measured: the per-refill
// prefill was ~1/3 of the ISR encode cost.
using MoonI80EncodeFn = void (*)(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                 bool closeFrame, bool needsPrefill);

bool moonI80Ws2812Init(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                       uint16_t wrGpio, size_t bufferBytes,
                       bool wantSecondBuffer, uint8_t clockMultiplier = 1);

// Bring the bus up in RING mode instead of whole-frame mode. `rowBytes` is what one row (one light
// across every strand) encodes to, `totalRows` the strand length; the platform sizes the ring from
// them. `encode` is called per drained buffer, from the pinned refill task the EOF ISR wakes, to fill
// the next slice. Returns false if the ring cannot be built (then the caller falls back to whole-frame).
// The ring geometry the driver ships with, and the values its self-test uses. Named here (not duplicated
// per caller) so the driver's control defaults and the platform's own use cannot drift apart.
constexpr uint8_t kRingRowsDefault = 16;   // lights per DMA buffer
constexpr uint8_t kRingBufsDefault = 12;   // buffers the DMA circulates

// `rowsPerBuf` (lights per DMA buffer) and `ringBufs` (pool depth) are the ring's GEOMETRY, and they are
// the caller's choice because the optimum is a measurement, not a derivation. RAM is the only axis that
// wants a small rowsPerBuf — it alone stops scaling with strand length at 1 (the only way a 48x256 frame
// is reachable at all); per-call encode overhead, interrupt rate and lap-time runway all want it big.
// Returns false (caller falls back to whole-frame) if the pool won't fit, if rowsPerBuf is 0, or if
// ringBufs is outside the platform's supported depth.
bool moonI80Ws2812InitRing(MoonI80Ws2812Handle& h, const uint16_t* dataPins, uint8_t laneCount,
                           uint16_t wrGpio, size_t rowBytes, uint32_t totalRows,
                           uint32_t rowsPerBuf, uint8_t ringBufs,
                           uint8_t clockMultiplier, MoonI80EncodeFn encode, void* user);

// Start one frame on the ring: prime the buffers, fire the DMA, and let the refill task (woken by the
// EOF ISR) refill behind it. Pair with moonI80Ws2812Wait(h, 0, …) — the ring reports completion on slot 0.
bool moonI80Ws2812TransmitRing(MoonI80Ws2812Handle& h);

// True when the handle was brought up as a ring (so the driver knows which transmit to call).
bool moonI80Ws2812IsRing(const MoonI80Ws2812Handle& h);

// Would a whole `bytes`-sized frame fit internal DMA RAM right now (leaving the WiFi/HTTP reserve)? The
// driver asks this to decide RING vs whole-frame: a shift frame that fits internal takes the proven
// whole-frame path; one that doesn't would fall to PSRAM and stall at the expander clock, so it rings
// instead. Reuses the exact heap-caps check moonI80Ws2812Init does. False on chips without LCD_CAM.
bool moonI80Ws2812InternalFits(size_t bytes);
uint8_t* moonI80Ws2812Buffer(const MoonI80Ws2812Handle& h, uint8_t buffer);
size_t moonI80Ws2812BufferCapacity(const MoonI80Ws2812Handle& h);
bool moonI80Ws2812Transmit(MoonI80Ws2812Handle& h, uint8_t buffer, size_t bytes);
bool moonI80Ws2812Wait(MoonI80Ws2812Handle& h, uint8_t buffer, uint32_t timeoutMs);
uint32_t moonI80Ws2812LastTransmitUs(const MoonI80Ws2812Handle& h);

// Ring diagnostic counters, surfaced so the driver can expose them as a read-only control (reliable
// polling via /api/state instead of scraping serial). All zero on a whole-frame handle. `nSlices` is the
// frame's slice count (light-count / rowsPerBuf); `eofTotal`/`doneGiven` are lifetime counts the EOF ISR
// bumps; `lastDrain` is the drainCount the last EOF saw (should reach nSlices each frame); `numItems`/
// `consumedItems` are the descriptor pool capacity vs what the mount loop used (a mismatch is the ≥256
// chain-sizing bug). Read-only, best-effort (volatile reads, no lock) — a diagnostic, not a contract.
struct MoonI80RingStats {
    bool     isRing = false;
    uint32_t nSlices = 0;
    uint32_t ringBufs = 0;       // kRingBufs (buffer-pool size; reuse happens when nSlices > this)
    uint32_t eofTotal = 0;       // lifetime EOF interrupts
    uint32_t doneGiven = 0;      // lifetime frame completions (last-slice EOF)
    uint32_t lastDrain = 0;      // drainCount at the last EOF
    uint32_t numItems = 0;       // descriptor pool capacity
    uint32_t consumedItems = 0;  // items the mount loop actually used (== numItems when sized right)
    uint32_t descErr = 0;        // GDMA descriptor-error count (>0 == the in-ISR encode corrupted the chain: B1)
    uint32_t maxEncodeUs = 0;    // worst ISR refill-encode time (the producer)
    uint32_t maxIsrGapUs = 0;    // worst gap between EOFs = DMA buffer-drain time (the deadline)
    // Ring-diagnosis fields — the instruments that isolated the three prime-only bugs (mount re-link,
    // multi-node buffers, EOF coalescing); the lapping work reads them the same way. Their scope lives in
    // backlog-light § MoonI80 streaming ring.
    uint32_t itemsPerBuf = 0;    // descriptor nodes per ring buffer (1 by construction since the clamp)
    int32_t  termNodeDiag = -1;  // the mount-time NULL terminator node (-1 = looping/lapping chain)
};
MoonI80RingStats moonI80Ws2812RingStats(const MoonI80Ws2812Handle& h);

void moonI80Ws2812Deinit(MoonI80Ws2812Handle& h);
// `useRing` makes the self-test ride the ring exactly when the render path does, so it verifies the SAME
// transport the driver is tuned to (not "only when the frame won't fit internal") — the instrument the
// ring's margin bug needs. `ringRows`/`ringBufs` are that ring's geometry (0 → the platform default). The
// bit-verify then measures the ACTUAL ring: a margin the eyes see scattered on the wall shows here as a
// bit fault at the same slice boundary — the machine reproduction of the wall (the margin rule,
// `ring-reuse-is-the-blocker`). `useRing=false` keeps the legacy auto-gate (ring iff the frame overflows
// internal RAM), which is what direct-mode continuity callers want.
RmtLoopbackResult moonI80Ws2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                        uint16_t wrGpio, uint16_t rxGpio,
                                        const uint8_t* frame, size_t frameBytes,
                                        size_t dataBytes, uint8_t rowBits,
                                        uint8_t clockMultiplier = 1,
                                        uint32_t ringRows = 0, uint32_t ringBufs = 0,
                                        bool useRing = false);

// INTRUSIVE loopback — DRIVER-AGNOSTIC, so it lives here (not per-family): bit-verify what the LIVE
// pipeline is ALREADY clocking on `rxGpio`, building no bus and leaving the running peripheral untouched
// (unlike the per-driver `*Loopback`, which tears the output down and rebuilds a private copy — a large
// contiguous alloc that fragments the heap and tests a replica). Because it only arms the RMT-RX (the
// render loop is the transmitter), it needs nothing driver-specific — every driver family (i80, esp_lcd,
// Parlio, RMT) shares this one entry, the same way they share `detail::captureAndVerifyFrame`. The caller
// pins a known per-light pattern (`sent`, `sentLen` channels) into the driver's source so the tapped
// strand's expected wire is deterministic. `dataBytes` = the tapped strand's WS2812 byte count (lights ×
// channels × 24 → kBits = dataBytes/3); `slotHz` = the STRAND's slot rate (bus rate ÷ expander multiplier,
// or the direct pixel-clock). A scattered ring shows as a bit fault at a slice boundary; a clean one PASSes.
RmtLoopbackResult ws2812LoopbackRide(uint16_t rxGpio, const uint8_t* sent, uint8_t sentLen,
                                     size_t dataBytes, uint8_t rowBits, uint8_t clockMultiplier);

// ---------------------------------------------------------------------------
// Parlio (Parallel IO) WS2812 output — the ESP32-P4's parallel LED path, a
// sibling of the LCD_CAM i80 functions above. Same autonomous-whole-frame DMA
// shape, but Parlio is simpler: it takes the data GPIOs directly (no
// sacrificial WR/DC lines — Parlio generates the pixel clock itself from
// `pclkHz`) and allows ANY lane count (1..8 here), so there is no all-8-pins
// rule. The same encoder feeds it (ParallelSlots.h — one bus word per slot, bit L =
// data line L). All inert on targets without Parlio, guarded by
// `if constexpr (platform::parlioLanes == 0)` in the driver.
// ---------------------------------------------------------------------------

// Opaque handle to one configured Parlio TX unit + one or TWO DMA frame buffers.
struct ParlioWs2812Handle { void* impl = nullptr; };

// Create a Parlio TX unit on `dataPins[0..laneCount)` clocked at `pclkHz` (the
// WS2812 slot rate), with a zeroed DMA-capable buffer 0 of `bufferBytes`. When
// `wantSecondBuffer` is true, also TRY a second buffer for the async double-buffer
// (same allocate-and-degrade contract as i80Ws2812Init); when false (default) no
// second buffer is allocated. No WR/DC pins — Parlio drives the clock internally.
// Returns false when buffer 0 (or the unit) fails.
bool parlioWs2812Init(ParlioWs2812Handle& h, const uint16_t* dataPins,
                      uint8_t laneCount, uint32_t pclkHz, size_t bufferBytes,
                      bool wantSecondBuffer);

// DMA frame buffer `buffer` (0 or 1; buffer 1 is null when it didn't fit) + the
// shared per-buffer capacity. See i80Ws2812Buffer for the single-buffer-degrade contract.
uint8_t* parlioWs2812Buffer(const ParlioWs2812Handle& h, uint8_t buffer);
size_t parlioWs2812BufferCapacity(const ParlioWs2812Handle& h);

// Start the autonomous DMA transfer of buffer `buffer`'s first `bytes`; pair
// with parlioWs2812Wait on the SAME buffer. No refill deadline once started
// (single-shot, not the loop-transmission mode Parlio also offers).
bool parlioWs2812Transmit(ParlioWs2812Handle& h, uint8_t buffer, size_t bytes);

// Block until buffer `buffer`'s in-flight transfer finishes, bounded by `timeoutMs`.
// Returns TRUE only when the transfer actually completed; FALSE on timeout — see i80Ws2812Wait for
// why the caller must not reuse the buffer then.
bool parlioWs2812Wait(ParlioWs2812Handle& h, uint8_t buffer, uint32_t timeoutMs);

// Duration in microseconds of the most recent completed DMA transfer — the pure wire/DMA output
// time (the WS2812 floor / fps ceiling). See i80Ws2812LastTransmitUs. 0 until the first completes.
uint32_t parlioWs2812LastTransmitUs(const ParlioWs2812Handle& h);

void parlioWs2812Deinit(ParlioWs2812Handle& h);

// Parlio loopback self-test — same contract + result shape as the LCD/RMT
// loopbacks: a private Parlio TX unit transmits the caller's real frame back to
// back while rmtWs2812RxCapture reads it off `rxGpio` (lane 0 carries the
// pattern) and every bit is verified. `dataBytes`/`rowBits` as in i80Ws2812Loopback.
RmtLoopbackResult parlioWs2812Loopback(const uint16_t* dataPins, uint8_t laneCount,
                                       uint16_t rxGpio, const uint8_t* frame,
                                       size_t frameBytes, size_t dataBytes,
                                       uint8_t rowBits);

// ---------------------------------------------------------------------------
// I2S audio input (digital MEMS microphone, e.g. INMP441). Two seams only:
// the I2S read (audioMic*) and the FFT kernel (audioFft). Everything else —
// DC strip, RMS, windowing, the magnitude->16-band log mapping, noise-floor/gain —
// is host-tested domain code (src/core/AudioLevel.h, AudioBands.h), so the level
// and band math runs in CI without hardware. On desktop audioMicRead returns 0
// (no capture) but audioFft is a real (naive) DFT, so the whole
// read->window->FFT->bands path is still exercised host-side.
// All inert on targets without I2S, guarded by `if constexpr (platform::hasI2sMic)`.
// ---------------------------------------------------------------------------

// `CodecType` + `AudioCodecPins` are defined in platform_config.h (included above),
// alongside the per-target `audioCodecType`/`audioCodecPins` defaults that use them.
//
// Configure the audio codec (record/mic path) over I2C, if the board has one.
// Some boards put an analog mic behind an I2S audio codec (configured over I2C)
// rather than a direct digital I2S MEMS mic; the codec must be brought up before
// the I2S read. This is a no-op returning true on a board with a direct mic
// (CodecType::None) or a target without a codec, so AudioService always calls it and
// the path stays uniform.
// Returns true when there's nothing to do (CodecType::None / no codec on this
// target) or the codec came up; false on an I2C/codec error (the module then
// idles with a status error, same as a failed mic init). AudioService calls this
// *after* audioMicInit: the I2S channel must already be driving MCLK before the
// codec is configured (the ES8311 won't answer I2C without MCLK running). The
// codec then presents standard I2S the read picks up.
bool audioCodecInit(CodecType type, const AudioCodecPins& pins, uint32_t sampleRate);

void audioCodecDeinit();

// Opaque handle to one configured I2S RX channel (standard/Philips mode).
struct AudioMicHandle { void* impl = nullptr; };

// Bring up an I2S RX channel reading the mic on the given pins at `sampleRate`
// (24-bit data in a 32-bit slot, mono). `mclkPin` drives the I2S master clock —
// −1 for a self-clocked direct MEMS mic (INMP441), or the codec's MCLK pin when a
// codec needs the clock to run (the ES8311 won't even answer I2C without it, so
// AudioService starts I2S *before* audioCodecInit on a codec board). Returns false
// on failure (bad pins, no I2S, out of memory) — the module idles with a status error.
bool audioMicInit(AudioMicHandle& h, uint16_t wsPin, uint16_t sdPin,
                  uint16_t sckPin, int16_t mclkPin, uint32_t sampleRate);

// Read up to `maxSamples` 32-bit samples into `out`; returns the count read
// (0 if none ready / not initialised). Non-blocking enough for the render tick.
size_t audioMicRead(AudioMicHandle& h, int32_t* out, size_t maxSamples);

void audioMicDeinit(AudioMicHandle& h);

// Real-input FFT kernel: `windowed` holds `n` (a power of two) windowed samples;
// fills `outMag` with the n/2 magnitude bins. esp-dsp's float `dsps_fft2r_fc32`
// on ESP32 (the FPU makes float faster than fixed-point); a naive O(n^2) DFT on
// desktop — correct, only fast enough for the host tests' small n.
void audioFft(const float* windowed, size_t n, float* outMag);

// ---------------------------------------------------------------------------
// I2C bus diagnostics — domain-neutral, not audio-specific. Probes a bus and
// reports which 7-bit addresses ACK, the standard `i2cdetect` operation. Used
// by the I2cScanModule diagnostic (src/core/I2cScanModule.h) to help bring up
// any I2C peripheral (a codec, a sensor, an expander) — confirm wiring and read
// off a device's address. Self-contained: opens a temporary master bus on the
// given pins, scans, tears it down. The bus is transient, so it only conflicts
// with a driver that *currently* holds the port (e.g. the ES8311 codec keeps
// I2C_NUM_0 open while AudioService is active) — that case is reported as
// kI2cBusUnavailable, not silently as "0 devices". Internal pull-ups enabled.
// ---------------------------------------------------------------------------

// Sentinel: the bus couldn't be opened (already held by another driver, or no
// I2C on this target) — distinct from a successful scan that found 0 devices.
inline constexpr size_t kI2cBusUnavailable = static_cast<size_t>(-1);

// Scan the I2C bus on (sda, scl); write the 7-bit addresses that ACK into
// `out` (caller-sized, capacity `maxOut`) and return the count found (capped at
// maxOut), or kI2cBusUnavailable if the bus couldn't be opened.
size_t i2cScan(uint16_t sda, uint16_t scl, uint8_t* out, size_t maxOut);

// Poll the IR receiver on `pin` for a decoded remote frame. Returns true and writes the
// frame into `codeOut` when a fresh code is available since the last call, false otherwise
// (nothing received, or IR decode unavailable on this target). Self-contained like i2cScan —
// it owns whatever peripheral it needs (an RMT RX channel on ESP32). Non-blocking: safe to
// call every tick. IrService is the sole caller. ESP32 decodes NEC over RMT; desktop has no IR
// hardware and always returns false.
bool irRead(uint16_t pin, uint32_t& codeOut);

// Release the IR RX channel so the pin it held is free for another module. irRead lazily reopens
// it on the next call, so this is safe to call any time; IrService calls it on disable (the pin
// then shows as freed in the pin map, and is genuinely reusable). No-op if no channel is open, and
// on desktop (no IR hardware).
void irStop();

// Open (or confirm) the IR RX channel on `pin` and report whether it's live — the difference between
// "a pin is configured" (which irRead can't distinguish from "no code this tick") and "the RMT-RX
// channel actually bound and is armed". IrService calls this to give a truthful status: a busy pin or
// a bad GPIO fails to open, and the user must see that, not a stale "ready". Idempotent for an
// unchanged pin (reuses the open channel). Returns true on ESP32 when the channel is live; desktop
// has no IR hardware, so it returns true (no channel to fail — the desktop status stays "ready").
bool irChannelReady(uint16_t pin);

} // namespace mm::platform
