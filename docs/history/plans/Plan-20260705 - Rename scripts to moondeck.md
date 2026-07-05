# Plan — Rename `moondeck/` → `moondeck/` (+ `moondeck/ci/` split)

PO-approved 2026-07-05 (refines the 2026-07-02 decision in `docs/backlog/rename-scripts-to-moondeck.md`). Its own isolated commit, next cycle — not folded into feature work.

## Decision (what ships)

1. **`moondeck/` → `moondeck/`** wholesale. The folder is MoonDeck's home; it holds the console (`moondeck.py`, `moondeck_ui/`, `moondeck_config.json`, `MoonDeck.md`) plus the build/check/scenario/run tooling MoonDeck invokes (28 scripts are MoonDeck cards; the rest are helpers of carded scripts or the build graph MoonDeck triggers).
2. **`moondeck/build/{package_desktop,verify_version}.py` → `moondeck/ci/`.** These two are the *only* truly-CI-only scripts (git-tag verification + release packaging, invoked by `.github/workflows/`, never a MoonDeck action, never build-wired). A `ci/` subfolder labels them accurately so `moondeck/` reads as MoonDeck's world.
3. **Keep function-based subfolders** (`build/`, `check/`, `docs/`, `scenario/`, `run/`, `diag/`, `report/`, `test/`). NOT reorganized by UI tab: the tab→folder map is many-to-many (the `pc` tab draws from 7 folders; `build/` serves `pc`+`esp32`+CI), so tab-based folders would couple code layout to UI layout and force placement choices for multi-tab scripts. The tab a card shows on is declared in `moondeck_config.json` (`"tab"`) — UI grouping is data, not directories. Function grouping is also the recognizable convention (*Common patterns first*).

### Considered and rejected
- **One folder per UI tab** — couples code to UI, bespoke, fights the many-to-many tab↔folder reality.
- **Split all CI/build-graph scripts out** — `compute_version.py` (CMake-invoked on every build, incl. MoonDeck's Build) and `generate_manifest.py` (imported by `generate_firmwares` ← `build_esp32`) are build-wired, so they must stay in `moondeck/`; only 2 files cleanly separate.
- **Collapse singleton folders** (`diag/`, `report/`, `test/`) — deferred; not part of this rename (keep the diff purely a move).

## What NOT to rename (the entanglement that justified keeping the name generic — noted, PO kept the rename)
`moondeck/` held more than MoonDeck, but the data shows nearly all of it is MoonDeck's dependency tree; the handful that isn't (CI-only → now `ci/`; the build-graph helpers → stay) is an acceptable minority under a folder named for its dominant purpose.

## Blast radius (measured, this tree)

- **Literal `moondeck/`**: 29 `.py`, 47 `.md`, 3 `.yml`, 2 CMake — replace `moondeck/` → `moondeck/`.
- **Split-path `"moondeck"` token** (the gotcha that broke the web-installer sweep): **10 Python test files** use `ROOT / "moondeck" / …` — replace the `"moondeck"` string token → `"moondeck"`. No JS split-paths. CMake uses the literal `/moondeck/` (covered).
- **`moondeck_config.json`** script paths are relative to the scripts dir (`"build/…"`, `"check/…"`) — unaffected by the folder rename EXCEPT the two moving to `ci/`: `build/package_desktop.py`→`ci/package_desktop.py`, `build/verify_version.py`→`ci/verify_version.py` (only if they get cards — they don't today, so only `.github/workflows` refs change).
- **`.github/workflows/{release,test}.yml`**: `run:` steps + `paths:` filters (`moondeck/**` → `moondeck/**`), and the two CI-only paths → `moondeck/ci/…`.
- **False-positive guard**: 161 `scripts`-as-substring hits (`description`, `scripting`, `LiveScript`, `livescripts`) must NOT be touched — anchor every replace to `moondeck/` (trailing slash) or the exact `"moondeck"` token, never a bare substring.

## Execution

Scripted sweep (one reusable, revertible script), then full gate verification:
1. `git mv scripts moondeck`
2. `git mv moondeck/ci/package_desktop.py moondeck/ci/` + `verify_version.py` (mkdir `ci/` first)
3. Anchored replaces: `moondeck/`→`moondeck/` in `.py`/`.md`/`.yml`/CMake; `"moondeck"`→`"moondeck"` in the 10 test files; `build/package_desktop.py`→`ci/package_desktop.py` and `build/verify_version.py`→`ci/verify_version.py` in `.github/workflows` + any doc that names them.
4. Update `.claude/settings.local.cleaned.json` allow entries pointing at `moondeck/…` (the gitignored live file re-accumulates).

## Verification (all green before commit)
`check_specs`, `check_devices`, `check_firmwares`, `ctest`, scenarios, `pytest test/python`, `node --test test/js/**`, desktop + one ESP32 build (CMake reaches the renamed dir via `find_program`/`execute_process`), MoonDeck starts + loads `moondeck_config.json` (all card paths resolve), docs site builds (`uv run moondeck/docs/build_docs.py`).

## After
Delete `docs/backlog/rename-scripts-to-moondeck.md` (shipped → git is the record, *Mandatory subtraction*). Mark this plan `(shipped)` once it lands.
