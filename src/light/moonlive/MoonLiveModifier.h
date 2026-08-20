#pragma once

#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveScript.h"
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
        // The script NAME, not the script — the text lives in a file the UI loads and saves
        // through /api/file. A module costs ~32 bytes here instead of a resident kilobyte.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kModifierPick);
        // Every control the script declared. System variables (`x`/`y`/`z`, `width`/`height`/
        // `depth`, `t`) are not controls and never appear here, so there is nothing to filter out.
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = script_.engine().declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = script_.engine().controlSlot(decls[i].offset);
            if (!slot) continue;
            // Published at the width the script declared. A uint16_t member reaches the UI as a
            // 16-bit control writing both its arena bytes; publishing it as a uint8 would drive
            // only the low one and leave the high half holding whatever it had.
            if (decls[i].type == moonlive::CtrlType::Uint16) {
                // Safe to view as a uint16_t: the compiler aligns every wide member to an even
                // arena offset (two backends cannot encode an odd halfword offset at all), and the
                // arena base comes from platform::alloc, which is aligned for any fundamental type.
                controls_.addUint16(decls[i].name, *reinterpret_cast<uint16_t*>(slot),
                                    decls[i].min, decls[i].max);
            } else {
                controls_.addUint8(decls[i].name, *slot,
                                   static_cast<uint8_t>(decls[i].min),
                                   static_cast<uint8_t>(decls[i].max));
            }
        }
    }

    /// Compile the script as written.
    ///
    /// ModifierBase::affectsPrepare returns true for every control, which is right here: a source
    /// edit and a scripted-control move both change where lights land, and the Layer has to rebuild
    /// its mapping either way.
    void prepare() override {
        // Ask for a Layer rebuild ONLY when a new program was actually installed. modifyLogical is
        // the static hook, running while the Layer builds its mapping, so an edit is invisible until
        // the Layer rebuilds. But the rebuild the Layer performs IS applyState(), which calls
        // prepare() again: setting the flag unconditionally makes the two call each other forever,
        // the mapping is rebuilt every frame, and the fixture renders nothing at all. sync()
        // returning false for an unchanged file is what breaks that cycle.
        if (script_.sync(moonlive::modifierSysVars(), *this)) needsRebuild_ = true;
        rebuildControls();
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
        if (!script_.ok()) return true;   // a broken script passes coordinates through unchanged
        // A control slot is a byte: a coordinate outside 0..255 cannot be represented, so it is
        // passed through untransformed rather than silently wrapping to a wrong position.
        if (pos.x < 0 || pos.x > 255 || pos.y < 0 || pos.y > 255 || pos.z < 0 || pos.z > 255)
            return true;

        auto* self = const_cast<MoonLiveModifier*>(this);
        uint8_t* sx = self->script_.engine().controlSlot(moonlive::kSysX);
        uint8_t* sy = self->script_.engine().controlSlot(moonlive::kSysY);
        uint8_t* sz = self->script_.engine().controlSlot(moonlive::kSysZ);
        if (!sx || !sy || !sz) return true;
        *sx = static_cast<uint8_t>(pos.x);
        *sy = static_cast<uint8_t>(pos.y);
        *sz = static_cast<uint8_t>(pos.z);
        // The box, clamped into the byte a control slot holds. A grid wider than 255 reports 255,
        // which is wrong but bounded — and that axis already cannot be scripted at all (the input
        // guard above passes it straight through), so no script sees the clamped value.
        auto clamp255 = [](lengthType v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
        if (uint8_t* sw = self->script_.engine().controlSlot(moonlive::kSysWidth))  *sw = clamp255(box_.x);
        if (uint8_t* sh = self->script_.engine().controlSlot(moonlive::kSysHeight)) *sh = clamp255(box_.y);
        if (uint8_t* sd = self->script_.engine().controlSlot(moonlive::kSysDepth))  *sd = clamp255(box_.z);

        // One light's worth of destination, which is why setXYZ(x, y, z) names no slot: a modifier
        // is handed a single coordinate per call and can write nothing else. (setRGB keeps its
        // index because an effect picks a pixel out of a whole buffer.)
        uint8_t out[3] = {*sx, *sy, *sz};   // seeded with the input, so a script that writes
                                            // nothing leaves the coordinate untouched
        // The fold moment: run `modifyLogical` if the script defined one, and leave the coordinate
        // untouched otherwise. The cold path (once per light at mapping build, not per frame), so
        // the lookup costs nothing measurable.
        if (!script_.engine().hasEntry(moonlive::kEntryModify)) return true;
        self->script_.engine().run(out, 1, 3, 0, moonlive::kEntryModify);

        pos.x = static_cast<lengthType>(out[0]);
        pos.y = static_cast<lengthType>(out[1]);
        pos.z = static_cast<lengthType>(out[2]);
        return true;
    }

    void release() override {
        script_.engine().free();
        // Forget what was compiled: release drops the program, so the next prepare() has to be
        // treated as a first compile. Keeping it made a disabled-then-re-enabled modifier inert —
        // the Layer folds while the engine is empty, then prepare() recompiles, sees the same
        // source, and never asks for the rebuild that would apply it.
        script_.invalidate();
        ModifierBase::release();
    }

    /// Replace the script. The next prepare() compiles it — the same path a UI edit takes, so a
    /// test and a user exercise identical code.
    void setScript(const char* name) { script_.setName(name); }

private:
    // The script this modifier folds coordinates with. Empty on a fresh card: it reports "no script"
    // and passes coordinates through untouched until one is named.
    mutable moonlive::MoonLiveScript script_;

    bool needsRebuild_ = false;   // a recompile happened; the Layer's mapping is stale
    Coord3D box_{0, 0, 0};        // the logical box, from modifyLogicalSize
};

}  // namespace mm
