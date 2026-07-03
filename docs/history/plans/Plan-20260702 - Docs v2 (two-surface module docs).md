# Plan — Docs v2: two-surface module documentation

Approved-pending. Executes the redefined [§ Documentation model](../../coding-standards.md#documentation-model) (agreed first, per CLAUDE.md). On `next-iteration`, continuing the docs overhaul.

## Goal

Collapse every module's documentation to **two surfaces** and delete the rest:
1. a hand-written **summary page** per module *group* (end-user, a 4-column table + cross-file prose), and
2. a **generated technical page** per module (`{core,light}/moxygen/<Module>.md`, 100% from the `.h`).

End state: the ~30 per-module standalone `.md` files are gone; each module's story lives in its `.h` (`///` → generated page) and its group summary row. This supersedes the earlier Phase-4b "infra API pages" model (virtual `moonmodules/api/`, `INFRA_HEADERS`), which becomes the general mechanism for *all* modules, retargeted to the domain-nested `moxygen/` dirs.

## Why this ticks the boxes (sanity check vs CLAUDE.md)

- **No duplication / Document once:** a fact lives in the `.h` **or** the summary row, never both, never a third per-module `.md`.
- **Default to subtraction:** the change *deletes* ~30 `.md` files; net-negative doc count.
- **Common patterns first:** the guide-over-generated-reference split is the docs.rs / Sphinx-autodoc / Doxygen pattern (named in the standard).
- **Complexity in core:** the generation machinery (`gen_api.py` + hook) is the one complex piece; every module then gets a page for free.

## Inventory (what moves where)

**Absorb into `.h` `///` + a summary row, then delete (~29 files):**
- **Core (12 + `ui.md`):** AudioModule, Control, DevicesModule, FilesystemModule, FirmwareUpdateModule, HttpServerModule, I2cScanModule, ImprovProvisioningModule, MoonModule, NetworkModule, Scheduler, SystemModule — each maps 1:1 to a `src/core/*.h`. `ui.md` has no `.h` (it documents the UI *system*, not a module) → its content folds into the core-UI summary page's prose, not a `///`.
- **Light supporting (10):** Buffer, MappingLUT, Layer, ModifierBase, Layouts, Layers, BlendMap, Drivers, EffectBase, LightConfig — each maps to a `src/light/**/*.h`.
- **Catalog detail (7):** NetworkSendDriver, RmtLedDriver, LcdLedDriver, HueDriver, ParlioLedDriver, PreviewDriver, MoonLiveEffect — already card-backed; their cross-file prose (where any) moves to a summary-page section, their `///` gets enriched, the detail `.md` is deleted.

**Keep (the 4 catalog summary pages, restructured to link moxygen):** `light/{effects,modifiers,layouts,drivers}/<type>.md`.

**Create (3 new summary pages, nested per the standard):**
- `core/ui/` — core UI modules (FileSystem, System+Audio+I2C, FirmwareUpdate, Network+Improv+Devices).
- `core/supporting/` — core supporting (Control, Scheduler, MoonModule, HttpServer).
- `light/supporting/` — light supporting (the 10 above).

## Staged execution (curated proof first, then sweep)

**Stage 1 — machinery (retarget the generator, one change):**
- `gen_api.py`: replace `INFRA_HEADERS` (flat list → `moonmodules/api/`) with a per-domain resolver that emits `moonmodules/core/moxygen/<Module>.md` and `moonmodules/light/moxygen/<Module>.md`. The header list broadens to *all* documented modules (core + light), discovered from `src/{core,light}` rather than hand-listed, so it can't drift as modules are added.
- `mkdocs_hooks.py`: `_API_MODULES` + `_rewrite_out_of_docs_links` retarget `.h` links to the new `{domain}/moxygen/<Module>.md` path (currently hardcodes `moonmodules/api/`). The domain is derivable from the module's src path.
- `.gitignore`: `docs/moonmodules/*/moxygen/` (gitignored, per the standard).
- Verify: build generates pages under both `core/moxygen/` and `light/moxygen/`; a summary link lands on one.

Re-staged (PO): ship a **working system first** (all generated pages + summary pages, old `.md` kept), commit that as a complete baseline, *then* optimize the generated pages, and delete the old `.md` only at the very end. De-risks: a committable, coherent system exists before any enrichment or deletion.

**Stage 2 — machinery + template shape (done):**
- **Moxygen template tuning.** Private members leaked (`Private Attributes`/`Private Methods`) because Doxygen's XML backend emits documented privates regardless of `EXTRACT_PRIVATE`. Fixed with a Handlebars denylist in `class.md` on the raw section `kind` (moxygen's own lever, no post-processing) — public-only reference.
- **`.md` on disk, standard flow.** `gen_api.generate()` writes each page to `docs/moonmodules/{domain}/moxygen/<Module>.md` (gitignored) so a human previews the `.md` directly and MkDocs discovers it as a normal source file.
- **Temporary cross-check header.** Each generated page opens with a `> _Migration cross-check (temporary):_` line linking the source `.h` and (while it still exists) the original `.md`, so a reviewer can confirm the `.md`'s content was absorbed. Removed at Stage 4.
- **Proof on all three flavours:** Control (core supporting), Buffer (light supporting), PreviewDriver (catalog) `///`-enriched incl. per-method descriptions.

