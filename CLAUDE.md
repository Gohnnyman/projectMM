# CLAUDE.md

## What This Is

A high-performance multi-platform system driving large LED installations and DMX fixtures. ESP32 is the primary target; also Teensy, macOS, Windows, Linux, RPi. System design: [docs/architecture.md](docs/architecture.md); coding conventions: [docs/coding-standards.md](docs/coding-standards.md). This file holds only the rules.

## Principles

1. **Minimalism.** Minimal flash, minimal memory, fastest hot path — and the periodic housekeeping that shares it is fast too. Minimal code, minimal documentation: every fact and every piece of logic has exactly one home — reference it, never copy it. Present tense only; history lives in git (`docs/backlog/`, `docs/history/`, and `docs/adr/` are the exemptions). One uniform building block: everything is a (Moon)module with the same known lifecycle.

2. **Industry standards.** The textbook solution, pattern, algorithm, and name — a codebase any experienced contributor understands in minutes. The standard, complete construct beats a hand-rolled special case, even when it's more lines. Any bespoke choice carries its one-line reason where it's introduced.

3. **Architecture first.** The domain-neutral core owns the hard constructs, written once; the light domain stays simple on top of it. Platform-specific code lives only in the platform layer. When core enforces a rule on one path, extend core to the next path — never paste the check into modules. No hacks: fix it the standard way the moment it's spotted, or backlog the real fix by name. Default to subtraction: the first question on any change is what it can remove.

4. **Guardrails everywhere.** Every behavior is pinned by tests, unit and scenario, whose descriptions read as functional documentation — a test states a behavior a user could understand, and a trivial test doesn't earn its place. Every commit is measured — performance, size, repo health — so growth and regression are visible the moment they happen. Judgment is reviewed; everything else is checked by the gate scripts. The final guardrail is physical: nothing counts as verified until it runs on real hardware — the bench, and the product owner's eyes, are the measurement.

