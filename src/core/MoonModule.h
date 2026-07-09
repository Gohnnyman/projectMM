#pragma once

#include "core/Control.h"
#include "platform/platform.h"

#include <cstring>

namespace mm {

/// A module's role for type identification (no RTTI needed) and for the UI's generic rendering.
/// Service is a user-added capability module in the `Services` container that bridges to the
/// outside world — hardware or network — and is user-add/deletable (the firmware is the same
/// whether or not the device has the capability wired). It covers both readers and writers:
/// gyro/IMU + mic/line-in (in), relay/GPIO + Home Assistant push (out), and modules that do both.
/// Read-vs-write is NOT a role distinction — direction is a per-module decision, not a role split —
/// so one role spans the category, justified by that named roster, not one member (core grows
/// slower than the domain, see CLAUDE.md). Services is the core-domain twin of the light domain's
/// `Layers`/`Drivers`: a top-level container of user-added children of one role.
enum class ModuleRole : uint8_t { Generic, Effect, Modifier, Driver, Layout, Layer, Service };

/// Lowercase role name for JSON/API output. Single source of truth so the role
/// string can't drift between /api/state and /api/types.
inline const char* roleName(ModuleRole role) {
    switch (role) {
        case ModuleRole::Effect:     return "effect";
        case ModuleRole::Modifier:   return "modifier";
        case ModuleRole::Driver:     return "driver";
        case ModuleRole::Layout:     return "layout";
        case ModuleRole::Layer:      return "layer";
        case ModuleRole::Service:    return "service";
        default:                     return "generic";
    }
}

/// The base class for everything in the system — effects, modifiers, layouts, drivers, and
/// system services all inherit from MoonModule. It is the one deliberate class hierarchy: a
/// single virtual-dispatch boundary and shallow subclasses, so the UI can render any module
/// generically with zero per-module UI code. The design goal is the smallest possible base:
/// zero bytes of instance overhead beyond the vtable pointer and control variables (the type
/// name lives in flash, not per instance), because on an ESP32 without PSRAM dozens of modules
/// load at once and every byte counts. Field order is grouped 8/4/2/1-byte to minimise padding.
///
/// **Lifecycle.** `setup()` / `teardown()` bracket the module's life; `loop()` / `loop20ms()` /
/// `loop1s()` are the three tick rates the Scheduler paces. Two build hooks sit apart from
/// `setup()`: `onBuildControls()` holds every `addX()` call and is idempotent + re-runnable (so a
/// Select changing the visible control set rebuilds cleanly), and `onBuildState()` is the single
/// derived-state hook (buffers, LUTs, the module's heap-byte report), reached at setup and via
/// `Scheduler::buildState()` whenever a control that changes physical dimensions fires
/// `controlChangeTriggersBuildState()`. This build sweep is what makes every config change apply
/// live, with no reboot. Controls bind by reference, so persisted values overlay the bound
/// variables before any `setup()` runs.
///
/// **Parent/child.** Modules form a tree — parent/child only, no arbitrary DAG. A dynamic
/// children array plus `addChild()` / `removeChild()` / `replaceChildAt()` / `moveChildTo()`
/// live once in this base (containers do not override them); the array starts empty (zero
/// allocation for leaf modules) and grows on demand. Children are distinguished by `role()`, and
/// a container filters by role at the call site (a Layer ticks only Effects, not Modifiers). Two
/// virtuals keep UI tree-mutation policy on the device: `acceptsChildRoles()` (what a parent
/// offers in "+ add child") and `userEditable()` (whether the user may delete/replace this
/// module). Parents own their children's lifecycle and propagate every hook down — only
/// top-level modules register with the Scheduler.
///
/// **Runtime add/remove lifecycle contract.** When a child is added or removed *after* the
/// parent's own setup has run, the caller drives the child's lifecycle: adding at runtime →
/// call `setup()` → `onBuildControls()` → `onBuildState()` on the new child; removing at runtime
/// → call `teardown()` on the child before removing it. A child added *before* the parent's
/// setup needs none of this — the parent's `setup()` propagates down to it.
///
/// **Enabled.** Every module has an `enabled` flag (default true), toggled from the UI card
/// header and via `POST /api/control`. The Scheduler always calls the three loop hooks regardless
/// of `enabled`; each module decides what "disabled" means — a rendering module early-returns
/// (its buffer freezes) while a system module ignores the flag (`respectsEnabled()` false) so the
/// user can't lock themselves out. `onEnabled(bool)` fires once per transition for one-shot
/// start/stop work, instead of polling `enabled()` in the hot path.
///
/// **Self-reporting.** Each module reports its own footprint and cost so the UI shows per-module
/// visibility at any depth: `classSize()` (set once at registration via `register_type<T>()`, no
/// per-class boilerplate), `dynamicBytes()` (heap set by `onBuildState()`), and `loopTimeUs()`
/// (average microseconds per tick over a 1-second window; `publishTiming()` recurses the tree
/// every second — parents time their children, the Scheduler times top-level modules). tickTimeUs
/// is the primary performance metric; FPS is derived as `1000000 / tickTimeUs`. `setStatus(msg,
/// severity)` carries a short user-facing message (Status / Warning / Error → ℹ️ / ⚠️ / ❌); the
/// slot stores a pointer with no copy, so callers pass a flash literal or a module-owned buffer.
/// `markDirty()` marks state touched so FilesystemModule can persist the subtree after a debounce.
///
/// **Prior art:** MoonLight's Node
/// (https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Nodes.h) — a ~29-byte base + vtable
/// with no `std::string` members (fixed-size strings), `addControl()` binding to a class variable
/// by reference and storing a `uintptr_t`, and `classSize()` reporting the actual instance size.
class MoonModule {
public:
    // Allocate modules in PSRAM when available (ESP32)
    void* operator new(size_t size) { return platform::alloc(size); }
    void operator delete(void* ptr) noexcept { platform::free(ptr); }

