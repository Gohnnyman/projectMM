#pragma once

#include "core/moonlive/MoonLive.h"
#include "light/layouts/LayoutBase.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstdio>
#include <cstring>

// MoonLiveLayout — a scripted LAYOUT: where the lights physically are, written as text on a running
// device instead of compiled in as a C++ class.
//
// A layout is the one part of the pipeline that differs for every physical build — a ring, a spiral
// staircase, a car grille, a costume. Each one has meant a new C++ class, a rebuild and a reflash.
// A script means the person who hung the lights can describe where they went, on the device, and
// see it immediately.
//
// This is the binding that needed the language to grow. A modifier's script transforms ONE
// coordinate because the Layer calls it once per light; a layout has no such per-light call to ride
// on — it has to place N lights itself, which takes a loop. That is why `for` landed first.
//
// **It allocates nothing, like every other layout.** `GridLayout` computes its count arithmetically
// and emits straight into the sink; `SphereLayout` walks the same loop twice, counting then
// emitting, "so they never disagree". A scripted layout does exactly the same: the script calls
// `addLight(x, y, z)` per light, and the binding points that call at a counter on the sizing pass
// and at the consumer's sink on the walk. Staging the coordinates in an array instead would cost
// 48 KB on a 16k-light fixture — memory a classic ESP32 does not have, and a mechanism no other
// layout uses, which matters because scripted and compiled layouts compose in one `Layouts`
// container and have to behave identically through this interface.
//
// **The count and the coordinates come from the same code.** `lightCount()` runs the script with a
// counting sink; `forEachCoord` runs it again into the caller's. Same script, same arithmetic, so
// the two answers cannot drift apart — the property SphereLayout's comment names.

namespace mm {

/// Layout whose physical light positions are a live-authored MoonLive script.
class MoonLiveLayout : public LayoutBase {
public:
    const char* tags() const override { return "📝"; }   // scripted

    void defineControls() override {
        controls_.addTextArea("source", source_, sizeof(source_));
        // Every control the SCRIPT declared — including any extents it loops over. A layout does not
        // RECEIVE a width: the pipeline derives its bounding box from the coordinates the layouts
        // actually place (Layouts::prepare, "max coordinate + 1 per axis"), so a width handed in
        // from outside would be a second, disagreeing source of truth. A script that wants one
        // declares it (`uint8_t width = 16; // @control 1..64`) and it becomes a real slider.
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = engine_.declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = engine_.controlSlot(decls[i].offset);
            if (!slot) continue;
            std::memcpy(ctrlNames_[i], decls[i].name, decls[i].nameLen);
            ctrlNames_[i][decls[i].nameLen] = '\0';
            controls_.addUint8(ctrlNames_[i], *slot, decls[i].min, decls[i].max);
        }
    }

    /// Compile the script. The lights themselves are placed by whoever asks — see lightCount().
    void prepare() override {
        compile();
        rebuildControls();
    }

    /// Run the script, counting what it places.
    ///
    /// The Layer sizes its buffer from this before asking for a single coordinate, which is why it
    /// cannot come from the walk. Running the script twice is what every other layout does — the
    /// alternative is caching coordinates, and that is the allocation this design exists to avoid.
    nrOfLightsType lightCount() const override {
        compile();
        if (!engine_.ok()) return 0;
        Counter c{0};
        runScript(&addToCounter, &c);
        return c.n;
    }

    /// Run the script again, emitting each light into the consumer's sink.
    void forEachCoord(const CoordSink& sink) const override {
        compile();
        if (!engine_.ok()) return;
        Emitter e{&sink, 0};
        runScript(&addToSink, &e);
    }

    void release() override {
        engine_.free();
        LayoutBase::release();
    }

    /// Replace the script. The next prepare() compiles it — the path a UI edit takes.
    void setSource(const char* s) {
        if (!s) return;
        std::snprintf(source_, sizeof(source_), "%s", s);
    }

private:
    /// Compile if the source has changed since the program that is loaded.
    ///
    /// Called from prepare(), and also from lightCount()/forEachCoord — because applyState() runs
    /// PARENT-FIRST (MoonModule.h): the container computes its bounding box by walking its children
    /// before those children have prepared. A layout whose count is arithmetic (GridLayout) does not
    /// notice; one that needs a compiled program would report an empty fixture to whoever asked
    /// first, and the pipeline would come up dark with no error anywhere.
    void compile() const {
        if (engine_.ok() && std::strcmp(source_, compiled_) == 0) return;   // already current
        auto* self = const_cast<MoonLiveLayout*>(this);
        moonlive::resetPrintBudget();
        if (self->engine_.compile(source_, moonlive::lightBuiltins())) self->clearStatus();
        else                                                          self->setStatus(self->engine_.error(), Severity::Error);
        std::snprintf(self->compiled_, sizeof(compiled_), "%s", source_);
        self->setDynamicBytes(engine_.ok() ? engine_.codeCap() : 0);
    }

    struct Counter { nrOfLightsType n; };
    struct Emitter { const CoordSink* sink; nrOfLightsType idx; };

    static void addToCounter(void* ctx, uint16_t, uint16_t, uint16_t) {
        static_cast<Counter*>(ctx)->n++;
    }
    static void addToSink(void* ctx, uint16_t x, uint16_t y, uint16_t z) {
        auto* e = static_cast<Emitter*>(ctx);
        e->sink->pixel(e->idx++, static_cast<lengthType>(x),
                       static_cast<lengthType>(y), static_cast<lengthType>(z));
    }

    /// Point addLight at `fn` and run the script once.
    ///
    /// The engine writes through a buffer it is handed, but this script writes through addLight
    /// instead — so it is given a single scratch light, enough to satisfy run()'s "somewhere to
    /// write" precondition without staging anything. A script that also calls setXYZ scribbles
    /// there harmlessly.
    void runScript(moonlive::AddLightFn fn, void* ctx) const {
        uint8_t scratch[3] = {0, 0, 0};
        moonlive::setAddLightSink(fn, ctx);
        const_cast<moonlive::MoonLive&>(engine_).run(scratch, 1, 3, 0);
        moonlive::setAddLightSink(nullptr, nullptr);
    }

    mutable moonlive::MoonLive engine_;

    // Default script — a grid, the layout almost every panel is. The nested loop and the index
    // arithmetic are the whole definition, which is the case for scripting a layout at all.
    char source_[512] =
        "uint8_t width = 16;  // @control 1..64\n"
        "uint8_t height = 16; // @control 1..64\n"
        "for (yy = 0; yy < height; yy = yy + 1) {\n"
        "  for (xx = 0; xx < width; xx = xx + 1) {\n"
        "    addLight(xx, yy, 0);\n"
        "  }\n"
        "}";

    // The source the loaded program was built from, so compile() is a no-op when current.
    mutable char compiled_[512] = {};

    char ctrlNames_[moonlive::kMaxCtrls][moonlive::kMaxControlName] = {};
};

}  // namespace mm
