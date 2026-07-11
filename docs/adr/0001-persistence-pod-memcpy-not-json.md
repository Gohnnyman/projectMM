# 1. Persist POD module state with memcpy, not JSON

Status: Accepted

## Context

Plan-09 attempted ~1700 LOC of JSON-based persistence for module state. It was fully abandoned. The premise "persistence is JSON" was assumed without justification: neither human-readability nor manual editability were real requirements. The JSON design spawned ~15 helpers (`rebuildControls`, `LoadAllFn`, `applyNode`, `serializeNode`, `cleanupTmpLeafCb_`, and more), which was the system signalling the design was too elaborate for the job. It forced a Scheduler reorder (3→5 phases) that bred secondary bugs (a duplicate-children bug, a MAC→deviceName guard, multiple "device shows nothing" failures), and needed five defensive null guards that masked an allocate-new-before-free fragmentation invariant.

## Decision

Persist POD module state with a single `memcpy(file, this + sizeof(MoonModule), classSize - sizeof(MoonModule))`, loading it back before any module's `setup()` / `defineControls()` by memcpy into member memory directly. Plan-10 took this path and shipped.

## Consequences

- Save and restore are one line each; no serializer/deserializer helper sprawl.
- Loading before `setup()` means no Scheduler phase reorder and none of its secondary bugs.
- POD-only: state is a flat memory image, so there is no schema-versioned migration across a struct-layout change (a future need would be its own decision, not a reason to pay for JSON now).
- The lesson that generalised: question a format premise before building to it; suspicious helper proliferation is a design smell; fix an invariant, do not paper it with per-call-site guards.