    MoonModule() = default;
    virtual ~MoonModule() { delete[] children_; }

    MoonModule(const MoonModule&) = delete;
    MoonModule& operator=(const MoonModule&) = delete;
    MoonModule(MoonModule&&) = delete;
    MoonModule& operator=(MoonModule&&) = delete;

    /// Default lifecycle propagates to children. Override to add container-specific logic.
    ///
    /// For loop / loop20ms / loop1s, the default ticks every child that passes the same
    /// enabled gate the Scheduler applies to top-level modules (!respectsEnabled() ||
    /// enabled() — tick when the module opted out of the gate, otherwise honour
    /// enabled()), and accumulates per-child timing the same way Scheduler does. Leaf
    /// modules (childCount_ == 0) pay one predicted-not-taken branch — sub-nanosecond.
    ///
    /// Override + chain convention for loop callbacks: parent work runs first, then
    /// chain to base to tick children (option A — parent prepares, children consume).
    /// Override + chain for setup runs the other way (chain to base first so children
    /// are initialised before the parent depends on them). teardown's base default
    /// reverse-iterates children; override and chain late so the parent shuts down its
    /// own state first.
    virtual void setup() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->setup(); }
    virtual void loop() { tickChildren(&MoonModule::loop); }
    virtual void loop20ms() { tickChildren(&MoonModule::loop20ms); }
    virtual void loop1s() { tickChildren(&MoonModule::loop1s); }
    virtual void teardown() { for (uint8_t i = childCount_; i > 0; i--) children_[i-1]->teardown(); }

    /// The single orchestration point for the resource lifecycle — the enabled decision lives HERE,
    /// in core, so a catalog module's onBuildState() is pure "build my state" with no enabled() check.
    /// Per node: build it when effectively-enabled (acquire), else tear it down (release). The
    /// recursion is owned here, not in onBuildState(): the enabled branch builds this node then
    /// recurses into each child's applyState() (parent-first — a Layer builds its LUT before its
    /// effects build against it), so each child is routed by ITS OWN effective-enabled — a disabled
    /// child under an enabled parent is torn down, not built. The disabled branch calls teardown(),
    /// which already recurses to the whole subtree (reverse order), so applyState() does not recurse
    /// again there. Reached from Scheduler::buildState() and the boot sweep; the disable toggle runs
    /// it too, so acquire-on-enable and release-on-disable are this one path.
    void applyState() {
        if (effectivelyEnabled()) {
            onBuildState();
            for (uint8_t i = 0; i < childCount_; i++) children_[i]->applyState();
        } else {
            teardown();   // recurses to children itself (reverse order)
        }
    }

