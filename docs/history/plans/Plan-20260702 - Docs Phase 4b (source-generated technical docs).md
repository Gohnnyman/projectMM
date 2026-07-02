# Plan — Phase 4b: source-generated technical docs (the inversion)

Approved-pending. The final phase of the docs overhaul ([docs/backlog/docs-system-overhaul.md](../../backlog/docs-system-overhaul.md)), on `next-iteration`, after the Doxide→moxygen tool evaluation (see the pilot plan for why moxygen won).

## The inversion (PO directive)

**Prerequisite: all *technical* documentation is generated from source code.** Turn the reasoning around — the source `.h` is the single home of technical content; the site's technical pages are *generated views* of it. No hand-written `.md` restates what a `///` comment can carry.

This extends the model already proven for **catalog** modules (effects/modifiers/layouts/drivers): their cards are generated (from the prose `### ` blocks + the runtime `controls_.add` calls) by `mkdocs_hooks.py`. Now **infrastructure** modules (core services + light-base) get the same treatment via **moxygen** (Doxygen → Markdown), generated at build time by the same hook.

### The two module kinds, both source-generated (unchanged distinction, now symmetric)

- **Catalog module types** (effects/modifiers/layouts/drivers): end-user **cards**, generated from the catalog `.md` prose blocks + the `controls_.add` calls. The card is the whole story; a `⌄ details` anchor + links sit in the Links column. **Unchanged** — already source-driven.
- **Services & infrastructure** (core `*Module`, light-base `Layer`/`Buffer`/`MappingLUT`/…): a **generated API page** per module, produced by Doxygen+moxygen from the `///` comments in the `.h`, at build time. This is the new work.

## Build-time generation (not runtime — MkDocs is static)

MkDocs → flat HTML on GitHub Pages; no server, so no click-time generation. Same outcome at **build time**: `mkdocs_hooks.py` runs Doxygen+moxygen during the build and injects each infra module's generated API page into the virtual file tree (exactly how it already injects `tests/unit-tests.md`). The card / overview links point to those pre-generated pages. To the reader: click the link → see the generated doc. Identical UX, works on static Pages.

## Deliverables

### 1. Generated infra API pages (the core inversion)
- Extend `mkdocs_hooks.py`: an `on_files` step that runs Doxygen (a `Doxyfile` scoped to a curated **infrastructure file list** — start with core/ + light-base, ~24 files, excluding catalog effects/modifiers/layouts/drivers) → moxygen (with the **compact custom template** from the pilot: no path leak via `STRIP_FROM_PATH`, no `--anchors`, member tables not duplicated) → inject the resulting `.md` under `moonmodules/api/<Module>.md` into the virtual tree. Generated fresh each build; never committed.
- Graceful skip where `doxygen`/`npx` are absent (like the installer-staging serve-only guard) so a contributor without doxygen can still build the rest of the site — the API pages just don't generate locally (they do in CI). Log clearly.
- CI: `apt install doxygen` in the deploy-pages job; `npx moxygen` needs Node (already on the runner). The one justified non-uv exception (like ESP-IDF), stated at the introduction site.

### 2. Retarget `.h` links → generated API pages
- The card's `[.h]` link (currently rewritten to a GitHub blob URL by `_rewrite_out_of_docs_links`) instead points to the module's **generated API page** where one exists (`moonmodules/api/<Module>.html`), falling back to the GitHub blob URL for files with no generated page. So "click .h" → the in-site generated reference, not raw GitHub.
- Same for the per-module overview pages' source links.

### 3. Prior art cleanup (PO directive)
- **projectMM v1/v2 prior art: removed entirely** (already done across 13 docs — verify none remains, incl. any that crept back).
- **MoonLight prior art: kept but demoted** — it lives only in the **end-user-facing** overview/card `.md` (as an `Origin:`/lineage line), NOT in the technical/generated layer. The generated API pages carry no prior art (Doxygen doesn't emit it; good).

### 4. Enrich Control.h + FireEffect.h to "perfect documentation" (the pilot's finish)
- Fold the current `Control.md` / FireEffect-card *technical* content into `///`/`///<` comments in the source, so the generated page is complete enough that a developer reads it and thinks "I understand exactly what this does." (Started in the pilot — the `ControlType` value table already generates from `///<`.)
- What CAN'T move to source stays in a thin overview `.md`: cross-file design rationale (Control's Memory/Persistence/Design sections), the 4-column Type×Storage×UI×DMX *matrix* (a format moxygen's flat enum table can't produce — keep as a hand-authored table in the overview), and the MoonLight lineage.
- FireEffect: its description folds into the class `///`; the **card keeps** the GIF + control ranges/user-descriptions (runtime `add()` calls moxygen can't see — catalog stays card-first).

## Where each kind of content lives (PO: `///` in the `.h` FIRST, `.md` only as secondary)

The ordering is strict — push everything into the source that can go there:
1. **The module's overview/description → the class `///` comment.** It generates into the API page AND shows on IDE hover. This is the primary home, not a `.md`.
2. **Per-entity technical content** (methods, enums + values, signatures, ranges) → `///`/`///<` in the `.h`. Generated.
3. **A hand-written `.md` ONLY as a last resort**, for what genuinely cannot live in one source file:
   - **Cross-file design rationale** (module interactions, buffer-lifecycle coupling) — no single `.h` owns it.
   - **Format-specific tables** moxygen can't emit (Control's 4-column Type×Storage×UI×DMX matrix).
   - **End-user prose + MoonLight lineage** — the card / a thin overview.
   A module whose entire story fits in `///` comments has **no `.md`** — just its generated API page.

## Files
- **Edit:** `scripts/docs/mkdocs_hooks.py` (Doxygen+moxygen generation + link retargeting), `mkdocs.yml` (nav for the generated API section; moxygen template path), `.github/workflows/release.yml` (`apt install doxygen`), the curated infra `.h` files (progressively `///`-enrich, starting Control.h + a few), the shrunk infra overview `.md` (Control.md → cross-file-only + matrix + lineage), `docs/coding-standards.md` (§ Documentation model: record the inversion + the moxygen mechanism, fill the "autodoc TBD" placeholder), CLAUDE.md pointer.
- **New:** a committed `Doxyfile` (scoped, compact settings) + the moxygen custom template under `scripts/docs/`.

## Verification
- Build generates an API page per infra module; a card `.h` link lands on it in-site; `Control` page reproduces Control.md's type reference from `///<`.
- Doxygen absent locally → build still succeeds (API pages skipped, logged); present in CI → pages appear.
- No v1/v2 anywhere; MoonLight only in end-user `.md`.
- Enriched `.h` compile clean (`///` inert); ctest green.
- A developer reviewing the generated Control + FireEffect pages judges them complete.

## Staged rollout (not big-bang)
- **This commit:** the generation machinery + retargeting + Control.h/FireEffect.h enriched + Control.md shrunk, as the proof. A *curated* infra file list (Control, the frame types, a couple of light-base), not all 24.
- **Later, incremental:** `///`-enrich more infra headers as they're touched (the "replaced as files are touched" pattern), growing the generated set; shrink each infra overview `.md` to cross-file-only as its source gets enriched.
