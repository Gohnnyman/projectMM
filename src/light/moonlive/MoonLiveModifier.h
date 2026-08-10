#pragma once

#include "core/moonlive/MoonLive.h"
#include "light/modifiers/ModifierBase.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstdio>
#include <cstring>

// MoonLiveModifier — a scripted MODIFIER: a coordinate transform authored live as a script instead
// of compiled in as a C++ class.
//
// This is the second binding of the MoonLive engine after MoonLiveEffect, and it is what shows the
// engine is domain-neutral: it needed no engine, IR, grammar or backend change. An effect script
// writes a COLOUR per light; a modifier script writes a POSITION. Both are "three values stored at
// an index", which is what the engine's StoreElem already emits.
//
// **The script does not loop, because the Layer already does.** `modifyLogical` is called once per
// physical light by the Layer's fold walk (Layer.h, the mapping build), so a script only ever
// transforms ONE coordinate. That is exactly the shape the grammar has today — `call(expr, …)` —
// which is why a modifier is the binding that fits the language as it stands. A scripted LAYOUT, by
// contrast, has to place N different positions in a single pass with no per-light call to ride on,
// so it needs a loop the language does not have yet; that is why this comes first.
//
// **How the script reads the coordinate.** `x`, `y` and `z` are system variables the light domain
// defines (`modifierSysVars`), so a bare `x` in an expression compiles to the same LoadCtrl a control
// read uses. Before each call the binding writes the light's position into those arena slots. No
// new IR op — the compiler resolves the name, and a script cannot declare one that shadows it.
//
// **Coordinates are bytes, so an axis spans 0..255**, on the way in AND on the way out: a script
// that computes a position past 255 keeps its low byte, so `(width - 1 - x) * 2` on a wide grid
// lands somewhere unintended rather than being discarded. The input guard below rejects an
// out-of-range coordinate before the script sees it; an out-of-range RESULT is the script's own.
// A control slot is one byte, which is the
// price of reusing the control path for inputs. That covers every grid we drive today; a wall
// longer than 255 on one axis (the 48x256 wall is exactly at it) needs the 16-bit element store
// that is backlogged with the same reason.

namespace mm {

/// Modifier whose coordinate transform is a live-authored MoonLive script.
class MoonLiveModifier : public ModifierBase {
public:
    const char* tags() const override { return "📝"; }   // scripted