    /// Called once when the enabled flag flips (the Scheduler runs a full buildState() right
    /// after, so onBuildState() re-derives state on the same toggle). Default no-op. Override
    /// ONLY for a genuine edge-triggered one-shot that is NOT "rebuild derived state" — e.g. a
    /// clean protocol DISCONNECT (MqttModule sends a courtesy MQTT frame + resets its backoff).
    /// Resource acquire/release does NOT belong here: buffers and peripherals are (re)built and
    /// freed in onBuildState(), keyed on effectivelyEnabled(), so a disabled module — or a child
    /// of a disabled parent — releases everything through the one sweep. The scheduler always
    /// invokes loop()/loop20ms()/loop1s() only while (effectively) enabled, so a disabled
    /// module's loop never runs.
    virtual void onEnabled(bool /*newEnabled*/) {}

    /// Cheap per-control reaction, tier 1 of the three-tier control-change split (mirrors
    /// MoonLight's onUpdate / requestMappings / onSizeChanged; see architecture.md § Rebuild
    /// propagation). Runs on EVERY change — recompute a small LUT, re-bind a socket, etc.
    /// The other tiers are `controlChangeTriggersBuildState()` (tier 2, the gate for the
    /// pipeline-wide sweep, true only for controls that change physical dimensions / mapping
    /// shape) and `onBuildState()` (tier 3, build derived state, reached via
    /// `Scheduler::buildState()` when tier 2 returns true).
    ///
    /// Called after a control's value is written from the UI/API. `controlName` is the
    /// changed control's name (stable; points into the descriptor). Default no-op.
    virtual void onUpdate(const char* /*controlName*/) {}

    /// Whether a value change to one of this module's controls triggers the pipeline-wide
    /// onBuildState() sweep. Default false — most controls are values read in the hot
    /// path that need no realloc. Layout and Modifier override to return true (their
    /// controls change physical dimensions / LUT shape). Most overriders ignore the name
    /// and return true for every control they expose.
    virtual bool controlChangeTriggersBuildState(const char* /*controlName*/) const { return false; }

    /// onBuildControls MUST be idempotent and pure: only `controls_.clear()` + `controls_.addX()`.
    /// No platform queries, no I/O, no allocations. HttpServerModule calls it again whenever a
    /// Select control changes the visible control set, so a second invocation must produce
    /// exactly the same result for unchanged inputs. Conditional branches may depend on any
    /// member variable.
    virtual void onBuildControls() { for (uint8_t i = 0; i < childCount_; i++) children_[i]->onBuildControls(); }

    /// Non-virtual helper: clear-and-rebuild for this module AND its descendants. The default
    /// onBuildControls cascades into children, so we must also clear their control lists first;
    /// otherwise the recursive append would duplicate every child's controls. Used after Select
    /// changes (in HttpServerModule) and anywhere else the conditional control set needs
    /// re-evaluation.
    void rebuildControls() {
        clearControlsRecursive();
        onBuildControls();
    }
    void clearControlsRecursive() {
        controls_.clear();
        for (uint8_t i = 0; i < childCount_; i++) children_[i]->clearControlsRecursive();
    }

