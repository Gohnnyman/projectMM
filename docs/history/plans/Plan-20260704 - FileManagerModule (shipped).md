# Plan — FileManagerModule: browse / create / delete / edit the device filesystem

## Context

The product owner wants to browse and manage the device's LittleFS filesystem from the web UI,
Windows-Explorer-like: navigate folders, see file name + size, create/delete/edit files and
folders. Today the only filesystem-facing module is **FilesystemModule** — but that is the
*persistence engine* (writes `/.config/<Type>.json`, loads at boot, reconciles the module tree).
A file *manager* is a different job; merging them would repeat the v1 `StatefulModule`
"one class, five jobs" anti-pattern (decisions.md).

**Product-owner decisions (2026-07-04):**
- **Two modules to start.** New `FileManagerModule` for browse/create/delete/edit; FilesystemModule
  stays the untouched persistence engine. *Phasing note:* a later merge stays open **if evidence
  warrants** (e.g. the manager wants to edit the `/.config/*.json` the engine owns) — but not up
  front, so we avoid conflating infrastructure with a feature.
- **Breadcrumb drill-in navigation**, not the always-expanded Explorer tree. The full hierarchy is
  reachable (click a folder to enter, breadcrumb to go up); one directory shown at a time. The
  always-expanded tree from the reference screenshot is a **follow-up** once the plumbing is proven.
- **Text/config file editing, size-capped.** Edit text files up to a cap (a few KB — the
  `/.config` JSONs, small scripts) via a textarea + `fsWriteAtomic`. Binary/oversized files show
  (name/size) but aren't edited.
- **Dates/NTP backlogged.** Show name + size now (both available). Real "last modified" needs a
  time source (NTP/SNTP) AND LittleFS mtime storage — a separate backlog item; the column shows
  "—" / is omitted until then.

## Files

1. **`src/platform/platform.h` + platform impls** — the listing seam needs **size** (today
   `FsListCb = void(*)(const char* name, bool isDir, void* user)` has no size). Extend it:
   `using FsListCb = void(*)(const char* name, bool isDir, uint32_t sizeBytes, void* user);` and
   fill `sizeBytes` in both impls (ESP32 LittleFS `stat`, desktop `std::filesystem::file_size`;
   dirs report 0). This is the only platform-layer change. (Keep it a *listing* callback — no new
   per-file `fsStat` seam unless a caller needs a single stat, which this feature doesn't.)

2. **New `src/core/FileManagerModule.h` (+ `.cpp` if bodies grow)** — a domain-neutral core
   module. `role() → Peripheral` (added via UI / boot-wired near System — decide at wiring; NOT the
   persistence engine). Holds the **current directory path** as state; exposes:
   - A `path` control (breadcrumb / current dir; read-only display + navigation via the actions).
   - A **List control** backed by a `FileListSource : ListSource` — `writeListRow` emits
     `{name, isDir, size}` per entry in the current dir (via `platform::fsList`), `writeListRowDetail`
     adds nothing extra for now. This is the DevicesModule ListSource pattern exactly.
   - Actions (buttons / API): **enter** a subdir, **up** a level, **mkdir**, **rm** (file or empty
     dir), **read** a file (into an editor buffer), **write** a file (atomic). Bounded + robust: a
     bad path, a too-large file, a non-empty dir delete all fail cleanly with a status, never crash
     (the Robustness rule).
   - Status line reports the current dir + the last action result.
   - **Complexity stays in core / the module stays simple:** the recursive/parse-heavy work already
     lives in `platform::fs*` + the ListSource; the module is "list this dir, do this one op."

3. **HTTP API** — the browse/read/write/mkdir/rm operations need endpoints the UI calls. Reuse the
   existing `/api/control` + List-control refresh where it fits (navigation = a control write that
   changes `path` and rebuilds the list); file **read**/**write** need a small dedicated path
   (`GET /api/file?path=…` → contents, `POST /api/file` → atomic write) since a file body isn't a
   control value. Keep these thin and transport-only; the actual fs work is `platform::fs*`. Guard:
   path-sanitise (no `..` escape outside the mount root), size-cap the write.

4. **UI (`src/ui/app.js` + CSS)** — a File Manager view for the module: a breadcrumb bar (current
   path, click a crumb to jump up), a row list (folder/file icon, name, size; click a folder to
   enter, click a file to open the editor), a create-folder + create-file affordance, a
   delete affordance per row, and a modal/inline **text editor** (textarea, Save = atomic write,
   size-capped, read-only for binary/oversized). Modern + intuitive; this is the one genuinely
   *custom* (non-generic) UI in the cut — justified because a file manager can't be a generic
   control grid. Keep it a recognisable master-detail list, not a bespoke tree.

5. **Docs** — `ui.md` File Manager entry (summary + the controls/actions); moxygen page from the
   `.h` `///` comments. A backlog note for the follow-ups (Explorer tree, NTP+mtime dates).

6. **Tests** — `unit_FileManagerModule`: list a dir (name/size/isDir), mkdir, create + read-back a
   file, delete a file, delete-non-empty-dir rejected, path-traversal (`../`) rejected, oversized
   write rejected, list a missing dir doesn't crash. Driven on desktop (`fsSetRoot` to a temp dir,
   the existing test seam) so the real `platform::fs*` path is exercised.

## Not doing (scope guards)

- **No always-expanded Explorer tree** — breadcrumb drill-in now; the tree is a follow-up.
- **No dates / NTP / mtime** — name + size only; backlogged.
- **No binary/large-file editing** — text, size-capped; binary shown not edited.
- **No merge into FilesystemModule** — two modules; merge only later if evidenced.
- **No move/rename/copy** in the first cut (create/delete/edit only) — add once browse+edit lands.

## Verification

- Desktop build (`-Werror`) + ESP32 build clean; `ctest` (the new FileManager tests, driven via
  `fsSetRoot` temp dir); scenarios green; `check_specs` (control↔doc), `check_devices` if a board
  gains the module.
- Bench: on a real ESP32, browse `/`, enter `.config`, open a `<Type>.json`, see name+size, edit a
  small text file and confirm the write survives (read back / reboot). Create + delete a folder.
- Robustness: `../` path rejected, non-empty-dir delete rejected, oversized write rejected — each a
  clean status, no crash.

## Follow-ups (backlog)

- **Explorer-style expandable tree** (nested folders, expand/collapse, details pane) — the
  presentation upgrade over breadcrumb drill-in.
- **Real "last modified" dates** — an NTP/SNTP time seam + LittleFS mtime storage (LittleFS doesn't
  store mtime by default; needs the attribute API). Both a separate item.
- **Move / rename / copy** operations.
- **Possible FileManager ↔ FilesystemModule convergence** — revisit once the manager is used
  against the `/.config` files, if a single "files" surface proves cleaner than two modules.
