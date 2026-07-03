# Plan — Docs Phase 3: drift validation (not snippet de-duplication)

Approved-pending. Continues the docs overhaul ([docs/backlog/docs-system-overhaul.md](../../backlog/docs-system-overhaul.md)) on `next-iteration`, on top of the Phase 1+2 commit.

## The pivot: the backlog's premise was wrong

Phase 3 in the backlog assumed `pymdownx.snippets` (`--8<--`) could single-source facts by pulling them from the `.h` into the `.md`. **Investigation shows it can't** — snippets pull text *verbatim*, and the `.h` / `.md` don't hold the same *text*, they hold the same *fact in two forms for two audiences*:

- `.h`: `controls_.addUint8("cooling", cooling, 1, 255)` — code + machine range, for the compiler/developer.
- `.md`: `` `cooling` — how fast heat dissipates… `` — human prose, for the reader.

A snippet can't turn `addUint8(…,1,255)` into "how fast heat dissipates", and pulling the raw comment/code into prose drags `//` delimiters and code syntax that read wrong. So the naive "include the `.h`" plan is off the table.

What the audit found instead — two buckets:

- **Bucket A — true duplication, worth guarding:** control **numeric ranges** (~50, hand-copied `.h`→`.md`) and attribution **URLs** (~51, the same GitHub URL in `.h //Author:` and `.md Origin:`). No sync today; a range/URL can change in code and the doc silently drifts.
- **Bucket B — NOT duplication, leave alone:** control *names* (already validated by `check_specs`), and architectural facts (audience-aware restatements — `architecture.md` = the principle, module `.md` = this module's behaviour, `.h` = the implementation). De-duping these would *harm* readability; they're working as designed.

## Decision (PO): validate, don't generate

For both Bucket-A types: **catch drift with a gate, keep the prose hand-authored.** Not build-time generation (which mixes generated + authored content per line and is more fragile). The human prose stays readable; a check fails when the `.h` and `.md` disagree, so "change one, forget the other" is caught at commit instead of shipping.

This is the pragmatic Phase 3: it solves the actual "change one thing, the other drifts" pain directly, at low risk, without forcing code-into-prose.

## Deliverables

Extend `scripts/check/check_specs.py` (the existing spec-drift gate — already parses `.h` control names and `.md` prose) with two new drift checks:

### 3a — control-range drift
- Parse each control's numeric bounds from the `.h` (`addUint8("floor", floor, 0, 255)` → `floor: 0–255`; also `addInt`, `addUint16`, etc.).
- For each control that HAS a range in the `.h`, check the module's `.md` prose mentions BOTH bounds (as `0–255`, `0-255`, `(0…255)`, `1 to 8`, etc. — tolerate the common human spellings).
- **Warn (not hard-fail) on drift** — the range appears in the `.h` but not (or differently) in the `.md`. Reason: some controls legitimately don't restate the range in prose (a pin list, an enum); a hard-fail would force noise. A warning surfaces real drift without blocking. (Settle in review: warn vs a hard-fail with an opt-out list.)
- Report format: `MODULE.md — control 'floor' range 0–255 (from .h) not found in prose`.

### 3b — attribution-URL drift
- Parse the URL(s) from each `.h` `// Author:` line.
- Check the same URL appears in the module's `.md` `Origin:` line (the `.md` wraps it in a markdown link; match on the bare URL substring).
- **Warn on drift** — a URL in the `.h` Author line absent from the `.md`. Report: `MODULE.md — author URL <url> (from .h) not in Origin line`.

### Wiring
- These run inside `check_specs.py` (already a commit gate — CLAUDE.md Event 1, gate 1), so no new gate, no CI change. The catalog pages route their controls through the table hook, but the *source* prose the checks read is the `.md`, unchanged.
- `docs/testing.md`: document the two new drift checks under the spec-check description.

## Explicitly OUT of scope (and why)
- **Snippet-including `.h` into `.md`** — can't bridge code↔prose (the pivot above).
- **Generating ranges into the doc** — PO chose validate-only; keeps prose readable.
- **Control names / architectural facts** — Bucket B, not duplication; de-dup would harm.
- **Phase 4 (Doxide)** — separate, later.

## Verification
- `check_specs.py` runs clean on the current tree, OR reports genuine drift (which we then fix in the `.md` — surfacing existing drift is a *feature* of landing this).
- The new checks are unit-tested where practical (a `test/python` case feeding a synthetic `.h`+`.md` pair, asserting drift is caught) — matches the host-side test tier.
- No change to rendered docs (checks only); catalog tables/build unaffected.

## Files
- **Edit:** `scripts/check/check_specs.py` (two drift checks), `docs/testing.md` (document them), any `.md` where the checks surface real drift (fix the prose).
- **Maybe new:** `test/python/test_check_specs_drift.py` (unit-test the new checks).
