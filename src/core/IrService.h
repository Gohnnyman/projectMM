#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"           // setControl — the generic control-set primitive
#include "core/FilesystemModule.h"    // noteDirty — schedule the debounced save on a learned bind
#include "platform/platform.h"        // irRead

#include <cstdint>
#include <cstdio>
#include <cstdlib>   // strtoul — rebuild learnedCode_ from persisted hex strings
#include <cstring>

namespace mm {

/// A core, domain-neutral IR-receiver peripheral: it decodes an IR remote on `pin` and drives
/// other modules' controls. It is the device's IR *input* — the role the WLED-app bridge plays
/// for the phone app, expressed for a physical remote.
///
/// **How it acts.** Every action routes through `Scheduler::setControl(module, control, value)`,
/// the one generic control-set primitive (also used by `/api/control`, Improv, and the WLED
/// bridge). IR never reaches into another module's internals; it composes against that primitive,
/// so adding a new action is one row in `kActions` — not new plumbing. The actions are
/// **brightness up/down** and **palette prev/next**, each a relative nudge of a target control.
///
/// **Buttons vs the remote.** Each action is a UI button (press it → the nudge happens now) AND a
/// learnable remote binding. The two share one path: a button press and a matching remote code
/// both call `runAction`.
///
/// **Learning (any remote, no firmware table).** Pick an action in the `learn` select; the next
/// decoded IR code binds to it (stored in that action's `code …` control, which persists like any
/// control). Press that remote button afterwards and its code is looked up → the action runs. This
/// is more flexible than a fixed per-remote preset table (MoonLight's model): it works with any
/// remote, the user teaches it live. A code bound to nothing is shown in `last code` and ignored.
///
/// **Relative adjust.** An action nudges a target control by a signed delta, clamped to the
/// control's own `[min, max]` — read generically from the target's `ControlDescriptor`, so the
/// same code adjusts a 0–255 brightness slider and an N-option palette Select without knowing
/// either's domain. A future LightsControl hub (docs/backlog) absorbs this as the normalised
/// interface effects read; `latestCode()` is the seam it consumes (the `AudioService::latestFrame`
/// pattern).
///
/// **Not auto-wired.** Factory-registered like AudioService, so a board with an IR
/// receiver adds it under the `Services` container via the installer device catalog (its `pin`
/// carrying that board's IR GPIO) or the user adds it from the UI. On the SE16 the IR line shares GPIO 5 with the Ethernet MISO
/// through the board's hardware switch; the pin is the receiver input.
///
/// **Prior art:** consumer IR remotes use the NEC protocol (a 32-bit address+command frame,
/// LSB-first, ~9 ms lead burst); the ESP-IDF RMT peripheral decodes it (the espressif
/// `ir_nec_transceiver` example). The decode itself lives behind `platform::irRead`.
/// @card IrService.png
class IrService : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Service; }

    void defineControls() override {
        controls_.addPin("pin", pin_);
        controls_.addSelect("learn", learn_, kLearnOptions, kActionCount + 1);
        for (uint8_t i = 0; i < kActionCount; i++) {
            // The learned code for this action, shown next to it. A Text control (persistable),
            // rendered read-only: the learn flow + persistence write it, the user doesn't type it —
            // the same "persist but tooling-set" pattern as SystemModule.deviceModel. (ReadOnly is
            // display-only and NOT persisted, so it would lose the binding on reboot; Text persists.)
            // No UI button per action: the remote drives the action once learned, so a button would
            // duplicate that — the `learn` select + these read-outs are the whole interface.
            controls_.addText(kActions[i].codeCtrl, codeStr_[i], sizeof(codeStr_[i]));
            controls_.setReadOnly(controls_.count() - 1, true);
        }
        // No "last code" control — a received code shows in the status line ("received 0x…" /
        // "learned … = 0x…"), so a separate read-out would duplicate it.
        MoonModule::defineControls();
    }

    // Setup state so the module says whether it can receive (a pin is required). Re-run on any
    // rebuild so setting the pin updates the status live. Also rebuild the fast uint32 lookup from
    // the persisted hex strings — persistence restores the codeStr_ buffers (Text controls), so
    // parse each back into learnedCode_ here (post-load) so a learned binding survives a reboot.
    /// Pure build (see MoonModule::prepare): rebuild the fast uint32 lookup from the persisted
    /// hex strings (persistence restores the codeStr_ Text controls, so parse each back post-load so a
    /// learned binding survives a reboot) and report readiness. The IR RX channel is opened lazily in
    /// tick()/irRead when enabled; the release lives in release(), which applyState() routes a
    /// disabled service to.
    void prepare() override {
        for (uint8_t i = 0; i < kActionCount; i++)
            learnedCode_[i] = codeStr_[i][0] ? std::strtoul(codeStr_[i], nullptr, 0) : 0;
        reportReady();
    }

    /// Release the IR RX channel (opened lazily in loop) so its pin is free for another module.
    /// applyState() calls this when the service, or a parent, is disabled; re-acquire is lazy on enable.
    void release() override { platform::irStop(); MoonModule::release(); }

    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "pin") == 0) reportReady();
        else if (std::strcmp(controlName, "learn") == 0) {
            if (learn_ != 0) setStatus("learning: press a remote button", Severity::Status);
            else reportReady();
        }
    }

    void tick() override {
        if (pin_ < 0) return;
        uint32_t code = 0;
        if (platform::irRead(static_cast<uint16_t>(pin_), code)) processCode(code);
    }

    /// The last decoded IR code (0 = none yet). The seam a future LightsControl consumes.
    uint32_t latestCode() const { return lastCode_; }

    /// Feed a decoded code as if it arrived from the receiver — the entry the host unit tests
    /// drive (platform::irRead is a stub on desktop). Mirrors DevicesModule::injectPacketForTest.
    void injectCodeForTest(uint32_t code) { processCode(code); }

