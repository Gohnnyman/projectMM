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
analyser can replace them.

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

### Is lizard the right tool? Yes — as a counter, not an analyser

The question this plan opened with, answered directly.

**Keep it.** It is actively maintained (1.23.0, June 2026), runs in ~1 s, and it does one
thing the rest of the stack does not: it produces a **number per commit** that we can trend.
clang-tidy tells you a function is too complex *today*; lizard tells you whether the codebase
is getting worse *over time*. Those are different jobs, and `repo-health.json` needs the
second.

**But only as a counter.** Its own README warns it is a fuzzy tokenizer, not a parser — no
macro expansion, confused by heavy templates. It can never express an architectural rule, so
it is not a platform to build on.

**Its 162 warnings are a threshold artifact, not a verdict:** 161 come from `CCN>10` alone,
and 80% of 2,185 functions sit at CCN ≤ 5. The real tail is 4 functions above CCN 50. A metric
that can never reach zero is a poor gate, which is what baselining fixes.

**Its VS Code extension is dead** (v1.0.1, Oct 2022) — do not build on it. The live in-editor
equivalent is clang-tidy's `readability-function-*` through clangd, which is layer 1.

**Overlap with clang-tidy, settled:** both can measure complexity, so they do not both gate.
**lizard owns the metric and the trend**; clang-tidy's complexity checks stay off. One number,
one owner.

**Deleted:** `check_hotpath.py` (170 lines) once layer 3 is green — the compiler subsumes it
transitively, which a regex never could.

**Declined:** cppcheck (modest unique yield next to a clean clang-tidy), SonarCloud (a second
findings home; no custom C++ rules at any tier), custom CodeQL queries (the one rule we named
is owned by layer 3), Semgrep (C++ GA is paywalled), Aikido (AppSec aggregator, wrong
category), PVS-Studio / CodeScene / MISRA suites (built for certification, not for us),
`-Weverything` and GCC `-fanalyzer` (Clang's and GCC's own docs advise against, respectively).

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

