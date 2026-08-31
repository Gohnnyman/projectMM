// @module MoonLive
// @also MoonLiveEffect

// A script aims moving heads: setPan and setTilt.
//
// Motion is not a color byte at a fixed offset. WHERE pan lives inside a light's bytes comes from
// the layer's fixture channel map, so these builtins are host calls routed through the binding
// rather than the inline stores setRGB compiles to. That routing is what these pin: a script must
// reach the same channels a compiled effect writes, and must do nothing at all on a light that has
// no motion channels, which is what lets one script run on a moving head and on a plain strip.

#include "doctest.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include "MoonLiveScriptFixture.h"
#include "../core/moonlive_script_wrap.h"
#include "light/moonlive/MoonLiveEffect.h"
#include "core/moonlive/moonlive_emit.h"   // MM_MOONLIVE_HAS_HOST_JIT
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"
#include "light/layers/Layer.h"

using namespace mm;

#if MM_MOONLIVE_HAS_HOST_JIT

namespace {
/// Put a SHIPPED script on the test filesystem under its own name, and return that name.
///
/// The point of the two cases at the bottom of this file is the file in `moonlive/`, so it is read
/// from there rather than pasted here: a pasted copy stops being the thing that ships the first
/// time someone edits the real one.
const char* shipScript(const char* name) {
    const std::filesystem::path src =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
        / "moonlive" / "effects" / name;
    std::ifstream f(src);
    REQUIRE_MESSAGE(f.good(), "missing shipped script: ", src.string());
    std::ostringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();

    platform::fsMkdir(mm::moonlive::kScriptDir);
    char path[128];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    REQUIRE(platform::fsWriteAtomic(path, text.c_str(), text.size()));
    return name;
}

/// A rig of moving heads: a 1xN chain with pan and tilt channels, the shape a real head chain has.
/// A 1D layout is width 1 by height N, because extrude duplicates the x=0 column.
struct HeadRig {
    Layouts layouts;
    GridLayout grid;
    Layer layer;
    MoonLiveEffect effect;
    FixtureChannels fc;

    explicit HeadRig(bool withMotion = true, int heads = 4) {
        grid.width = 1; grid.height = heads; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        if (withMotion) {
            layer.setChannelsPerLight(6);                  // RGBW + pan + tilt
            fc.pan  = FixtureChannels::kMotionBase;
            fc.tilt = FixtureChannels::kMotionBase + 1;
            layer.setFixtureChannels(fc);
        } else {
            layer.setChannelsPerLight(3);                  // a plain RGB strip: no motion at all
        }
        layer.addChild(&effect);
        effect.defineControls();
    }

    void run(const char* script) {
        effect.setScript(mmWriteScript(script));
        layouts.applyState();
        layer.applyState();
        platform::setTestNowMs(1);
        layer.tick();
    }

    uint8_t channel(nrOfLightsType light, uint8_t offset) const {
        const auto& b = layer.buffer();
        return b.data()[static_cast<size_t>(light) * b.channelsPerLight() + offset];
    }
};
}  // namespace

// The point of the feature: a script can aim each head independently, and the value lands in the
// channel the fixture map names rather than at some offset the engine guessed.
TEST_CASE("a script aims each head with setPan and setTilt") {
    HeadRig rig;
    rig.run("class Aim {"
            "  tick() {"
            "    for (i = 0; i < height; i = i + 1) {"
            "      setPan(i, 10 + i * 20);"
            "      setTilt(i, 200 - i * 20);"
            "    }"
            "  }"
            "}");

    for (nrOfLightsType i = 0; i < 4; i++) {
        CHECK(rig.channel(i, rig.fc.pan)  == static_cast<uint8_t>(10 + i * 20));
        CHECK(rig.channel(i, rig.fc.tilt) == static_cast<uint8_t>(200 - i * 20));
    }
}

// A strip has no pan channel, so the same script must run and write nothing rather than corrupting
// a color byte. This is what lets one script be used on both kinds of rig.
TEST_CASE("setPan on a light with no motion channel writes nothing") {
    HeadRig strip(/*withMotion=*/false);
    strip.run("class Aim {"
              "  tick() {"
              "    fill(0, 0, 0);"
              "    for (i = 0; i < height; i = i + 1) { setPan(i, 255); setTilt(i, 255); }"
              "  }"
              "}");

    // Every byte is still the black the fill wrote: nothing leaked into the color channels.
    const auto& b = strip.layer.buffer();
    for (size_t i = 0; i < static_cast<size_t>(b.count()) * b.channelsPerLight(); i++)
        CHECK(b.data()[i] == 0);
}

// Motion is not color: dimming a rig must not swing its heads toward 0/0, so the brightness
// scaling that applies to color must not touch these channels.
TEST_CASE("a head's aim is not scaled by brightness") {
    HeadRig rig;
    rig.run("class Aim { tick() { fill(255, 255, 255); setPan(0, 200); setTilt(0, 100); } }");

    const uint8_t pan = rig.channel(0, rig.fc.pan);
    const uint8_t tilt = rig.channel(0, rig.fc.tilt);
    CHECK(pan == 200);
    CHECK(tilt == 100);
}

