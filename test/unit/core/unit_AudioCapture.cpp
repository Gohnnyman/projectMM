// @module AudioService

// The desktop OS-capture seam (platform_desktop_audio.cpp, miniaudio-backed): what shared code
// may rely on regardless of host. Capture HARDWARE is not assumed: on a locked-down or headless
// host, init failing cleanly is correct behavior (miniaudio's null backend makes success the CI
// norm, delivering silence), so the lifecycle cases assert no-crash coherence around whichever
// outcome the host gives. First local run on macOS triggers the OS microphone permission prompt
// for the invoking terminal, expected, once.

#include "doctest.h"
#include "core/AudioService.h"
#include "platform/platform.h"

#include <cstring>

#if defined(MM_PLATFORM_DESKTOP) || !defined(ESP_PLATFORM)

// Device enumeration always offers at least "default", at entry 0, from stable platform storage.
TEST_CASE("audio capture enumeration offers 'default' first, from platform-owned storage") {
    const char* const* options = nullptr;
    const size_t n = mm::platform::audioCaptureDevices(&options);
    REQUIRE(n >= 1);
    REQUIRE(options != nullptr);
    CHECK(std::strcmp(options[0], "default") == 0);
    // A second enumeration returns the same stable pointer array (the Select borrows it).
    const char* const* again = nullptr;
    mm::platform::audioCaptureDevices(&again);
    CHECK(again == options);
}

// The full lifecycle neither crashes nor wedges, whatever the host's audio situation: a
// successful init delivers a readable (possibly silent) stream; a failed init degrades to
// false with reads returning nothing.
TEST_CASE("audio capture lifecycle is clean on any host: init, read, deinit, repeat") {
    mm::platform::AudioMicHandle h;
    const bool up = mm::platform::audioCaptureInit(h, 0, 22050);
    int32_t buf[256];
    const size_t got = mm::platform::audioMicRead(h, buf, 256);
    if (!up) CHECK(got == 0);          // failed init must not fabricate samples
    mm::platform::audioMicDeinit(h);
    CHECK(h.impl == nullptr);
    mm::platform::audioMicDeinit(h);   // double deinit is safe
    // The cycle repeats cleanly (the reinit path AudioService uses on every device change).
    const bool up2 = mm::platform::audioCaptureInit(h, 0, 22050);
    CHECK(up2 == up);                  // same host, same answer
    mm::platform::audioMicDeinit(h);
}

// An index past the device list fails loudly rather than opening some other device (the
// stale-persisted-index case: OS device order changed since the pick).
TEST_CASE("audio capture refuses an out-of-range device index") {
    mm::platform::AudioMicHandle h;
    CHECK_FALSE(mm::platform::audioCaptureInit(h, 200, 22050));
    mm::platform::audioMicDeinit(h);
}

// A capture device delivering silence must not raise the I2S wire diagnosis: on desktop
// there are no pins to check, and silence is a quiet room or an idle loopback (the flagship
// BlackHole case between songs). The verdict is compile-time-gated to hasI2sMic.
TEST_CASE("a silent capture device raises no I2S wire status") {
    static_assert(!mm::platform::hasI2sMic,
                  "this desktop-only case pins the desktop side of the gate");
    mm::AudioService a;
    a.mode = 0;   // Local
    a.applyState();
    for (int i = 0; i < 3; i++) { a.tick(); a.tick1s(); }   // three diagnosis windows
    const char* st = a.status();
    if (st != nullptr) {
        CHECK(std::strstr(st, "sdPin") == nullptr);
        CHECK(std::strstr(st, "sckPin") == nullptr);
    }
    a.release();
}

#endif