    /// Tier-3 of the control-change split (see onUpdate above): the module (re)allocates
    /// / recomputes whatever derived state it owns — an effect's heap, a Layer's mapping
    /// LUT, the Drivers output buffer. Default propagates to children. Reached via
    /// Scheduler::buildState() (whole-tree) when a tier-2 gate returns true.
    ///
    /// **This is the sole resource-lifecycle gate.** Build derived state (buffers AND
    /// peripherals) for the current controls and `effectivelyEnabled()`. When
    /// `!effectivelyEnabled()` — the module is disabled, or a parent is — build the **empty**
    /// state: release every buffer and peripheral it holds, so a disabled subtree frees
    /// everything. The Scheduler runs a full buildState() right after any enable/disable toggle,
    /// so this hook (not onEnabled) is where acquire-on-enable and release-on-disable both
    /// happen — one path, boot and runtime alike. A module therefore never checks enabled()
    /// in setup(): setup() is enabled-independent one-time wiring; the acquire lives here.
    ///
    /// Same role as JUCE's `prepareToPlay` or UIKit's `layoutSubviews` — a framework-driven
    /// "set up your derived state for the current config" hook with a no-op default. The verb
    /// is "build" (not "rebuild") on purpose: the operation is idempotent and history-agnostic
    /// — it builds the correct state from current values whether or not it ran before, so boot
    /// and a later control change are the same call, not "build" then "rebuild". The whole
    /// chain shares the verb: controlChangeTriggersBuildState → Scheduler::buildState() →
    /// onBuildState(). Mirrors the onBuildControls precedent (build the surface vs build the
    /// state) and the canonical hooks (prepareToPlay/layoutSubviews never say "re" either).
    ///
    /// Intentionally coarse: each module builds its whole derived state, and the Scheduler
    /// sweeps the whole tree. That's fine because structural changes are rare and the builds
    /// are idempotent (FireEffect only reallocs when count != heatCount_). If a module
    /// ever grows two independently-buildable aspects where one control touches only one of
    /// them (`width` reshapes a LUT but `gamma` only re-tints a cache, both expensive),
    /// the cheapest upgrade is to forward the changed control name —
    /// `onBuildState(const char* changedControl)` — and branch inside. The tier-2 gate
    /// (controlChangeTriggersBuildState) already carries the name, so it's a one-parameter change.
    /// Don't add it pre-emptively; no module needs the distinction today.
    ///
    /// **A leaf operation — builds THIS node only.** The tree recursion + the enabled decision live
    /// in applyState() (core), not here: applyState() calls this when a node is effectively-enabled,
    /// then recurses into children. So an override builds its own state and does NOT chain to a base
    /// recursion (there is none). A container that must prepare something for its children before they
    /// build (Drivers hands each driver the shared buffer, Layer builds its LUT) does that work in its
    /// own onBuildState() body — applyState() then visits the children next, so they build against it.
    virtual void onBuildState() {}

    /// Read this module's first output light as RGB into out[3], returning true if it has
    /// one. Domain-neutral seam (core declares it, the output-owning module overrides):
    /// the WLED-compatibility shim uses it to tint the app's device card with the live
    /// first-LED colour. Default: no output → false.
    virtual bool firstOutputRgb(uint8_t /*out*/[3]) const { return false; }

    const char* name() const { return name_; }
    void setName(const char* n) {
        if (!n) { name_[0] = 0; return; }
        size_t len = std::strlen(n);
        if (len >= sizeof(name_)) len = sizeof(name_) - 1;
        std::memcpy(name_, n, len);
        name_[len] = 0;
    }

    /// typeName is the stable factory key (such as "NoiseEffect"), set once by ModuleFactory.
    /// Stored as `const char*` pointing at the factory's string literal — zero per-instance
    /// copy, lives in flash. Caller must pass a string with static lifetime (string literal
    /// or factory-owned storage); do not pass stack-local or temporary buffers.
    /// Distinct from name() which is a per-instance human label and may be overridden
    /// ("Noise" instead of "NoiseEffect"); typeName() stays the factory key.
    const char* typeName() const { return typeName_; }
    void setTypeName(const char* tn) { typeName_ = tn ? tn : ""; }

    bool enabled() const { return enabled_; }
    void setEnabled(bool e) {
        if (enabled_ == e) return;
        enabled_ = e;
        onEnabled(e);
    }

    /// Whether the Scheduler should honor `enabled()` for this module's loop callbacks.
    /// Default true — disabled modules don't have their loop fns called. Override to
    /// return false for system modules that must keep running regardless (HttpServer,
    /// Network, Filesystem) so the user can re-enable other modules through them.
    virtual bool respectsEnabled() const { return true; }

