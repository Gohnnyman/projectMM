// @module Correction

#include "doctest.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"

#include <cstdint>

// Pins the per-driver output correction: brightness LUT, channel reorder, and RGBW
// white derivation. The Drivers container owns a Correction, rebuilds it on a
// brightness/light-preset change, and hands it to each physical driver, which calls
// apply() per light. These tests pin the transform so a regression in the LUT fill,
// the preset→role-offset mapping, or the white math fails here. A light is a span of
// outChannels bytes with a named offset per colour role (offRed/offGreen/offBlue, and
// offWhite for the RGBW family; kAbsent = no white), so the checks read the observable
// apply() output plus outChannels, not an internal permutation table.

using mm::Correction;
// At brightness=255, the LUT maps every input value to itself (no scaling).
TEST_CASE("Correction brightness LUT: full brightness is identity") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    for (int v = 0; v < 256; v++) CHECK(c.briLut[v] == v);
}

// At brightness=128, every entry is roughly halved using scale8 (255→128, 128→64, 2→1).
TEST_CASE("Correction brightness LUT: half brightness halves each value (scale8)") {
    Correction c;
    mm::test::rebuildFromPreset(c, 128, mm::test::PresetOrder::RGB);
    CHECK(c.briLut[0] == 0);
    CHECK(c.briLut[255] == 128);   // (255*128)/255 = 128
    CHECK(c.briLut[128] == 64);    // (128*128)/255 = 64
    CHECK(c.briLut[2] == 1);       // (2*128)/255 = 1
}

// RGB preset at full brightness passes the source RGB through unchanged (3 output channels, no white).
TEST_CASE("Correction RGB preset: apply is identity at full brightness") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    CHECK(c.outChannels == 3);
    CHECK(c.offWhite == Correction::kAbsent);   // no white channel for the RGB family
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[3] = {};
    c.apply(src, out);
    CHECK(out[0] == 10);
    CHECK(out[1] == 20);
    CHECK(out[2] == 30);
}

// GRB preset swaps R and G in the output (G first, then R, then B) — for WS2812-like drivers.
TEST_CASE("Correction GRB preset: channels reordered, 3 output channels") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::GRB);
    CHECK(c.outChannels == 3);
    CHECK(c.offWhite == Correction::kAbsent);   // no white channel for the RGB family
    const uint8_t src[3] = {10, 20, 30};  // R=10 G=20 B=30
    uint8_t out[3] = {};
    c.apply(src, out);
    CHECK(out[0] == 20);  // G
    CHECK(out[1] == 10);  // R
    CHECK(out[2] == 30);  // B
}

// BGR preset reverses the channel order entirely (B, G, R).
TEST_CASE("Correction BGR preset: full reverse") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::BGR);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[3] = {};
    c.apply(src, out);
    CHECK(out[0] == 30);  // B
    CHECK(out[1] == 20);  // G
    CHECK(out[2] == 10);  // R
}

// RGBW preset adds a fourth white channel derived as min(R, G, B) per pixel.
TEST_CASE("Correction RGBW preset: 4 channels, white = min(r,g,b)") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    CHECK(c.outChannels == 4);
    CHECK(c.offWhite == 3);   // white derived into the 4th channel
    const uint8_t src[3] = {10, 20, 30};  // min = 10
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[0] == 10);  // R
    CHECK(out[1] == 20);  // G
    CHECK(out[2] == 30);  // B
    CHECK(out[3] == 10);  // W = min(10,20,30)
}

// GRBW preset combines the GRB reorder with the W derivation (G, R, B, W=min).
TEST_CASE("Correction GRBW preset: reordered RGB + white") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::GRBW);
    CHECK(c.outChannels == 4);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[0] == 20);  // G
    CHECK(out[1] == 10);  // R
    CHECK(out[2] == 30);  // B
    CHECK(out[3] == 10);  // W = min
}

// Brightness scaling runs before white derivation so W = min of the *scaled* RGB values.
TEST_CASE("Correction: brightness applied BEFORE white derivation") {
    // White must be min of the *scaled* channels, not the raw ones.
    Correction c;
    mm::test::rebuildFromPreset(c, 128, mm::test::PresetOrder::RGBW);  // half brightness
    const uint8_t src[3] = {100, 200, 60};  // scaled: 50, 100, 30 → min = 30
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[0] == 50);   // (100*128)/255
    CHECK(out[1] == 100);  // (200*128)/255
    CHECK(out[2] == 30);   // (60*128)/255
    CHECK(out[3] == 30);   // min(50,100,30) — proves white uses scaled values
}

