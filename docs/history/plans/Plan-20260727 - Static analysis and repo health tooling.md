# Plan: static analysis + repo-health tooling

**Date:** 2026-07-27 · **Status:** in progress — clang-tidy, lizard and clang-query landed (steps 2, 5, 8). Open: warnings tier-zero (1), `[[clang::nonblocking]]` (3), RTSan (4), CodeQL housekeeping (6)

Temporary document: this text becomes the PR description and the file is deleted once the
plan is realized (CLAUDE.md § Branch).

## The question

*Is lizard the right tool, and how do we monitor the repo against our own principles?*

Answered by researching what serious C++ projects actually **configure** — LLVM, Chromium,
ClickHouse, SerenityOS, Godot, ESPHome, Zephyr, ESP-IDF — rather than by running each tool
once and scoring the output. That distinction matters, and getting it wrong the first time is
what produced the earlier draft of this plan (see *What we got wrong* at the end).

## The stack

Five layers, cheapest and most immediate first. Each catches something the layer above cannot.

| # | Layer | Catches | Where |
|---|---|---|---|
| 0 | **Compiler warnings** | shadowing, double promotion, fallthrough, null deref | Every build, all targets |
| 1 | **clang-tidy** (curated) | bug patterns, performance, portability | Editor as you type **+** CI |
| 2 | **Sanitizers** (ASan+UBSan, TSan) | real memory/threading faults at run time | Desktop CI lanes |
| 3 | **RTSan + `[[clang::nonblocking]]`** | allocation/blocking in the render loop, **transitively** | Compile time + run time |
| 4 | **CodeQL** (stock suite) | untrusted input, whole-program taint, use-after-free | CI, Security tab |

Alongside them, three tools that measure or extend rather than analyse:

| Tool | Job | Status |
|---|---|---|
| **lizard** | Complexity *number* per commit, for the trend | Keep, baselined |
| **Our Python checks** | Cross-artifact contracts nothing else can see | Keep as-is |
| **clang-query** | Home for the next bespoke rule | Shipped — 3 rules in `check_clang_query.py` |

**Our Python checks** stay untouched: ~0.4 s for all four, and they cover contracts whose
other half is a Markdown page, a JSON catalog or a built binary — not a C++ question, so no
analyzer can replace them.

**clang-query** is the designated home for the *next* bespoke rule: the stack enforces the
rules we have, this is how we add the ones we invent. Matchers are plain text in the repo,
minutes to write, no plugin to build and no LLVM ABI coupling — which is why it beats a
compiled clang-tidy check (LLVM-version-locked, and **clangd will not load one**, so a custom
check would never appear in the editor). Same compilation database as layer 1, so adopting it
later costs nothing now. Off until a rule needs it: a tool running zero rules is overhead.

**First named candidate: large static array declarations** (`char buf[512]` and friends), which
bloat RAM on a 180 KB-heap device and which nothing else in the stack reports. PROBED and it
works — `varDecl(hasType(constantArrayType()))` matches, and `set output dump` yields
`file:line`, the name, and the full type (`char[80]`), so the byte size parses straight out.

Two things that probe established, both of which shape the eventual script:

- **There is no size-threshold matcher.** `hasSize(64)` is exact-match only; `sizeGreaterThan`
  does not exist. So the matcher takes ALL constant arrays and the "> N bytes" filter, the
  our-files-only filter, and the dedupe (template instantiations repeat a declaration) happen
  in Python — the same shape as `check_lizard.py`, which already parses a tool's raw output
  and applies our policy on top.
- **It needs the same `-isysroot` fix as clang-tidy.** Without it, `Control.cpp` reported 5
  matches; with it, 14. Identical silent-under-report trap to the one recorded below — a wrong
  answer, not an error. Any clang-query script must reuse `check_clang_tidy._toolchain_args()`.

Unprobed: whether stack arrays, class members and statics are worth separating (they are
different problems — a 512-byte local is a stack-depth risk, a 512-byte member is per-instance
RAM), and where the threshold should sit. Both are policy questions for when the rule lands.

### Is lizard the right tool? Yes — as a counter, not an analyzer

The question this plan opened with. **Answered and shipped** (step 2).

Keep it, because it produces a **number per commit** that repo-health trends — clang-tidy tells
you a function is complex today, only a trend says the codebase is getting worse. But only as a
counter: its own README calls it a fuzzy tokenizer, not a parser (no macro expansion, confused
by templates), so it can never express an architectural rule. Its VS Code extension is dead
(v1.0.1, Oct 2022) — the in-editor equivalent is clang-tidy through clangd.

