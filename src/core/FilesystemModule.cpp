#include "core/FilesystemModule.h"

#include "core/Control.h"
#include "core/JsonSink.h"   // fixed-buffer mode used by writeValue()
#include "core/JsonUtil.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "platform/platform.h"

#include <climits>  // INT16_MIN/MAX in applyValue's Int16 clamp
#include <cstdio>
#include <cstring>

namespace mm {

FilesystemModule::~FilesystemModule() {
    if (instance_ == this) instance_ = nullptr;
}

void FilesystemModule::setScheduler(Scheduler* s) {
    scheduler_ = s;
    instance_ = this;
    if (s) {
        s->setLoadAllHook(&loadAllHookTrampoline_);
        // Scheduler::setControl calls this after a mutation so a control set from anywhere
        // (IR, WLED bridge, /api/control) schedules the same debounced save. noteDirty is a
        // static, so a plain function pointer suffices — no trampoline needed.
        s->setNoteDirtyHook(&FilesystemModule::noteDirty);
    }
}

void FilesystemModule::setup() {
    if (!platform::fsMount()) {
        std::printf("FilesystemModule: mount failed — persistence disabled\n");
        return;
    }
    mounted_ = true;
    platform::fsMkdir(CONFIG_DIR);
    std::printf("FilesystemModule: mounted, %zu / %zu bytes used\n",
                platform::filesystemUsed(), platform::filesystemTotal());
}

// FilesystemModule is a non-UI persistence engine: it holds no controls (hence no
// defineControls override), so it renders no card in the module tree — a card here would
// confuse an end user next to the File Manager. Its one piece of status, "last saved", is
// displayed by FileManagerModule, which reads it via FilesystemModule::instance()->lastSavedStr().
// The filesystem-usage gauge likewise lives on FileManagerModule (that's where filesystem state
// is topical).

void FilesystemModule::tick1s() {
    if (!mounted_ || !scheduler_) return;
    updateLastSavedStr();
    if (!dirtyPending_) return;
    if (platform::millis() - lastDirtyMs_ < DEBOUNCE_MS) return;
    flush();
}

// Refresh the "lastSaved" display string — "never" before the first save,
// otherwise how long ago the last successful write happened.
void FilesystemModule::updateLastSavedStr() {
    if (!everSaved_) {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "never");
        return;
    }
    uint32_t agoSec = (platform::millis() - lastSaveMs_) / 1000;
    if (agoSec < 60) {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "%us ago",
                      static_cast<unsigned>(agoSec));
    } else if (agoSec < 3600) {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "%um ago",
                      static_cast<unsigned>(agoSec / 60));
    } else {
        std::snprintf(lastSaveStr_, sizeof(lastSaveStr_), "%uh ago",
                      static_cast<unsigned>(agoSec / 3600));
    }
}

void FilesystemModule::flush() {
    if (!mounted_ || !scheduler_) return;
    bool allSaved = true;
    for (uint8_t i = 0; i < scheduler_->moduleCount(); i++) {
        MoonModule* m = scheduler_->module(i);
        if (!m || m == this) continue;
        if (subtreeDirty(m)) {
            // Only clear the dirty flag when the write actually succeeded —
            // otherwise a failed write would silently drop the pending change.
            if (saveSubtree(m)) {
                clearSubtreeDirty(m);
                lastSaveMs_ = platform::millis();
                everSaved_ = true;
            } else {
                allSaved = false;
            }
        }
    }
    // Keep dirtyPending_ set if anything failed, so tick1s retries.
    dirtyPending_ = !allSaved;
}

void FilesystemModule::flushPending() {
    if (instance_) instance_->flush();
}

void FilesystemModule::noteDirty() {
    if (!instance_) return;
    instance_->lastDirtyMs_ = platform::millis();
    instance_->dirtyPending_ = true;
}

// ---- Scheduler hook trampoline (C-style for typedef compatibility) ----
void FilesystemModule::loadAllHookTrampoline_(Scheduler* s) {
    if (instance_) instance_->loadAll(s);
}