    /// True unless this module — or an ancestor that respects the enabled flag — is disabled.
    /// The single predicate the resource-lifecycle gate keys off: `onBuildState()` acquires
    /// when this is true and releases the module's buffers/peripherals when it is false, so a
    /// disabled parent's whole subtree releases (the disable cascade). A `respectsEnabled()==false`
    /// ancestor is neutral — always-on, it never forces a child on or off. Off the hot path
    /// (called from the cold onBuildState sweep, not loop()); an inherited/computed property in
    /// the shape of a scene-graph `worldVisible` or a CSS cascade — walk to the root, cheap.
    bool effectivelyEnabled() const {
        for (const MoonModule* m = this; m; m = m->parent())
            if (m->respectsEnabled() && !m->enabled()) return false;
        return true;
    }

    /// Whether this module appears in the UI (/api/state → nav card). Default true. A pure engine
    /// with no user-facing controls returns false so it isn't shown as an empty card — e.g.
    /// FilesystemModule (the persistence engine; its one status readout lives on FileManagerModule)
    /// and HttpServerModule (the web server itself). The state serializer skips any module whose
    /// appearsInUi() is false.
    virtual bool appearsInUi() const { return true; }

    /// Dirty flag — set by HttpServerModule when a control changes. FilesystemModule (or any
    /// consumer interested in "this module's state has been touched") observes it in loop1s()
    /// and clears it after persisting.
    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void clearDirty() { dirty_ = false; }

    MoonModule* parent() const { return parent_; }
    void setParent(MoonModule* p) { parent_ = p; }

    /// Marks this module as wired-by-code rather than wired-by-persistence. The
    /// FilesystemModule's applyNode trim loop preserves code-wired children even
    /// when the on-disk file doesn't describe them — the upgrade-day case where
    /// a new firmware revision adds a code-created child (ImprovProvisioning
    /// as a child of NetworkModule) whose existence the device's saved Network.json
    /// predates. Without this flag the child would get trimmed on every boot.
    ///
    /// Convention: only main.cpp's boot wiring calls markWiredByCode(). Children
    /// added via the HTTP add-module API or recreated by applyNode's factory call
    /// stay unmarked — those are user/persistence-driven and should follow the
    /// file's tree shape exactly.
    void markWiredByCode() { wiredByCode_ = true; }
    bool isWiredByCode() const { return wiredByCode_; }

    ControlList& controls() { return controls_; }
    const ControlList& controls() const { return controls_; }

    /// Read one of this module's controls by name generically (no per-consumer control scan). Returns
    /// `dflt` when the control is absent or the wrong type. The domain-neutral way for another module
    /// (HttpServerModule's WLED shim, MqttModule) to read a target's control without a light include
    /// or a hand-rolled scan — one implementation, so the absent-control default can't disagree
    /// between callers.
    bool readBool(const char* name, bool dflt) const {
        for (uint8_t i = 0; i < controls_.count(); i++) {
            const ControlDescriptor& c = controls_[i];
            if (c.ptr && c.type == ControlType::Bool && std::strcmp(c.name, name) == 0)
                return *static_cast<const bool*>(c.ptr);
        }
        return dflt;
    }
    uint8_t readUint8(const char* name, uint8_t dflt) const {
        for (uint8_t i = 0; i < controls_.count(); i++) {
            const ControlDescriptor& c = controls_[i];
            // All three types back onto a `uint8_t*`: Uint8 (brightness), Select (a dropdown stored as
            // an option index), and Palette (a palette-picker stored as a builtins-array index). The
            // Palette omission here silently failed driversPalette()/similar callers to dflt=0,
            // which then made HttpServerModule's WLED shim report seg[0].col as Rainbow's
            // representative colour (white) regardless of the actually-picked palette — the "the HA
            // wheel snaps back to the centre and the card renders white" bug pinned on the bench.
            if (c.ptr && std::strcmp(c.name, name) == 0 &&
                (c.type == ControlType::Uint8 || c.type == ControlType::Select ||
                 c.type == ControlType::Palette))
                return *static_cast<const uint8_t*>(c.ptr);
        }
        return dflt;
    }

    /// Role for type identification (no RTTI needed).
    virtual ModuleRole role() const { return ModuleRole::Generic; }

    /// Curated emoji tags for the module picker's chip filter — extras beyond the
    /// role chip (which the UI derives from role() on its own). A short string of
    /// emoji, such as "🔥" or "🌊💧". Default "" — most modules add nothing. The
    /// return value is a flash string literal; no per-instance RAM cost.
    virtual const char* tags() const { return ""; }

