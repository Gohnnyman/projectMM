# Documentation system overhaul — investigation + phased proposal

Forward-looking design study (per CLAUDE.md, `docs/backlog/` is exempt from present-tense). Written in response to the product-owner brief: the docs are ~259K words across 19.5K lines of `.md`; end users can't navigate them, a small change touches source + tests + docs at once, and technical detail lives in `.md` rather than in the code. Goals: **no duplication**, a **top-down end-user path**, **easy developer drill-down into `.h`/`.cpp`**, and **tests visible to end users** — all hosted on GitHub Pages.

Follows the *Refactor for simplicity* process rule: alternatives enumerated, gains/losses named, leanest option recommended, presented as a proposal — nothing moves until the PO picks.

## What we have today (measured)

- **~259K words / 19.5K lines** of `.md`. Biggest buckets: `docs/history/` (97K words, incl. 47 plan files), `docs/backlog/` (57K), `docs/moonmodules/` (38K), `docs/tests/` (25K, **auto-generated**), top-level `docs/` (29K).
- **GitHub Pages today publishes only the web installer** (`docs/install/`), *not the docs*. So the docs are read as raw `.md` on github.com — no nav, no search, no landing page. This is the root of "impossible for end users to read": **there is no doc *site* at all yet.**
- **One generation loop already works and proves the model:** `scripts/docs/generate_test_docs.py` reads test metadata (`// @module` tags in `test/unit/*.cpp`, JSON fields in `test/scenarios/*.json`) and emits `docs/tests/{unit,scenario}-tests.md`. The same parser (`_test_metadata.py`) feeds MoonDeck's `/api/tests` UI. Source of truth = the test file. This is the pattern to extend, not replace.
- **Duplication hotspots** (each fact lives in N places, changing one forces the others):
  | Fact | Lives in | Sync today |
  |---|---|---|
  | Control name (`"sparking"`) | `.h` (definition) + `.md` spec + test code | `check_specs.py` checks *presence* in `.md`, not accuracy |
  | Control range/default (8000/16000/22050/44100) | `.h` array + `.md` prose | none — hand-copied |
  | Author/attribution | `.h` `// Author:` + `.md` `Origin:` (40+ effects, two formats) | none |
  | Module name (`FireEffect`) | class + `registerType(...)` string + test `@module` + `.md` anchor + `deviceModels.json` | `check_specs`/`check_devices` partial |
  | Architectural fact (buffer persists; AudioModule respects `enabled`) | `.h` comment + module `.md` + `architecture.md` | none |
- **No structured doc-comments in source.** `.h`/`.cpp` carry rich *inline `//` rationale* but no Doxygen/`///` API blocks. So "move technical detail into the code" is a real move, not a relabel.

## The core insight

Two different problems wear the same "docs too big" coat, and they have **opposite** fixes:

1. **Navigation / readability** (end users). Fix = a rendered site with a top-down nav and search. Additive: nothing is deleted, the raw `.md` gets a front door. **Cheap, immediate, low-risk.**
2. **Duplication** (developers). Fix = single-source each fact and *generate* the copies (or include them). Subtractive and invasive: it changes where facts live and how they're authored. **Do incrementally, proven per fact-type before rollout.**

Phase them in that order: the site is the quick win that makes everything else visible; de-duplication is the slow structural win. Do **not** bundle them — a big-bang "new site + moved all facts into code" is the kind of change *Refactor for simplicity* exists to stop.

## The tools (all GitHub-Pages-native)

| Tool | Role | Why it fits here |
|---|---|---|
| **Material for MkDocs** | The site: nav tree, instant search, versioning. | PO-named default; the recognised standard (WLED-adjacent projects, FastLED-ecosystem, thousands of firmware projects use it). Ships to Pages via one CI job. `nav:` in `mkdocs.yml` *is* the top-down structure. |
| **`pymdownx.snippets`** (`--8<--`) | De-dup mechanism #1: pull real source lines into a doc. | Built into Material. A spec can embed the actual `controls_.addX(...)` block or an author comment *from the `.h`* — one source of truth, rendered in two places. No new tool. |
| **`mkdocs-gen-files` / `mkdocs-macros`** | De-dup mechanism #2: generate whole doc pages from data at build time. | Standard MkDocs plugins. Lets `generate_test_docs.py`-style generation run *inside* the site build instead of committing generated `.md`. Kills the "forgot to regenerate" drift. |
| **Doxide** | Developer drill-down: parse `.h`/`.cpp` with Tree-sitter → **Markdown** → rendered *in the same Material site*. | This is the ".h/.cpp viewer layered on top" the PO asked for. Unlike classic Doxygen (1998-era HTML, a separate ugly site), Doxide emits Markdown that lives in *our* nav, *our* search, *our* theme — developers drill from a module's user page straight into its annotated source, one site. |

Rejected: **classic Doxygen HTML** (separate site, dated UI, not integrated), **Sphinx/Breathe** (Python-doc-shaped, heavier, C++ via Breathe is awkward), **Docusaurus/VitePress** (Node toolchain, no C++ story), **mdBook** (great but no C++ integration, weaker search). Material+Doxide is the least bespoke combination that hits all four goals on Pages.

## Phased transition

### Phase 0 — Stand up the site (no content changes) — *now, ~half a day*
- Add `mkdocs.yml` with Material, `pymdownx.snippets`, instant search. Author a `nav:` tree that imposes the **top-down end-user order** (see below) over the *existing* files — no file moves, no rewrites.
- Add a `build-docs` CI job that `mkdocs build`s and publishes to Pages **alongside** the installer (installer already owns `/install/`; docs take `/` or `/docs/`).
- Add a **landing page** (`docs/index.md`) — the front door end users currently lack: "what is this → install → first light → effects → drill down."
- **Outcome:** every existing word is now navigable + searchable, zero duplication introduced, zero risk. This alone solves the *"impossible for end users to read"* complaint.