    void defineControls() override {
        controls_.addTextArea("source", source_, sizeof(source_));
        // Every control the script declared. System variables (`x`/`y`/`z`, `width`/`height`/
        // `depth`, `t`) are not controls and never appear here, so there is nothing to filter out.
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

    /// Compile the script as written.
    ///
    /// ModifierBase::affectsPrepare returns true for every control, which is right here: a source
    /// edit and a scripted-control move both change where lights land, and the Layer has to rebuild
    /// its mapping either way.
    void prepare() override {
        // The script compiles as written. `x`/`y`/`z` (the light being transformed) and
        // `width`/`height`/`depth` (the box it sits in) are SYSTEM VARIABLES the light domain
        // defines — the binding writes their slots per call, and the compiler reserves the names so
        // a script cannot declare one and shadow the value it is being handed.
        moonlive::resetPrintBudget();
        if (engine_.compile(source_, moonlive::lightBuiltins(),
                            moonlive::modifierSysVars())) {
            clearStatus();
        } else {
            setStatus(engine_.error(), Severity::Error);
        }
        rebuildControls();
        setDynamicBytes(engine_.heapBytes());
        // Ask for a rebuild ONLY when the script actually changed. modifyLogical is the static
        // hook — it runs while the Layer builds its mapping — so an edit is invisible until the
        // Layer rebuilds. But the rebuild the Layer performs IS applyState(), which calls prepare()
        // again: setting the flag unconditionally makes the two call each other forever, the
        // mapping is rebuilt every frame, and the fixture renders nothing at all. Comparing the
        // compiled source is what breaks that cycle.
        if (std::strcmp(source_, compiled_) != 0) {
            std::snprintf(compiled_, sizeof(compiled_), "%s", source_);
            needsRebuild_ = true;
        }
    }

    /// The Layer polls this after ticking its modifiers and rebuilds its mapping once if any asks.
    bool consumeNeedsRebuild() override {
        const bool r = needsRebuild_;
        needsRebuild_ = false;
        return r;
    }

    /// The Layer hands every modifier the running logical box before it folds any coordinate.
    /// Stash it so the script can read `width`/`height`/`depth`.
    void modifyLogicalSize(Coord3D& size) override { box_ = size; }

    /// Transform one coordinate. Called by the Layer once per physical light while it builds the
    /// mapping — the cold path, not per frame.
    bool modifyLogical(Coord3D& pos) const override {
        if (!engine_.ok()) return true;   // a broken script passes coordinates through unchanged
        // A control slot is a byte: a coordinate outside 0..255 cannot be represented, so it is
        // passed through untransformed rather than silently wrapping to a wrong position.
        if (pos.x < 0 || pos.x > 255 || pos.y < 0 || pos.y > 255 || pos.z < 0 || pos.z > 255)
            return true;

        auto* self = const_cast<MoonLiveModifier*>(this);
        uint8_t* sx = self->engine_.controlSlot(moonlive::kSysX);
        uint8_t* sy = self->engine_.controlSlot(moonlive::kSysY);
        uint8_t* sz = self->engine_.controlSlot(moonlive::kSysZ);
        if (!sx || !sy || !sz) return true;
        *sx = static_cast<uint8_t>(pos.x);
        *sy = static_cast<uint8_t>(pos.y);
        *sz = static_cast<uint8_t>(pos.z);
        // The box, clamped into the byte a control slot holds. A grid wider than 255 reports 255,
        // which is wrong but bounded — and that axis already cannot be scripted at all (the input
        // guard above passes it straight through), so no script sees the clamped value.
        auto clamp255 = [](lengthType v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        if (uint8_t* sw = self->engine_.controlSlot(moonlive::kSysWidth))  *sw = clamp255(box_.x);
        if (uint8_t* sh = self->engine_.controlSlot(moonlive::kSysHeight)) *sh = clamp255(box_.y);
        if (uint8_t* sd = self->engine_.controlSlot(moonlive::kSysDepth))  *sd = clamp255(box_.z);

        // One light's worth of destination. The script addresses it as index 0 today; the
        // index argument is real (setXYZ(index, x, y, z), the same shape as setRGB), so a
        // script written against a future `for` loop uses the identical call.
        uint8_t out[3] = {*sx, *sy, *sz};   // seeded with the input, so a script that writes
                                            // nothing leaves the coordinate untouched
        self->engine_.run(out, 1, 3, 0);

        pos.x = static_cast<lengthType>(out[0]);
        pos.y = static_cast<lengthType>(out[1]);
        pos.z = static_cast<lengthType>(out[2]);
        return true;
    }

    void release() override {
        engine_.free();
        // Forget what was compiled: release drops the program, so the next prepare() has to be
        // treated as a first compile. Keeping it made a disabled-then-re-enabled modifier inert —
        // the Layer folds while the engine is empty, then prepare() recompiles, sees the same
        // source, and never asks for the rebuild that would apply it.
        compiled_[0] = '\0';
        ModifierBase::release();
    }

    /// Replace the script. The next prepare() compiles it — the same path a UI edit takes, so a
    /// test and a user exercise identical code.
    void setSource(const char* s) {
        if (!s) return;
        std::snprintf(source_, sizeof(source_), "%s", s);
    }

private:
    mutable moonlive::MoonLive engine_;

    // Default script — a mirror on x. Chosen because it is instantly readable on a bench strand
    // (the pattern runs the other way) and is a modifier people actually reach for, so a working
    // binding looks like something rather than like nothing.
    char source_[moonlive::kMaxScriptBytes] = "setXYZ(0, width - 1 - x, y, z);";

    // The source the CURRENT mapping was built from; a rebuild is needed only when it changes.
    // sizeof(source_), never a literal: this is the string compared to decide whether to rebuild,
    // so a copy too small to hold source_ truncates, never matches, and the mapping rebuilds every
    // frame — the blank-screen loop the comparison exists to prevent.
    char compiled_[sizeof(source_)] = {};

    char ctrlNames_[moonlive::kMaxCtrls][moonlive::kMaxControlName] = {};

    bool needsRebuild_ = false;   // a recompile happened; the Layer's mapping is stale
    Coord3D box_{0, 0, 0};        // the logical box, from modifyLogicalSize
};

}  // namespace mm