void FilesystemModule::loadAll(Scheduler* s) {
    if (!mounted_) {
        // setup() hasn't run yet (we're in phase 2, before phase 3 setup). Mount now
        // so we can read; setup() later calls fsMount again (idempotent).
        if (!platform::fsMount()) return;
        mounted_ = true;
        platform::fsMkdir(CONFIG_DIR);
    }
    for (uint8_t i = 0; i < s->moduleCount(); i++) {
        MoonModule* m = s->module(i);
        if (!m || m == this) continue;
        loadSubtree(m);
    }
}

// ---- Load ----
void FilesystemModule::loadSubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return;
    // Read the WHOLE file into a heap buffer sized to it — no fixed ceiling, so a large saved config
    // (many light presets, a wide fixture) loads in full instead of being truncated to a fixed buffer
    // and failing to parse. Mirrors the streaming save (saveSubtree): both sides are cap-free.
    const long size = platform::fsSize(path);
    if (size <= 0) return;
    char* buf = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!buf) { std::printf("FilesystemModule: out of memory loading %s (%ld bytes)\n", path, size); return; }
    const int n = platform::fsRead(path, buf, static_cast<size_t>(size) + 1);
    if (n > 0) {
        buf[n] = '\0';                   // applyNode parses buf as a C-string
        applyNode(m, buf, "");
    }
    platform::free(buf);
}

// Overlay every persistable control's saved value onto the module's current control list, in list order.
void FilesystemModule::overlayControls(MoonModule* m, const char* json, const char* prefix) {
    char key[MAX_KEY];
    auto& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        if (!isPersistable(c.type)) continue;
        std::snprintf(key, sizeof(key), "%s%s", prefix, c.name);
        applyValue(c, json, key);
    }
}

// Restore a code-wired child's saved state when its boot index differs from the index the file recorded
// it at (a reorder of code-wired siblings). Scan the saved child entries ("<prefix><j>.type") for the
// one whose type matches `wired`, and apply that entry's subtree to it. If the file has no entry for this
// wired child (it predates the child), there is nothing to restore and the child keeps its defaults.
void FilesystemModule::applyWiredChildFromJson(MoonModule* wired, const char* json, const char* prefix) {
    for (uint8_t j = 0; ; j++) {
        char typeKey[MAX_KEY];
        std::snprintf(typeKey, sizeof(typeKey), "%s%u.type", prefix, static_cast<unsigned>(j));
        char typeName[32] = {};
        mm::json::parseString(json, typeKey, typeName, sizeof(typeName));
        if (typeName[0] == 0) return;   // walked past the last saved child — no match, keep defaults
        if (std::strcmp(typeName, wired->typeName()) != 0) continue;
        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(j));
        applyNode(wired, json, childPrefix);
        return;
    }
}

