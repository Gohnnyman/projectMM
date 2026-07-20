# 15. The source tree splits by domain/type; library origin is a tag, not a folder

Date: 2026-07-06

## Status

Accepted

## Context

A module carries three orthogonal axes: its **domain** (`core` vs `light`), its **type** (effect / modifier / layout / driver), and its **library** (the origin it was learned from (MoonLight, WLED, MoonModules, projectMM-native). The `src/`, `docs/`, `test/`, and `assets/` trees all had to pick which axes become folders.

Domain and type are unambiguous: every module has exactly one domain and one type. Library is not: an effect's origin is frequently *blended*, not a single fact: `DistortionWavesEffect` cites MoonLight + WLED + v1 + v2; `GameOfLifeEffect` cites MoonLight + MoonModules + v1; several modules have no clear single origin. A folder axis forces one answer to a multi-valued question, and a wrong or shifting answer costs a multi-file move (src + assets + tests + the registered doc path). Library also duplicates a dimension the `tags()` emoji already carries, and the emoji can carry *several* origins where a folder cannot. The end user does not care about a module's library except as a UI filter, which the emoji chip already provides.

## Decision

The tree is **`<core|light> / <type> / Module`**, flat within a type. Library does **not** become a folder level; it rides where it is free and non-duplicative:

- **In code / assets / tests:** the `tags()` emoji (drives the UI origin-filter; may be multi-valued). Leaf files are flat within their type folder: `src/light/effects/DistortionWaves.h`, `docs/assets/light/effects/DistortionWaves.gif`, `test/unit/light/unit_DistortionWaves.cpp`.
- **In docs:** library rides in the **page** dimension, not a folder: one catalog page per type (`effects.md`) with library *sections* inside, splitting to per-library page *names* (`effects_wled.md`) only when a section outgrows its page. A doc page is forgiving about fuzzy origin: a blended-lineage effect goes on one page with its full origin in the row's tags, and mis-filing is a one-line edit, not a multi-file move. `docs` is thus the one area where `type` is expressed as part of a page name rather than a folder, because the docs compact to per-type/per-library pages, and library, the only axis with an explosion problem, rides along in that name.

| | core/light | type | leaf | library |
|---|---|---|---|---|
| **src** | `light/` | `effects/` | `DistortionWaves.h` | tag in `tags()` |
| **assets** | `light/` | `effects/` | `DistortionWaves.gif` | — |
| **tests** | `light/` | `effects/` | `unit_DistortionWaves.cpp` | — |
| **docs** | `light/` | the page name (`effects.md`, later `effects_<library>.md`) | (row inside) | the page split |

## Consequences

Every drawback of library-as-folder is dropped at once: fuzzy-origin filing, two-places-disagree, reclassification churn, sparse subfolders, and deep paths all disappear. A module's origin can be blended or can change without a file move; only the `tags()` emoji and, at most, a one-line catalog-row edit change.

The one open growth path (non-blocking): when a library's section outgrows its catalog page, split it to a per-library page name (`effects_wled.md`, …), a lift, not a rewrite, since the flat page names and within-page sections are already in place for it.

The live catalog pages (`docs/moonmodules/light/{effects,modifiers,layouts}.md`) and `docs/coding-standards.md` cite this decision for *why* the tree is shaped the way it is.
