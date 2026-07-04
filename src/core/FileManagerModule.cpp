#include "core/FileManagerModule.h"

#include "platform/platform.h"   // fs* primitives

#include <cstdio>
#include <cstring>

namespace mm {

void FileManagerModule::onBuildControls() {
    // Every control is UI-hidden: the whole File Manager surface is the tree panel (app.js
    // renderFileManager), which drives these via /api/control and reads them from /api/state. The
    // `hidden` flag keeps them bound for persistence + the API while the generic control list skips
    // them, so the panel is the single, self-contained UI with no duplicate raw controls beside it.
    controls_.addBool("show hidden", showHidden_);      // reveal dot-prefixed entries (e.g. .config)
    controls_.setHidden(controls_.count() - 1, true);
    controls_.addText("path", path_, sizeof(path_));    // absolute op target (mkdir / delete), UI-set
    controls_.setHidden(controls_.count() - 1, true);
    controls_.addButton("new folder");                  // mkdir the folder at `path`
    controls_.setHidden(controls_.count() - 1, true);
    controls_.addButton("delete");                      // remove the file or empty folder at `path`
    controls_.setHidden(controls_.count() - 1, true);
    // Filesystem-usage gauge (used / total bytes), read from the platform. Shown below the tree in
    // the panel — the File Manager is where filesystem space is relevant, so it owns the control.
    // Bound only when the platform reports a real partition (desktop / a no-data-partition chip
    // reports 0). loop1s refreshes the used value; the total is fixed.
    totalBytes_ = static_cast<uint32_t>(platform::filesystemTotal());
    usedBytes_ = static_cast<uint32_t>(platform::filesystemUsed());
    if (totalBytes_ > 0) controls_.addProgress("filesystem", usedBytes_, totalBytes_);
    MoonModule::onBuildControls();
}

void FileManagerModule::loop1s() {
    if (totalBytes_ > 0) usedBytes_ = static_cast<uint32_t>(platform::filesystemUsed());
}

void FileManagerModule::setup() {
    MoonModule::setup();
    // `show hidden` is a transient view preference, not device config — force it off on every boot
    // regardless of any persisted value (setup() runs after persistence overlays it). A file manager
    // opens with hidden entries hidden; the user re-toggles per session.
    showHidden_ = false;
}

void FileManagerModule::onUpdate(const char* c) {
    if      (std::strcmp(c, "new folder") == 0) makeDir();
    else if (std::strcmp(c, "delete")     == 0) removeEntry();
}

// Both ops target the absolute path in `path_`, which the tree UI fills from the selected node.
// safePath rejects a `..` traversal and roots the path at the mount — the one place that's checked.
void FileManagerModule::makeDir() {
    char full[kPathMax];
    if (path_[0] == 0 || !safePath(path_, full, sizeof(full))) {
        setStatus("invalid path", Severity::Warning); return;
    }
    // %.80s bounds the path so the status always fits statusBuf_ (a 128-char path won't overflow a
    // 96-byte buffer — the ESP32 -Werror=format-truncation build enforces this).
    if (platform::fsMkdir(full)) { std::snprintf(statusBuf_, sizeof(statusBuf_), "created %.80s", path_); setStatus(statusBuf_); markDirty(); }
    else { setStatus("mkdir failed", Severity::Error); }
    path_[0] = 0;
}

void FileManagerModule::removeEntry() {
    char full[kPathMax];
    if (path_[0] == 0 || !safePath(path_, full, sizeof(full))) {
        setStatus("invalid path", Severity::Warning); return;
    }
    // fsRemove deletes a file or an EMPTY dir; a non-empty dir fails cleanly (reported below).
    if (platform::fsRemove(full)) { std::snprintf(statusBuf_, sizeof(statusBuf_), "deleted %.80s", path_); setStatus(statusBuf_); markDirty(); }
    else { setStatus("delete failed (folder not empty?)", Severity::Warning); }
    path_[0] = 0;
}

// Traversal guard for the control-driven ops (mkdir/delete). The `/api/file` + `/api/dir` HTTP
// endpoints guard their own path input the same way (HttpServerModule::fileQueryPath) — two entry
// paths into the filesystem, each rejecting `..` at its boundary. Reject any `..`, root at the mount.
bool FileManagerModule::safePath(const char* rel, char* out, size_t cap) {
    if (!rel || std::strstr(rel, "..")) return false;   // no parent-escape anywhere in the path
    const int n = std::snprintf(out, cap, "%s%s", rel[0] == '/' ? "" : "/", rel);
    return n > 0 && static_cast<size_t>(n) < cap;
}

} // namespace mm
