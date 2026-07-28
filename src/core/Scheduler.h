#pragma once

#include "core/MoonModule.h"

#include <array>
#include <cstdint>

namespace mm {

/// Domain-neutral orchestrator that owns the top-level module list, runs the 4-phase
/// boot, drives the per-tick loop callbacks, and provides tree-walk utilities (delete,
/// name-uniquify).
///
/// **Boot phases:** see the `setup()` comment in Scheduler.cpp.
///
/// **Tick:** gates each top-level module by `enabled()` / `respectsEnabled()` and
/// dispatches `tick` / `tick20ms` / `tick1s`. A per-second window averages the tick
/// time and publishes each module's loop time.
///
/// **Loop rates:** three cadences cover every module. `tick()` is the hot path for
/// effects and drivers, called every iteration — the Scheduler handles pacing (yielding
/// to other tasks between iterations via `taskYIELD()` on ESP32, an optional sleep on
/// desktop). `tick20ms()` runs every ~20 ms for UI updates, control reads, and network
/// polling. `tick1s()` runs every ~1 second for diagnostics, reconnects, and
/// housekeeping. Not every module needs `tick()`: system modules (HTTP, WiFi) use
/// `tick20ms()` or `tick1s()` only.
///
/// **Timing:** effects animate off a synchronized clock (millis from the platform), so
/// the visual speed is frame-rate independent — the same at 30 fps and 60 fps. For
/// multi-device sync a leader synchronizes this clock across devices; no frame counter
/// is needed.
///
/// **Execution model:** `tick()` runs every top-level module inline, in one loop, in
/// declared order; each module then drives its own children. There is no per-module
/// task and no core-affinity field — a single render loop, not a task-per-module fan-out.
///
/// **Module ordering:** child modules run in their declared order within the parent,
/// and top-level modules also run in declared order. The UI supports reordering, backed
/// by the scheduler. Relationships are parent/child only — there is no arbitrary
/// dependency graph.
///
/// **Prior art:** MoonLight's `effectTask` / `svelteTask` — two FreeRTOS tasks (effects
/// on core 1, system/drivers on core 0) with a per-node `tick()` every frame and a
/// `tick20ms()` for slow updates.
///
/// This is the `.h` interface; bodies live in Scheduler.cpp.
class Scheduler {
public:
    /// Function-pointer hook invoked between phase 1 (`defineControls`) and phase 3
    /// (`setup`). Used by FilesystemModule to overlay persisted control values onto bound
    /// variables before modules' setup() runs. Scheduler stays independent of
    /// FilesystemModule's type (no circular include). Wired in via setLoadAllHook from
    /// main.cpp.
    using LoadAllFn = void(*)(Scheduler*);
    void setLoadAllHook(LoadAllFn fn) { loadAllHook_ = fn; }

    /// Hook invoked after a control mutation so the persistence layer can schedule a
    /// debounced save (FilesystemModule::noteDirty). Same decoupling as setLoadAllHook —
    /// Scheduler stays independent of FilesystemModule's type. No-op if unset.
    using NoteDirtyFn = void(*)();
    void setNoteDirtyHook(NoteDirtyFn fn) { noteDirtyHook_ = fn; }

    void addModule(MoonModule* mod);
    void setup();
    void tick() MM_NONBLOCKING;
    void release();

    uint32_t elapsed() const;
    void prepareTree();

    uint32_t tickTimeUs() const { return tickTimeUs_; }
    uint32_t fps() const { return tickTimeUs_ > 0 ? 1000000 / tickTimeUs_ : 0; }
    uint8_t moduleCount() const { return moduleCount_; }
    MoonModule* module(uint8_t i) const { return i < moduleCount_ ? modules_[i] : nullptr; }

    static void deleteTree(MoonModule* mod);

    /// Ensure `mod`'s name is tree-globally unique. If something else already uses
    /// the same name, suffix with " 2", " 3", … until unique. Caller must have
    /// placed `mod` in the tree already (otherwise the lookup wouldn't see it).
    /// See Scheduler.cpp for the why and the name-length cap.
    void ensureUniqueName(MoonModule* mod);

    /// Walk the whole tree and disambiguate every duplicated name. First
    /// occurrence keeps its name; later ones get " 2", " 3", … suffixes.
    /// Cold-path: called once after persistence load in setup().
    void deduplicateNamesInTree();

    /// First module in tree-walk order with this name, or nullptr if none.
    MoonModule* firstByName(const char* name);

    /// The single live Scheduler, or nullptr before setup() / after release(). Mirrors
    /// FilesystemModule::instance_ — the one Scheduler is statically reachable so a module
    /// created by the factory (IrService) can call setControl() without a per-module injection.
    static Scheduler* instance() { return instance_; }

    /// Outcome of setControl — the generic control-set primitive's result. Transport
    /// layers map these to their own status (HTTP → 404 / 400 / 409, …).
    enum class SetControlResult : uint8_t {
        Ok,
        ModuleNotFound,   ///< no module with that name in the tree
        ControlNotFound,  ///< module exists but has no such control
        OutOfRange,       ///< numeric value outside the control's bounds
        Malformed,        ///< value didn't parse
        ReadOnly,         ///< tried to write a display-only control
    };

    /// Set one control by (module name, control name) to a value, applying the full
    /// control-change reaction: parse+validate, rebuild the module's control list, fire
    /// onControlChanged, mark dirty for persistence, and prepareTree() when the control reshapes
    /// dims/mapping. `valueJson` is a small JSON object read for its "value" key
    /// (`{"value":128}`) — the same shape /api/control, Improv, and the WLED bridge send.
    /// This is THE domain-neutral way for any module (IR, buttons, network bridges) to
    /// drive another module's control: they compose against this one primitive instead of
    /// reaching into a target's internals. The special control name "enabled" toggles the
    /// module's enabled flag. Returns the outcome; a transport maps it to its status codes.
    SetControlResult setControl(const char* moduleName, const char* controlName,
                                const char* valueJson);

private:
    void walkAndEnsureUnique(MoonModule* mod);
    static MoonModule* firstInTree(MoonModule* mod, const char* name);

    static inline Scheduler* instance_ = nullptr;
    std::array<MoonModule*, 32> modules_{};
    uint8_t moduleCount_ = 0;
    LoadAllFn loadAllHook_ = nullptr;
    NoteDirtyFn noteDirtyHook_ = nullptr;
    uint32_t startTime_ = 0;
    uint32_t lastLoop20ms_ = 0;
    uint32_t lastLoop1s_ = 0;
    uint32_t tickTimeUs_ = 0;
    uint32_t tickAccumUs_ = 0;
    uint32_t frameCount_ = 0;        // frames in current 1-second window (for averaging)
    uint32_t lastTimingUpdate_ = 0;   // 1-second window start
};

// --- Drivers master-state reads, shared by every consumer that mirrors the light state (the WLED
// /json shim, MQTT). Domain-neutral: they name the `Drivers` module + its controls and read via the
// generic MoonModule::read* helpers, so the absent-control defaults are defined ONCE here instead of
// per consumer (a device with no `on` control still reads as on; brightness/palette default to 0). ---
inline bool driversOn(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readBool("on", true) : true;   // absent → on
}
inline uint8_t driversBrightness(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readUint8("brightness", 0) : 0;
}
inline uint8_t driversPalette(Scheduler* s) {
    MoonModule* d = s ? s->firstByName("Drivers") : nullptr;
    return d ? d->readUint8("palette", 0) : 0;
}

} // namespace mm
