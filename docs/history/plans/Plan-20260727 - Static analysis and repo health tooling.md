# Plan: static analysis + repo-health tooling

**Date:** 2026-07-27 · **Status:** proposed

Temporary document: this text becomes the PR description and the file is deleted once the
plan is realized (CLAUDE.md § Branch).

## Why

Two questions started this: *"is lizard the right tool?"* and *"how do I monitor the repo
against our principles, drill down, and find what to work on?"* Research into the 2025-26
field (SonarQube, CodeScene, Qlty, Semgrep, Aikido, clang-tidy, CodeQL, cppcheck, and what
Zephyr/ESP-IDF/Chromium actually do) produced one clear answer:

> Keep the bespoke checks as the rule **content**; adopt SARIF + GitHub code scanning as the
> shared **reporting plane**; add the two tools that can express what our checks cannot.

Our Python checks encode domain knowledge no shelf tool ships (spec/doc drift, catalog
consistency, flash budgets). What they lack is not intelligence but infrastructure: PR
annotations, baselining, history. GitHub gives that free to anything emitting SARIF.

But "keep the checks" is not a free pass: two of the nine are code-semantics rules that a
real tool does better, and they should retire. Step 4 audits all nine and records a verdict
for each, so this plan removes as well as adds.

The full research is in the PR discussion; the decisions that survive it are below.

## Findings that shape the plan

- **lizard is maintained** (1.23.0, June 2026) but is a *fuzzy tokenizer, not a parser* — its
  own README warns about templates and macros. It measures size and branching; it can never
  express an architectural rule. **Keep it as a counter, not a platform.**
- **Its VS Code extension is dead** (v1.0.1, Oct 2022). The live-in-editor equivalent is
  clang-tidy's `readability-function-cognitive-complexity` through clangd.
- **Our 162 lizard warnings are a threshold artifact**, not a health verdict: 161 come from
  `CCN>10` alone, and 80% of 2,185 functions sit at CCN ≤ 5. The tail is 4 functions above
  CCN 50. A metric that can never reach zero is a poor ratchet — hence baselining.
- **Clang 22 is installed locally and `[[clang::nonblocking]]` works today.** Verified: it
  caught a `push_back` two levels deep through a helper and traced the chain into libc++ —
  something a regex lint cannot do in principle. **The attribute is inherited by overrides**,
  so annotating `MoonModule::tick()` once covers every module.
- **The repo is public**, so CodeQL and Actions minutes are free.
- **CodeQL C/C++ build-free scanning went GA in Oct 2025**, so no ESP-IDF cross-build needs
  tracing in CI, and QL has transitive call-graph reachability (`f.calls*(g)`) — the one
  thing that expresses "allocation *reachable from* tick()".
- **Aikido is the wrong category** (AppSec/ASPM aggregator, generic rules over C++).
  **Semgrep's C++ is GA only in the paid tier.** Both rejected.

## Shape: a POC, then one judgement

This is **not** a commitment to adopt seven tools in order. It is a **proof of concept**: run
each candidate against this codebase, on a throwaway branch, long enough to see what it
actually finds here — then make **one** decision about the final set, combining the best of
each and discarding the rest.

The reason is that none of the research answers the question that matters. "clang-tidy is
industry standard" is true and useless; what matters is *what it finds in `src/light/` that
we care about*, and whether that is worth its config. A tool that is excellent in general and
silent on our code has not earned a place in our gates.

**Phase 1 — POC (throwaway, nothing lands in main).** Run every candidate, capture what it
found on our code, and record the cost of running it. Timebox each; a tool that needs a week
of fighting to produce a first result has told us something.

**Phase 2 — one judgement.** With every candidate's real output in hand, pick the final set
in a single decision and write it down: which tool owns which rule, what got deleted, and
what we deliberately declined. Then implement only that.

Two candidates are exempt from the POC, and it is worth being explicit about why:

- **`[[clang::nonblocking]]`** is not a tool adoption — it is three lines in a header and a
  compiler flag we already have. It has effectively been POC'd already: verified on this
  machine, on real code, catching a transitive allocation. Nothing to evaluate.
- **The lizard baseline** is not a tool choice either; lizard is already in the gates and the
  whitelist just makes it usable. Do it whenever, independent of everything else.

Everything else — clang-tidy, CodeQL (both jobs), SARIF reporting, cppcheck addons,
SonarCloud, RTSan — gets tried before it gets adopted.

## What each POC must produce

