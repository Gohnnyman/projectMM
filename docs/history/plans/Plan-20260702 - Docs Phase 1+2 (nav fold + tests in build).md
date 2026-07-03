# Plan — Docs Phase 1 + Phase 2 (nav fold + generated tests in the build)

Approved-pending. Continues the docs overhaul ([docs/backlog/docs-system-overhaul.md](../../backlog/docs-system-overhaul.md)) on the `next-iteration` branch, on top of the Phase 0 commit. Phases 1 and 2 land together (Phase 1 is mostly already done by Phase 0's nav, so it's a small finish folded into the higher-value Phase 2).

## Phase 1 — finish the audience-split nav (small, mostly done)

Phase 0's `nav:` already splits User guide vs Developer reference and excludes `history`/`backlog`. The remaining Phase-1 work:
- **Group the "Tests" nav section under the user-facing top level** (it currently sits between Effects and Developer reference — confirm that's the right spot for an end-user path; likely yes).
- **No file moves.** Phase 1 was always nav-only. If the current nav already reads top-down cleanly, Phase 1 is declared done with at most a label/ordering tweak — do NOT invent restructuring to justify the phase.

## Phase 2 — fold test-doc generation into the build + surface tests per module

Two deliverables, both leaning on existing infrastructure (`scripts/docs/_test_metadata.py` already parses tests and feeds both the doc generator and MoonDeck; `render_unit_tests()`/`render_scenarios()` in `generate_test_docs.py` are importable pure functions; `cases_for_module()`/`paths_for_module()` already return per-module test data).

### 2a — generate the test pages at build time (stop committing them)
- Add **`mkdocs-gen-files`** (standard MkDocs plugin) with a small `scripts/docs/gen_pages.py` hook that, at `mkdocs build` time, calls the existing `render_unit_tests(collect_unit_files())` / `render_scenarios(collect_scenario_files())` and writes `tests/unit-tests.md` + `tests/scenario-tests.md` into MkDocs' virtual file tree.
- **Delete the committed `docs/tests/unit-tests.md` + `docs/tests/scenario-tests.md`** (~25K words leave the repo — pure subtraction). They regenerate fresh on every build.
- **Retire the commit-time drift gate** for these files: `generate_test_docs.py --check` (CLAUDE.md Event 1 context) and any CI drift check become unnecessary — the pages can't drift when they're built from source every time. Keep `generate_test_docs.py` itself (CLI still useful for a quick local look, and MoonDeck may reference it), but it no longer *writes into the repo* as the source of truth for the site. Decision to settle in review: keep the CLI writing to `docs/tests/` for non-site consumers, or make it print-only. Leaning: keep it writing (MoonDeck/CLI convenience) but drop the CI `--check` gate, and gitignore `docs/tests/*.md` so a stray local run doesn't dirty the tree.
- Add `mkdocs-gen-files` to `build_docs.py`'s inline PEP-723 deps and the CI docs build.

### 2b — "Tests proving this works" on each module page
- A `mkdocs-gen-files` (or a macro via `mkdocs-macros-plugin`) step that, for each module page, injects a **Tests** section listing the unit cases + scenarios that exercise it, sourced from `cases_for_module(name)` / `paths_for_module(name)`.
- This **replaces the hand-authored `[Tests](../../../tests/unit-tests.md#foo)` links** in effects.md/modifiers.md — which were the brittle, drift-prone anchors we just fixed by hand. Auto-generated from metadata, they can't rot: a module with no test shows "no tests yet" (honest), a module with tests shows them. Delivers the PO's "a GitHub issue is solved by adding a test, visible to end users."
- Scope decision for review: inject into the *rendered* page only (via gen-files/macros, source `.md` stays clean) vs. write into the source `.md`. **Leaning: rendered-only** — keeps the source `.md` free of generated blocks (consistent with 2a's "generated, not committed" principle) and avoids a new drift class.

## Why together, why now
- Phase 1 is too small to be its own change; it's nav polish that belongs with the next docs commit.
- Phase 2 is the highest value-to-effort remaining phase and is **subtraction** (deletes 25K committed words + a CI gate + the hand-authored test links). It builds directly on the 24 tests just added.
- Continues on `next-iteration` (PO's workflow: exercise the branch before merging). No dependency on Phase 0 being *merged* — only on its files, which are on the branch.

## Files
- **New:** `scripts/docs/gen_pages.py` (the mkdocs-gen-files hook).
- **Edit:** `mkdocs.yml` (add `gen-files` [+ `macros` if 2b uses it] plugins; nav tweak), `scripts/docs/build_docs.py` (inline dep), `.gitignore` (`docs/tests/*.md`), `docs/moonmodules/light/effects/effects.md` + `modifiers/modifiers.md` (drop the hand `[Tests]` links, replaced by the injected section), CLAUDE.md (drop the test-doc `--check` gate note), `docs/testing.md` (describe build-time generation).
- **Delete:** `docs/tests/unit-tests.md`, `docs/tests/scenario-tests.md` (regenerated at build).

## Verification
- `uv run scripts/docs/build_docs.py` builds; `tests/unit-tests.html` + `tests/scenario-tests.html` exist in `site/` (generated, not from committed source).
- Each effect/modifier page shows its Tests section; a no-test module shows "no tests yet" (none should, after the 24 additions).
- 0 broken anchors; the 254 intentional out-of-`docs/` source-link warnings unchanged (Phase-4 marker).
- ctest/scenarios/spec-check still green (Phase 2 touches no C++; the deleted `.md` were generated artifacts).
- MoonDeck's test view (which uses the same `_test_metadata.py`) still works — shared parser untouched.

## Out of scope (later phases, separate go-ahead)
- Phase 3 (de-dup facts via snippet-includes), Phase 4 (Doxide source drill-down).