    /// Comma-separated role names this module accepts as user-added children
    /// ("effect,modifier"). "" = accepts none — the default, covering
    /// leaf modules and fixed-shape containers. A container overrides this to
    /// tell the UI's "+ add child" picker what to offer. Comma-separated
    /// string (not a bitmask) so it serialises straight into /api/types and a
    /// multi-role parent (Layer → "effect,modifier") needs no enum-set type.
    /// This is what makes the UI domain-neutral: it reads the accepted roles
    /// from here instead of hardcoding which module types are containers.
    /// Like tags(), overrides MUST return static-lifetime storage (a string
    /// literal or static const char[]): ModuleFactory::registerType`<T>`() probes
    /// the type once and stores this pointer in the static type registry, so a
    /// pointer to a temporary or member buffer would dangle.
    virtual const char* acceptsChildRoles() const { return ""; }

    /// Whether the user may delete or replace this module from the UI. Default
    /// true — most user-added modules are freely editable. A load-bearing or
    /// code-wired child overrides to false (PreviewDriver, whose deletion
    /// would kill the live 3D preview). The flag lives on the CHILD because the
    /// child knows whether it's safe to remove; the parent only decides what
    /// can be added (acceptsChildRoles). Surfaced per-instance in /api/state.
    virtual bool userEditable() const { return true; }

    /// Generic children — grows on demand, only allocates during setup.
    bool addChild(MoonModule* child) {
        if (!child) return false;
        if (childCount_ == childCapacity_) {
            uint8_t newCap = childCapacity_ == 0 ? 4 : childCapacity_ * 2;
            auto** newArr = new MoonModule*[newCap];
            for (uint8_t i = 0; i < childCount_; i++) newArr[i] = children_[i];
            delete[] children_;
            children_ = newArr;
            childCapacity_ = newCap;
        }
        child->setParent(this);
        children_[childCount_++] = child;
        return true;
    }

    bool removeChild(MoonModule* child) {
        for (uint8_t i = 0; i < childCount_; i++) {
            if (children_[i] == child) {
                child->setParent(nullptr);
                for (uint8_t j = i; j + 1 < childCount_; j++) children_[j] = children_[j + 1];
                childCount_--;
                return true;
            }
        }
        return false;
    }

    /// Replace child at position i with fresh. Caller owns lifecycle of the removed
    /// (returned) child — teardown + delete. Returns nullptr if i is out of range.
    /// Used by FilesystemModule at load time to swap a child whose type differs from
    /// the persisted JSON; the caller tears down + Scheduler::deleteTree's the old child.
    MoonModule* replaceChildAt(uint8_t i, MoonModule* fresh) {
        if (i >= childCount_ || !fresh) return nullptr;
        MoonModule* old = children_[i];
        if (old) old->setParent(nullptr);
        fresh->setParent(this);
        children_[i] = fresh;
        return old;
    }

    /// Move child to absolute position newIndex (0..childCount-1). Intermediate siblings
    /// shift toward the vacated slot. Returns false if child isn't found, newIndex is out
    /// of range, or the move is a no-op (already at newIndex). Used by the UI reorder path;
    /// the caller's follow-up Scheduler::buildState() rebuilds any order-dependent LUT.
    bool moveChildTo(MoonModule* child, uint8_t newIndex) {
        if (newIndex >= childCount_) return false;
        for (uint8_t i = 0; i < childCount_; i++) {
            if (children_[i] != child) continue;
            if (i == newIndex) return false;  // no-op
            if (newIndex > i) {
                // Shift left to fill the gap
                for (uint8_t j = i; j < newIndex; j++) children_[j] = children_[j + 1];
            } else {
                // Shift right to make room
                for (uint8_t j = i; j > newIndex; j--) children_[j] = children_[j - 1];
            }
            children_[newIndex] = child;
            return true;
        }
        return false;
    }

    uint8_t childCount() const { return childCount_; }
    MoonModule* child(uint8_t i) const { return i < childCount_ ? children_[i] : nullptr; }