For every candidate, the same six answers, so the final judgement compares like with like.
The first three are about **coverage** (what it does for us now), the next two about
**configurability and extensibility** (what it will do for us later), and the last about cost.

**Coverage — what it catches today**

1. **Real findings on our code.** Not "it supports X" — the actual list, and how many of them
   we would act on. A tool with 200 findings we would all ignore scores worse than one with
   three we would fix.
2. **What it uniquely covers.** The rules it can enforce that nothing else in the candidate
   set can. This is what earns it a place; overlap alone earns nothing.
3. **Signal quality.** False-positive rate on our code, and whether a finding is actionable
   or just true. This is what decides whether it gets gated on or merely reported.

**Configurability — can we shape it to this codebase?**

4. **Tuning, scoping and baselining.** Can thresholds and rule sets be set *per directory*,
   so `src/core/` can be stricter than `src/platform/`? Can existing violations be frozen so
   the gate fails only on new ones? Is the config a file in the repo (reviewable, versioned)
   or CLI flags buried in a script? Can a single justified violation be suppressed **at the
   line, with its reason**, the way our `-Wno-…` and `// hot-path-ok:` conventions require?

   *Concretely, each tool must answer:* how would we express "CCN limit 8 in core, 12 in the
   light domain", and how would we silence one deliberate exception without silencing the rule?

**Extensibility — can it absorb the rules we have not invented yet?**

5. **Writing a new rule.** The stated expectation is that project-specific rules keep coming.
   So for each tool: **write one real custom rule during the POC** and record what it took —
   an hour, a day, or "not possible". Suggested probe, since it is a rule we would actually
   want and no tool ships it: *a control must be registered below the control it depends on*
   (coding-standards § Conventions), or failing that, any rule from CLAUDE.md the tool could
   plausibly express.

   Also record: what *class* of rule is reachable (lexical? per-function AST? whole-program
   call graph?), whether the rule lives in our repo as reviewable source, and whether writing
   it couples us to a toolchain version or a vendor.

**Cost**

6. **Cost to run and keep.** Setup, CI wall-clock, config surface, and what breaks when the
   toolchain moves. A check that only works on one developer's machine is not a gate.

Record these per tool in the PR as the POC runs, not from memory afterwards. **A tool that
scores well on 1–3 but badly on 4–5 is a trap**: it looks good on the day we adopt it and
becomes the thing we cannot bend six months later.

The scorecard to fill in — one row per candidate, so phase 2 is a comparison and not a
recollection:

| Tool | Findings we'd act on | Uniquely covers | False positives | Per-dir config / baseline / line-suppress | Custom rule: effort + class | Cost |
|---|---|---|---|---|---|---|
| clang-tidy | ~90 of 384 unique (34 enum-size + ~56 to review) | enum sizing, narrowing, in-editor via clangd | **high: 66% noise + `infinite-loop` 26/28 false** | per-dir `.clang-tidy` + `InheritParentConfig`; `NOLINT(reason)`; no baseline file | via clang-query: **minutes**, per-function AST only | 4.6 s full run; needs a compiler-matched `compile_commands.json` |
| clang-query *(new candidate)* | n/a — a rule engine, not a finder | bespoke per-function/lexical rules, no plugin build | n/a (we write the rule) | matchers are plain text in-repo | **minutes**; reaches the 245 control registrations the ordering rule needs | free; same database as clang-tidy |
| CodeQL (stock) | | | | | | |
| CodeQL (custom QL) | | | | | | |
| cppcheck (+addon) | | | | | | |
| SonarCloud | | | | | | |
| lizard *(incumbent)* | | | | | | |
| our Python checks *(incumbent)* | | | | | | |

The two incumbents are in the table deliberately: they have to survive the same comparison as
the newcomers, or the audit is only looking one way.

## The rule this plan runs under

The goal is **one lean, coherent, industry-standard toolset** — not a pile of overlapping
analysers that each half-cover the same rule. A tool that duplicates another tool's job is
worse than no tool: it doubles the config, splits the findings across two reports, and makes
"where is this rule enforced?" unanswerable.

So every step below ends with the same three questions, answered in writing in the PR before
the step counts as done:

1. **What does this tool now own, exclusively?** Name the rules it is the single home for.
2. **What did it make redundant?** Name what is now deleted or scheduled for deletion. A
   step that adds a tool and retires nothing needs an explicit reason why.
3. **Is it still the leanest way to get this?** If a tool earns its place for one rule only,
   say so and consider whether that rule is worth a whole tool.

