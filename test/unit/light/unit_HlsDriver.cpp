// @module HlsDriver
// @also Drivers, Correction

// The HLS driver's encode hand-off, pinned without an ffmpeg. CI never has one installed, so the
// desktop platform RECORDS the spawn argv and the frames the driver piped (platform.h § HLS test
// seam), exactly as the NDI seam records frames. These tests state what ffmpeg would receive; the
// bench then only has to confirm a player shows it.

#include "doctest.h"
#include "light/drivers/HlsDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include <string>

namespace {

// Seam + virtual time are process-global; the guard restores both however a case exits
// (the NdiSeamGuard rationale, unit_NdiDriver.cpp).
struct EncSeamGuard {
    explicit EncSeamGuard(mm::platform::EncoderTestMode mode) {
        mm::platform::setTestEncoderMode(mode);
    }
    ~EncSeamGuard() {
        mm::platform::setTestEncoderMode(mm::platform::EncoderTestMode::Off);
        mm::platform::setTestNowMs(0);
    }
};

// A wall wired as production wires it: the Layout gives the Layer its physical size and the
// driver reads that, never its own controls.
struct Wall {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    Wall(uint16_t width, uint16_t height) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

void setUp(mm::HlsDriver& driver, mm::Buffer& source, Wall& wall, mm::nrOfLightsType count) {
    REQUIRE(source.allocate(count, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    driver.setLayer(&wall.layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::encoderTestClearFrames();
}

void paint(mm::Buffer& b, mm::nrOfLightsType i, uint8_t r, uint8_t g, uint8_t bl) {
    uint8_t* p = b.data() + static_cast<size_t>(i) * 3;
    p[0] = r; p[1] = g; p[2] = bl;
}

}  // namespace

// Without ffmpeg the driver is inert but SAFE, and says why: the state every machine without it
// is in, including CI, so it is the default path rather than an edge case.
TEST_CASE("HlsDriver reports a missing ffmpeg instead of failing") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::ForceMissing};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);

    driver.prepare();
    CHECK(std::string(driver.status()).find("ffmpeg not found") != std::string::npos);

    mm::platform::setTestNowMs(1000);
    driver.tick();                                   // must not crash, must pipe nothing
    CHECK(mm::platform::encoderTestFrameCount() == 0);
}

// The ffmpeg invocation IS the encode contract: raw RGB in at the grid size and chosen rate,
// zerolatency x264 at the chosen bitrate, 1 s segments on a short rolling playlist (the live
// tuning behind the documented 2-5 s latency), segments deleted as they age out.
TEST_CASE("HlsDriver hands ffmpeg the exact live-HLS invocation") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(6, 4);
    setUp(driver, source, wall, 24);
    driver.targetFps = 25;
    driver.bitrateKbit = 4000;
    driver.prepare();

    const std::string args = mm::platform::encoderTestArgs();
    CHECK(args.find("-f rawvideo -pix_fmt rgb24 -s 6x4 -r 25 -i -") != std::string::npos);
    CHECK(args.find("-c:v libx264 -preset veryfast -tune zerolatency -g 25") != std::string::npos);
    CHECK(args.find("-b:v 4000k") != std::string::npos);
    CHECK(args.find("-f hls -hls_time 1 -hls_list_size 6 -hls_flags delete_segments")
          != std::string::npos);
    CHECK(args.find("/.hls/stream.m3u8") != std::string::npos);
}

// One frame piped per tick within the rate: the grid's pixels, tight RGB, corrected: what the
// wall shows is what the stream shows, pixel for pixel.
TEST_CASE("HlsDriver pipes the grid pixel-exact") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();

    paint(source, 0, 10, 20, 30);
    paint(source, 7, 40, 50, 60);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::encoderTestFrameCount() == 1);
    CHECK(mm::platform::encoderTestFrameSize(0) == 4u * 2u * 3u);
    const uint8_t* f = mm::platform::encoderTestFrameData(0);
    REQUIRE(f != nullptr);
    CHECK(f[0] == 10); CHECK(f[1] == 20); CHECK(f[2] == 30);      // first light
    CHECK(f[21] == 40); CHECK(f[22] == 50); CHECK(f[23] == 60);   // eighth light, at 7*3
}

// targetFps is a ceiling the driver enforces itself: the render loop ticks faster and the frames
// beyond the rate are simply not encoded.
TEST_CASE("HlsDriver encodes no faster than targetFps") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.targetFps = 10;                            // 100 ms budget per frame
    driver.prepare();

    mm::platform::setTestNowMs(1000);
    driver.tick();                                    // sends
    mm::platform::setTestNowMs(1050);
    driver.tick();                                    // inside the budget: skipped
    mm::platform::setTestNowMs(1101);
    driver.tick();                                    // budget passed: sends
    CHECK(mm::platform::encoderTestFrameCount() == 2);
}

// A full pipe is a dropped frame, counted and shown, never a blocked render tick.
TEST_CASE("HlsDriver drops frames the encoder refuses, and says so") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();

    mm::platform::setTestEncoderWriteResult(0);       // pipe refuses the first byte
    mm::platform::setTestNowMs(1000);
    driver.tick();
    mm::platform::setTestNowMs(2000);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == 0);   // nothing piped...
    driver.tick1s();
    CHECK(std::string(driver.status()).find("2 frames dropped") != std::string::npos);   // ...but visible
}

// A dead encoder restarts from the housekeeping tick, and the stream resumes.
TEST_CASE("HlsDriver restarts a dead encoder and resumes streaming") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();

    mm::platform::setTestEncoderWriteResult(-1);      // the process is gone
    mm::platform::setTestNowMs(1000);
    driver.tick();
    driver.tick1s();
    CHECK(std::string(driver.status()).find("restarted") != std::string::npos);

    mm::platform::setTestEncoderWriteResult(1);       // back to normal recording
    mm::platform::setTestNowMs(2000);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == 1);
}

// release() stops the encoder and leaves nothing behind; a re-prepare comes back streaming.
TEST_CASE("HlsDriver releases and re-prepares cleanly") {
    EncSeamGuard seam{mm::platform::EncoderTestMode::Record};
    mm::Buffer source;
    mm::HlsDriver driver;
    Wall wall(4, 2);
    setUp(driver, source, wall, 8);
    driver.prepare();
    CHECK(std::string(driver.status()).find("streaming 4x2") != std::string::npos);

    driver.release();
    mm::platform::setTestNowMs(1000);
    driver.tick();                                    // closed: pipes nothing
    CHECK(mm::platform::encoderTestFrameCount() == 0);

    driver.prepare();
    mm::platform::setTestNowMs(2000);
    driver.tick();
    CHECK(mm::platform::encoderTestFrameCount() == 1);
}