The centrepiece, and the part the first attempt botched. Real projects use one of three
shapes; ours is **archetype B — enable families, disable individually with a stated reason**
(the [SerenityOS](https://github.com/SerenityOS/serenity/blob/master/.clang-tidy) shape).

Nobody serious enables `cppcoreguidelines-*` or `hicpp-*` wholesale: mostly aliases plus
bounds/cast rules that firmware register access must violate. ClickHouse disables the family
as *"impractical… also slow"*; ESPHome — a large ESP32 C++ codebase, our closest peer — does
the same and still runs `WarningsAsErrors: '*'`.

Starting config, **derived bottom-up from ESPHome's** (their `.clang-tidy` is `*` minus 175
checks with `WarningsAsErrors: '*'` — battle-tested on a large ESP32 C++ codebase, the closest
peer we have), then tuned top-down against our own tree. The full file is written at
implementation time; the shape is `*` minus ~78 disables, `HeaderFilterRegex: 'src/(core|light)/'`.

**Tuning it was iterative, and the numbers show why the first attempt failed:**

| Config | Unique findings in our code |
|---|---:|
| My original shotgun (`bugprone-*,performance-*,concurrency-*`) | 384, of which 66% one noisy check |
| `*` + ESPHome's family disables only | **6,073** |
| + their style-check disables | 3,949 |
| + the `cert-*` family (`cert-err33-c` alone was 3,678 — every `snprintf`) | **131** |

**131 real findings** — a tractable, mostly-actionable list. Three checks ESPHome disables that
I had wrongly called useful in the first evaluation: `performance-enum-size`,
`bugprone-narrowing-conversions`, `bugprone-easily-swappable-parameters`. They run the same
class of memory-constrained device and still reject all three.

**Triage outcome: 125 → 47.** Every finding was read against the actual code rather than
trusted. The remaining 47 are all `clang-analyzer-*` — the path-sensitive family — and they
were invisible until `WarningsAsErrors` was switched on: the report parser rejected the
`,-warnings-as-errors` suffix clang-tidy then appends and dropped every finding. So the
"0" this section originally claimed was partly a parser bug, which is the sixth silent-zero
this exercise produced. Backlogged: *clang-tidy: triage the 47 clang-analyzer findings*.
The split, which is the number worth remembering for the next tool evaluation:

| Disposition | Count | Examples |
|---|---:|---|
| Real defects, fixed | 2 | `std::forward` inside a loop (below); a duplicated `TEST_CASE` + its duplicate include |
| Genuine improvements, applied | ~20 | `localtime`→`localtime_r`, `std::numbers::pi`, `scoped_lock`, `ranges::any_of`, two accidentally-private overrides, a throwing test-rig destructor |
| Deliberate convention → check disabled with a measured reason | ~80 | see the disable table in `.clang-tidy` |
| Deliberate at one site → `NOLINT` with a reason | 12 | Bresenham's assign-and-test; asserting moved-from state *is* the test |

**The real bug it found** is in `HttpServerModule.cpp`: `visitModuleLeaves(mod, std::forward<Fn>(fn))`
called inside a `for` loop, at two levels. Forwarding moves the callable into the first module,
so every later sibling receives a moved-from object. It survived because the callables in use
happen to be cheap-to-copy lambdas, which is precisely the kind of latent, works-by-luck defect
a reviewer skims past.

**The disabled checks are the more interesting result.** Four families were wrong on *every*
occurrence, each because it collides with a deliberate convention: `bugprone-signed-char-misuse`
(12/12 — and its suggested fix would turn the `-1` unset-pin sentinel into GPIO 255, a real bug),
`performance-no-int-to-ptr` (9/9, the `uintptr_t` tagged-pointer field), `bugprone-infinite-loop`
(28/28, `uint8_t` counters), `bugprone-implicit-widening-of-multiplication-result` (47 findings,
0 reachable — margin ~87,000×). A check that is wrong every time trains you to ignore its family,
which costs more than it catches; the reasoning for each is recorded in `.clang-tidy` so the next
reader does not re-litigate it.

A sample false positive for contrast: `WledPacket.h:80` "memcpy result is not
null-terminated" — the buffer is pre-zeroed by a `memset`, as the adjacent comment says. That
one gets a `NOLINTNEXTLINE` with the reason, which is the intended workflow.

`bugprone-infinite-loop` **stays enabled** despite being 26/28 false on our `uint8_t` counters
in the first run: the FPs are a known LLVM issue with fixes still landing, and an FP there
sometimes reveals a genuinely missing `volatile`. Each gets a `NOLINTNEXTLINE` with a reason.

Rejected checks stay listed in the config with their reason, so they are never re-litigated —
[the Chromium pattern](https://github.com/chromium/chromium/blob/main/.clang-tidy).

**A structural catch for our layout:** clang-tidy picks its config from the *translation
unit's main file*, so a `src/light/.clang-tidy` would **not** govern our header-only light
modules — they are compiled as part of some `.cpp` elsewhere. Per-directory strictness on the
core/light split therefore does not work as one might assume. The working shape is one root
config plus a relaxed `test/.clang-tidy` (`InheritParentConfig: true`), with
`HeaderFilterRegex` covering the headers.

## Implementation

Each step is independent and revertible. **✅ done · ◻ not started · ◐ partly done.**

1. ◻ **Warnings tier-zero** — add `-Wshadow -Wnon-virtual-dtor -Wdouble-promotion
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
3. ◻ **`[[clang::nonblocking]]`** on `MoonModule::tick/tick20ms/tick1s` (three lines; the
   attribute is inherited by overrides, so ~90 modules are covered) + `-Wfunction-effects` on
   the desktop build. Then delete `check_hotpath.py`.
4. ◻ **RealtimeSanitizer** — add `realtime` to the sanitizer matrix in `test.yml`. Only
   meaningful after step 3, since it keys off the same attributes.
5. ◐ **clang-tidy** — config landed, clangd wired, and the tree triaged from 125 findings to
   **0** (see the triage table above). What remains is the ratchet: `WarningsAsErrors` is still
   `''`, and cannot go to `'*'` until the 47 clang-analyzer findings are triaged — switching it
   on today fails the gate. One line, and it is what stops a zero decaying, so it lands WITH that
   triage. Original text: land the config above, wire **clangd first** (editor-only, gates nothing,
   and it filters slow checks automatically). Then one full run per family: real bug → fix;
   FP → `NOLINTNEXTLINE` with a reason; loud-and-useless → the disable list with a comment.
   When a family is clean it joins `WarningsAsErrors`. Target end state is **zero baseline** —
   at 50k LOC that is reachable, which is why we skip CodeChecker and diff-only CI entirely.
6. ◻ **CodeQL housekeeping** — exclude vendored code (`test/doctest.h` produced the only
   critical we do not own) and decide whether it gates PRs or stays a weekly sweep.
7. ◐ **Triage the 4 real CodeQL findings** — the 3 `localtime` criticals are DONE (a portable
   `isoTimestamp` helper in `main_desktop.cpp`, `localtime_r`/`localtime_s` behind the file's
   existing `_WIN32` branch); clang-tidy's `concurrency-mt-unsafe` flagged the identical three,
   so two independent tools agreeing was the signal to fix rather than suppress. Remaining: file
   modes `0666` → `0600` where it matters (desktop only; meaningless on LittleFS).

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

### clang-query — probed as the bespoke-rule engine

The one part of the first evaluation worth keeping, because it tested *authoring a rule*
rather than reading default output.

A matcher took minutes and no build:
`match callExpr(callee(functionDecl(hasName("malloc"))), hasAncestor(functionDecl(matchesName("tick.*"))))`
→ 0 matches, a true clean result (broad control matchers returned 42 and 245, proving the
machinery works rather than silently passing). It reaches the **245 control registrations** a
future "a conditional control is registered below the control it depends on" rule would need.

Class of rule reachable: per-function AST and lexical position — **not** whole-program
reachability. That limit is fine, because the one rule needing reachability (allocation
reachable from `tick()`) is owned by layer 3.

### `[[clang::nonblocking]]` — verified locally

Clang 22 caught a `push_back` two levels deep through a helper, tracing the chain into libc++.
The attribute is **inherited by overrides**, so one annotation on the base covers every module.

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
