#pragma once

// This is the .h interface. Bodies live in FilesystemModule.cpp — splitting them
// out keeps the every-time-it-edits-recompile cost off the rest of the tree.

#include "core/MoonModule.h"

#include <cstddef>
#include <cstdint>

namespace mm {

class Scheduler;
struct ControlDescriptor;

/// Control-list-driven JSON persistence — writes control values to flash so settings
/// survive a reboot. Always loaded, runs first in the scheduler so its load hook fires
/// before any other module's `setup()`
///
/// **Storage:** one flat JSON file per top-level MoonModule under
/// `/.config/<TypeName>.json` (the filename is `MoonModule::typeName()`). Children are
/// encoded positionally with an `<index>.` key prefix — no nested objects, no arrays,
/// loaded with the cheap first-match key helpers (parseString/Int/Bool). A control
/// whose *value* is structured (a List control's array of objects) is read back with
/// the recursive reader in `core/JsonUtil.h` via the control's own restore hook: the
/// file's top level stays flat, the structure lives inside one control's value string.
/// `ReadOnly` and `Progress` controls are never persisted — they are derived values,
/// not state.
///
/// **Structural reconciliation.** The `type` field per child drives reconciliation at
/// load time: when the JSON describes a child type at position N that differs from the
/// live tree's child at N, the loader factory-creates the JSON type, runs its
/// `onBuildControls()`, and swaps it into place. Children present in the live tree but
/// missing from the JSON are torn down; children in the JSON beyond the live tree's end
/// are appended. Phases 3+4 cascade into the reconciled tree, so newly-created children
/// are fully initialised like any other.
///
/// **Boot flow (Scheduler::setup, four phases):**
///   - phase 1 `onBuildControls()` — every module binds its full control set (incl. hidden)
///   - phase 2 `loadAllHook` — this module reads each file and overlays bound variables
///   - phase 2b `rebuildControls()` — re-runs onBuildControls so conditional hidden flags see the persisted values
///   - phase 3 `setup()` — modules' own init runs with persisted values in members
///   - phase 4 `onBuildState()` — buffers sized to final values
///
/// The Scheduler exposes `setLoadAllHook()` as a function pointer so it stays
/// independent of this module's type (no circular include); the hook is wired from
/// `setScheduler()`.
///
/// **Save flow.** HttpServerModule calls `markDirty()` + `noteDirty()` on every
/// successful mutation — control changes AND tree-shape changes (add / delete / move a
/// module marks the parent dirty so its file is rewritten with the new child set).
/// `noteDirty()` stamps `lastDirtyMs_`; `loop1s()` waits `DEBOUNCE_MS` (2 s) after the
/// last dirty mark, then walks the tree and serialises any subtree with a dirty
/// descendant to a flat JSON blob, written atomically (write to `.tmp`, then rename). A
/// subtree's dirty flag clears only after its write succeeds; a failed write leaves it
/// set so `loop1s()` retries. `flushPending()` forces all dirty subtrees through
/// synchronously (the reboot handler calls it so an add-then-reboot doesn't lose the
/// change); losing power before the debounce expires loses the in-flight change — the
/// cost of debouncing for fewer flash writes.
///
/// **Conditional visibility.** Modules with conditional controls bind their full
/// control set unconditionally and toggle a `hidden` flag per descriptor, so the
/// persistence layer can find and overlay a value regardless of the live conditional
/// state while the UI honours the flag. A Select change at runtime triggers
/// `rebuildControls()` to re-evaluate the flags.
///
/// **Platform layer.** Filesystem access goes through `platform::fs*` (mount, mkdir,
/// read, atomic write-then-rename, used/total). ESP32 uses LittleFS on a dedicated
/// partition; desktop uses `std::filesystem` rooted at `build/` (overridable via
/// `fsSetRoot` for test isolation). Save/load shares one `MAX_FILE_BYTES` buffer; a
/// subtree that serialises larger than that fails the write.
///
/// **First boot:** no files exist → load is a no-op, modules run with default
/// member-initialised values; after the first UI change the debounce creates the file.
/// A missing key keeps the default, an unknown key is silently ignored (no schema
/// migration).
/// @card FilesystemModule.png
class FilesystemModule : public MoonModule {
public:
    static constexpr const char* CONFIG_DIR = "/.config";
    static constexpr size_t MAX_FILE_BYTES = 2048;
    static constexpr size_t MAX_PATH = 64;
    static constexpr size_t MAX_KEY = 48;
    static constexpr uint32_t DEBOUNCE_MS = 2000;