### Phase 1 — Restructure nav for the two audiences — *small*
- Split the nav (not the files, yet) into **User guide** (README intro, getting-started, effect catalog, per-board install) and **Developer/Reference** (architecture, coding-standards, module specs, generated tests, source-drill). `history/` + `backlog/` stay out of the published nav (internal).
- Move the *most* end-user-hostile prose (deep architecture) below a "Developer" fold so a user's top-down path never hits it unless they drill.
- **Outcome:** the "top-to-down, easy navigate" structure, still additive.

### Phase 2 — Fold generated tests into the site + surface them to users — *small, high-value*
- Run the existing test-doc generation *at site-build time* via `mkdocs-gen-files` (stop committing `docs/tests/*.md` — the 25K generated words leave the repo, generated fresh each build). Kills the "forgot to regenerate" drift class entirely.
- On **each effect/module user page**, auto-embed "Tests proving this works: …" from the same test metadata — so an end user reading about *Fire* sees the tests that pin it. This is the PO's *"github issues will be solved adding a new test to proof it, this should be visible to end users."* The link from issue → test → visible-on-the-module-page becomes the norm.
- **Outcome:** tests visible to users; 25K words of committed generation deleted (subtraction).

### Phase 3 — De-duplicate facts, one fact-type at a time — *incremental, the slow win*
Prove each on **one module**, then sweep. Order by leverage:
1. **Author/attribution** → single source in the `.h`, `--8<--` snippet-include into the `.md`. Deletes 40+ hand-maintained `Origin:` copies. (Lowest risk: it's a comment.)
2. **Control names + ranges/defaults** → the `.h` `controls_.addX(...)` block is already the source of truth; either snippet-include it, or extend `check_specs.py` into a *generator* that emits the control table into the spec. Deletes the hand-copied range prose; upgrades `check_specs` from "checks presence" to "owns the table."
3. **Cross-doc architectural facts** → pick the one true home (usually `architecture.md` or the `.h`), replace the other copies with a link/anchor per *Document a thing once, reference it generically* (already a principle — this enforces it mechanically).
- **Outcome:** "change a small thing, many files change" shrinks to "change the source, the copies regenerate."

### Phase 4 — Developer drill-down into source — *medium, do last*
- Add **Doxide**: annotate `.h` public API with its lightweight comment style, generate Markdown into the Developer section of the site. Start with `core/` (the stable base), then light domain.
- This is where "technical details in the code, not `.md`" lands: the module `.md` shrinks to *wire contracts + cross-wiring + prior art* (its already-stated job per CLAUDE.md), and the *API-level* detail is generated from annotated source.
- **Outcome:** developers drill user-page → spec → annotated source, one site, search across all of it.

## Alternatives considered (for the record)

- **A — Site only, never de-dup.** Solves navigation, ignores the duplication complaint. Rejected: PO's *main* goal is no-duplication.
- **B — De-dup first, site later.** Restructures authoring before anyone can see the payoff; high risk, no visible win for weeks. Rejected: wrong order.
- **C — Big-bang Material + Doxide + full de-dup in one branch.** Everything at once. Rejected by *Refactor for simplicity*: unreviewable, all-or-nothing.
- **D (recommended) — Phased: site now (additive, safe), de-dup incrementally (proven per fact-type), Doxide last.** Each phase ships a visible win and is independently revertible.

## Decisions locked (PO, 2026-07)

- **Scope:** Phase 0 approved (stand up the site). Phases 1–4 each need a separate go-ahead.
- **Site URL:** docs at Pages root `/`; installer stays at `/install/`. The site is assembled in CI into a throwaway dir; the repo addition is `mkdocs.yml`.
- **`docs/` de-overloading (landed with Phase 0):** `docs/` had held three unlike things. The standalone web installer moved out to a top-level **`web-installer/`** (it's an app, not docs; deployed URL unchanged at `/install/`). The transient `history/` + `backlog/` stay in `docs/` but excluded from the site — they're compaction-bound, so relocating them is discarded churn. Result: `docs/` = published doc-site source + transient internal notes (excluded).
- **`scripts/` → `moondeck/`:** approved, but deferred to its own next commit (see `rename-scripts-to-moondeck.md`).
- **Doxide comment style:** approved as the source-annotation convention for Phase 4 (Doxygen-family, recognised standard).

## Open questions for the PO

1. **Site URL layout:** docs at Pages root `/` with installer under `/install/` (docs are the front door), or docs under `/docs/` keeping `/` as the installer landing? (Recommend: docs at `/`, a prominent "Install" button routing to `/install/`.)
2. **Doxide comment style** is a new per-`.h` convention — acceptable as a *recognised* standard (it's Doxygen-family), or do we want inline `//` to stay the only comment form? (Affects Phase 4 only.)
3. **Scope of Phase 0 approval:** stand up the site now (safe, additive) and defer 1–4 for separate go-aheads, or approve the whole arc?

## Source

- Investigation basis: `scripts/docs/generate_test_docs.py`, `scripts/check/check_specs.py`, `.github/workflows/release.yml` (Pages deploy), the `docs/` tree.
- Prior art / tooling: [Material for MkDocs](https://squidfunk.github.io/mkdocs-material/), [pymdownx snippets](https://facelessuser.github.io/pymdown-extensions/extensions/snippets/), [Doxide](https://github.com/lawmurray/doxide).