The 162 warnings are a threshold artifact, not a verdict: 161 come from `CCN>10` alone and 80%
of functions sit at CCN ≤ 5, with a real tail of 4 above CCN 50. A metric that can never reach
zero is a poor gate, which is what the baseline fixes.

### One rule, one owner

| Rule | Enforced by |
|---|---|
| No allocation/blocking in the render path | `[[clang::nonblocking]]` + RTSan |
| No platform code outside `src/platform/` | `check_platform_boundary.py` |
| Specs match the code | `check_specs.py` |
| Catalog matches the modules | `check_devices.py` |
| Untrusted input is memory-safe | CodeQL |
| Bug patterns / performance | clang-tidy |
| Complexity does not grow | lizard (baselined) |
| Size/LOC/docs do not grow silently | `repo_health.py` |

## How clang-tidy gets configured

Archetype B — `*` minus an explicit disable list, each entry carrying its reason — derived
bottom-up from ESPHome's (our closest peer: a large ESP32 C++ codebase) then tuned against this
tree. **The config and every reason now live in [`.clang-tidy`](../../../.clang-tidy)**; that
file is the record, not this one.

Tuning mattered, and the numbers are why the first attempt failed: a shotgun
(`bugprone-*,performance-*,concurrency-*`) gave 384 findings of which 66% were one noisy check;
`*` plus ESPHome's family disables gave **6,073**; adding their style disables 3,949; adding the
`cert-*` family (aliases of `bugprone-*`, and `cert-err33-c` alone was 3,678 — every `snprintf`)
brought it to **131**. Then triage took it to 30.

## Implementation

Each step is independent and revertible. **✅ done · ◻ not started · ◐ partly done.**

1. ✅ **Warnings tier-zero** — DONE. All five landed on `-Wall -Wextra -Werror`; the ESP32
   build is clean under them too. Three real findings, all fixed: a float→double promotion in
   `Rings241Layout` (a softfloat call on the FPU-less Xtensa), a parameter shadowing a control
   field in `RubiksCubeEffect`, and a test double with virtual functions and a public
   non-virtual destructor. Original text: add `-Wshadow -Wnon-virtual-dtor -Wdouble-promotion
   -Wimplicit-fallthrough -Wnull-dereference` to the existing `-Wall -Wextra -Werror`.
   `-Wdouble-promotion` catches accidental `double` math: real cost on Xtensa, and it enforces
   the integer-math rule. Trial `-Wconversion` separately — expect to reject it for `light/`.
2. ✅ **Baseline lizard** — DONE. `docs/metrics/whitelizard.txt` freezes today's 162 and
   `check_lizard.py` fails only on new violations (verified both ways: a probe function makes it
   exit 1, removing it returns green). Deliberately NOT at the repo root — that is lizard's
   default whitelist path, and a baseline that applies itself silently zeroed the KPI's
   complexity count. The KPI and repo-health now record the RAW number via shared code; the
   baseline is applied only by the gate. `repo-health.json` gained a `complexity` block, so the
   trend the plan asked for actually exists now.
3. ✅ **`[[clang::nonblocking]]`** — DONE. `MM_NONBLOCKING` (platform.h) on
   `tick/tick20ms/tick1s`, `-Wfunction-effects` on the desktop build, the "clang-hotpath"
   MoonDeck card, and `docs/metrics/hotpath-baseline.txt` freezing the 107 known
   (file, callee) pairs. `check_hotpath.py` deleted; the pre-commit gate now runs
   `check_nonblocking.py --incremental` (~0.7 s: it rebuilds only what the commit touched, so
   it answers "did this change add a blocking call" rather than re-listing the baseline).

   **It reports, it does not gate.** `-Wno-error=function-effects` stays, and the gate never
   fails the event. Two reasons, both learned here: failing on the ~50 known findings would
   block every build until the architecture work lands, and a gate nobody can satisfy gets
   disabled rather than obeyed; and a NEW blocking call may be entirely legitimate — a driver
   that must wait for hardware — so forcing a failure pushes people to suppress it under time
   pressure, which is the thing this exists to prevent. The report states the finding and marks
   it NEW; the product owner judges. Other consumers can still choose to fail on it.

   Four things the original estimate got wrong, all measured:
   - **Not three lines.** A bare attribute gives 209 findings; annotating five platform
     functions collapses it to 10. The bulk of the findings were unannotated helpers, not
     violations. Threading the attribute through every override touched ~85 files.
   - **The ESP32 is GCC** — no attribute, no warning, and `-Werror` + `-Wattributes` means a
     bare attribute breaks the firmware build. Hence the macro (on GCC it expands to `noexcept`,
     keeping the exception contract; only the clang attribute and the warning are absent).
   - **`check_hotpath.py` was NOT the ESP32's safety net**, as first assumed. Measured: it
     scanned 67 tick METHODS, all in `src/core/` and `src/light/`, zero in `src/platform/esp32/`
     (that layer has no tick methods — it is free functions the tick path calls into). All 60 of
     its files compile on desktop, so the compiler check covers the identical set. It reported
     **0** where the compiler reports 165.
   - **The indirect call was the real hole.** `tickChildren` dispatches through a member
     pointer; the attribute had to go in the POINTER TYPE, or every module's tick escaped the
     check. Passing an unannotated method is now a compile error.

   Findings that remain are real and shown, never suppressed: **50 on `tick()`**, 6 on
   `tick20ms()`, 95 on `tick1s()`. The sharp ones are UDP `sendTo`/`recvFrom` in
   `AudioService::tick` — socket I/O every frame. Moving that work off the render path is
   backlogged as architecture (backlog-core: "move blocking work off the render callbacks").