5. **Robustness.** Unbreakable in use: any input, any order, any size — degrade visibly, never crash, and every discovered crash becomes a test. Every setting applies live; no reboot to apply configuration ([architecture.md § Live reconfiguration](docs/architecture.md#live-reconfiguration-every-change-applies-without-a-reboot)). Out of scope: power loss, brown-out, corrupted updates.

## The Process

Every change follows the same timeline: **main → branch → build → test → document → commit → merge → release**. The **product owner** (PO) is the person initiating a branch — any contributor can be one. The PO initiates every event and every gate list — never start one unprompted; if unsure, ask ("Feature work is done; run pre-commit, or do you want to look first?"). This holds even when a gate script would only be *checking* work in progress: running `precommit.py`/`premerge.py` to see where things stand is still starting a gate list, and it writes the logs the PO's own run reports from. Verify work in progress with the individual tools instead (a build, `ctest`, one check script); the event scripts are the PO's to fire. A conditional check runs only when its objective trigger matches; an applicable-but-skipped check needs a one-line reason in the commit/PR/release notes. Each cycle produces visible output, and each cycle subtracts: remove code and docs that no longer earn their place, or know why nothing can go — `backlog/` and `history/` shrink too. External contributors follow the same timeline: fork, branch, PR into main — the same checks and review apply.

### Main

Main is always releasable: what's on main ships as the latest *pre-release*; tagged releases are cut from it. Work never starts on it: feature work branches. One exception: a small, already-verified hotfix commits directly to main.

### Branch

**The product owner creates every branch — never the agent.** Branching is a git operation, and
git is PO-controlled (§ Roles): the agent works on whatever branch it is given, and asks when a
change does not belong there. This holds even when a branch seems obviously right (a one-line
fix, keeping main clean) — creating one silently moves work somewhere the PO is not looking.

1. **Pick.** One module/effect/driver/capability — the product owner picks what to build next.
2. **Spec.** Specs before code: the module spec and the UI spec sufficient to implement from (a draft may sit in the backlog until it ships); when in doubt, ask.
3. **Plan.** Plan mode before every feature; save the approved plan to `docs/history/plans/` as `Plan-YYYYMMDD - <title>.md` — a temporary document: it ends up as the PR description and the file is deleted once the plan is realized; the merged PR is the design record. **Deleting a plan is the product owner's call — never the agent's.** "The code is written" is not "the plan is realized": a plan is realized when its *verification* is done too, including the judgement steps (thresholds tuned, results read together, the bench check). Ask; do not infer it from a green build. For a restructure ("make it simpler/cleaner"): enumerate 2–4 end states, name what each gains and loses, pick the leanest that solves the actual problem; propose as a question, implement only what's picked; surface follow-ups before starting so it's one coherent refactor.

### Build

Implement against the architecture ([docs/architecture.md](docs/architecture.md)) and the coding standards ([docs/coding-standards.md](docs/coding-standards.md)). Verify with the tests and on the bench, and invite the product owner to judge the result — their eyes are the measurement (§ Principles, Guardrails). Everything build/flash/run/monitor: [docs/building.md](docs/building.md).

```sh
cmake --build build                                     # desktop build (zero warnings)
ctest --test-dir build --output-on-failure              # unit tests
uv run moondeck/scenario/run_scenario.py                # scenario tests
uv run moondeck/build/build_esp32.py --firmware <fw>    # ESP32 firmware build
uv run moondeck/build/flash_esp32.py --firmware <fw> --port <port>
uv run moondeck/check/check_specs.py                    # spec/doc drift check
```

All Python goes through `uv run`, never bare `python` (full rule: [coding-standards](docs/coding-standards.md)).

Keep a branch under ~100 changed files: past that CodeRabbit declines the PR outright rather than reviewing part of it, so the branch silently loses a review layer. Split, or say so in the PR.

**MoonDeck** is the project's tooling: every build, flash, monitor, test, and check task is one Python script under `moondeck/`, and MoonDeck itself is the local web dashboard that runs those same scripts for a human ([moondeck/MoonDeck.md](moondeck/MoonDeck.md) is the per-script reference). Agents invoke the scripts from the command line — one set of scripts, two front ends — and every gate invokes one of them. Deliberately our own scripts rather than an embedded toolchain like PlatformIO: the firmware builds vendor-native against pinned ESP-IDF versions, and the tooling covers far more than compile-and-flash — one script per task keeps humans, agents, and CI on the identical path (rationale: [building.md § MoonDeck](docs/building.md#moondeck--the-dev-console)).

### Test

New behavior is pinned before it ships: a unit test for module logic, a scenario test for a full pipeline, and every discovered crash becomes a regression test (§ Principles, Guardrails + Robustness). Test descriptions read as functional documentation — a statement a user could understand — and a trivial test doesn't earn its place. Placement: [coding-standards § Tests](docs/coding-standards.md#tests); inventory and strategy: [docs/testing.md](docs/testing.md).

### Document

Docs land with the code, not at merge time: the module's spec and catalog card describe what actually shipped ([coding-standards § Documentation model](docs/coding-standards.md#documentation-model)); a breaking change gets its entry in [docs/MIGRATING.md](docs/MIGRATING.md); a shipped backlog item or spec draft is deleted. The merge gate only verifies this happened.

### Commit

Git only with the PO in the loop: staging, committing, and pushing happen only when the PO explicitly triggers them. **The PO verifies EVERY changed file before it is committed.** That is the rule the others serve: nothing reaches history unseen. Two things follow, and both have been broken. **The trigger is the words "commit now", never a task instruction** — "fix it", "do step 4", "the build is broken", even "hotfix it on main" say what to change and nothing about recording it; finishing the work is not a prompt to commit it. And **a "commit now" covers only the files the PO has actually looked at** — touch one more, anything at all, and the tree again holds something unverified, so the go-ahead is void until they see it. Stop at a clean tree, say exactly which files changed, and wait. On main exactly as on a branch; a one-line fix exactly as a feature. What and when to commit or merge is 100% the product owner's call — never ask or propose commit timing. One combined commit per cycle (no partial commits; hygiene changes fold into the next one). Branches and commits may bundle multiple topics: not every small change gets its own commit — the pre-commit and pre-merge checks would be too much overhead.

On "run pre-commit": `uv run moondeck/event/precommit.py`. It runs every gate whose trigger the change matches and reports PASS / FAIL / SKIP / MANUAL. Then wait for an explicit "commit now".

**"commit now" applies to the diff the PO just reviewed, and any later edit cancels it.** The PO reviews every line before committing (§ Roles), so the go-ahead is scoped to the files as they stood when it was given. Change one afterwards — a review finding, a CI fix, a doc touch-up — and the order is void: say what changed and wait for a fresh "commit now". This holds however small the change and however clearly an earlier instruction seems to cover it ("we commit in one go" says how *many* commits, not *when*).

Commit message: title ≤ 72 characters, imperative. Then a 1–3 sentence end-user TL;DR (no file lists). Then the performance one-liner, measured for every supported target by running `collect_kpi.py --commit` with a board attached. Then change sections as bullets: **Core**, **Light domain**, **UI**, **Scripts/MoonDeck**, **Tests**, **Docs/CI**, **Reviews** (🐇 external / 👾 Reviewer, one bullet per finding: flagged → done/accepted/deferred + why). Core and Light domain are the preferred default categories (a core-module test → Core; a script fix touching a light driver → Light domain). No hard wraps inside a part. Full performance block at the bottom.

**Reviewer at commit-time:** run the Reviewer on the staged diff when the commit is large (roughly ten files or more across areas) or on PO request — start it first so the other checks run in parallel; findings fixed or accepted-with-reason before "commit now".

**Handling review findings** — from the Reviewer, CodeRabbit, or a human: *verify each finding against current code; fix only still-valid issues, skip the rest with a brief reason, keep changes minimal, and validate.* A reviewer reads a snapshot and can be wrong or already out of date, so a finding is a claim to check, not an instruction to apply. Work through **every** finding, lowest severity first — a nit is a one-line fix while attention is cheap, and leaving the small ones for later means they are never done. Rising to the serious findings last also means the cheap context is already loaded.

### Merge

The PO pushes the branch; external review runs on the PR; findings are processed on the branch. On "run pre-merge": `uv run moondeck/event/premerge.py`, which re-runs the mechanical checks over the whole branch diff and lists the judgment gates it cannot decide.

Those judgment gates: review feedback addressed; the Reviewer agent over the whole branch diff (start it first, it runs in parallel; scope: boundaries, bespoke conventions, unnecessary abstractions, duplication, hot path, spec conformance, bloat); lessons carried forward only when VERY important — most learning lives in the commit/PR record; a truly important gotcha → `lessons.md`, a major architectural decision → a new ADR, a hardened rule → CLAUDE.md or coding-standards; docs sync; the PR title and description matching the actual diff; the performance snapshot when tick-path code changed; a README refresh when build, flash, or first-run changed.

### Release

On "run pre-release": `uv run moondeck/event/prerelease.py`. The mechanical checks run; the rest is judgment it lists for the PO — merge gates passed on the tagged commit, the real-hardware test (PO only), no open release-blockers, the per-release criteria done, release notes, cross-platform smoke on a major/minor bump, and the principles audit for forward-looking language (the Reviewer agent can run that one).

## Roles & Collaboration

The product owner is the critical success factor. The PO reviews every line before committing, specifies requirements, controls all git operations, tests on hardware, decides what's built, and filters agent suggestions critically. The agent writes; the product owner thinks. Tight PO control is deliberate: it is what keeps the system lean and predictable.

| | Agent | Model | Focus |
|--|-------|-------|-------|
| 🤖 | **Architect** | Opus | System design, boundary review |
| 👽 | **Developer** | Sonnet | Implementation, one step at a time |
| 👾 | **Reviewer** | **Fable** (Opus fallback) | Pre-merge branch review + large-commit review; model fixed |
| 🛸 | **Tester** | Sonnet | Tests, verifying rules in code |
| 💀 | **Runner** | Haiku | Script runs, checks, build verification |
| 🔬 | **Researcher** | **Fable** | Read-only fan-out: inventories, blast radius, prior art |

Agents never commit. **Delegate the mechanical roles**: parallelizable or substantial → delegate (gate fan-out → Runner; pinning a fixed bug → Tester; broad mapping → Researcher); a single fast check → inline.

**Ask, don't guess.** Asking the product owner is always preferred over guessing.

**A question is answered, not acted on.** When the product owner asks a question, answer it and stop; changes happen only after explicit agreement.

**Sanity-check every request.** Hold it against README, this file, and architecture.md. If it conflicts, push back briefly with the specific reference; the product owner can still overrule.

**Anti-stalling.** If a build error or test failure survives 2 fix attempts: STOP. Ask, or roll back and re-approach.

**Bench boards are free test rigs.** Build and flash freely to verify work; re-probe ports first. A *rigorous* change (anything that could brick, boot-loop, or wipe a board: flash erases, boot/partition/build-config changes, a first flash of an untested board) gets a one-sentence heads-up and a go-ahead first — the test is reversibility.

**Invite the product owner to test, then STOP.** If the PO could see or judge the result, hand it over ("running on X, look at Y") and wait for their observation before concluding, documenting, or moving on. Leave the state running; don't revert, reflash, or reconfigure what they were about to look at.

What the agent reads: always CLAUDE.md + architecture.md + coding-standards.md; per commit, only the relevant module specs; never automatically `docs/history/` or `docs/backlog/`.

## Documentation

Published at [moonmodules.org/projectMM](https://moonmodules.org/projectMM/); sources under `docs/`:

- [architecture.md](https://moonmodules.org/projectMM/architecture.html) — system design
- [coding-standards.md](https://moonmodules.org/projectMM/coding-standards.html) — how code is written
- [building.md](https://moonmodules.org/projectMM/building.html) — build/flash/run per target
- [testing.md](https://moonmodules.org/projectMM/testing.html) — test inventory and strategy
- [performance.md](https://moonmodules.org/projectMM/performance.html) — per-module timing/memory per platform
- [MIGRATING.md](https://moonmodules.org/projectMM/MIGRATING.html) — breaking-change log
- [backlog/](https://moonmodules.org/projectMM/backlog/index.html) — forward-looking to-build lists (core / light / mixed)
- [adr/](https://moonmodules.org/projectMM/adr/index.html) — immutable architecture decision records (Nygard format); immutable except the status line: superseded/amended ADRs get a dated pointer to their successor
- [friend-repos/](https://github.com/MoonModules/projectMM/tree/main/docs/friend-repos): monthly activity digests of related open-source LED projects
- [history/](https://moonmodules.org/projectMM/history/index.html): lessons, prior-project inventories
- [moonmodules/](https://github.com/MoonModules/projectMM/tree/main/docs/moonmodules) — module catalog pages + generated technical pages

Docs describe the system as it is; git is the history; specs precede implementation. **Documentation model**: [coding-standards.md § Documentation model](docs/coding-standards.md#documentation-model).

`history/` is the distilled experience of prior projects (WLED, StarLight, MoonLight, …), credited per module. `backlog/` is its forward mirror. Agents read both only when planning. Both shrink under mandatory subtraction.