// rebuild() can switch the output channel count between RGB (3) and RGBW (4) on the fly.
TEST_CASE("Correction: rebuild switches output channel count RGB<->RGBW") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGB);
    CHECK(c.outChannels == 3);
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    CHECK(c.outChannels == 4);
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::GRB);
    CHECK(c.outChannels == 3);
    CHECK(c.offWhite == Correction::kAbsent);   // white dropped back to absent
}

using mm::WhiteMode;

// whiteMode = Min is the default and derives W = min(scaled R,G,B) leaving RGB intact —
// this is the byte-identical behaviour the earlier RGBW tests already pin. whiteMode =
// None forces the white channel to 0 each frame (for effects that drive W themselves) —
// written, not skipped, so a reused buffer can't keep a stale value (see the assertion below).
TEST_CASE("Correction whiteMode None: white channel forced to 0, RGB intact") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    c.whiteMode = WhiteMode::None;
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[4] = {0, 0, 0, 77};   // pre-fill W with a stale value from a prior frame
    c.apply(src, out);
    CHECK(out[0] == 10);
    CHECK(out[1] == 20);
    CHECK(out[2] == 30);
    // None synthesises no white, but MUST still write the channel to 0 each frame: the driver's
    // corrected_ buffer is reused frame-to-frame, so leaving W unwritten would keep the stale 77 and
    // stick the white LED on after a switch to None.
    CHECK(out[3] == 0);
}

// whiteMode = Accurate pulls the common white component OUT of RGB (so the white LED
// carries it) rather than adding it on top — R,G,B each drop by min(R,G,B).
TEST_CASE("Correction whiteMode Accurate: white subtracted from RGB") {
    Correction c;
    mm::test::rebuildFromPreset(c, 255, mm::test::PresetOrder::RGBW);
    c.whiteMode = WhiteMode::Accurate;
    const uint8_t src[3] = {10, 20, 30};  // min = 10
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[0] == 0);    // R: 10 - 10
    CHECK(out[1] == 10);   // G: 20 - 10
    CHECK(out[2] == 20);   // B: 30 - 10
    CHECK(out[3] == 10);   // W: min(10,20,30)
}

using mm::ChannelRole;

// A Custom wiring is described by a channel-role array; rebuild() derives the colour
// offsets from it. Here: white first, then B, G, R — a 4-channel arbitrary order that no
// curated preset names, proving the role array reaches any wiring.
TEST_CASE("Correction roles array: arbitrary Custom wiring derives correct offsets") {
    Correction c;
    const ChannelRole roles[4] = {ChannelRole::White, ChannelRole::Blue,
                                  ChannelRole::Green, ChannelRole::Red};
    c.rebuild(128, roles, 4);   // half brightness
    // Offsets derived from the array: each role's channel index.
    CHECK(c.offWhite == 0);
    CHECK(c.offBlue == 1);
    CHECK(c.offGreen == 2);
    CHECK(c.offRed == 3);
    CHECK(c.outChannels == 4);
    CHECK(c.briLut[255] == 128);   // LUT refreshed (brightness applied)
    const uint8_t src[3] = {200, 100, 60};  // scaled: 100, 50, 30 → min = 30
    uint8_t out[4] = {};
    c.apply(src, out);
    CHECK(out[3] == 100);  // R at channel 3
    CHECK(out[2] == 50);   // G at channel 2
    CHECK(out[1] == 30);   // B at channel 1
    CHECK(out[0] == 30);   // W = min at channel 0
}

// A colour role absent from the array stays kAbsent and apply() doesn't write it — a wiring
// can carry any SUBSET of colour roles (e.g. a 2-channel R,B light with no green channel).
TEST_CASE("Correction roles array: absent colour role is not emitted") {
    Correction c;
    const ChannelRole roles[2] = {ChannelRole::Red, ChannelRole::Blue};   // no green channel
    c.rebuild(255, roles, 2);
    CHECK(c.offRed == 0);
    CHECK(c.offBlue == 1);
    CHECK(c.offGreen == Correction::kAbsent);   // green has nowhere to go
    CHECK(c.outChannels == 2);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[2] = {0, 0};
    c.apply(src, out);
    CHECK(out[0] == 10);   // R
    CHECK(out[1] == 30);   // B — green (20) simply not written
}