    /// Singleton is registered in setScheduler() (called by main.cpp on the real
    /// FilesystemModule), NOT in the constructor. The factory creates short-lived
    /// probe instances for /api/types defaults capture; the probe's destructor would
    /// otherwise clear instance_ and break noteDirty()/flushPending() for the rest
    /// of the device's life.
    FilesystemModule() = default;
    ~FilesystemModule() override;

    /// Persistence must keep flushing dirty subtrees regardless of the `enabled` toggle —
    /// otherwise the user could lose changes by accidentally disabling this module via
    /// the UI before the 2s debounce expires.
    bool respectsEnabled() const override { return false; }

    /// Non-UI: a pure persistence engine with no controls — not shown as a card (its "last saved"
    /// status is displayed by FileManagerModule). See MoonModule::appearsInUi.
    bool appearsInUi() const override { return false; }

    void setScheduler(Scheduler* s);
    void setup() override;
    void loop1s() override;

    /// The engine's live "last saved" buffer — "never" before the first save, else "5m ago". This
    /// is the persistence engine's status; FileManagerModule binds its read-only "lastSaved" control
    /// straight to this buffer (no copy — same no-per-instance-copy pattern SystemModule uses for
    /// its statics), and this module's loop1s keeps it current. Returns the mutable buffer because
    /// addReadOnly takes a char* it points the control at. FilesystemModule itself has NO controls —
    /// it's a non-UI engine, not a card in the module tree.
    char* lastSavedStr() { return lastSaveStr_; }

    /// The live singleton (the main-wired instance registered in setScheduler), or null before
    /// boot wiring. FileManagerModule reads lastSavedStr() through this.
    static FilesystemModule* instance() { return instance_; }

    /// Synchronous save of every dirty subtree, bypassing the debounce. Same work
    /// loop1s does once the debounce expires. Exposed for tests so they can assert
    /// the file appears without wall-clock waits; production callers shouldn't need this.
    void flush();

    /// Static convenience for callers (such as the reboot handler) that need to force any
    /// debounced saves through before a teardown — mirrors noteDirty's call style.
    static void flushPending();

    /// Called by HttpServerModule on every successful control mutation so the
    /// 2s debounce starts. Cheap timestamp record; the actual walk happens in loop1s().
    static void noteDirty();

private:
    static inline FilesystemModule* instance_ = nullptr;
    Scheduler* scheduler_ = nullptr;
    bool mounted_ = false;
    bool dirtyPending_ = false;
    bool everSaved_ = false;       ///< false until the first successful save
    uint32_t lastDirtyMs_ = 0;
    uint32_t lastSaveMs_ = 0;
    char lastSaveStr_[24] = "never";  ///< "last saved" status string; FileManagerModule reads it via lastSavedStr()
    /// Shared load/save buffer — load runs once at boot (phase 2), save runs in loop1s after
    /// the 2s debounce. Mutually exclusive, so one buffer is enough. Kept off the task stack
    /// since 2KB plus recursive applyNode/writeNode frames is uncomfortably close to the ESP32
    /// default task stack ceiling (4–8KB).
    char fileBuf_[MAX_FILE_BYTES] = {};

    // ---- Internals ----
    void updateLastSavedStr();
    static void loadAllHookTrampoline_(Scheduler* s);
    void loadAll(Scheduler* s);
    void migrateRenamedConfigs();
    void loadSubtree(MoonModule* m);
    void applyNode(MoonModule* m, const char* json, const char* prefix);
    void applyValue(const ControlDescriptor& c, const char* json, const char* key);
    bool saveSubtree(MoonModule* m);
    bool writeNode(MoonModule* m, char* buf, size_t bufLen, int& pos, const char* prefix,
                   bool firstField = true);
    bool writeValue(const ControlDescriptor& c, char* buf, size_t bufLen, int& pos);
    static bool subtreeDirty(MoonModule* m);
    static void clearSubtreeDirty(MoonModule* m);
    static bool pathFor(MoonModule* m, char* out, size_t n);
};

} // namespace mm