void FilesystemModule::applyNode(MoonModule* m, const char* json, const char* prefix) {
    char key[MAX_KEY];
    // Overlay the saved values. A module whose CONTROL SET depends on one of its own control VALUES
    // (the canonical case: ParallelLedDriver's `peripheral` Select, which swaps the bus backend and
    // with it the backend-owned controls — clockPin, dcPin, the ring cluster) needs a second pass:
    // the first overlay writes `peripheral`, but the backend-owned controls are still bound to the
    // DEFAULT backend's members, so their saved values land on a backend about to be discarded. So
    // overlay, then rebuildControls() (which re-runs defineControls → swaps the live backend to match
    // the just-applied `peripheral`, re-binding the control list to the RIGHT backend's members), then
    // overlay again onto the now-correct controls. rebuildControls' schema-hash gate no-ops the refire
    // when nothing changed (the common case: a module with no value-dependent schema), and the second
    // overlay is idempotent value writes — so this is safe and cheap for every module. Without it, any
    // reload that rebuilds the control set (a reboot, an INT_WDT restart) silently reverts every
    // backend-owned control (clockPin, the ring geometry) to its default.
    overlayControls(m, json, prefix);
    m->rebuildControls();
    overlayControls(m, json, prefix);

    std::snprintf(key, sizeof(key), "%senabled", prefix);
    // Note: we can't distinguish "key absent" from "key=false" with the flat parser.
    // The convention: every saved file includes "enabled", so if the file exists and
    // applyNode is reached we assume the key is present. Production callers always
    // emit enabled (see writeNode). If the user hand-edited the file and dropped it,
    // they get enabled=false (matches the default-after-bad-edit behavior).
    m->setEnabled(mm::json::parseBool(json, key));

    // Reconcile children with the JSON's tree shape. Walk each saved child entry ("<prefix><idx>.type")
    // and place the corresponding live child. The JSON index `i` and the live position `pos` are
    // DECOUPLED: `i` always advances; `pos` advances only when a child is actually placed there. This
    // decoupling is what makes a single bad entry non-fatal — a JSON entry that produces no live child
    // (an unknown/renamed type, or a stale slot over a code-wired child) is skipped WITHOUT dropping the
    // user modules the file records after it. User modules are created fresh here in file order via
    // addChild, so their user-chosen order (a UI reorder) round-trips; code-wired children pre-exist and
    // are skipped in place (see the isWiredByCode() branch below).
    //
    // The decoupling is what keeps a single bad entry from taking the rest of the tree down with it — the
    // failure mode a naive "break on any mismatch/unknown" reconcile has, where one unresolvable entry
    // drops every module the file records after it. The two entries that must skip-not-break:
    //   - a stale slot over a code-wired child (the file predates the wired child, or names a different
    //     type where it now sits): keep the wired instance, advance past it;
    //   - a renamed/removed module type (the documented ADR-0013 migration, e.g. a pre-consolidation
    //     MoonLedDriver/MultiPinLedDriver entry): that entry drops, the rest stay.
    uint8_t pos = 0;
    for (uint8_t i = 0; ; i++) {
        char typeKey[MAX_KEY];
        std::snprintf(typeKey, sizeof(typeKey), "%s%u.type", prefix, static_cast<unsigned>(i));
        char typeName[32] = {};
        mm::json::parseString(json, typeKey, typeName, sizeof(typeName));
        if (typeName[0] == 0) break;

        MoonModule* live = m->child(pos);
        if (!live || std::strcmp(live->typeName(), typeName) != 0) {
            // A code-wired child that mismatches the saved type here is a STALE SLOT (the file predates
            // this code-wired child, or names a different type where it now sits — e.g. boot wired the
            // code-wired siblings in a different order than the file recorded them). Never replace/destroy
            // the wired instance: keep it and advance past it. But first restore ITS saved values — find
            // the JSON entry that names THIS wired child's type and overlay that entry's controls, so a
            // code-wired child's persisted state survives even when its saved index differs from its boot
            // index (a reorder). Without this the wired child keeps its defaults on every reboot.
            if (live && live->isWiredByCode()) {
                applyWiredChildFromJson(live, json, prefix);
                pos++;
                continue;
            }
            MoonModule* created = ModuleFactory::create(typeName);
            if (!created) {
                // Unknown/renamed type (ADR-0013 migration): the module drops. Skip this JSON entry and
                // keep reconciling the rest — do NOT advance `pos`, so the file's later user modules still
                // map to the correct live position.
                continue;
            }
            created->defineControls();
            if (live) {
                MoonModule* old = m->replaceChildAt(pos, created);
                if (old) { old->release(); Scheduler::deleteTree(old); }
            } else {
                m->addChild(created);
            }
        }

        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        applyNode(m->child(pos), json, childPrefix);
        pos++;
    }
    uint8_t jsonChildCount = pos;   // live children reconciled; the trim loop keeps these, prunes the rest
    // Trim live children beyond what the JSON describes, EXCEPT children that
    // were wired by code at boot (main.cpp annotates those via markWiredByCode).
    // A code-wired child is preserved across persistence loads even when the
    // on-disk file predates its addition — the upgrade-day case where a new
    // release adds a code-created child (e.g. ImprovProvisioningModule under
    // NetworkModule) whose existence the device's saved file doesn't yet know
    // about. Without this exemption the child would get trimmed on every boot.
    //
    // Walks back-to-front so removeChild's left-shift of later siblings doesn't
    // skip an entry. Any code-wired child at index >= jsonChildCount stays; its
    // position relative to the JSON-described children may not match what the
    // file expects, but on the first dirty event the next save writes the
    // current (post-merge) tree shape and from then on the file matches.
    uint8_t i = m->childCount();
    while (i > jsonChildCount) {
        i--;
        MoonModule* extra = m->child(i);
        if (!extra) continue;
        if (extra->isWiredByCode()) continue;
        extra->release();
        m->removeChild(extra);
        Scheduler::deleteTree(extra);
    }
}