**Overlaps are decided, not accumulated.** Where two tools *can* check the same thing, one is
designated the owner and the other's version is turned off — briefly running both to compare
is fine, permanently running both is not. Known candidates, to be settled as we go:

| Rule | Candidate owners | Decision |
|---|---|---|
| No allocation/blocking in the render path | `check_hotpath.py`, Clang function-effects, RTSan, CodeQL | Static owner = Clang attributes (step 2); RTSan is the *dynamic* complement, not a duplicate; delete `check_hotpath.py`; do **not** also write a CodeQL query for it |
| Platform boundary | `check_platform_boundary.py`, CodeQL | Settle at step 6 — keep exactly one |
| Function complexity | lizard, clang-tidy `readability-function-*` | Settle at step 5 — lizard for the *metric/trend*, clang-tidy for the *in-editor nudge*; do not gate on both |
| Finding transport | bespoke stdout, SARIF | SARIF, once step 4 lands |
| Drill-down + history | hand-built dashboard, GitHub code scanning, SonarCloud | Settle at step 7 |
| **Security / untrusted input** | **nothing today**, CodeQL, cppcheck, PVS-Studio | **CodeQL** — no overlap to settle, because nothing currently covers this at all |

The end state to aim for: **each rule has exactly one enforcing tool, each tool has a clear
job, and every finding arrives through the same channel.** If we cannot say that at the end,
the plan has failed regardless of how many tools we adopted.

## The candidates

What follows is the POC backlog, not a running order. Each entry says what to try and what
would make it worth keeping; the six answers above get recorded for each as it runs.

Two of them (1 and 2) are the exempt pair from above — do them whenever, they need no
evaluation. The rest are candidates until phase 2 says otherwise.

### 1. Baseline lizard (small)

Turn an unreachable number into a working ratchet.

- Generate `moondeck/check/whitelizard.txt` freezing today's 162 violations.
- Run lizard with `-W`, so the gate fails only on **new** violations.
- Keep feeding the count into `repo-health.json` for the trend.

*Outcome:* complexity becomes a gate that can pass, and any new offender is visible
immediately. Existing debt stays measured but stops crying wolf.

### 2. `[[clang::nonblocking]]` on the tick methods (small, highest value)

Replace a best-effort regex lint with a compiler-verified, transitive guarantee.

- Annotate `MoonModule::tick/tick20ms/tick1s` (three lines).
- Add `-Wfunction-effects` to the desktop build; the ESP32 build keeps its own toolchain.
- Fix or explicitly opt out whatever it surfaces. Early probe suggests the hot path is
  already clean, so fallout should be small.
- Keep `check_hotpath.py` until the compiler check is green on both toolchains, then delete
  it — the compiler subsumes it, and per *Default to subtraction* the weaker check goes.

*Risk:* the ESP32 toolchain's clang may lag on this attribute; the desktop build is the gate
either way, so this degrades to "checked on one target", which is still more than today.

### 3. RealtimeSanitizer in the sanitizer matrix (small)

The dynamic counterpart: `kind: [address, thread, realtime]` in `test.yml`, running the
existing scenarios. Catches at run time what the static analysis over-approximates.

*Note:* only meaningful once step 2 lands, since RTSan keys off the same attributes.

### 4. Re-evaluate every existing check, then migrate (medium)

Before adding tooling, audit what we already run. The MoonDeck **Check** group holds nine
entries; each gets a verdict — *keep as is*, *migrate*, or *delete* — and the reasoning is
recorded here so a future reader knows why each survived.

The distinction that decides most of them: a check validating **C++ semantics** can move to
a shelf tool; a check validating **consistency between artifacts** (code ↔ docs, code ↔ JSON
catalog, source ↔ built binary) cannot, because the other half of the contract is not C++.
No static analyser models "this module's spec page lists the controls the header declares".

