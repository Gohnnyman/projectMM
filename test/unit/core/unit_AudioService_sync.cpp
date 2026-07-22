// @module AudioService
// @also WledAudioSyncPacket

// Drives AudioService's WLED audio-sync socket lifecycle on the host through the public
// tick() — the same entry the scheduler calls on-device. Covers: lazy open once per mode
// (syncEnsureSocket latches), the send path reaching "sending", send throttling, and the
// receive path over a real localhost UDP round-trip (frame replacement + the fresh→stale
// listening fallback of the pure sink). platform::networkReady() is true on desktop, so the lazy open fires
// on the first tick — mirroring a device once its interface is up.
//
// Time is driven deterministically with platform::setTestNowMs() (the animation-test idiom)
// so the throttle/fallback windows are exact and the suite never sleeps a real second; only
// the actual UDP delivery is real, polled with a bounded retry (the NetworkReceiveEffect
// localhost-round-trip pattern). A test port (not 11988) avoids colliding with a running
// projectMM desktop app that would hold the real sync port.

#include "doctest.h"
#include "core/AudioService.h"
#include "light/WLEDAudioSyncPacket.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>

using namespace mm;

namespace {
constexpr uint16_t kTestSyncPort = 21988;   // a free high port, not the real 11988

// The status read-out is published by tick1s(); call it after a tick() so the assertions
// below see the current state string rather than the setup() baseline.
const char* status(AudioService& a) { a.tick1s(); return a.syncStatusForTest(); }

// A guard that freezes virtual time for a case and restores the real clock on scope exit,
// so a thrown assertion can't leak frozen time into the next case.
struct FrozenClock {
    FrozenClock(uint32_t ms) { platform::setTestNowMs(ms); }
    ~FrozenClock() { platform::setTestNowMs(0); }
    void advance(uint32_t ms) { now_ += ms; platform::setTestNowMs(now_); }
    uint32_t now_ = 1;
};
}  // namespace

TEST_CASE("AudioService Local+send: lazy-opens once and reports sending") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0;
    a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();                  // build: syncReinit() — socket NOT opened here (boot-safe)
    CHECK(std::strstr(status(a), "waiting") != nullptr);   // no tick() yet → still waiting

    a.tick();                        // networkReady() true on desktop → opens now
    CHECK(std::strcmp(status(a), "sending") == 0);
    CHECK(a.syncOpenForTest());

    // Idempotent: a second tick doesn't re-open (the latch holds).
    a.tick();
    CHECK(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "sending") == 0);

    a.release();
    CHECK_FALSE(a.syncOpenForTest());
}

TEST_CASE("AudioService Local+send: broadcasts are throttled to ~kSyncSendIntervalMs") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0;
    a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // opens + first send (frameCounter bumps once)
    REQUIRE(a.syncOpenForTest());

    // More ticks within the same interval must not each emit — the frame counter (bumped
    // only on an actual send) does not advance while the throttle window is open.
    const uint8_t c0 = a.syncFrameCounterForTest();
    a.tick();
    a.tick();
    CHECK(a.syncFrameCounterForTest() == c0);   // throttled: no send per tick

    // After the interval elapses, exactly one more send is allowed.
    clk.advance(AudioService::syncSendIntervalMsForTest() + 5);
    a.tick();
    CHECK((uint8_t)(a.syncFrameCounterForTest() - c0) == 1);

    a.release();
}

TEST_CASE("AudioService Receive: a localhost WLED packet drives frame_, then auto-blends back") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 1;   // receive network
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // binds kTestSyncPort
    REQUIRE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "listening") == 0);

    // Send a real WLED v2 packet to the bound port over loopback.
    AudioFrame peer;
    peer.level = 222; peer.levelSmoothed = 111; peer.peakHz = 660; peer.peakMag = 55;
    for (int i = 0; i < 16; i++) peer.bands[i] = static_cast<uint8_t>(i * 8);
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, peer, /*frameCounter=*/1, /*peak=*/false);

    platform::UdpSocket tx;
    REQUIRE(tx.open());
    REQUIRE(tx.connect("127.0.0.1", kTestSyncPort));
    REQUIRE(tx.sendTo(pkt, WLED_SYNC_PACKET_SIZE));

    // Loopback delivery is async in real time — poll tick() until the peer frame lands
    // (bounded, ≤100 iterations). Virtual time stays frozen, so the frame counts as fresh.
    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        a.tick();
        landed = a.audioFrame()->level == 222 && a.audioFrame()->peakHz == 660;
        if (!landed) platform::delayMs(1);   // real wait for the datagram, not virtual time
    }
    CHECK(landed);
    CHECK(a.audioFrame()->levelSmoothed == 111);
    CHECK(std::strcmp(status(a), "receiving") == 0);   // fresh peer audio

    // Receive is a pure network sink: advance virtual time past the fallback window with no new
    // packet — the peer goes stale and the status falls back to "listening" (bound, no fresh peer).
    // The last frame is held; the local mic never runs in this mode. Deterministic: no real sleep.
    clk.advance(AudioService::syncFallbackMsForTest() + 20);
    a.tick();
    CHECK(std::strcmp(status(a), "listening") == 0);

    tx.close();
    a.release();
}

TEST_CASE("AudioService Receive: a failed bind backs off instead of retrying every tick") {
    FrozenClock clk(1);
    // Force the bind to fail deterministically. The obvious approach — hog the port with a second
    // socket — is NOT portable: on Linux, SO_REUSEADDR on a UDP socket bound to INADDR_ANY permits
    // the overlapping bind, so the hog succeeds and the failure never happens. (That silently broke
    // this test on Linux for as long as it existed; nothing caught it because CI did not compile the
    // C++ tests until the sanitizer job.) A privileged port is no better — modern macOS lets a
    // non-root process bind port 80.
    platform::setTestBindFails(true);

    AudioService a;
    a.mode = 1;   // receive network
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // first bring-up attempt → bind fails
    CHECK_FALSE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "receive: bind failed") == 0);

    // Within the backoff window, further ticks must NOT retry — the socket stays closed and
    // the status is unchanged (no per-tick socket() churn). tick1s() only reasserts the
    // baseline while syncOpen_ is false, so the string staying put is the observable proof.
    a.tick();
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());
    CHECK(std::strcmp(a.syncStatusForTest(), "receive: bind failed") == 0);

    // Let the bind succeed and advance past the backoff — the next tick retries and succeeds.
    platform::setTestBindFails(false);
    clk.advance(AudioService::syncOpenRetryMsForTest() + 5);
    a.tick();
    CHECK(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "listening") == 0);

    a.release();
}

TEST_CASE("AudioService Local (not sending): no socket, reports off") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = 0;   // local audio, not sending (sync == off)
    a.applyState();
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "off") == 0);
    a.release();
}
