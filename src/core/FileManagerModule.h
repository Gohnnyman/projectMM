#pragma once
// Core service module — `.h` interface, bodies in FileManagerModule.cpp (the core `.h`+`.cpp`
// convention: it bridges the platform fs layer + has real logic, so implementation edits recompile
// only the .cpp, not every TU that includes this header).

#include "core/MoonModule.h"

#include <cstddef>

namespace mm {

/// Browse and manage the device filesystem from the UI — the counterpart to FilesystemModule,
/// which is the *persistence engine* (writes `/.config/*.json`, loads at boot). This module is the
/// user-facing **file manager**: an expand/collapse folder tree, each node's name + size, and
/// create / delete / edit of files and folders.
///
/// **Browsing lives in the UI.** The tree is a client-side lazy tree (the standard VS Code /
/// Explorer / web-file-tree shape): each folder loads *its own* children on first expand from the
/// `/api/dir?path=` endpoint (a single-level listing — the same `platform::fsList` the seam offers),
/// and expand-state is UI state. The module owns none of that; it only exposes the *operations*,
/// keeping the domain module simple and the recursion/paging out of core state and off `/api/state`.
///
/// **Hidden entries.** A leading `.` (e.g. the `.config` persistence dir) is hidden unless the
/// `show hidden` toggle is on — the standard dotfile convention. The toggle is a persisted bool the
/// tree reads and forwards to `/api/dir` as the `hidden` filter flag.
///
/// **Operations.** `path` is set by the UI to the absolute target (a selected tree node); `new
/// folder` creates the folder at `path`, `delete` removes the file or empty folder at `path`. A
/// file's contents are read/written over the `/api/file` HTTP endpoints (a file body isn't a control
/// value). Every op is bounded and robust: a bad path, a non-empty-dir delete, or a `..` traversal
/// fails with a status line and never crashes (the Robustness rule).
///
/// **Not shown yet:** last-modified dates need a time source (NTP) + LittleFS mtime storage, both
/// backlogged — the tree is name + size for now.
///
/// **Prior art:** the lazy-loaded folder tree is the standard file-explorer shape (a node loads its
/// children when expanded); the `/api/dir` + `/api/file` split mirrors the listing-vs-contents split
/// every file API draws (WebDAV PROPFIND vs GET).
class FileManagerModule : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Peripheral; }

    void onBuildControls() override;
    void onUpdate(const char* c) override;

private:
    static constexpr size_t kPathMax = 128;   // matches the platform fsTranslate ceiling

    void makeDir();                // mkdir the folder at path_
    void removeEntry();            // remove the file / empty dir at path_

    // Reject a ".." traversal, root the path at the mount — the one place traversal is checked.
    static bool safePath(const char* rel, char* out, size_t cap);

    char path_[kPathMax] = "";     // absolute op target (mkdir/delete), set by the tree UI
    char statusBuf_[96] = "";
    bool showHidden_ = false;      // reveal dot-prefixed entries (forwarded to /api/dir by the UI)
};

} // namespace mm