| Check | Lines | What it validates | Proposed verdict |
|---|---:|---|---|
| `check_specs` | 438 | code ↔ docs drift (control names, ranges, docPath anchors) | **Keep** — cross-artifact; nothing else can see both sides. Add SARIF output. |
| `check_platform_boundary` | 79 | no platform `#include`/`#ifdef` outside `src/platform/` | **Migrate to CodeQL** (step 6), keep both briefly, then delete the weaker one. It is a code-semantics rule and CodeQL sees includes properly. |
| `check_hotpath` | 170 | regex for allocation/blocking in tick methods | **Delete** once step 2 is green on both toolchains — the compiler subsumes it transitively, which the regex cannot do at all. |
| `check_devices` | 292 | installer catalog ↔ registered module types + assets on disk | **Keep** — cross-artifact (JSON ↔ C++ ↔ image files). Add SARIF. |
| `check_firmwares` | 48 | `firmwares.json` matches the `FIRMWARES` dict | **Keep** — pure generated-file drift check, trivially cheap. |
| `check_esp32_built` | 126 | firmware binary newer than its sources | **Keep** — build-state check, not code analysis. |
| `collect_kpi` | — | performance + size measurement | **Keep** — measurement, not a rule. |
| `repo_health` | — | the ratchet snapshot | **Keep**, but see step 7 for its UI half. |
| `check_sanitizers` | — | ASan/TSan wrapper | **Keep**, extended by step 3. |

Then give every *surviving* check a `--sarif` flag and upload each under its own category in
CI.

*Outcome:* findings become PR line annotations with GitHub's own open/fixed/dismissed
lifecycle — baselining and history we would otherwise have to build — and the check list
shrinks by the two that a real tool does better.

*This step is where the plan pays for itself in subtraction:* two checks retire, and the
remaining seven stop needing bespoke reporting.

### 5. clang-tidy + clangd (medium)

- Root `.clang-tidy` with a deliberately small set (`bugprone-*`, `performance-*`,
  `concurrency-*`), tightened per-directory — `InheritParentConfig` maps onto our
  core/light/platform split.
- Desktop build already emits a compilation database; add `CMAKE_EXPORT_COMPILE_COMMANDS`
  if missing.
- Same config drives clangd, so the rules appear in the editor as you type.

*Rule:* never enable a check family we are not willing to gate on. Expand one at a time.

**Overlap to settle here:** clang-tidy's `readability-function-size` and
`readability-function-cognitive-complexity` measure what lizard measures. Do not gate on
both. The split that keeps each earning its place: **lizard owns the metric and the trend**
(it feeds `repo-health.json`, and its whitelist is the ratchet), **clang-tidy owns the
in-editor nudge** (it tells you while you type, which lizard cannot). If that split feels
like a distinction without a difference once both are running, drop the clang-tidy checks —
the trend matters more than the nudge.

### 6. CodeQL with a custom query pack (larger)

CodeQL is doing **two jobs**, and they justify themselves separately. An earlier draft of
this plan treated it as an optional extra for custom rules only; that undersold it.

**Job one — security analysis of untrusted input. This one stands on its own.** The firmware
parses six network packet formats (`ArtNetPacket`, `DdpPacket`, `E131Packet`,
`WLEDAudioSyncPacket`, `MqttPacket`, `WledPacket`) plus HTTP, doing ~22 `memcpy` operations
on data that arrives from the LAN — on a device with no MMU and no process isolation, where a
buffer overrun is remote code execution on someone's lighting controller. **We run no
security scanning at all today.** CodeQL's stock C/C++ suite is built precisely for this:
taint tracking from a network source to a memory-unsafe sink, reported as a data-flow path.
Nothing else in this plan looks for that class of bug — the compiler checks effects, lizard
counts branches, our Python checks read text. This is a genuine hole, and it is the strongest
argument for CodeQL regardless of the custom-rule question.

**Job two — bespoke rules whose enforcement needs the call graph.** Assessed on its own
merits *after* job one is running, against a specific named rule (see the prohibition below).

- Advanced-setup workflow, `build-mode: none`, nightly + on PR.
- Stock `cpp` suite first, with the `security-and-quality` query set — that is job one, and
  it is the part to keep even if we never write a line of QL.
- Only then, if a rule needs it, an in-repo query pack.

**Do NOT write a CodeQL query for allocation-reachable-from-`tick()`.** Step 2 already owns
that rule at compile time, where it is faster and closer to the author. A second
implementation would be exactly the overlap this plan exists to avoid — and the earlier draft
of this plan proposed it, which is how easily it happens.

The platform-boundary rule is the honest CodeQL candidate: it is code semantics, and CodeQL
sees includes properly where a regex sees text. If it lands, `check_platform_boundary.py`
is **deleted**, not kept alongside.

*Cost:* QL has a real learning curve. The VS Code extension (GitHub-published, released
within the week) is the authoring environment — note it is for *writing queries*, not live
diagnostics.

### 7. Decide the dashboard question (decision, not code)

With steps 4 and 6 in place, GitHub code scanning already provides drill-down, history and
ranked findings for free. **Re-evaluate whether the hand-built repo-health drill-down still
earns its place** — it may be replaceable by code scanning plus optionally switching on
SonarQube Cloud's free automatic analysis (one click, no CI change, deletable).