4. ✅ **RealtimeSanitizer** — DONE. `realtime` added to the sanitizer matrix in `test.yml`,
   the runtime half of step 3: `-Wfunction-effects` proves what it can at compile time, RTSan
   catches what it cannot — allocation or blocking reached through virtual dispatch or a
   function pointer. It keys off the same `MM_NONBLOCKING` attribute, so the lane costs one
   matrix entry.

   Two things the step needed that the plan did not anticipate:
   - **The lane must pin clang 20+, and install it.** GCC rejects `-fsanitize=realtime`
     outright; `ubuntu-latest` defaults to GCC *and* its `/usr/bin/clang++` is **18**, which
     rejects the flag at the compiler-probe stage — CI caught this after a local check on
     Homebrew clang 22 passed. The lane now installs `clang-20` from apt.llvm.org. Only this
     lane changes compiler; ASan/TSan stay on the runner default.
   - **`RTSAN_OPTIONS=halt_on_error=0`.** The render path has ~50 known blocking calls, so
     halting would fail the lane on every run. It reports; the log is the signal — the same
     report-don't-gate shape as the compile-time half.

   Verified locally: RTSan intercepts a `malloc` inside a `[[clang::nonblocking]]` function and
   prints the stack.

5. ✅ **clang-tidy** — config landed, clangd wired, the MoonDeck card reports into the log, and
   the tree triaged from 125 findings to **30** (`.clang-tidy` runs `*` minus a documented disable
   list; what remains is the path-sensitive `clang-analyzer-*` family, roughly half of it in test
   code). Remaining findings are tracked in backlog-core.md, not here.

   **`WarningsAsErrors` stays `''` — that is the finished state, not a missing step.** clang-tidy
   is a *report*: it shows what it finds, and other tools decide what to do with that. Making
   findings build errors would fail every build on the 30, and a gate nobody can satisfy gets
   disabled rather than obeyed — it would also push people to `NOLINT` under time pressure, the
   opposite of what a report is for. Same rule as the hot-path check.

   Superseded by the above: the original plan said each clean family joins `WarningsAsErrors` and
   the target is a zero baseline. Neither is the goal any more — reporting correctly is.
6. ✅ **CodeQL housekeeping. DONE.** `.github/codeql-config.yml` excludes `test/doctest.h`
   via `paths-ignore` — the standard mechanism, rather than dismissing the same alert by hand
   after every scan. It is the only vendored source in the repo.

   **Gating decision: it stays a sweep, and that is settled.** Same rule as clang-tidy and the
   hot-path check — a report states what it finds, consumers decide. It runs on push to this
   branch plus a weekly cron, never on `pull_request`, so it cannot block a merge. The Security
   tab keeps the open/fixed alert lifecycle, which is the baselining we would otherwise build.

   Also fixed here: the workflow triggers on branch `next-iteration`, but the branch had been
   named `next` — so CodeQL silently never ran on any push. Renamed the branch to match.
