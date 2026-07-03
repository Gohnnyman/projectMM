# Rename top-level `scripts/` → `moondeck/`

Forward-looking (backlog is exempt from present-tense). Decided by the PO on 2026-07-02, to be done as its **own separate commit** in the next cycle — deliberately NOT folded into the docs-site / `web-installer/` commit (that one is scoped to the `docs/` separation; this is a larger orthogonal sweep and stays isolated for a clean, revertible diff).

## Why

Consistency with the `web-installer/` move: top-level folders should name what they hold. MoonDeck is the human-facing dev console (`uv run moondeck.py`); the folder is its home.

> Note (agent pushback recorded, PO overrode): `scripts/` is the more *recognizable* convention and it holds more than MoonDeck (`build/`, `check/`, `docs/`, `run/` groups the CLI gates invoke directly — see CLAUDE.md § Build). The rename narrows an accurate name. PO's call stands; captured here so the trade-off is on record, not to relitigate.

## Scope (measured)

~77 files / ~376 occurrences of the string `scripts/`. Load-bearing categories:

- **CLAUDE.md** — every gate command (`uv run scripts/check/…`, `scripts/build/…`, `scripts/docs/…`), the Build section, the `scripts/**` commit-gate triggers.
- **Both CI workflows** — `.github/workflows/{release,test}.yml`: `run:` steps, `paths:` filters (`scripts/**`, `scripts/build/**`).
- **CMake** — `find_program(UV…)` + `execute_process` / `add_custom_command` that shell into `scripts/…`, and `src/ui/embed_ui.cmake`.
- **`moondeck_config.json`** — its own `"script": "<group>/<name>.py"` entries (relative to the scripts dir; check whether the loader prepends `scripts/` or the config is already relative).
- **Cross-references** in `scripts/MoonDeck.md`, `docs/building.md`, module specs, and every script's own docstring/help text.
- **The deferred/allow lists** in `.claude/settings.local*.json` (gitignored live file re-accumulates; the tracked `.cleaned.json` needs updating).

## The gotcha that WILL bite (learned from the `web-installer/` sweep)

A naive `s{scripts/}{moondeck/}` regex **misses split-component path construction**. The `web-installer` sweep left three JS test files broken because they built the path as `join(ROOT, "docs", "install", …)` — separate string args, not the literal `docs/install/`. The equivalent here:

- **Python:** `Path(...) / "scripts" / ...`, `os.path.join(ROOT, "scripts", ...)`.
- **JS:** `join(ROOT, "scripts", ...)`.
- **CMake:** `${CMAKE_SOURCE_DIR}/scripts` and `"${CMAKE_SOURCE_DIR}" "scripts"` forms.

Grep for **all** of these split forms (`"scripts"`, `'scripts'`, `/ "scripts"`, `, "scripts"`) BEFORE running the literal-path replace, or things break silently and only a test run catches them.

Also protect against false positives: any identifier or word containing `scripts` that is NOT the folder (e.g. `livescripts`, `ESPLiveScript`, `description`, `subscripts`) — anchor the replace to `scripts/` with trailing slash and to the split-string forms, never a bare substring.

## Verification (before the commit lands)

- All gates green: `check_specs`, `check_devices`, `check_firmwares`, `ctest`, scenarios, `uv run --with pytest … test/python`, `node --test test/js/**`.
- Desktop + at least one ESP32 build (CMake reaches the renamed scripts via `find_program`/`execute_process`).
- MoonDeck starts and loads its config (`moondeck_config.json` script paths resolve).
- Docs site builds (`uv run <newpath>/docs/build_docs.py`).
- The gitignored `.claude/settings.local.json` allow-list entries pointing at `scripts/…` still match (they re-accumulate; the tracked `.cleaned.json` is the baseline to update).

## Decision

- **Separate commit, next cycle.** Not in the docs-site commit.
- Delete this backlog note once the rename ships (per *Mandatory subtraction* — the git commit is the record).

## Source

- Basis: the reference sweep done for the `web-installer/` move (this session); `git grep scripts/` for the live count.