The `repo-health.json` ratchet stays regardless: no static analyser tracks flash size per
firmware variant, and that is our tightest constraint.

## Explicitly not doing

- A lizard VS Code extension (dead upstream, and clang-tidy covers it live).
- Compiled clang-tidy plugin checks — LLVM-version-coupled, and **clangd will not load
  them**, so they would never appear in the editor. CodeQL does the same job better.
- Semgrep for C++ architectural rules (paid GA; free tier experimental and per-file).
- Aikido, and any enterprise MISRA suite (Klocwork/Parasoft/QAC/Polyspace).
- Porting the Python checks *into* a shelf tool for its own sake. The checks are fine; it is
  the reporting plane that standardises.

## Running the POC

**Cheapest-first, because an early answer changes later questions.** Enabling CodeQL's stock
suite is a workflow file and no code change; clang-tidy needs a config and a compilation
database; SARIF output needs a flag per check. Start where the answer costs least.

A rough order, adjustable as results come in:

1. **CodeQL stock suite** — one workflow file, free, and it answers the biggest open question
   (do we have real security findings in six untrusted-input parsers?). If it finds nothing,
   that is worth knowing; if it finds something, everything else waits.
2. **clang-tidy** on the desktop compilation database with a small check set — one run tells
   us whether it finds anything we would act on.
3. **SARIF from one Python check** — prove the reporting plane on `check_platform_boundary`
   (the smallest at 79 lines) before converting the rest.
4. **RTSan**, once the Clang attributes are in — it keys off them.
5. **cppcheck addon / SonarCloud** — only if a gap survives the above.

Timebox each. The output is one filled-in scorecard row per tool, recorded in the PR.

**Include the custom-rule probe in the timebox.** A tool's config and extensibility only
reveal themselves when you try to bend it — the day-one experience of every analyser is
good, and that is not what we are measuring.

## Phase 2: the judgement

One decision, made once, written down. It must state:

- **The final set** — which tools we keep, and which rule each one *owns*.
- **What is deleted** — every check the new set makes redundant, retired in the same change.
  If nothing is deleted, the plan failed its own subtraction test.
- **What we declined and why** — so this research is not repeated in six months. Candidates
  rejected in the POC are as valuable a record as the ones adopted.
- **Where findings go** — ideally one channel for everything.

**The measure of success is not how many tools we adopted.** It is whether a new contributor
can say, for any rule, which single thing enforces it — and whether the toolset came out
smaller and clearer than the pile of overlapping analysers we could have accumulated instead.

**Two entries exist to remove things, not add them** — candidate 4 (retire `check_hotpath`,
migrate `check_platform_boundary`) and candidate 7 (re-evaluate the hand-built drill-down
against what code scanning gives free).

## POC results

Recorded as each candidate runs. Raw numbers, not impressions.

### clang-tidy — run 2026-07-27, one translation unit (`HttpServerModule.cpp` + its headers)

Setup cost was low: no `CMAKE_EXPORT_COMPILE_COMMANDS` in our CMakeLists, but generating a
database into a throwaway build dir took one command and touched nothing in the repo.
Homebrew LLVM 22 was already installed.

Checks tried: `bugprone-*`, `cppcoreguidelines-no-malloc`, `performance-*`, `concurrency-*`.
**119 findings from ONE translation unit**, distributed:

| Check | Count | Verdict |
|---|---:|---|
| `bugprone-easily-swappable-parameters` | 85 | **noise** — 71% of the total from one check almost nobody enables |
| `performance-enum-size` | 14 | **worth considering** — each is 3 wasted bytes per enum; on a 180 KB-heap ESP32 that is not nothing, and it aligns with our minimal-memory principle |
| `bugprone-infinite-loop` | 6 | **false positive** — verified `AudioBands.h:74`, a plain `for (uint8_t e = 0; e <= 16; e++)`. Not infinite. |
| `bugprone-narrowing-conversions` | 4 | plausible, needs review |
| `bugprone-branch-clone` | 4 | plausible, needs review |
| `bugprone-assignment-in-if-condition` | 3 | plausible, needs review |
| `bugprone-implicit-widening-of-multiplication-result` | 2 | plausible, needs review |
| `clang-diagnostic-error` (`'cstdint' file not found`) | 1 | **extraction gap** — header resolution imperfect even with the database |

