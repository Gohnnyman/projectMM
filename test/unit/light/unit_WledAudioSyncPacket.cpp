// @module WledAudioSyncPacket

// Pins the WLED audio-sync wire format — the 44-byte v2 packet projectMM broadcasts on
// UDP 11988 and that WLED / MoonLight (D_WLEDAudio.h) receive. A wire format breaks
// silently, so build → parse is round-tripped AND a golden byte vector fixes the exact
// offsets: the packet is a fixed compatibility contract (netmindz/WLED-sync), not ours to
// drift. (Same rigor as the Improv frame golden vector.)

#include "doctest.h"
#include "light/WLEDAudioSyncPacket.h"

#include <cstdint>
#include <cstring>

using namespace mm;

// Build a frame with distinct, checkable values in every field.
static AudioFrame sampleFrame() {
    AudioFrame f;
    f.level = 200;
    f.levelSmoothed = 150;
    f.peakHz = 440;
    f.peakMag = 77;
    for (int i = 0; i < 16; i++) f.bands[i] = static_cast<uint8_t>(i * 16);  // 0,16,...,240
    return f;
}

TEST_CASE("build produces a 44-byte v2 packet with the exact WLED layout") {
    AudioFrame f = sampleFrame();
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    size_t n = buildWledAudioSync(pkt, f, /*frameCounter=*/7, /*peak=*/true);

    CHECK(n == 44);
    CHECK(WLED_SYNC_PACKET_SIZE == 44);
    CHECK(WLED_SYNC_PORT == 11988);

    // header "00002" (+ NUL) at offset 0
    CHECK(std::memcmp(pkt, "00002", 6) == 0);
    // gap1 (offset 6-7) is zero
    CHECK(pkt[6] == 0); CHECK(pkt[7] == 0);
    // sampleRaw = level (float LE) at offset 8
    CHECK(wledGetFloatLE(pkt + 8) == doctest::Approx(200.0f));
    // sampleSmth = levelSmoothed at offset 12
    CHECK(wledGetFloatLE(pkt + 12) == doctest::Approx(150.0f));
    // samplePeak / frameCounter at 16 / 17
    CHECK(pkt[16] == 1);
    CHECK(pkt[17] == 7);
    // fftResult[16] = bands at offset 18
    for (int i = 0; i < 16; i++) CHECK(pkt[18 + i] == static_cast<uint8_t>(i * 16));
    // gap2 (offset 34-35) is zero
    CHECK(pkt[34] == 0); CHECK(pkt[35] == 0);
    // FFT_Magnitude = peakMag at 36, FFT_MajorPeak = peakHz at 40
    CHECK(wledGetFloatLE(pkt + 36) == doctest::Approx(77.0f));
    CHECK(wledGetFloatLE(pkt + 40) == doctest::Approx(440.0f));
}

TEST_CASE("build -> parse round-trips every AudioFrame field") {
    AudioFrame f = sampleFrame();
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, 3, false);

    AudioFrame out;
    REQUIRE(parseWledAudioSync(pkt, WLED_SYNC_PACKET_SIZE, out));
    CHECK(out.level == f.level);
    CHECK(out.levelSmoothed == f.levelSmoothed);
    CHECK(out.peakHz == f.peakHz);
    CHECK(out.peakMag == f.peakMag);
    for (int i = 0; i < 16; i++) CHECK(out.bands[i] == f.bands[i]);
}

TEST_CASE("parse rejects wrong length, wrong header, v1, and null") {
    AudioFrame f = sampleFrame();
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, 0, false);
    AudioFrame out;

    // exact-length valid packet parses
    CHECK(parseWledAudioSync(pkt, 44, out));
    // one byte short / one byte long — rejected (WLED sends exactly 44)
    CHECK_FALSE(parseWledAudioSync(pkt, 43, out));
    CHECK_FALSE(parseWledAudioSync(pkt, 45, out));
    // wrong header ("00001" = the legacy v1 packet) — rejected, not crashed
    uint8_t v1[44]; std::memcpy(v1, pkt, 44); std::memcpy(v1, "00001", 6);
    CHECK_FALSE(parseWledAudioSync(v1, 44, out));
    // an 83-byte v1-sized buffer — rejected on length
    uint8_t big[83] = {}; std::memcpy(big, "00001", 6);
    CHECK_FALSE(parseWledAudioSync(big, sizeof(big), out));
    // null
    CHECK_FALSE(parseWledAudioSync(nullptr, 44, out));
}

TEST_CASE("golden vector — the exact bytes on the wire (the compatibility contract)") {
    // A fixed frame → the exact 44 bytes any WLED receiver must accept. If this changes,
    // interop with WLED/MoonLight breaks — the test is the alarm.
    AudioFrame f;
    f.level = 100; f.levelSmoothed = 50; f.peakHz = 1000; f.peakMag = 25;
    for (int i = 0; i < 16; i++) f.bands[i] = static_cast<uint8_t>(i);   // 0..15
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, /*frameCounter=*/1, /*peak=*/false);

    // Little-endian IEEE-754 for 100.0, 50.0, 25.0, 1000.0
    const uint8_t golden[44] = {
        '0','0','0','0','2','\0',   // 0-5  header
        0, 0,                       // 6-7  gap1
        0x00,0x00,0xC8,0x42,        // 8-11  sampleRaw   = 100.0f
        0x00,0x00,0x48,0x42,        // 12-15 sampleSmth  = 50.0f
        0,                          // 16    samplePeak
        1,                          // 17    frameCounter
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,  // 18-33 fftResult
        0, 0,                       // 34-35 gap2
        0x00,0x00,0xC8,0x41,        // 36-39 FFT_Magnitude = 25.0f
        0x00,0x00,0x7A,0x44,        // 40-43 FFT_MajorPeak = 1000.0f
    };
    CHECK(std::memcmp(pkt, golden, 44) == 0);
}
