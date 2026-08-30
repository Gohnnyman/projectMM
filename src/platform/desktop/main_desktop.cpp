#include <csignal>
#include <cstdint>   // mm_main() takes uint16_t; <unistd.h> used to pull this in on POSIX
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>

#ifdef _WIN32
#include <io.h>      // _write
#else
#include <unistd.h>
#endif

extern void mm_main(volatile bool& keepRunning, uint16_t httpPort);

static volatile bool running = true;
static bool cleanExit = false;

// Write s to stderr without stdio — safe inside a signal handler.
static void safeWrite(const char* s) {
    size_t len = std::strlen(s);
    while (len > 0) {
#ifdef _WIN32
        int n = ::_write(2 /* stderr fd */, s, static_cast<unsigned int>(len));
#else
        ssize_t n = ::write(STDERR_FILENO, s, len);
#endif
        if (n <= 0) break;
        s += n; len -= static_cast<size_t>(n);
    }
}

static void crashHandler(int sig) {
    const char* name = sig == SIGSEGV ? "SIGSEGV"
                     : sig == SIGABRT ? "SIGABRT"
                     : sig == SIGFPE  ? "SIGFPE"
#ifdef SIGBUS
                     : sig == SIGBUS  ? "SIGBUS"
#endif
                                      : "SIGNAL";
    safeWrite("\n*** CRASH: ");
    safeWrite(name);
    safeWrite(" ***\n");
    // SA_RESETHAND (POSIX) or signal() one-shot semantics (Windows) already
    // restored SIG_DFL; re-raise for OS coredump.
    raise(sig);
}

// Local time as an ISO-8601 stamp, thread-safely. `std::localtime` returns a pointer to a
// SHARED static tm, so two threads formatting a timestamp can each see the other's value —
// flagged independently by clang-tidy (concurrency-mt-unsafe) and CodeQL (critical). The
// reentrant form is spelled differently per platform, hence the branch.
static void isoTimestamp(char* out, size_t n) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);        // MSVC: arguments reversed relative to POSIX
#else
    localtime_r(&t, &tm);
#endif
    std::strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
}

// Fires on std::exit() — distinguishes reboot (platform::reboot prints its own
// line first) from a genuine unexpected exit with no preceding crash signal.
static void atExitHandler() {
    if (!cleanExit) {
        char tbuf[32];
        isoTimestamp(tbuf, sizeof(tbuf));
        std::fprintf(stderr, "*** process exited without clean shutdown at %s ***\n", tbuf);
        std::fflush(stderr);
    }
}

// Show the UI the moment the server is up. A desktop user launching from Finder or a file manager
// has no terminal to read a URL out of, and typing localhost:8080 is exactly the step that loses
// someone on their first run. Best effort: a failure is silent, because the banner above already
// printed the address and a missing browser must not stop the server.
//
// --no-browser opts out, for a headless box or a service manager where opening a browser on the
// server's own display is wrong.
static void openLocalUi(uint16_t port) {
    char url[64];
    std::snprintf(url, sizeof(url), "http://localhost:%u/", static_cast<unsigned>(port));
    char cmd[128];
#if defined(_WIN32)
    std::snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", url);
#elif defined(__APPLE__)
    std::snprintf(cmd, sizeof(cmd), "open \"%s\" >/dev/null 2>&1 &", url);
#else
    std::snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", url);
#endif
    // Degrade visibly: a box with no xdg-open (the headless case --no-browser exists for) would
    // otherwise do nothing at all, leaving the user waiting for a window that is never coming.
    // The URL is already on stdout above, so this only has to say the auto-open failed.
    if (std::system(cmd) != 0) {
        std::printf("  (could not open a browser automatically, open %s yourself)\n", url);
    }
}

