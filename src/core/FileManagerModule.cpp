#include "core/FileManagerModule.h"

#include "core/FilesystemModule.h"   // instance()->lastSavedStr() for the "last saved" readout
#include "platform/platform.h"       // fs* primitives

namespace mm {

void FileManagerModule::defineControls() {
    // Only `show hidden` is a control: the whole File Manager surface is the tree panel (app.js
    // renderFileManager), which lists over /api/dir and reads the gauges from /api/state; the
    // mkdir/delete OPS are their own HTTP endpoints (POST/DELETE /api/dir?path=) — not persisted
    // controls — so a create/delete carries its path in the request, not in device storage. The
    // `show hidden` flag keeps it bound for the API while the generic control list skips it.
    controls_.addBool("show hidden", showHidden_);      // reveal dot-prefixed entries (e.g. .config)
    controls_.setHidden(controls_.count() - 1, true);
    // Filesystem-usage gauge (used / total bytes), read from the platform. Shown below the tree in
    // the panel — the File Manager is where filesystem space is relevant, so it owns the control.
    // Bound only when the platform reports a real partition (desktop / a no-data-partition chip
    // reports 0). Read the total ONCE (it's fixed) the first time controls are built; the used value
    // is refreshed only on the throttled tick1s. defineControls() is re-runnable (a Select can rebuild
    // the control set on any control write), so it must NOT re-scan the filesystem here — a LittleFS
    // usage scan walks blocks and isn't free. Rebuild from the cached values instead.
    if (totalBytes_ == 0) {
        totalBytes_ = static_cast<uint32_t>(platform::filesystemTotal());
        usedBytes_ = static_cast<uint32_t>(platform::filesystemUsed());
    }
    if (totalBytes_ > 0) {
        controls_.addProgress("filesystem", usedBytes_, totalBytes_);
        controls_.setHidden(controls_.count() - 1, true);   // renders as the usage bar in the panel, not generically
    }
    // "last saved" readout — how long ago config was persisted. The value is OWNED by the
    // FilesystemModule engine (non-UI); the File Manager just displays it here (this is where
    // filesystem state is topical). Bind the control straight to the engine's live buffer — no
    // per-instance copy — the same no-copy pattern SystemModule uses for its static strings. The
    // engine is the boot-wired singleton (alive for the device's life), and its tick1s keeps the
    // string current. Bound only when the engine exists (it's constructed before this module).
    if (FilesystemModule* fs = FilesystemModule::instance()) {
        controls_.addReadOnly("lastSaved", fs->lastSavedStr());
        controls_.setHidden(controls_.count() - 1, true);   // shown in the panel header, not generically
    }
    MoonModule::defineControls();
}

void FileManagerModule::tick1s() MM_NONBLOCKING {
    if (totalBytes_ > 0) usedBytes_ = static_cast<uint32_t>(platform::filesystemUsed());
}

void FileManagerModule::setup() {
    MoonModule::setup();
    // `show hidden` is a transient view preference, not device config — force it off on every boot
    // regardless of any persisted value (setup() runs after persistence overlays it). A file manager
    // opens with hidden entries hidden; the user re-toggles per session.
    showHidden_ = false;
}

// mkdir/delete are HTTP endpoints (POST/DELETE /api/dir?path=) in HttpServerModule: a create/delete
// carries its path in the request and touches the filesystem directly, so this module holds no op
// state and writes nothing to persisted config. The path guard (reject `..`, root at mount) lives
// once in HttpServerModule::parseFilePath, shared with /api/file + /api/dir GET.

} // namespace mm
