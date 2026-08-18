#pragma once

#include "light/effects/EffectBase.h"
#include "core/moonlive/MoonLive.h"
#include "light/moonlive/MoonLiveScriptFile.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstring>
#include <cstdio>

// MoonLiveEffect — a scripted effect rendered by the MoonLive engine (§3.3 of
// livescripts-analysis-top-down.md). The thin binding side of the engine/binding seam: it
// IS a first-class EffectBase (role, controls, lifecycle, generic UI), and its tick()
// delegates to a compiled MoonLive over this effect's own buffer.
//
// The effect holds a `source` text control; prepare compiles it through the engine and
// tick() runs the emitted native code over the buffer (emit → allocExec → call → write). A
// source edit recompiles live; a parse error shows in the module status and the layer goes
// dark — robust, no reboot.

namespace mm {

/// Effect whose render is a live-authored MoonLive script.
class MoonLiveEffect : public EffectBase {
public:
    const char* tags() const override { return "📝"; }   // scripted
    Dim dimensions() const override { return Dim::D2; }

    // The effect carries its script's NAME as an editable, persisted text control, plus a control
    // for every variable the script DECLARED (`uint8_t speed = 50; // @control 0..99`). The
    // engine exposes the declared list after a compile; each becomes a real uint8 control bound by
    // reference to the engine's live control-arena slot, so a slider write lands in the slot the
    // next render tick reads, with no recompile (the live-edit guarantee). Naming a different
    // script recompiles (the script-editor loop), which re-derives the control set.
    void defineControls() override {
        // The script NAME, not the script. The text lives in a file the UI loads, edits and
        // saves through /api/file — so a module costs ~32 bytes here instead of a resident
        // kilobyte, and a script is bounded by the filesystem rather than by this array.
        controls_.addText("script", script_, sizeof(script_));
        // Every control the script declared. System variables (`width`, `height`, `depth`, `t`)
        // are not controls and never appear here, so there is nothing to filter out.
        uint8_t n = 0;
        const moonlive::DeclaredControl* decls = engine_.declaredControls(n);
        for (uint8_t i = 0; i < n; i++) {
            uint8_t* slot = engine_.controlSlot(decls[i].offset);
            if (!slot) continue;   // engine not compiled yet (first sweep) — controls appear after prepare
            // The engine owns its declared names (MoonLive::compile copies them out of the
            // source before the text is freed), so the descriptor can borrow that pointer
            // directly — a second per-binding pool would be the same fact in two places.
            controls_.addUint8(decls[i].name, *slot, decls[i].min, decls[i].max);
        }
    }

    // Naming a different script must recompile: route it through the prepare rebuild sweep so the
    // new one swaps in live (the script-editor loop). A SCRIPTED CONTROL's value change must NOT
    // recompile: it just updates an arena byte the running native code reads next tick. So only
    // "script" triggers a rebuild; every scripted control returns false (the live-edit path).
    bool affectsPrepare(const char* controlName) const override {
        return std::strcmp(controlName, "script") == 0;
    }

    // Compile the source on the cold rebuild path. A failed compile (parse error or no exec
    // memory) surfaces in the module status and leaves tick() a no-op — the effect renders
    // dark, the device keeps running (robustness + no-reboot). A *source* edit re-enters here and
    // recompiles, so a new script swaps in live; a broken edit just shows its diagnostic.
    void prepare() override {
        // The script compiles as written. `width`/`height`/`depth` are SYSTEM VARIABLES the light
        // domain defines (effectSysVars) — an effect is not told its size by a user, it renders into
        // whatever layer it sits in, and the layer already knows. A script declaring its own `width`
        // would be a second, disagreeing answer: set it to 16 on an 8x8 panel and the effect draws
        // off the edge. The compiler reserves the name, so that cannot happen.
        moonlive::resetPrintBudget();
        const char* err = nullptr;
        if (moonlive::compileScriptFile(engine_, script_, moonlive::lightBuiltins(),
                                        moonlive::effectSysVars(), err)) {
            clearStatus();
        } else {
            setStatus(err, Severity::Error);
        }
        // The compile re-derives the declared-control set, so rebuild the control list to surface
        // it (the same rebuildControls() pattern NetworkModule uses when a state change reshapes
        // its controls). Each scripted control re-binds to its (stable-address) arena slot.
        rebuildControls();
        // Report the exec block as the module's heap use (codeCap, the word-rounded allocation),
        // so the UI card's "+ dynamic" reflects the JIT'd program — 0 when the compile failed.
        setDynamicBytes(engine_.heapBytes());
    }

    void tick() MM_NONBLOCKING override {
        // The native emitter stores R,G,B at offsets +0/+1/+2 with channelsPerLight() only as the
        // stride (moonlive_lower_*: addr = index * cpl, then 3 writes). A 0/1/2-channel layer would
        // let the last light's +1/+2 write run past the buffer, so a sub-RGB layout renders dark.
        const auto cpl = channelsPerLight();
        if (cpl < 3) return;
        if (!engine_.ok()) return;
        // Refresh the system variables before the script runs: a layer can be resized live, and a
        // script holding last frame's width would draw to the old geometry.
        writeSysVar(moonlive::kSysWidth,  width());
        writeSysVar(moonlive::kSysHeight, height());
        writeSysVar(moonlive::kSysDepth,  depth());
        // The draw builtins (line) render through the same canvas every native effect uses,
        // installed for exactly one run and detached after, so a script can only ever draw into
        // the layer it is ticking in.
        moonlive::setDrawCanvas(canvas());
        // The frame moment: run `tick` if the script defined one. A script that defines only
        // `modifyLogical` renders nothing here and folds coordinates instead, which is the author's
        // choice rather than an error.
        if (engine_.hasEntry(moonlive::kEntryTick))
            engine_.run(buffer(), nrOfLights(), cpl, elapsed(), moonlive::kEntryTick);
        moonlive::setDrawCanvas({});
    }

    void release() override {
        engine_.free();   // release the exec block — the destructor role
        EffectBase::release();
    }

    /// Replace the script. The next prepare() compiles it — the same path a UI edit takes, so a
    /// test and a user exercise identical code.
    /// Point the module at a script in the shared script directory. The file itself is written by
    /// the UI (or the File Manager); this only says WHICH one, and the next prepare() compiles it.
    void setScript(const char* name) {
        if (!name) return;
        std::snprintf(script_, sizeof(script_), "%s", name);
    }

private:
    moonlive::MoonLive engine_;
    // Default script — random pixels: each tick lights one random light in a random RGB color.
    // A live, always-visible starting example (and a good demo-reel slot). The index random16(256)
    // covers a typical grid; setRGB bounds-guards it (an index past the light count is skipped, and
    // 0×0 is safe), so most ticks land on a real light and the demo stays visibly lit.
    // Publish one system variable into its arena slot, saturating to the uint8 a slot holds — a
    // layer wider than 255 reports 255 rather than wrapping to a small number and drawing garbage.
    void writeSysVar(uint8_t offset, uint16_t value) {
        if (uint8_t* slot = engine_.controlSlot(offset))
            *slot = static_cast<uint8_t>(value > 255 ? 255 : value);
    }


    // A fresh card starts with NO script: it reports "no script" and renders nothing until one
    // is named. Naming a default here would make every new module compile the same effect.
    char script_[moonlive::kMaxScriptName + 1] = "";
};

}  // namespace mm
