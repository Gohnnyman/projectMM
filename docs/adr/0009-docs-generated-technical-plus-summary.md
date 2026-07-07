# 9. Two doc surfaces: generated technical + hand-written summary

Status: Accepted

## Context

Per-module `.md` files were being hand-maintained and hand-shrunk, drifting from the code they described. The technical facts (controls, ranges, members) already lived in the `.h`, so keeping a parallel prose copy was duplication that rotted.

## Decision

Every module has exactly two reader surfaces. The `.h` is the single home of technical content (`///` comments); a Doxygen→moxygen pipeline **generates** one technical page per module. A thin hand-written summary page (a table, end-user facing) is the only prose that stays. The prior art is docs.rs / Sphinx-autodoc / Doxygen (a hand-written guide over a generated API reference). The full model lives in [coding-standards.md § Documentation model](../coding-standards.md#documentation-model).

## Consequences

- A technical fact is stated once (in the `.h`) and never re-typed in prose.
- The pipeline exposed traps that became build rules, each worth keeping: batch the external tool (one Doxygen pass + one `moxygen`, not 132 per-header invocations, ~150 s → ~7 s); write generated files only on content change (an unconditional write into a watched dir is an infinite rebuild loop); a present-but-failing generator must fail loud, not return empty; and a doc path duplicated in `main.cpp` `registerType` drifts silently, so `check_specs.py` gates that every docPath resolves to a real page + anchor.
- The verification rule that generalised: a generated artifact's ground truth is the rendered output, so verify an anchor against the built HTML (`grep id=` the `.html`), never a re-derived slug.
