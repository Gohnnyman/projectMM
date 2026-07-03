# Plan — Docs Phase 4b: Doxide pilot (evidence-first)

Approved-pending. The autodoc phase of the docs overhaul ([docs/backlog/docs-system-overhaul.md](../../backlog/docs-system-overhaul.md)), on `next-iteration`.

## What the Doxide evaluation actually showed (built + ran it, 2026-07)

Before planning, Doxide was **built from source and run on real projectMM headers**. Findings — these ground the whole plan:

1. **Doxide renders ONLY Doxygen-commented entities.** Given our plain `//` comments it produced an empty page. A `struct` with a `/** */` comment produced a clean per-class Markdown page (member table, Material admonitions) — genuinely nice, MkDocs-integrated output. **So Doxide needs the comment style converted to `/** */` / `///` first.**
2. **Its Tree-sitter parser choked on real C++20** on the first core file — `auto** newArr = new MoonModule*[n]` and `(c->*fn)()` both "parse error, will continue". Doxygen wouldn't. The own-parser fragility is real and showed up immediately.
3. **Adoption is near-zero** (author's own projects; single maintainer; ~1.3k stars). Low lock-in though — output is Markdown, so if Doxide dies we keep the `.md`.

Conclusion: a full Doxide rollout (convert ~139 headers, risk parser failures across the codebase) is high-cost, high-risk, and premature. But the *output quality on clean, commented code is good*. So: **pilot it on a small, clean, comment-converted subset, in-site, and decide expansion on the real result** — not on reputation, not on a blanket sweep.

## Pilot scope (deliberately minimal)

**Convert 2–3 clean `core/` headers to Doxygen-style comments and render them into the site with Doxide. Nothing else.** No CI wiring yet (local build only for the pilot); no touching the 139; no C++20-heavy files.

Candidate headers (verified clean of the constructs that choked the parser — no `auto**` / pointer-to-member): `ImprovFrame.h`, `AudioFrame.h`, `AudioLevel.h`, `Control.h`, `Scheduler.h`. Pick **2–3** that are genuinely useful to see as API docs (leaning `ImprovFrame.h` — already snippeted, so we compare snippet-vs-Doxide on the same file — plus `Control.h` or `Scheduler.h`).

### Steps
1. **Comment conversion (bounded).** On the 2–3 pilot headers only, convert the class/struct/key-method `//` comments to `/** */` / `///` where it adds value. Keep it light — the parser reads the *structure*; a one-line `/** */` per public entity is enough to start. Note the diff size as the per-file cost signal for a future sweep.
2. **`doxide.yaml`** at repo root (or `docs/`) listing just the pilot files, `output:` into a `docs/api/` (or a virtual dir).
3. **Local render + eyeball.** Build with the local Doxide binary; inspect the generated `.md` in the Material site (drop it under a `Developer / Source (pilot)` nav entry). Compare to: (a) the GitHub source link, (b) the Phase-4a snippet, on `ImprovFrame.h`.
4. **Assess against criteria (below); write the verdict into this plan.** Decide: expand (and how far), or stop at pilot / drop.

### NOT in the pilot
- CI integration (the from-source CMake build in the ubuntu deploy-pages job) — deferred until the pilot proves the output is worth it. If the pilot passes, CI wiring is its own step (build-and-cache the binary, or vendor a prebuilt Linux binary — the one justified uv-exception, like ESP-IDF).
- Converting more than 3 headers.
- Any C++20-heavy / template-heavy header (parser risk).

## Decision criteria (write the verdict here after the pilot)
- **Output quality:** does the rendered API page beat the GitHub source link for a reader? (If not → drop; the links already work.)
- **Comment cost:** how invasive was the `/** */` conversion per file? Extrapolate to 139.
- **Parser robustness:** did it choke on any pilot header? How often would it across the codebase?
- **CI feasibility:** is the from-source build cacheable/reliable enough to add to deploy-pages?

Only if all four clear does a wider rollout make sense. A likely outcome given the evaluation: **Doxide for `core/` (stable, clean, the API integrators care about), snippets for the rest** — a hybrid, not a blanket conversion.

## Files (pilot)
- **Edit:** 2–3 `src/core/*.h` (comment conversion), `mkdocs.yml` (a pilot nav entry + the generated dir), maybe `.gitignore` (generated `docs/api/`).
- **New:** `doxide.yaml`, this plan.
- **Note:** the built Doxide binary lives at `/tmp/doxide-eval/build/doxide` for the local pilot (not committed).

## VERDICT (pilot ran 2026-07, on ImprovFrame.h)

Converted 4 comment blocks (`//` → `///`, **11 lines, no rewrite**), ran Doxide, staged its Markdown into our real Material site, screenshotted the rendered page. Against the four criteria:

1. **Output quality — GOOD.** The `ImprovFrameParser` page renders natively in *our* Material site: our theme/nav/search/favicon, a member table, the method signature in a blue Material "function" admonition, the prose comment beneath. This is genuinely nicer than the GitHub source link for *reading the API surface* (types, signatures, what each does) — and it's in-site, the Phase-0 one-site vision realised. ✅
2. **Comment cost — LOW per file, but multiply by 139.** 11 lines for one header, purely mechanical (`//`→`///` above each documented entity; inline `//` on members needs `///<`). Cheap per file, but a full sweep is still ~139 files of mechanical edits — bounded, not free.
3. **Parser robustness — MIXED (the real caveat).** Two failures found: (a) on the FIRST core file (MoonModule.h) the Tree-sitter parser errored on `auto** x = new T*[n]` and `(c->*fn)()` and skipped them — Doxygen wouldn't. (b) **Enum *values* and their inline `//` comments do NOT render** — ImprovFeedResult's four values + their meanings were dropped; only the enum's own comment shows. For an enum-heavy codebase that's a real gap (the values are often the point). Doxygen with `///<` trailing comments would show them.
4. **CI feasibility — deferred, but the binary built clean** (cmake + libyaml, ~1 min). Not yet wired; if adopted, build-and-cache in deploy-pages or vendor a prebuilt Linux binary (the one justified uv-exception).

**Conclusion: Doxide is viable and the output is good, but with two known gaps (parser skips some C++20; enum values don't render).** The hybrid the plan predicted is confirmed the right shape: **use Doxide for the stable, clean, class/struct-shaped `core/` API where it renders well, and keep `pymdownx.snippets` for enum-value tables / wire-format constants / anything Doxide drops.** Not a blanket 139-file conversion.

**Recommendation for the actual Phase-4b commit (next):** wire Doxide into CI for a curated `core/` file list (start ~5–8 clean headers), converting only those; add the `Developer / Source` nav section; keep the snippets for the enum/constant cases. Grow the file list as headers are touched, per the "existing em-dashes replaced as files are touched" incremental pattern — never a big-bang sweep. The pilot itself is reverted (the `///` on ImprovFrame.h can stay as harmless — it's valid and already improves the source-doc quality).

### Second pilot: a CATALOG item (GameOfLifeEffect.h) — Doxide adds nothing here

Ran Doxide on a catalog effect too. Result: **near-empty page, zero value, and it sharpens the scope boundary.** The generated `GameOfLifeEffect` page showed only the class name + class comment — no controls, no methods, no state. Three reasons, all structural:
1. Public members carry plain `//` (or no) comments → Doxide skips them.
2. The parser choked on `uint8_t backgroundColorR = 0, backgroundColorG = 0, …` — **comma-separated declarations on one line**, very common C++ — 4 parse errors, members skipped.
3. **The user-facing surface of a catalog effect is its CONTROLS, which are `controls_.addX(...)` *calls inside `onBuildControls()`* — a method body, not declarations. No static doc tool (Doxide or Doxygen) can see them.** Our own card generator reads those same calls and *is* the only thing that surfaces them.

So for every catalog item the card already carries what a reader wants (description, controls+ranges, GIF, source link), and Doxide would produce a title-only page that duplicates the card and adds nothing — pure cost.

**Sharpened boundary:** Doxide is for the **services & infrastructure** layer only — the class/struct/enum-shaped `core/` and light-*base* code whose declarations *are* the API (parsers, buffers, the module base, wire-format types). It is **NOT** for the catalog module types (effects/modifiers/layouts/drivers): those are documented by their cards (generated from the runtime `controls_.add` calls), which no static tool can replicate. This maps exactly onto the two-module-kinds distinction already in coding-standards.md § Documentation model.

## Third + fourth pilots: cxxdox and moxygen — the tool choice, decided

After Doxide's Tree-sitter parser choked on ordinary C++20 (comma-separated declarations, `auto**`, pointer-to-member) and dropped enum values, two robust-parser alternatives were piloted on the same two files.

**cxxdox** (`mkdocs-cxxdox`, libclang, an MkDocs plugin): configured + loaded correctly, but **couldn't render on macOS** — it ships prebuilt wheels for Linux + Windows only (bundled libclang 21), no mac wheel, and its `cindex.py` is version-locked to libclang 21 (mac has clang 17/22). CI (ubuntu, Linux wheel) would work and be uv-installable, but local mac dev breaks — losing the "same build locally and in CI" property. Also: libclang needs a **real compile context** (sysroot + full `-I` set) to parse a header, heavier than syntax-only tools. Adoption: v0.1.6, essentially KFR-only — the weakest of all.

**moxygen** (real Doxygen → XML → Markdown, a Node tool) — **the clear winner:**
- **Doxygen parsed our C++20 with ZERO errors** — ate exactly the comma-declarations Doxide choked on. 17 XML files, both the core parser AND GameOfLifeEffect (the catalog file Doxide gave nothing for).
- **Rendered the FULL class** — every member + method + inheritance + source line — and **enum values in a table** (`CurrentState`/`Rpc`/… — the exact thing Doxide dropped).
- **Worked from our plain `//` comments** (`JAVADOC_AUTOBRIEF` + `EXTRACT_ALL`) — **no comment conversion needed** for structure (add `///`/`///<` later to enrich descriptions).
- **Rendered cleanly in our Material site** — Material tables, our theme/nav.
- Doxygen is the **de-facto-standard parser** (25 years, LLVM/Qt/…) — best on the *Common patterns first* axis of any candidate.
- **Cost:** a two-step pipeline (Doxygen binary + the `moxygen` npm tool). Neither is uv-native — Doxygen is a brew/apt binary, moxygen is `npx`. But both are trivially available on CI (`apt install doxygen`, `npx moxygen`), and Doxygen needs *less* per-file compile-context wrangling than libclang.

### DECISION: moxygen (Doxygen → Markdown) for Phase 4b

It's the only option that is simultaneously robust (zero parse errors on our real code), complete (enum values + full members, incl. the catalog file), recognizable (Doxygen is THE standard), and MkDocs-native (Markdown output into our one site). The cost is a Doxygen + Node CI dependency — heavier than uv, but standard and reliable, and justified at the introduction site like the ESP-IDF Python exception.

**Still true regardless of tool:** autodoc is for **services & infrastructure** only. For catalog module types the card remains the doc (controls are runtime `add()` calls no static tool sees). moxygen *can* show a catalog class's raw C++ members — useful to a developer — but that's a *secondary* developer view, not the user-facing card.

### Phase-4b implementation shape (next commit)
- CI: `apt install doxygen` + `npx moxygen` in the deploy-pages job (and a local `scripts/docs/` wrapper so `build_docs` can run it where doxygen is present; skip gracefully where it isn't, like cxxdox's local-mac gap — but moxygen degrades better since doxygen is brew-installable on mac).
- A `Doxyfile` scoped to a curated `services & infrastructure` file list (core/ + light-base), `GENERATE_XML`, `EXTRACT_ALL`, `JAVADOC_AUTOBRIEF`.
- moxygen → `.md` into a `Developer / Source` MkDocs nav section.
- Enrich the highest-value headers' comments with `///`/`///<` incrementally (as files are touched), but structure works from plain `//` on day one.
- Keep `pymdownx.snippets` for the wire-format/constant embeds already in place.
