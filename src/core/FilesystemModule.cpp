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

void FilesystemModule::applyNode(MoonModule* m, const char* json, const char* prefix) {
    char key[MAX_KEY];
    auto& cs = m->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        auto& c = cs[i];
        if (!isPersistable(c.type)) continue;
        std::snprintf(key, sizeof(key), "%s%s", prefix, c.name);
        applyValue(c, json, key);
    }
    std::snprintf(key, sizeof(key), "%senabled", prefix);
    // Note: we can't distinguish "key absent" from "key=false" with the flat parser.
    // The convention: every saved file includes "enabled", so if the file exists and
    // applyNode is reached we assume the key is present. Production callers always
    // emit enabled (see writeNode). If the user hand-edited the file and dropped it,
    // they get enabled=false (matches the default-after-bad-edit behavior).
    m->setEnabled(mm::json::parseBool(json, key));

    // Reconcile children with the JSON's tree shape. For each position, look up
    // "<prefix><idx>.type"; if it differs from the live child (or no live child
    // exists), factory-create the JSON type and place it at that position. The
    // newly-created child gets defineControls() here so the recursive applyNode
    // below can overlay its persisted values. Phases 3+4 (setup, prepare)
    // cascade into the new child automatically.
    // Walk JSON child positions in order; stop when "<idx>.type" is absent. No fixed cap —
    // the JSON itself terminates the loop. childCount_ is a uint8_t so the practical ceiling
    // is 255 children per parent, far above any realistic tree.
    uint8_t jsonChildCount = 0;
    for (uint8_t i = 0; ; i++) {
        char typeKey[MAX_KEY];
        std::snprintf(typeKey, sizeof(typeKey), "%s%u.type", prefix, static_cast<unsigned>(i));
        char typeName[32] = {};
        mm::json::parseString(json, typeKey, typeName, sizeof(typeName));
        if (typeName[0] == 0) break;

        MoonModule* live = m->child(i);
        if (!live || std::strcmp(live->typeName(), typeName) != 0) {
            // Position-replace can also destroy a code-wired child if the file
            // describes a different type at this slot. Bail out of further
            // reconciliation rather than killing it — the trim loop below then
            // preserves the code-wired tail, and the next save will rewrite the
            // file with the current (correct) tree shape. The rest of the JSON
            // past this position is dropped on this boot; that's better than
            // losing a code-wired child.
            if (live && live->isWiredByCode()) break;
            MoonModule* created = ModuleFactory::create(typeName);
            if (!created) {
                // Factory failed (type not registered). Stop here so subsequent JSON
                // children don't get applied to misaligned live slots; jsonChildCount
                // stays at the last successfully reconciled position, and the trim loop
                // below removes any live children past that point.
                break;
            }
            created->defineControls();
            if (live) {
                MoonModule* old = m->replaceChildAt(i, created);
                if (old) { old->release(); Scheduler::deleteTree(old); }
            } else {
                m->addChild(created);
            }
        }

        jsonChildCount = i + 1;
        char childPrefix[MAX_KEY];
        std::snprintf(childPrefix, sizeof(childPrefix), "%s%u.", prefix, static_cast<unsigned>(i));
        applyNode(m->child(i), json, childPrefix);
    }
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