**Stage 3 — "working system" (→ commit):**
- Generate all 132 pages (cross-check header on each); build the 3 summary pages (core-UI, core-supporting [done], light-supporting), same `### `-block → 4-col-table transform every summary page uses.
- **Keep every old `.md`.** No deletions, no retargeting of old pages' links. The new system lands *alongside* the old — a complete, committable baseline. **Commit here.**

**Stage 4 — optimize (incremental, per module):**
- Sweep the `///` comments module-by-module so each generated page reads as excellent developer docs (consolidate existing `//` into `///`, add per-method/attribute descriptions, fold the old `.md`'s cross-file rationale into the class `///` or the summary page). Cross-check against the still-present old `.md` via the temporary header.
- Apply the two rendering gotchas from the standard on every `///` touched (backtick `<…>`, no mid-sentence `.`).
- **Control-name drift guard.** A summary page's `- name — …` param lines are hand-authored (the controls come from runtime `controls_.add("name", …)` calls no static tool sees — same reason catalog cards hand-author them). They can drift when a control is renamed in the `.h`. Extend `check_specs.py` (which already validates numeric *ranges* between `.h` and catalog docs) to also validate that each summary-page param *name* matches a real `controls_.add("<name>", …)` in the module's source — so a renamed/removed control fails the check. Applies to every summary page with a Parameters column (Core UI + the few supporting modules with controls).

**Stage 5 — switchover + reconciliation (last, atomic):**
- Once the generated pages are good: **atomically** delete all ~29 old per-module `.md`, retarget every inbound link (links from other deleted `.md` vanish with them; links from permanent docs — architecture.md, surviving summary pages — repoint at the summary page or the generated page), and remove the temporary cross-check header from `gen_api.py`.
- `mkdocs.yml` nav (34 per-module entries): replace the per-module page list with the summary pages + a generated-pages note (moxygen pages are link-reachable, not nav-listed — same policy as history/backlog).
- `check_specs.py`: exemption path already updated to `{domain}/moxygen/` + discovery list (Stage 2).
- **architecture.md line ~112** ("Each MoonModule is documented in `docs/moonmodules/` as it is built") → rewrite to the two-surface model, so the two docs agree (PO-approved to land in this change).
- Clean the stale Phase-4b `api/` naming in `gen_api.py`'s docstring.

## Files

- **Edit:** `scripts/docs/gen_api.py` (domain-nested output + module discovery), `scripts/docs/mkdocs_hooks.py` (link retarget), `scripts/check/check_specs.py` (exemption path), `mkdocs.yml` (nav), `.gitignore` (moxygen dirs), `docs/architecture.md` (line ~112), the ~29 `src/**/*.h` (enrich `///`), the 4 catalog summary pages (link retarget).
- **New:** 3 summary pages (`core/ui/`, `core/supporting/`, `light/supporting/`).
- **Delete:** ~29 per-module `.md` (12 core + `ui.md` + 10 light-supporting + 7 catalog-detail — as each is absorbed), `docs/poc/`.

## Out of scope (named, not silently dropped)

- **Committing the moxygen output** — stays gitignored; the commit+drift-gate is a later one-line flip if PR-review of generated docs earns it.
- **Test-doc generation via moxygen** — the standard already rules it out (unit tests are macros, scenarios are JSON); `generate_test_docs.py` is untouched.
- **Splitting catalog pages** (`effects_wled.md` / `effects_moonmodules.md`) — future, when a page gets too long.

## Verification

- `docs/moonmodules/` contains only summary pages (no per-module `.md`); every module has a `{domain}/moxygen/<Module>.md` at build.
- A summary link lands on the generated page in-site; no broken nav/links (mkdocs build clean).
- Enriched `.h` compile clean (`///` inert); `ctest` + scenarios green; `check_specs.py` green.
- architecture.md and coding-standards.md agree on the doc model (no contradiction).
- Net doc-file count **down** (~29 deleted, 3 created).
- PO reads a generated page (Control) + a summary page and judges them complete.
