# Plan — File Manager: desktop drag-drop (tier 1) + filesystem usage bar in the panel

## Context
Two follow-ups on the shipped File Manager (PR #36):
1. **Drag-and-drop from the desktop filesystem** — tier 1 only: text/config/`.ml` files ≤ `kFileApiCap` (8 KB). Drop onto a tree folder → upload via the existing `/api/file` write endpoint.
2. **Filesystem usage bar in the panel** — the LittleFS used/total progress bar currently on the FilesystemModule card moves *visually* below the tree in the File Manager. FilesystemModule keeps owning + computing it (it owns the fs mount); the File Manager just renders it, read from `/api/state`. The bar is hidden on the FilesystemModule card so it lives in one place.

## Item 1 — Drag-drop tier 1

### Backend: byte-exact write (fixes the NUL-truncation for real)
- `HttpServerModule.cpp` POST route (line ~200): replace `std::strlen(body)` with the true body length. `headerEnd` + `totalRead` are in scope: `size_t bodyLen = headerEnd ? (size_t)(totalRead - (int)(body - buf)) : 0;` — pass that to `handleWriteFile`. This makes writes byte-exact (a body with an embedded NUL no longer truncates), which the shipped editor Save also benefits from.
- With byte-exact writes, the editor's binary **read-only guard** stays (a `<textarea>` still can't safely round-trip binary), but the *write path* is now correct for any bytes a client sends. Keep the guard; it's about the textarea, not the endpoint.
- No new endpoint — `/api/file?path=<dir>/<name>` POST already creates/overwrites.

### UI: drop handler on tree folder rows
- In `renderFileManager`, add `dragover`/`dragleave`/`drop` listeners on each **folder** `rowEl` (and the root tree container, so a drop on empty space targets root).
- `dragover`: `e.preventDefault()` + add a `.fm-row--drop` highlight class so the target is obvious.
- `drop`: `e.preventDefault()`, for each `File` in `e.dataTransfer.files`:
  - **Size guard (tier 1):** if `file.size > 8192`, skip it with a visible note (status text / alert) — tier 1 is text/config only; binary/large is backlogged. Log what was skipped (no silent truncation, per the principles).
  - Read via `await file.text()` (tier 1 = text), POST to `/api/file?path=<joinFsPath(folderPath, file.name)>`.
  - On success, `st.expanded.add(folderPath)` to reveal it; after all files, `renderFileManager` to re-list.
- Reuse `joinFsPath`, the existing POST shape from `＋ file`/editor Save. Extract a small `fmUploadFile(destDir, file)` helper so the drop loop stays readable.
- CSS: `.fm-row--drop { outline: 2px dashed var(--accent); }` (or a background tint).

### UI: per-file download (device → desktop)
- True drag-*out* is not portable (browser `DownloadURL` is Chrome-only + needs contents up-front). The standard equivalent is a **download link**: `/api/file?path=…` GET already serves the file contents + length, so a per-row `⤓` is `<a href="/api/file?path=<childPath>" download="<name>">` — forces a save-to-desktop with the right filename, every browser, any file type, **zero backend change**.
- Add a `⤓` button/link to each **file** row (next to the existing per-row affordances), styled like `.fm-del`. Folders get no `⤓` (folder-as-zip is backlogged — needs a bundled client-side zip lib + recursion, a real app.js/flash cost).

### Not doing (tier 1 scope guards)
- No binary/large (>8KB) upload — `file.text()` + size cap; a too-big or binary file is skipped with a note.
- No recursive folder drops — `dataTransfer.items` webkitGetAsEntry() recursion is tier 3.
- No chunked write — the 8KB cap keeps it a single POST.
- No folder download (zip) — needs a bundled zip lib + recursion; backlogged next to folder-upload, both flagged for flash cost.

## Item 2 — Filesystem usage bar below the tree

### FilesystemModule: hide the control on its own card
- `FilesystemModule.cpp` onBuildControls: after `addProgress("filesystem", ...)`, `controls_.setHidden(controls_.count()-1, true)` — same generic-hidden pattern the File Manager's op controls use. FilesystemModule still computes/refreshes `fsUsedVal_` in loop1s; the control stays in `/api/state` (hidden is a UI-render hint only), so the File Manager can read it.
- `lastSaved` stays visible on the FilesystemModule card (it's persistence-engine state, not a fs-browser concern).

### File Manager panel: render the bar below the tree
- In `renderFileManager`, after the tree, read the FilesystemModule's `filesystem` progress control from `state` (find the module by type `"FilesystemModule"`, its control named `"filesystem"` → `value` + `total`).
- Render a `<progress value=used max=total>` + a label (mirror `fmtProgressLabel` — "X KB / Y KB"). Reuse the existing progress styling; wrap in a `.fm-usage` row.
- If FilesystemModule or the control isn't present (e.g. desktop with fs total 0), render nothing (graceful).

### Docs
- `ui.md`: File Manager entry — note the usage bar below the tree + drag-drop (tier 1). FilesystemModule entry — drop the `filesystem` usage bullet (it's shown in the File Manager now), keep `lastSaved`.
- `backlog-core § File Manager follow-ups`: mark drag-drop tier 1 shipped; keep tiers 2/3 (binary/large, folders). The Content-Length write note is now done — remove it.

## Tests
- `unit_HttpServerModule_apply` or a new small test: a POST to `/api/file` with a body containing a NUL byte writes the **full** length (byte-exact), not truncated at the NUL — pins the `strlen`→`contentLen` fix. (If no socket harness, at minimum a `handleWriteFile` length-path unit check.)
- Drag-drop itself is browser DOM — not unit-testable in ctest; covered by desktop manual smoke + the byte-exact write test.
- `unit_FileManagerModule` unchanged (module surface didn't change).

## Verification
- Desktop: drag a small `.json`/`.txt`/`.ml` from Finder onto a folder in the tree → appears, editable. Drag a >8KB file → skipped with a note. Usage bar shows below the tree; gone from the FilesystemModule card.
- ESP32-S3: same, on real LittleFS; confirm the usage bar reads sane used/total.
- Gates: desktop build, ctest, scenarios, spec check, ESP32 build, KPI.
- Save plan to `docs/history/plans/Plan-20260704 - File Manager drag-drop + usage bar.md`.