int main(int argc, char** argv) {
    // --port N: the HTTP port. Defaults to 8080 because ports below 1024 need root on POSIX, but
    // Home Assistant's WLED integration hardcodes port 80 with no way to specify another, so testing
    // that path on desktop needs `sudo projectMM --port 80`.
    uint16_t httpPort = 8080;
    bool openBrowser = true;
    for (int i = 1; i < argc; i++) {
        const bool isPort = std::strcmp(argv[i], "--port") == 0;
        if (isPort && i + 1 < argc) {
            // endptr check: "80abc" must be an error, not port 80 — silently accepting a typo'd
            // value binds the wrong port and the failure surfaces much later as "HA can't connect".
            char* end = nullptr;
            const long v = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || v <= 0 || v > 65535) {
                std::printf("--port must be a number 1..65535, got \"%s\"\n", argv[i]);
                return 1;
            }
            httpPort = static_cast<uint16_t>(v);
        } else if (isPort) {
            std::printf("--port needs a value\n"); return 1;
        } else if (std::strcmp(argv[i], "--no-browser") == 0) {
            openBrowser = false;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: projectMM [--port N] [--no-browser]\n"
                        "  --port N       HTTP port (default 8080; 80 needs root)\n"
                        "  --no-browser   do not open the UI on start\n");
            return 0;
        } else {
            // An unknown argument is a user error, not noise to ignore: "--prot 80" silently
            // running on 8080 is the same late-surfacing failure as the typo'd value.
            std::printf("unknown argument \"%s\" — try --help\n", argv[i]);
            return 1;
        }
    }
    // Unbuffer so every line lands in projectMM.log before a crash.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

#ifdef _WIN32
    // Winsock is initialized by a static RAII guard in platform_desktop.cpp,
    // so it covers test binaries too (they link mm_platform but have their own
    // main()). Nothing to do here.

    // Windows has no sigaction — use signal() with one-shot semantics (the
    // handler is restored to SIG_DFL on entry, so re-raise produces the OS
    // crash dialog). No SIGPIPE on Windows; broken socket writes return an
    // error from send(), which the code already checks.
    signal(SIGSEGV, crashHandler);
    signal(SIGFPE,  crashHandler);
    signal(SIGABRT, crashHandler);
    // signal() is one-shot on Windows, so the handler is already reset after the first Ctrl+C and
    // a second one force-quits: the same escalation SA_RESETHAND gives the POSIX branch below.
    signal(SIGINT,  [](int) { running = false; });
#else
    struct sigaction sa{};
    sa.sa_handler = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);

    // SA_RESETHAND: the FIRST Ctrl+C asks for a clean stop, and restores the default handler so a
    // SECOND one kills the process outright. Without it a shutdown that wedges (a driver teardown
    // waiting on a socket, say) leaves Ctrl+C doing nothing at all, however many times it is
    // pressed, and the only way out is another terminal. Reported from a Linux bench: "it didn't
    // shut down fully and was slowly eating more cpu cycles ... it got SIGTERM'd".
    struct sigaction saInt{};
    saInt.sa_handler = [](int) {
        running = false;
        // Say so, because the first press otherwise looks ignored while teardown runs. safeWrite,
        // not printf: only async-signal-safe calls are legal in a handler.
        safeWrite("\nStopping. Press Ctrl+C again to force quit.\n");
    };
    sigemptyset(&saInt.sa_mask);
    saInt.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &saInt, nullptr);

    // Ignore SIGPIPE — write() on a closed TCP connection delivers it by default,
    // which kills the process silently (no crash report) when a browser tab closes
    // mid-response. We handle broken writes via return-value checks instead.
    signal(SIGPIPE, SIG_IGN);
#endif

    std::atexit(atExitHandler);

    char tbuf[32];
    isoTimestamp(tbuf, sizeof(tbuf));
    // The banner, then the URL, then how to stop. This window IS the app on a desktop: there is no
    // tray icon and no GUI, so it has to say what it is and how to end it. The tick lines that
    // follow print for the first 60 s and then go quiet unless the log level asks for them, so this
    // stays readable once the machine settles.
    std::printf("projectMM started at %s\n", tbuf);
    std::printf("\n  projectMM is running: http://localhost:%u/\n", static_cast<unsigned>(httpPort));
    std::printf("  Close this window (or press Ctrl-C) to stop.\n\n");

    if (openBrowser) openLocalUi(httpPort);

    mm_main(running, httpPort);

    cleanExit = true;
    isoTimestamp(tbuf, sizeof(tbuf));
    std::printf("projectMM exited cleanly at %s\n", tbuf);
    return 0;
}