    /// Per-module memory reporting: classSize() is the instance size (set once at registration),
    /// dynamicBytes() the heap this module allocated (set by onBuildState()).
    size_t classSize() const { return classSize_ > 0 ? classSize_ : sizeof(MoonModule); }
    void setClassSize(size_t s) { classSize_ = s; }
    size_t dynamicBytes() const { return dynamicBytes_; }
    void setDynamicBytes(size_t b) { dynamicBytes_ = b; }

    /// Per-module status slot. A short user-facing message the module wants the
    /// user to see right now — NetworkModule writes "Eth: 192.168.1.210", Layer
    /// writes "buffer reduced — not enough memory". The pointer is owned by the
    /// caller (flash literal or a module-owned char buffer); the slot doesn't
    /// copy. `nullptr` = nothing to show. `severity` qualifies the message so the
    /// UI can pick the right emoji (Status ℹ️ neutral info, Warning ⚠️ silent
    /// degradation, Error ❌ something failed). Emitted in /api/state + /api/system
    /// only when set.
    enum class Severity : uint8_t {
        Status,   ///< ℹ️ neutral info, current state ("connected")
        Warning,  ///< ⚠️ silent degradation ("buffer reduced")
        Error,    ///< ❌ something failed ("WiFi auth failed")
    };
    const char* status() const { return status_; }
    Severity severity() const { return severity_; }
    void setStatus(const char* msg, Severity sev = Severity::Status) {
        status_ = msg;
        severity_ = sev;
    }
    void clearStatus() { status_ = nullptr; severity_ = Severity::Status; }

    /// Per-module timing: parents time children, Scheduler times top-level modules.
    /// loopTimeUs() is the average microseconds per tick over the last 1-second window.
    uint32_t loopTimeUs() const { return loopTimeUs_; }
    void addAccumUs(uint32_t us) { accumUs_ += us; }

    /// Called by Scheduler every ~1 second. Averages the accumulated tick time and recurses
    /// into children.
    void publishTiming(uint32_t frameCount) {
        loopTimeUs_ = frameCount > 0 ? accumUs_ / frameCount : 0;
        accumUs_ = 0;
        for (uint8_t i = 0; i < childCount_; i++) {
            children_[i]->publishTiming(frameCount);
        }
    }

protected:
    ControlList controls_;

    /// Shared body for the loop / loop20ms / loop1s base defaults. Iterates children,
    /// gates each by the same rule the Scheduler applies to top-level modules
    /// (!respectsEnabled() || enabled() — children that opted out of the enabled
    /// gate keep ticking; the rest tick only when enabled), dispatches the same
    /// callback, and accumulates per-child timing. Pulled out so the three base
    /// defaults stay one-liners and the gating + timing rule lives in one place.
    void tickChildren(void (MoonModule::*fn)()) {
        for (uint8_t i = 0; i < childCount_; i++) {
            MoonModule* c = children_[i];
            if (!c->respectsEnabled() || c->enabled()) {
                uint32_t start = platform::micros();
                (c->*fn)();
                c->addAccumUs(platform::micros() - start);
            }
        }
    }

private:
    // Display name buffer. Sized to fit the longest stripped name with headroom:
    // ModuleFactory's displayNameFor strips the role-noun suffix so the longest
    // names today are 13 chars ("GlowParticles", "PlasmaPalette") + null. char[16]
    // leaves a few bytes of room for future modules. Names longer than this are
    // truncated by setName(). 8 bytes saved per module vs the previous char[24]
    // (~240 bytes total RAM on a typical tree).
    char name_[16] = {};
    const char* typeName_ = "";  // points into flash (factory string literal); see setTypeName comment
    bool enabled_ = true;
    bool dirty_ = false;
    bool wiredByCode_ = false;
    MoonModule* parent_ = nullptr;
    MoonModule** children_ = nullptr;
    uint8_t childCount_ = 0;
    uint8_t childCapacity_ = 0;
    size_t classSize_ = 0;
    size_t dynamicBytes_ = 0;
    const char* status_ = nullptr;  // see status() / setStatus()
    Severity severity_ = Severity::Status;
    uint32_t loopTimeUs_ = 0;
    uint32_t accumUs_ = 0;
};

} // namespace mm