private:
    // How an action changes its target control: a relative nudge (clamped to the control's bounds)
    // or a boolean toggle (read the current value, write its inverse). A toggle can't be expressed
    // as a delta — +1 on a 0/1 control clamps 0→1 but leaves 1→1 — so it's its own kind.
    enum class ActionKind : uint8_t { Delta, Toggle };
    // One action: a UI button + a learnable remote code, changing (module, control). Adding an
    // action is one row here. `delta` is used by Delta actions; Toggle ignores it.
    struct Action {
        const char* button;    // the UI button name
        const char* codeCtrl;  // the readonly control showing this action's learned code
        const char* module;
        const char* control;
        int delta;             // Delta actions: the signed nudge; Toggle actions: unused (0)
        ActionKind kind;
    };
    static constexpr Action kActions[] = {
        {"on/off",          "code on/off",          "Drivers", "on",         0,   ActionKind::Toggle},
        {"brightness up",   "code brightness up",   "Drivers", "brightness", +16, ActionKind::Delta},
        {"brightness down", "code brightness down", "Drivers", "brightness", -16, ActionKind::Delta},
        {"palette next",    "code palette next",    "Drivers", "palette",    +1,  ActionKind::Delta},
        {"palette prev",    "code palette prev",    "Drivers", "palette",    -1,  ActionKind::Delta},
    };
    static constexpr uint8_t kActionCount = sizeof(kActions) / sizeof(kActions[0]);
    // learn select: index 0 = off, 1..N = arm learning for kActions[index-1].
    static constexpr const char* kLearnOptions[] = {
        "off", "on/off", "brightness up", "brightness down", "palette next", "palette prev",
    };

    // Handle a decoded code: in learn mode bind it to the armed action (and persist); otherwise
    // look it up and run the bound action, or report it as unassigned. The shared entry for a real
    // receive (loop) and a test injection.
    void processCode(uint32_t code) {
        lastCode_ = code;   // the raw frame; surfaced via latestCode() + the status line below

        if (learn_ != 0) {
            const uint8_t idx = learn_ - 1;   // learn select is 1-based (0 = off)
            learnedCode_[idx] = code;
            std::snprintf(codeStr_[idx], sizeof(codeStr_[idx]), "0x%08lX",
                          static_cast<unsigned long>(code));
            std::snprintf(statusBuf_, sizeof(statusBuf_), "learned %s = 0x%08lX",
                          kActions[idx].button, static_cast<unsigned long>(code));
            setStatus(statusBuf_);
            learn_ = 0;
            // Persist the new binding: markDirty flags the subtree, noteDirty stamps the debounce
            // timer tick1s watches. A bound code is written straight to codeStr_ here (not via
            // setControl), so it must schedule the save itself — else the binding could be lost
            // before an unrelated save happens to run.
            markDirty();
            FilesystemModule::noteDirty();
            return;
        }
        for (uint8_t i = 0; i < kActionCount; i++) {
            if (learnedCode_[i] != 0 && learnedCode_[i] == code) { runAction(kActions[i]); return; }
        }
        std::snprintf(statusBuf_, sizeof(statusBuf_), "received 0x%08lX (unassigned)",
                      static_cast<unsigned long>(code));
        setStatus(statusBuf_);   // status only — nothing persistent changed, so no dirty mark
    }

    // Read the target control's current value + bounds, apply the clamped delta, set it through the
    // shared primitive — the same path the UI slider takes, so the change rebuilds the correction /
    // active palette and persists identically. Reports what it changed.
    void runAction(const Action& a) {
        Scheduler* sched = Scheduler::instance();
        if (!sched) return;
        MoonModule* target = sched->firstByName(a.module);
        if (!target) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%s: no %s module", a.button, a.module);
            setStatus(statusBuf_, Severity::Warning);
            markDirty();
            return;
        }
        auto& ctrls = target->controls();
        for (uint8_t i = 0; i < ctrls.count(); i++) {
            auto& c = ctrls[i];
            if (std::strcmp(c.name, a.control) != 0) continue;
            if (a.kind == ActionKind::Toggle) {
                // Read the current bool (stored as a 1-byte value) and write its inverse.
                const bool next = controlIntValue(c) == 0;
                sched->setControl(a.module, a.control, next ? "{\"value\":true}" : "{\"value\":false}");
                std::snprintf(statusBuf_, sizeof(statusBuf_), "%s.%s → %s",
                              a.module, a.control, next ? "on" : "off");
                setStatus(statusBuf_);
                markDirty();
                return;
            }
            int next = controlIntValue(c) + a.delta;
            if (next < c.min) next = c.min;
            if (next > c.max) next = c.max;
            char valueJson[32];
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", next);
            sched->setControl(a.module, a.control, valueJson);
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%s.%s → %d", a.module, a.control, next);
            setStatus(statusBuf_);
            markDirty();
            return;
        }
    }

    // Report setup readiness (static messages → no format buffer needed).
    // Report the TRUE receive state, not just "a pin is set": a pin can be set while the RX channel
    // can't bind (a busy pin, a bad GPIO), so "pin set" alone would say ready on a dead peripheral.
    // So open-or-confirm the channel and report what actually happened:
    //   pin unset        → warn "set pin to receive"
    //   channel bound    → "ready" (channel armed; a keypress then shows "received 0x…")
    //   channel won't open → error, so a genuinely-broken pin is visible, not masked as ready.
    // Called from prepare() (cold path) and on a pin change, so the open cost isn't on the hot path.
    void reportReady() {
        if (pin_ < 0) { setStatus("set pin to receive", Severity::Warning); return; }
        if (platform::irChannelReady(static_cast<uint16_t>(pin_))) setStatus("ready");
        else setStatus("IR channel failed to open — pin busy or invalid?", Severity::Error);
    }

    // A 1-byte control (Uint8 / Select / Bool) read as int via its descriptor pointer — covers every
    // kActions target: the brightness Uint8, the palette Select, and the on/off Bool (a bool is a
    // 1-byte object, so reading it through the same uint8_t* yields 0/1 for the Toggle action).
    static int controlIntValue(const ControlDescriptor& c) {
        return c.ptr ? *static_cast<const uint8_t*>(c.ptr) : 0;
    }

    int8_t   pin_ = -1;                        // IR receiver GPIO; -1 until a board/user sets it
    uint8_t  learn_ = 0;                        // learn select: 0=off, 1..N=arm kActions[idx-1]
    uint32_t learnedCode_[kActionCount] = {};  // bound code per action (0 = unbound)
    char     codeStr_[kActionCount][12] = {};  // hex read-out per learned code (persisted control)
    uint32_t lastCode_ = 0;                     // last decoded frame (0 = none)
    char     statusBuf_[48] = "";              // storage for dynamic setStatus() text
};

} // namespace mm