7. ✅ **Triage the real CodeQL findings. DONE.** The 3 `localtime` criticals were fixed earlier
   (a portable `isoTimestamp` helper in `main_desktop.cpp`); clang-tidy's `concurrency-mt-unsafe`
   flagged the identical three, and two independent tools agreeing was the signal to fix rather
   than suppress.

   The file-mode finding was NOT a literal `0666` — nothing in the tree ever had one. It is
   `std::fopen(..., "wb")` in `fsWriteAtomic` and the streaming-write path, which creates at
   `0666 & ~umask` (0644 in practice). These files are `/.config/*.json`, holding WiFi PSKs and
   MQTT passwords. Both now go through one `openTempOwnerOnly` helper: POSIX gets
   `O_CREAT|O_EXCL` with an explicit `0600`, Windows keeps plain `fopen` (no `mode_t`; files
   inherit the parent ACL). Verified on disk — 0644 before, 0600 after — and end-to-end against
   the running desktop binary: a live control change rewrote `SystemModule.json` as `-rw-------`.

   Open alerts are currently **0**, so there is nothing further to triage.

8. ✅ **clang-query — the bespoke-rule report. DONE.** `check_clang_query.py`, MoonDeck card
   "AST Rules". Shipped with two rules and the frame for more. Measured on this tree: 290
   RAM-costing arrays over 10 bytes (193 local, 97 member) and 78 heap allocation sites.
   Thresholds settled from the real numbers, not in advance — excluding constexpr/static
   storage took the array list from >1000 to 362, which is what made a 10-byte default usable.
   Two traps hit while building it, both silent-zero: a bare top-level `anyOf()` of `Stmt`
   matchers is ambiguous and matches NOTHING while exiting 0 (needs a `stmt(...)` wrapper), and
   the guard against that must key on more than `"error:"` — clang-query says "Input value has
   unresolved overloaded type". Original design: One MoonDeck entry,
   `check_clang_query.py`, holding a GROWING LIST of rules we invent — not one script per rule.
   Each rule is a matcher plus a Python predicate, and the report prints a section per rule, so
   rule two costs a list entry rather than a new script, a new card and a new help page.

   **First rule: large array declarations** (`char buf[512]`), the RAM-bloat question nothing
   else in the stack answers. Probed and working (§ clang-query above): match all
   `constantArrayType()` declarations, then filter in Python — there is no size-threshold
   matcher, so `> N bytes`, our-files-only, and the dedupe of repeated template instantiations
   are ours to do. Reuse `check_clang_tidy._toolchain_args()`: without `-isysroot` the same
   silent under-report appears (5 matches instead of 14).

   **PO's categories, each probed against the real tree:**

   | Ask | Verdict |
   |---|---|
   | Stack vs heap vs member | ✅ Separable. `varDecl` = locals/statics, `fieldDecl` = members (my first probe used only `varDecl` and silently missed EVERY class member). `hasStaticStorageDuration()` and `isConstexpr()` split flash-resident tables from real RAM. |
   | Fixed number vs named constant | ❌ **Not recoverable.** The AST folds `kMaxLanes` to `[16]` before we see it — `busPinBuf_` reads as `uint16_t[16]` with no trace of the spelling. Source text has it (730 literal-sized vs 105 `kConstant`-sized) but only via regex, which is the fragile approach this whole plan moved away from. Recommend dropping. |
   | Heap allocs and frees | ✅ Works. `cxxNewExpr()` / `cxxDeleteExpr()` match, as does `callExpr(callee(functionDecl(hasAnyName("malloc","calloc","realloc","free","heap_caps_malloc",…))))` with exact locations. Needs the our-files filter — a raw `cxxNewExpr()` run returns standard-library internals like `new _Codecvt`. |
   | 10-byte threshold | ⚠️ **Measured, and it is too low as a flat rule.** Three sampled files alone hold 245 array declarations: 212 are >10 bytes, 106 >64, 25 >256. Extrapolated across `src/`, >10 bytes is over a thousand findings. |
   | Nothing fixed, all dynamic | Agreed as the principle — but see below on what the 10-64 byte band actually contains. |

   **Why a flat 10-byte threshold would misfire.** Sampling the 10-64 byte band shows it is
   almost entirely small fixed string buffers and constant lookup tables, and several are the
   minimalism principle already applied rather than violated:
   - `MoonModule::name_[16]` — its own comment records it was shrunk FROM `char[24]` to save
     8 bytes per module (~240 bytes across a typical tree). Flagging it reports a past win as a
     problem.
   - `kRGB[3]` / `kGRB[3]` / `kBuiltins[]` — `static constexpr`, so they live in FLASH and cost
     no RAM at all. 16 of 17 array decls in `LightPresetsModule.h` are static-storage.

   So the useful report is not "arrays over N bytes" but **arrays that cost RAM**, which the
   probed matchers can express: exclude `isConstexpr()` / `hasStaticStorageDuration()`, then
   report locals (stack-depth risk), members (per-instance RAM × instance count) and mutable
   statics (unconditional RAM) as three separate lists. At that point a threshold near 10 bytes
   is plausible, because the flash-resident noise is already gone. **Measure before fixing it.**

   Still open, and to be decided on the first real run rather than now: the exact threshold per
   category, and whether the count needs `check_lizard.py`'s baseline shape or is small enough
   to be a plain report.

   The rule after that is unassigned on purpose — the point of the step is the *frame*, so the
   next rule is a matcher and a threshold rather than a project.