**Reading:** the default-ish check set is unusable as a gate — one check produces 71% of the
output and another produces confident false positives. But a *narrow* set looks genuinely
useful, and `performance-enum-size` in particular speaks directly to a project principle.

**Implication for adoption:** the value is real but conditional on curation. This is the
"never enable a family we are not willing to gate on" rule proving itself on day one: had we
adopted the recommended set wholesale, we would have imported 85 findings we do not care
about and 6 that are wrong.

#### Full-codebase run (same day)

`run-clang-tidy` over every `.cpp` in `src/core` + `src/light`, 4 jobs: **4.6 seconds wall
clock** — cheap enough to gate on. Raw output was 6,305 findings, but that counts each header
once per translation unit that includes it; **deduplicated it is 384 unique findings.** Always
deduplicate before judging a header-heavy codebase, or the number is meaningless.

| Check | Unique | Verdict |
|---|---:|---|
| `bugprone-easily-swappable-parameters` | 254 | **noise** — 66%, consistent with the single-file sample |
| `performance-enum-size` | 34 | **worth doing** — 3 bytes each, and it matches our minimal-memory principle |
| `bugprone-branch-clone` | 31 | needs review |
| `bugprone-infinite-loop` | 28 | **broken here — see below** |
| `bugprone-narrowing-conversions` | 17 | needs review |
| `clang-diagnostic-error` | 9 | extraction gaps |
| `bugprone-throwing-static-initialization` | 6 | needs review — plausibly real |
| others | 5 | needs review |

**`bugprone-infinite-loop` is systematically wrong on this codebase: 26 of its 28 findings
have the loop counter incremented on the very line it flags.** Verified examples:
`for (uint8_t e = 0; e <= 16; e++)`, `for (uint8_t i = childCount_; i > 0; i--)`. The check
cannot track `uint8_t` counters — and `uint8_t` counters are a deliberate convention here
(coding-standards § Prefer integers). A check that misfires on a house convention is worse
than useless: it would train us to ignore its whole family.

**Toolchain gotcha worth recording:** the first `compile_commands.json` was generated with
Apple Clang (`/usr/bin/c++`) while the analysis ran under Homebrew LLVM 22, so every file hit
`'cstdint' file not found`. That produced the 9 extraction errors AND silently blocked
clang-query entirely. **The database's compiler must match the tool's.** Regenerating with
`-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++` fixed it completely.

#### Custom-rule probe (criterion 5) — clang-query

Tried `clang-query` before any compiled plugin, per the research's "cheap path first".
Findings:

- **Writing a matcher is minutes, not days.** `match callExpr(callee(functionDecl(hasName(
  "malloc"))), hasAncestor(functionDecl(matchesName("tick.*"))))` — one line, no build, no
  LLVM-linked plugin, runs against the existing database.
- **Verified it detects rather than silently passing.** The malloc-in-tick probe returned 0
  matches (a true clean result); a deliberately broad control matcher returned 42 and 245,
  proving the matcher machinery works.
- **The bespoke ordering rule's raw material is reachable.** A matcher for
  `add(Uint8|Uint16|Int16|Pin|Select|Text)` member calls found **245 registrations** through
  `main.cpp`, with source locations — everything the "a conditional control is registered
  below the control it depends on" rule needs. Note controls live in headers, so the matcher
  must run against a TU that includes them, not the `.cpp` files.
- **Class of rule reachable: per-function AST, lexical position.** *Not* whole-program call
  reachability — clang-query has no call graph, so "allocation *reachable from* tick()" stays
  out of reach (that is CodeQL's or the compiler's job, and the compiler already owns it).
- **Cost/coupling:** matchers are plain text in our repo, no plugin to build, no LLVM version
  lock. The one coupling is matcher syntax across major LLVM releases, which is far cheaper
  than the ABI coupling a compiled clang-tidy check carries.

**Verdict so far:** clang-tidy's value is real but *entirely conditional on curating the
check list* — the recommended set is 66% noise plus one systematically broken check.
clang-query is the pleasant surprise: cheap, capable of our per-function bespoke rules, and
with no maintenance tail.

**Not yet done:** the clangd in-editor experience, and turning the ordering probe into an
actual enforced rule.

### CodeQL — workflow written, awaiting a run

`.github/workflows/codeql.yml` added: `build-mode: none`, `security-and-quality`, manual +
weekly. Cannot produce results until pushed (CodeQL runs in CI, not locally, without the
CLI). The question it answers is the untrusted-input one — six packet parsers, ~22 `memcpy`
on LAN data, no security analysis today.