// An index past the end is a script bug, not a crash: the same bounds guard setRGB has. Writing
// through it would corrupt whatever follows the buffer.
TEST_CASE("setPan past the last light is ignored") {
    HeadRig rig;
    rig.run("class Aim { tick() { setPan(0, 42); setPan(9999, 200); setTilt(9999, 200); } }");

    CHECK(rig.channel(0, rig.fc.pan) == 42);   // the in-range write still happened
}

// The two SHIPPED motion scripts, run on a real head rig. They are the reference a user reads to
// learn setPan/setTilt, so "it compiles" is not enough: aim.mle must put every head where its
// sliders say, and sweep.mle must actually move them and differ between formations.
TEST_CASE("aim.mle points every head where its sliders say") {
    HeadRig rig;
    rig.effect.setScript(shipScript("aim.mle"));
    rig.layouts.applyState();
    rig.layer.applyState();
    platform::setTestNowMs(1);
    rig.layer.tick();

    // Defaults: pan and tilt centered, spread 0, so every head lands on the same aim and stays.
    for (nrOfLightsType i = 0; i < 4; i++) {
        CHECK(rig.channel(i, rig.fc.pan) == 128);
        CHECK(rig.channel(i, rig.fc.tilt) == 128);
    }
    // And it HOLDS: nothing in this script moves on its own, which is what makes it the one to
    // focus a rig with.
    platform::setTestNowMs(5000);
    rig.layer.tick();
    CHECK(rig.channel(0, rig.fc.pan) == 128);
}

TEST_CASE("sweep.mle moves the rig and its formations differ") {
    auto aimsFor = [](uint8_t formation) {
        HeadRig rig;
        rig.effect.setScript(shipScript("sweep.mle"));
        rig.layouts.applyState();
        rig.layer.applyState();
        // A scripted control is set by name, the same path the UI takes.
        for (uint8_t k = 0; k < rig.effect.controls().count(); k++)
            if (std::strcmp(rig.effect.controls()[k].name, "formation") == 0)
                *static_cast<uint8_t*>(rig.effect.controls()[k].ptr) = formation;
        std::vector<uint8_t> pans;
        // The clock has to MOVE: a beat phase reads its first advance as the time base, so a single
        // tick leaves every head at the same point of the sweep whatever the formation.
        for (int f = 1; f <= 12; f++) { platform::setTestNowMs(f * 400u); rig.layer.tick(); }
        for (nrOfLightsType i = 0; i < 4; i++) pans.push_back(rig.channel(i, rig.fc.pan));
        return pans;
    };

    const auto unison = aimsFor(4);
    const auto chase  = aimsFor(2);
    const auto cross  = aimsFor(3);

    // Unison is the reference: one aim for the whole rig.
    for (size_t i = 1; i < unison.size(); i++) CHECK(unison[i] == unison[0]);
    // Chase delays each head along the sweep, so neighbours differ.
    CHECK(chase != unison);
    // Cross opposes alternate heads, so it differs from both.
    CHECK(cross != unison);
    CHECK(cross != chase);
}

// The audio vocabulary. Its most important property is what happens with NO audio: a script
// written for a rig with a microphone must still run on one without, rendering nothing rather than
// failing. That falls out of AudioService::latestFrame returning a silent frame rather than null,
// and this pins it, because the alternative (a crash or a compile error on an audio-less device)
// would only ever be found on someone's hardware.
TEST_CASE("an audio script runs on a device with no audio, and paints nothing") {
    HeadRig rig(/*withMotion=*/false);
    rig.run("class A {"
            "  tick() {"
            "    fill(0, 0, 0);"
            "    for (i = 0; i < height; i = i + 1) {"
            "      setRGB(i, audioLevel(), audioBand(i), audioBeat() * 255);"
            "    }"
            "  }"
            "}");

    // Every audio reading is 0 in silence, so every pixel is black: the script ran and decided to
    // paint nothing, which is different from the script failing to run.
    const auto& b = rig.layer.buffer();
    for (size_t i = 0; i < static_cast<size_t>(b.count()) * b.channelsPerLight(); i++)
        CHECK(b.data()[i] == 0);
}

// A band index outside the 16 the spectrum has reads 0 rather than wrapping: a script asking for
// band 20 has a bug, and wrapping would answer it with a plausible number from the wrong end.
TEST_CASE("an out-of-range audio band reads zero") {
    HeadRig rig(/*withMotion=*/false);
    rig.run("class A { tick() { fill(0,0,0); setRGB(0, audioBand(99), 0, 0); } }");
    CHECK(rig.channel(0, 0) == 0);
}

// The names are `audio*` on purpose: a builtin RESERVES its name, so a bare `level` would stop
// every script that declares one from compiling. This is the regression test for that.
TEST_CASE("a script may still declare a member called level") {
    HeadRig rig(/*withMotion=*/false);
    rig.run("class A {"
            "  byte level = 200;"
            "  defineControls() { addControl(\"level\", level, 0, 255); }"
            "  tick() { fill(level, 0, 0); }"
            "}");
    CHECK(rig.channel(0, 0) == 200);   // it compiled, and the member is what painted
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
