# Plan — Docs site Phase 0 (MkDocs Material at Pages root)

Approved 2026-07-02. The full multi-phase design study lives in [docs/backlog/docs-system-overhaul.md](../../backlog/docs-system-overhaul.md); this plan is **only Phase 0** — the additive site-standup. Phases 1–4 (nav split, generated-test folding, fact de-duplication, Doxide source drill-down) each need a separate go-ahead and are not built here.

## Goal

End users currently read 259K words of docs as raw `.md` on github.com — no landing page, no nav, no search, because **GitHub Pages today publishes only the web installer** (`docs/install/`), never the docs. Phase 0 gives the existing docs a rendered front door with zero content changes and zero file moves.

## What ships

1. **`mkdocs.yml`** at repo root — Material for MkDocs, instant search, `pymdownx.snippets` (for later phases), and a `nav:` tree imposing a top-down end-user → developer order over the *existing* files. No file is moved or rewritten; `nav:` just orders what's there. `history/` and `backlog/` stay out of the published nav (internal).
2. **`docs/index.md`** — the landing page end users lack: what projectMM is → a prominent "Flash an ESP32" call-to-action routing to `/install/` → first-light → effects → developer drill-down. Preserves every affordance of the old `docs/landing/index.html` (Flash button, GitHub/Releases links) but as the docs home, with Docs/Getting-started links now resolving to *rendered* pages instead of github.com blobs.
3. **`moondeck/docs/build_docs.py`** — a PEP-723 (`# /// script`) uv wrapper around `mkdocs build`, matching the existing `moondeck/docs/` convention and the uv-everywhere rule. (As shipped: CI runs a plain `build`, which fails on missing pages / bad nav; broken links and stale anchors stay `warn`-level per the `validation:` block in `mkdocs.yml`. `--strict` is a *local* anchor-audit option, documented in `moondeck/MoonDeck.md`, not the CI gate — the intentional out-of-`docs/` source links would make `--strict` fail the build.)
4. **CI wiring** — the existing `deploy-pages` job (in `.github/workflows/release.yml`, gated to `main`) builds the MkDocs site into `pages/` root **instead of** copying the single `docs/landing/index.html`. `pages/install/` staging is untouched. Add `docs/**` + `mkdocs.yml` to the workflow's `paths:` so a docs-only change to `main` redeploys.

## Decisions locked (PO)

- **Scope:** Phase 0 only; later phases separately approved.
- **URL:** docs at Pages root `/`; installer stays at `/install/` (no installer links break; only `mkdocs.yml` is added to the repo — the site is assembled in CI into a throwaway dir, exactly as the installer already is, so no new repo folders).
- **Doxide comment style:** approved for the eventual Phase 4 (recorded for later).

## Out of scope (deferred to later phases)

- De-duplicating facts (control names, attribution, ranges) — Phase 3.
- Folding generated test docs into the build / surfacing tests on module pages — Phase 2.
- Doxide source drill-down — Phase 4.
- Any rewrite, move, or deletion of existing `.md` content.

## Verification

- `uv run moondeck/docs/build_docs.py --strict` builds locally with no warnings; every nav entry resolves; search index generates.
- Old `docs/landing/index.html` retired (its Flash button + links live on in `docs/index.md`); `check_specs.py` + the doc-generation `--check` still green (Phase 0 touches no specs/tests).
- CI `deploy-pages` publishes the rendered docs at `/`, installer still reachable at `/install/`.

## Files

- **New:** `mkdocs.yml`, `docs/index.md`, `moondeck/docs/build_docs.py`.
- **Edit:** `.github/workflows/release.yml` (build MkDocs into `pages/` root; add docs paths to `paths:`).
- **Delete:** `docs/landing/index.html` (folded into `docs/index.md`) — and drop its `docs/landing/**` from the workflow `paths:`.

## Landed alongside: `docs/install/` → top-level `web-installer/`

During implementation the PO asked to fix the deeper structural issue the site standup surfaced: `docs/` held three unlike things — doc-site source, a standalone web app (`install/`), and transient internal notes (`history/` + `backlog/`). Decision (PO, "go for the best, not to keep technical debt" — a young project optimizes for the end state):

- **Moved the installer app out of `docs/`** to a top-level `web-installer/` (`git mv`, history preserved). It's an application, not documentation — the miscategorization worth fixing permanently. The **deployed URL stays `/install/`**: the release workflow maps `web-installer/` → `pages/install/`, so QR codes, deployed-device OTA URLs, and existing links keep working (zero external breakage).
- **Left `history/` + `backlog/` in `docs/`**, excluded from the site (`exclude_docs`). They're transient — slated for compaction — so relocating them would be discarded churn; keeping them out of the *published* site is all the reader-facing goal needs.

Swept ~104 references (`docs/install/` → `web-installer/`) across scripts, both CI workflows, three check scripts, MoonDeck, JS + Python test suites, CLAUDE.md commit-gate triggers, and published docs. **Gotcha caught:** a literal-path regex misses split-component construction — `join(ROOT, "docs", "install", …)` (3 JS tests) and `"docs" / "install"` (moondeck.py) needed separate patterns; verified by running the JS/Python suites (all green).

Deferred to a **separate next commit** (PO): top-level `scripts/` → `moondeck/` — larger orthogonal sweep, spec'd in `docs/backlog/rename-scripts-to-moondeck.md`.
