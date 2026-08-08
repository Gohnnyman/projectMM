# hpwit/new-parser (ESPLiveScript2) — monthly activity digest

What landed on [hpwit/new-parser](https://github.com/hpwit/new-parser), month by month. External-context reference — a factual log of a friend repo's activity, not projectMM's own history or roadmap. Newest month on top. The reusable prompt that generates these lives in [README.md](README.md).

The library: **ESPLiveScript2**, Yves Bazin's (hpwit) from-scratch C++ rewrite of [ESPLiveScript](https://github.com/hpwit/ESPLiveScript) — the same idea (a small C-like language compiled on-device to real Xtensa machine code, no interpreter, so a script runs at near-native speed) reimplemented independently rather than refactored. The library ships inside the repo as `asmparser2/` (PlatformIO name `ESPLiveScript2`, at v1.3.0). Summarised via the GitHub commits API.

**Repo note:** the repository name is `new-parser`, but the library and its README call it **ESPLiveScript2** — the name to search for. Sibling digest for v1: [hpwit-ESPLiveScript.md](hpwit-ESPLiveScript.md).

## Timeline note (added 2026-08-06)

Added to the digest set on 2026-08-06, after the product owner flagged the rewrite. History to date, from the commit log: created March 2025, six commits across March–May 2025, then **dormant for over a year**, then **12 commits in the first days of August 2026** — the rewrite as it now stands is days old at the time of writing. July 2026 is therefore empty, and the August work is summarised in next month's digest rather than pre-empted here.

What the rewrite is, from its README (context for future months, not an endorsement):

- A **verifiable** compiler is the stated reason for rewriting rather than refactoring: the whole toolchain (tokenizer, parser, assembler, loader) builds and runs as an ordinary host program with no ESP32 or Arduino framework involved.
- Its tests run the **actual compiled bytes** on a real Xtensa CPU emulator (QEMU, Espressif's ESP32/ESP32-S3 machine models) and check real results, and every example script from v1's own corpus is compiled and checked against that pipeline.
- Day-to-day script authoring is said to be unchanged from v1; the differences are in the implementation and its testability.

## July 2026

No activity: no commits on `main` in July 2026, and no issues. (The repo was dormant between May 2025 and August 2026 — the current rewrite work begins 2026-08-01, outside this window.)

_Checked: commits author-dated 2026-07-01..2026-07-31 on `main` (0); issues created 2026-07-01..2026-07-31 (0) and closed in the same window (0); no versioned release published in July 2026._