// A non-colour role (Pan) occupies a channel but apply()'s RGB path ignores it — the channel
// is left for the fixture role writer, and outChannels still counts it.
TEST_CASE("Correction roles array: non-colour role reserves a channel apply() skips") {
    Correction c;
    const ChannelRole roles[4] = {ChannelRole::Pan, ChannelRole::Red,
                                  ChannelRole::Green, ChannelRole::Blue};
    c.rebuild(255, roles, 4);
    CHECK(c.outChannels == 4);
    CHECK(c.offRed == 1);
    CHECK(c.offGreen == 2);
    CHECK(c.offBlue == 3);
    const uint8_t src[3] = {10, 20, 30};
    uint8_t out[4] = {77, 0, 0, 0};   // channel 0 (Pan) pre-set; apply() must leave it
    c.apply(src, out);
    CHECK(out[0] == 77);   // Pan channel untouched by the RGB path
    CHECK(out[1] == 10);   // R
    CHECK(out[2] == 20);   // G
    CHECK(out[3] == 30);   // B
}

// WarmWhite / Yellow / UV are synthesised from RGB off the SAME whiteMode as White, so a fixture
// carrying them lights up (best-effort approximations, not a colour model yet): WW ≈ min(RGB),
// Yellow ≈ min(R,G), UV ≈ the blue-excess max(0, B-max(R,G)). This is the "all channels burn so
// you can eyeball a fixture" behaviour the finding asked for; a real per-emitter model comes later.
TEST_CASE("Correction: WarmWhite/Yellow/UV synthesised from RGB via whiteMode") {
    Correction c;
    const ChannelRole roles[6] = {ChannelRole::Red, ChannelRole::Green, ChannelRole::Blue,
                                  ChannelRole::WarmWhite, ChannelRole::Yellow, ChannelRole::UV};
    c.rebuild(255, roles, 6);
    CHECK(c.offWarmWhite == 3);
    CHECK(c.offYellow == 4);
    CHECK(c.offUV == 5);
    const uint8_t src[3] = {40, 100, 200};   // R=40 G=100 B=200
    uint8_t out[6] = {};
    c.apply(src, out);
    CHECK(out[0] == 40);   // R
    CHECK(out[1] == 100);  // G
    CHECK(out[2] == 200);  // B
    CHECK(out[3] == 40);   // WW = min(40,100,200)
    CHECK(out[4] == 40);   // Yellow = min(R,G) = min(40,100)
    CHECK(out[5] == 100);  // UV = B - max(R,G) = 200 - 100 (fires on the blue excess)
}

// UV stays dark on a warm colour (no blue excess), and every synthesised emitter is forced to 0
// under whiteMode=None so none holds a stale value — the same reuse-safety the White channel has.
TEST_CASE("Correction: UV dark on warm colours; whiteMode None zeroes WW/Y/UV") {
    Correction c;
    const ChannelRole roles[6] = {ChannelRole::Red, ChannelRole::Green, ChannelRole::Blue,
                                  ChannelRole::WarmWhite, ChannelRole::Yellow, ChannelRole::UV};
    c.rebuild(255, roles, 6);
    {   // warm colour: R,G high, B low → UV = max(0, B-max(R,G)) = 0
        const uint8_t src[3] = {200, 180, 20};
        uint8_t out[6] = {};
        c.apply(src, out);
        CHECK(out[5] == 0);            // UV dark: no blue excess
        CHECK(out[4] == 180);          // Yellow = min(200,180)
    }
    {   // whiteMode None: every synthesised emitter forced to 0, even with stale pre-fill
        c.whiteMode = WhiteMode::None;
        const uint8_t src[3] = {40, 100, 200};
        uint8_t out[6] = {0, 0, 0, 55, 66, 77};   // stale WW/Y/UV
        c.apply(src, out);
        CHECK(out[3] == 0);            // WW zeroed
        CHECK(out[4] == 0);            // Yellow zeroed
        CHECK(out[5] == 0);            // UV zeroed
    }
}