void FilesystemModule::applyValue(const ControlDescriptor& c, const char* json, const char* key) {
    // Per-type parse + validate + apply lives in Control.cpp. Use Clamp:
    // a stale on-disk value from a schema change should snap to the new
    // bounds (Uint8 200 → max 100), not silently drop to 0. The HTTP API
    // uses Strict instead so a bogus client value surfaces as a 400.
    (void)applyControlValue(c, json, key, ApplyPolicy::Clamp);
}

// ---- Save ----
// Returns true only when the file was written. On failure (path/overflow/write
// error) the caller must keep the subtree dirty so the change isn't lost.
bool FilesystemModule::saveSubtree(MoonModule* m) {
    char path[MAX_PATH];
    if (!pathFor(m, path, sizeof(path))) return false;
    // Serialize the whole subtree into a buffer-mode JsonSink — a growable heap buffer with NO
    // fixed ceiling (the same primitive /api/state streams through), so a large config (many light
    // presets, a wide fixture wiring) persists in full instead of silently truncating. Written
    // atomically once complete.
    JsonSink sink;                       // heap/buffer mode: grows as needed, no cap
    sink.append("{");
    writeNode(m, sink, "", /*firstField=*/true);
    sink.append("}");
    if (sink.overflowed()) {             // only trips on an allocation failure now, not a size cap
        std::printf("FilesystemModule: out of memory serializing %s\n", path);
        return false;
    }
    if (platform::fsWriteAtomic(path, sink.data(), sink.size())) {
        std::printf("FilesystemModule: saved %s (%zu bytes)\n", path, sink.size());
        return true;
    }
    std::printf("FilesystemModule: write failed for %s\n", path);
    return false;
}

// Append this module's persistable controls, its enabled flag, and (recursively) its children to
// `sink`. `firstField` is true when this is the first field-emitter inside its containing `{` — the
// top-level call passes true; a recursive child call passes false because the parent already emitted
// its `"N.type"` field, so the child must prefix a comma before its first control. No size limit:
// the sink grows; the old overflow-returns-bool plumbing is gone (an allocation failure surfaces via
// sink.overflowed() at the top level).
void FilesystemModule::writeNode(MoonModule* m, JsonSink& sink, const char* prefix, bool firstField) {
    bool first = firstField;
    auto& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        if (!isPersistable(c.type)) continue;
        sink.appendf("%s\"%s%s\":", first ? "" : ",", prefix, c.name);
        writeControlValue(sink, c);      // the shared value serializer (same as /api/state)
        first = false;
    }
    sink.appendf("%s\"%senabled\":%s", first ? "" : ",", prefix, m->enabled() ? "true" : "false");
    for (uint8_t i = 0; i < m->childCount(); i++) {
        MoonModule* child = m->child(i);
        if (!child) continue;  // addChild rejects nullptr today; defend against future invariants
        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        // Emit "0.type":"NoiseEffect" so the reader can detect tree-shape mismatches.
        sink.appendf(",\"%stype\":\"%s\"", childPrefix, child->typeName());
        writeNode(child, sink, childPrefix, /*firstField=*/false);
    }
}

// ---- Dirty walking ----
bool FilesystemModule::subtreeDirty(MoonModule* m) {
    if (!m) return false;
    if (m->dirty()) return true;
    for (uint8_t i = 0; i < m->childCount(); i++) {
        if (subtreeDirty(m->child(i))) return true;
    }
    return false;
}

void FilesystemModule::clearSubtreeDirty(MoonModule* m) {
    if (!m) return;
    m->clearDirty();
    for (uint8_t i = 0; i < m->childCount(); i++) clearSubtreeDirty(m->child(i));
}

// ---- Paths ----
// Filename = "/.config/<TypeName>.json". Single instance assumed; multi-instance gets a
// .N suffix when that becomes a requirement (item 12 — module switching).
bool FilesystemModule::pathFor(MoonModule* m, char* out, size_t n) {
    if (!m || m->typeName()[0] == 0) return false;
    int w = std::snprintf(out, n, "%s/%s.json", CONFIG_DIR, m->typeName());
    return w > 0 && static_cast<size_t>(w) < n;
}

} // namespace mm
