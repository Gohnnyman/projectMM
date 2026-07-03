# Plan — Docs Phase 4a: source snippets now, Doxide next commit

Approved-pending. Final phase of the docs overhaul ([docs/backlog/docs-system-overhaul.md](../../backlog/docs-system-overhaul.md)), on `next-iteration`, after Phase 3.

## The split (PO decision)

The goal is *the `.h`/`.cpp` files are the basis for documentation*. Two ways to get there:

- **Doxide** (the backlog's Phase 4) renders full `.h` API docs into the site — but it's a **from-source C++ binary** (can't use `uv`; needs a CMake CI build), and our 139 `.h` files use **plain `//` prose**, not the Doxygen `///`/`@param` style Doxide reads richly. Heavy on two fronts.
- **`pymdownx.snippets`** (already enabled, uv-native) embeds **real source excerpts** into a doc — no new tool, no comment conversion.

**PO wants Doxide eventually** (it's the fuller realisation of "source as the doc basis") but agrees to try snippets first. So:

- **This commit — Phase 4a (snippets):** targeted source-embeds where the source genuinely *is* the spec.
- **Next commit — Phase 4b (Doxide):** given its own isolated commit to evaluate the from-source CI build + comment-style question on real output, abort-clean if it doesn't pay.

**Why this order helps 4b** (the "does 2 make 3 easier?" question): snippets don't share CI/build plumbing with Doxide, but they *de-risk the decision* — we see how our plain-`//` comments read when surfaced, learn which `.h` regions are doc-worthy (the same regions Doxide would document), and decide the comment-conversion scope with eyes open instead of mid-build. Concrete-first.

## Phase 4a deliverables (snippets)

Pick **targets where the source is the authoritative spec and the doc currently hand-copies it** — so embedding removes a real drift risk (the same class Phase 3 guards, solved by making source *be* the doc). Not blanket; a handful of high-value cases:

1. **Improv frame constants** — `ImprovProvisioningModule.md` hand-documents `0x01`/`0x03` frame types + the magic; embed the real `ImprovFrameType` enum + `kImprovMagic`/`kImprovMaxPayload` from `src/core/ImprovFrame.h` (mark the region `// --8<-- [start:frame-constants]` / `[end:...]`).
2. **Preview wire format** — `PreviewDriver.md` hand-writes `[0x03][count:u32]…` layouts; embed the real header-packing constants / the frame-type bytes from `src/light/drivers/PreviewDriver.h` where they're defined in code.
3. **(If clean) one more** — a control-enum or a small struct another spec hand-copies. Only where it reads well; stop at 2–3.

Mechanism: add `// --8<-- [start:name]` / `[end:name]` markers around the source region (a comment the compiler ignores), and `--8<-- "src/…/File.h:name"` in the `.md`. `base_path` is already `$config_dir` (repo root), so `src/…` resolves.

For each embed, **note what we learned** for 4b: did the plain `//` comment around it read well as doc, or would it need a richer comment? (This is the 4b input.)

## Phase 4b (Doxide) — spec for the NEXT commit (not built now)

Recorded so it's ready to evaluate:
- **Install:** Doxide is `git clone --recurse-submodules` + CMake build (C++ toolchain + LibYAML). CI options: build-and-cache the binary in the deploy-pages job, or vendor a prebuilt Linux binary. NOT `uv` — the one place the uv-everywhere rule bends (like the ESP-IDF Python exception; justify at the introduction site).
- **Comments:** decide from 4a's findings how much of the 139 `.h` to give Doxide-style comments. Likely **start with `core/`** (the stable base), a handful of files, evaluate the rendered output, then decide whether to sweep the light domain. Do NOT convert all 139 up front.
- **Integration:** Doxide emits Markdown → into a `Developer / Source` nav section of the existing Material site. The out-of-docs `../src/*.h` links (currently rewritten to GitHub by mkdocs_hooks.py) get repointed at the in-site Doxide pages where they exist.
- **Abort criteria:** if the CI build is too fragile or the rendered output doesn't beat the GitHub source links, drop it — 4a already delivers a lighter version of the goal.

## Verification (4a)
- The embedded snippets render as fenced code in the doc; the values match the source (because they *are* the source). Editing the `.h` constant changes the rendered doc on next build — the single-source proof.
- `check_specs.py` + docs build stay green; no rendered-doc regressions elsewhere.
- The hand-copied `[0x03]…` prose that the snippet now covers is trimmed to a one-line pointer (the code block is the authority) — net subtraction where it applies.

## Files (4a)
- **Edit:** the 2–3 source `.h` (add `--8<--` markers, comment-only), the 2–3 `.md` (replace the hand-copied block with the snippet include + trim surrounding prose), `docs/backlog/docs-system-overhaul.md` (mark Phase 4 split).
- **New:** this plan.

## What 4a shipped + the 4b learnings (recorded 2026-07)

Two targeted embeds landed:
1. **ImprovFrame.h `:frame-constants`** → ImprovProvisioningModule.md. Real `ImprovFrameType` enum + magic/version/payload constants. The source *is* the authority; renders as clean, syntax-highlighted C++. **This is the ideal case** — actual code constants an integrator needs verbatim.
2. **PreviewDriver.h `:wire-format`** → PreviewDriver.md. The `[0x03][count:u32]…` layout — but it lives in a `//` **comment block**, not code. It renders with the `//` prefixes intact (correct — it *is* source), acceptable in a `cpp` fence but visibly "a source comment," and the doc's richer behavioural prose stays around it.

**The 4b (Doxide) input:** our plain `//` comments surface as literal commented source. For real code (enums/constants/structs) that's great. For prose-in-comments it's serviceable but shows why Doxide's structured comments would render cleaner — so **4b's comment-conversion effort is real but bounded**: the highest-value Doxide output is on the *code* (signatures, types, constants), which needs little/no comment change; the prose polish is optional. Start Doxide on `core/` where the types are the payload, evaluate, decide on the light domain.