## Evidence

### CodeQL — run, and it delivered

CodeQL 2.26.1, `build-mode: none`: ~3 min, **179 rules, 867 results**, no build required.

- **3 × critical** `localtime` in `main_desktop.cpp` — real (shared static, not thread-safe),
  desktop-only. FIXED via a portable `isoTimestamp` helper; clang-tidy independently flagged the
  same three as `concurrency-mt-unsafe`.
- **1 × critical** use-after-free in `test/doctest.h` — vendored; exclude it.
- **5 × high** files created mode `0666` — meaningless on LittleFS, minor hardening on desktop.
- **1 × high** `Control.cpp:168`, `uint8_t o < c.max` where `max` is `int32_t` — benign today
  (`addSelect` takes a `uint8_t` count) but a latent trap if that signature widens.

**The six network packet parsers produced no taint-flow findings** — positive evidence about
~22 `memcpy` calls on LAN data that nothing else in the stack could have given.

### clang-query — the reachability limit

Reachable rule class: per-function AST and lexical position — **not** whole-program
reachability. That limit still shapes what goes here: the one rule needing reachability
(allocation reachable from `tick()`) is owned by step 3's compiler check, not by a matcher.

### Two gotchas worth keeping

- **The compilation database's compiler must match the tool's.** A database generated by Apple
  Clang while analysing under Homebrew LLVM produced `'cstdint' file not found` on every file
  and silently blocked clang-query entirely.
- **`workflow_dispatch` only offers a "Run workflow" button on the default branch**, so a POC
  workflow on a feature branch needs a `push:` trigger. And `/code-scanning/alerts` returns
  `0` for a non-default branch unless you pass `?ref=refs/heads/<branch>` — that zero does not
  mean "clean".

## What we got wrong

Recorded because the mistake is instructive, and because the first draft of this plan
**declined clang-tidy on bad evidence**.

The first evaluation ran each tool *once, unconfigured*, and scored the raw output. For
clang-tidy that meant enabling `bugprone-*,performance-*,concurrency-*` — a shotgun — then
counting the noise that choice produced and calling it a tool defect. 66% of the findings came
from `bugprone-easily-swappable-parameters`, **a check that SerenityOS, ClickHouse, ESPHome,
Godot and the Zephyr template all disable by name**. Turning it off is standard practice.

The comparison was also structurally unfair: CodeQL was given a written workflow with a
curated query set, clang-tidy got one command line, and the two outputs were then compared as
though that were like-for-like.

The lesson, and the reason this plan now leads with configuration: **a static-analysis tool's
default output is not its verdict.** Evaluating one without a curated check list measures the
evaluator's config, not the tool.

### The silent-failure modes, all of which read as "clean"

Five separate defects in the tooling each produced a plausible-looking report rather than an
error. This is the part worth carrying forward, because every one of them would have let a
green check certify an unanalysed tree:

| Defect | What it looked like |
|---|---|
| `#` inside a YAML `>-` folded scalar is not a comment | Every disable after the first comment was folded into the check string; `abseil-*` stayed on → 12,181 findings |
| `run-clang-tidy` shells out to `clang-tidy` **by name** | Not on PATH → exits 0 having analysed nothing → "0 findings" |
| `-extra-arg VALUE` (space form) is silently ignored | Only `-extra-arg=VALUE` works → 0 findings |
| Same trap in `-checks` | `--check X` filtered nothing; the filter had never worked |
| Compilation database recorded a *different* compiler | `'cstdint' file not found` on 129/129 files; unparsed files are never analysed, so the findings were debris |

The defence now in `check_clang_tidy.py`: it refuses to report when more than ten files fail to
compile, because "most files errored" is a broken run, not a result. The general rule — **verify
a zero before believing it.** After reaching 0 findings, running a deliberately-disabled check
(`--check readability-magic-numbers`) returned 3,307, which is what proves the pipeline is
actually reading the code. A zero with no control is indistinguishable from a tool that ran
nothing.
