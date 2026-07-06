# Folder-structure decision — library is a tag, not a folder

A *Refactor for simplicity* decision (per CLAUDE.md), recorded here because the live catalog pages
(`effects.md` / `modifiers.md` / `layouts.md`) cite it for *why* the tree is shaped the way it is. The
execution has shipped (assets + tests type-split; docs flattened to `domain/type` with catalog +
generated `moxygen/` pages); this is the surviving **rationale**, not a to-build list.

## The three axes

1. **Domain** — `core` vs `light`. Structured in `src/`, `docs/`, `test/`, `assets/`.
2. **Module type** — effects / modifiers / layouts / drivers. Structured in each of those areas.
3. **Library** — a module's *origin* (MoonLight, WLED, MoonModules, projectMM-native). **A tag and a
   doc split — NOT a folder axis.**

## Decision: `domain / type` folders; library is a tag (+ a doc split)

The structure is **`<core|light> / <type> / Module`**, flat within type. Library does **not** become a
folder level.

**Why library is not a folder** (the deciding analysis, still true): an effect's origin is frequently
*blended*, not a single fact — `DistortionWavesEffect` cites MoonLight + WLED + v1 + v2;
`GameOfLifeEffect` cites MoonLight + MoonModules + v1; several have no clear origin. A folder forces
one answer to a multi-valued question, and a *wrong* or *shifting* answer means a multi-file move
(src + assets + tests + the registered doc path). It also duplicates the dimension the `tags()` emoji
already carries (and the emoji can carry *several* origins; a folder can't). **The end user does not
care about a module's library** — they filter by the emoji chip in the UI if they want origin at all.
So library stays where it's free and non-duplicative:

- **In code / assets / tests:** the `tags()` emoji (drives the UI origin-filter; can be multi-valued).
  The leaf files are flat within their type folder (`src/light/effects/DistortionWaves.h`,
  `docs/assets/light/effects/DistortionWaves.gif`, `test/unit/light/unit_DistortionWaves.cpp`).
- **In docs:** library rides in the **page** dimension, not a folder — a catalog page per type
  (`effects.md`) with library *sections* inside today, splitting to per-library page *names*
  (`effects_wled.md`) as a section outgrows its page. This is the one area with a doc-explosion
  problem, and a doc page is *forgiving* about fuzzy origin (a blended-lineage effect goes on one page
  with its full origin in the row's tags; mis-filing is a one-line edit, not a multi-file move). So
  the drawbacks that make library-as-*folder* bad are soft for library-as-*doc-page*.

This drops every drawback of library-as-folder (fuzzy-origin filing, two-places-disagree,
reclassification churn, sparse subfolders, deep paths) at once.

## The one rule, across all four areas

| | core/light | type | leaf | library |
|---|---|---|---|---|
| **src** | `light/` | `effects/` | `DistortionWaves.h` | tag in `tags()` |
| **assets** | `light/` | `effects/` | `DistortionWaves.gif` | — |
| **tests** | `light/` | `effects/` | `unit_DistortionWaves.cpp` | — |
| **docs** | `light/` | the page name (`effects.md`, later `effects_<library>.md`) | (row inside) | the page split |

`docs` is the one area where `type` is expressed as part of a **page name** rather than a folder,
because the docs compact to per-type/per-library pages — and library, the only axis with an explosion
problem, rides along in that name. Everywhere else: plain `domain/type` folders, library as a tag.

## Still open (future growth, not blocking)

- **Per-library page split** (`effects_wled.md`, …) — a lift-not-rewrite when a library's section
  outgrows its page; the flat page names + within-page sections are already in place for it.
