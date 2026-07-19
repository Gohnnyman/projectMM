# Plan — Flexible light profile: named channel-role offsets (RGB now, moving heads over ArtNet next)

## Context

Today `Correction` (`src/light/drivers/Correction.h`) models a light as a fixed **RGB(W) channel order**: `order[4]` is a permutation, `LightPreset` an 8-entry enum, each preset a hand-written `switch` case. PR #45 proposes extending that enum to **24** (all white-position permutations). The product owner correctly flagged this as combinatorial explosion: it enumerates orders nobody ships, and it's the wrong axis of flexibility.

The real generalisation — proven in MoonLight (`src/MoonLight/Layers/LightsHeader.h`, studied for **ideas only**, not copied) — is: a light is a **variable-width channel array** with **named role-offsets** (`red/green/blue/white/pan/tilt/…`) into it, where a sentinel marks an absent role. A preset is then just a *default set of offsets*, not a distinct type. This subsumes `order[]` entirely (today's `order[]` **is** the rgb role-offsets), ends the enum explosion, and opens the door to non-RGB fixtures (moving heads) with no special-casing.

Ideas carried (written fresh against our architecture, per *Industry standards, our own code* / *No WLED-MM derivation*):
1. A light has a **variable `channelsPerLight`** (3 for a strip, up to 32 for a moving head).
2. **Named role-offsets** index into that width — the effect says *what* (`setRGB`), the light knows *where*.
3. **A sentinel (`UINT8_MAX`) = role absent** → the writer for that role is a branch-simple no-op, keeping the hot path clean.
4. **Preset = a default offset-set** — curated shipped orders + a `Custom` editor, not a permutation enum.

NOT carried: MoonLight's `LightsHeader` wire-struct, `Coord3D`, Svelte-monitor byte layout, semaphore/compositing model — all their architecture. We write our own `Correction` generalisation against our flat-data hot path and our `Control` system.

## Verified current state (file:line)

- **`Correction`** (`Correction.h`): `briLut[256]`, `order[4]`, `outChannels` (3|4), `deriveWhite`. `rebuild(brightness, preset)` decodes an 8-case `switch` into `order[]/outChannels/deriveWhite`. `apply(src, out)` reads 3-channel RGB src → writes `outChannels` bytes via `order[]`. **Already channel-count-generic in shape** — nothing hardcodes 3 except the src read.
- **`LightPreset` enum** (`Correction.h:11`): 8 entries, index-aligned with `kLightPresetOptions` string array; the `Select` value casts straight to the enum. PR #45 wants this → 24.
- **`Drivers`** (`Drivers.h:122,153,168,181`): owns the single `lightPreset` Select (`default 2 = GRB`) at container level + `brightness`; `onControlChanged` on `brightness`/`lightPreset` calls `correction_.rebuild()`; hands each child a `const Correction*`. **This is the global-vs-per-driver seam** the PO wants moved down.
- **`NetworkSendDriver`** (`NetworkSendDriver.h`): **already speaks ArtNet** (`protocol 0`, `buildArtDmxPacket`, port 6454 — ArtNet IS DMX-over-Ethernet). The whole send path is **already channel-generic**: `correction_->outChannels`, `sourceBuffer_->channelsPerLight()`, `srcCh`/`outCh` strides. `apply()` per light, windowed. `onCorrectionChanged()` resizes `corrected_`. → moving-head output is a **small reach**: transport + variable-stride machinery already exist.
- **`DriverBase`** (`DriverBase.h:54`): `onCorrectionChanged()` no-op hook already exists for RGB↔RGBW stride change; reused for wider fixtures.
- **`Buffer`**: `channelsPerLight()` variable already — the buffer layer does not assume 3.
- **No DMX/fixture/pan/tilt anywhere today** (grep clean). Moving heads are genuinely new domain.

## Design

### The model (top-down, projectMM)

`Correction` grows from "RGB order" to a **channel-role profile**: a variable `channelsPerLight` plus named role-offsets into a light's byte span. `apply()` writes only *populated* roles (offset ≠ `kAbsent`). Today only the RGB(W) roles are populated → **byte-identical output to now**.

Role-offset representation — **fixed struct of named offsets** (the PO's picked Option 1; the flat-data, hot-path-friendly choice per *Data over objects*):
```
static constexpr uint8_t kAbsent = 255;   // role not present on this fixture
struct Correction {
    uint8_t  briLut[256];
    uint8_t  channelsPerLight = 3;         // fixture width (3 strip … up to 32 moving head)
    // RGB(W) roles — the SOURCE→OUTPUT offset for each colour role. kAbsent = not emitted.
    uint8_t  offRed = 1, offGreen = 0, offBlue = 2;   // GRB default (matches today's default 2)
    uint8_t  offWhite = kAbsent;
    bool     deriveWhite = false;
    // Fixture roles (populated by fixture profiles; kAbsent on plain strips) — increment 4:
    uint8_t  offPan = kAbsent, offTilt = kAbsent /* … as writers land */;
    // apply(): for each populated role, write briLut(src[role]) at its offset.
};
```
`outChannels` is replaced by `channelsPerLight` (the fixture width; the send loop already reads a per-light stride). Curated presets seed the offset fields; `Custom` sets them from UI.

### Increment 1 — role-offset foundation (identical output; the structure IS the deliverable)

- Replace `order[4]/outChannels` with `channelsPerLight` + named role-offsets + `kAbsent`.
- `rebuild()` keeps seeding RGB(W) offsets from the curated preset table (below); the `switch` becomes a small `{offR,offG,offB,offW}` lookup, not per-case field assignment.
- `apply()` writes populated roles only. Prove byte-identical: existing `Correction`/`NetworkSend` unit tests pass **unchanged**.
- Update `NetworkSendDriver` / `DriverBase` / `Drivers` to read `channelsPerLight` where they read `outChannels` (mechanical; the stride math is already there).

### Increment 2 — curated presets + `Custom` (ends the 24-explosion; the PR #45 counter-offer)

- Curate `kLightPresetOptions` to **shipped** orders (`RGB, GRB, BGR, RGBW, GRBW`, + any the PO names), each a `{offR,offG,offB,offW}` literal in a small table. One-line justification comment: "curated to shipped wire formats, not all 24 permutations (WLED-style curated color-order list)."
- Add a `Custom` option that reveals **per-position source pickers** (the PO's picked UI): for each output position, a Select of source role `{R,G,B,W}`; picking `Custom` writes the offsets directly. Curated presets = pre-fill shortcuts over the same fields.
- The `switch`/enum collapses to: preset index → offset literal, or `Custom` → user offsets.

### Increment 3 — per-driver profile + per-driver brightness

- Move the `lightPreset` (+ Custom offset controls) from the `Drivers` container **down to each driver** (the PO's earlier ask). Each LED/network driver owns its profile; `Drivers` keeps the global brightness + palette.
- Add a **per-driver brightness correction**: `effective = global × local` baked into `briLut` at `rebuild()` (cold path). Hot path unchanged — still one LUT lookup per channel. `briLut[v] = (v × global × local)/(255×255)`.

### Increment 4 (Option 2, on top) — a moving-head fixture over ArtNet

- Populate fixture roles (`offPan/offTilt/…`) + add **role writers** (`setPan`/`setTilt`-style) that write at their offset, no-op when `kAbsent`.
- A minimal **fixture-profile control**: `channelsPerLight` (fixture width) + the role offsets, so a user declares "24-channel head, pan@1, tilt@3, rgb@6."
- `NetworkSendDriver` emits the wide layout over **ArtNet** (already the transport). One end-to-end non-RGB fixture so the PO can run a moving head over ArtNet and exercise the offset system.
- **Not gated to network**: the wide-channel model stays in the light layer; a Parlio driver clocking a 24-ch fixture stays expressible (PO's explicit constraint — no `if (networkOnly)`).
- Test target: a scenario driving a moving-head profile → assert the ArtNet DMX frame places pan/tilt/rgb at the declared offsets.

## Files

- **Modify:** `src/light/drivers/Correction.h` (the model — inc 1,2,4), `src/light/drivers/Drivers.h` (per-driver move, global×local brightness — inc 3), `src/light/drivers/NetworkSendDriver.h` (channelsPerLight rename inc 1; moving-head emit inc 4), `src/light/drivers/DriverBase.h` (per-driver preset/offset controls — inc 3), the other `*LedDriver` headers (channelsPerLight rename — inc 1, mechanical).
- **Tests:** extend `unit_Correction*` (role-offset apply, curated presets, Custom offsets, kAbsent no-op); extend `unit_NetworkSendDriver*` (channelsPerLight); new scenario for a moving-head ArtNet frame (inc 4).
- **Docs:** `docs/moonmodules/light/drivers.md` (curated preset list + Custom + per-driver brightness + fixture profile); a one-line `Correction` technical-page note. `check_specs.py` validates control-name agreement.
- **Backlog:** collapse the PR #45-driven "24 preset" backlog note into "curated presets + Custom offsets (shipped in this plan)"; keep a forward item only for role writers beyond pan/tilt (zoom/gobo/rgb2) if not built in inc 4.

## Verification

- `cmake --build build` clean (0-warn); `ctest` + scenarios green. Inc 1: existing `Correction`/`NetworkSend` tests pass **unchanged** (byte-identical proof).
- Hot path: `apply()` stays allocation-free, integer-only, one LUT lookup/channel; per-driver brightness folds into the cold-path LUT rebuild (no hot-path cost — the PO's "hotpath superfast" requirement).
- KPI: no ESP32 flash growth beyond the new controls; tick/FPS flat.
- Inc 4: ArtNet frame for a declared moving-head profile places roles at the right DMX offsets (scenario).

## Scope guard & sequencing

- **Ships as 4 increments, separate commits** (the PO decides commit timing). Inc 1 is behaviour-preserving groundwork; 2 is the visible UX + PR #45 counter; 3 is the per-driver + brightness ask; 4 is the moving-head proof.
- Do **not** build role writers beyond what inc 4 needs (no speculative zoom/gobo/rgb2 unless the moving-head target uses them) — *Concrete first*.
- Do **not** gate the wide-channel model to network output (PO constraint).
- The curated preset list is **subtraction** vs PR #45's +16 entries — fewer options, more capability via Custom.

## PR #45 impact

This plan is the counter-offer to PR #45's 24-preset extension: decline the enum growth, offer curated-presets + Custom-offsets instead (smaller, and gives any wiring not just 24). The drafted PR #45 reply (`docs/backlog/pr45-reply-draft.md`) gets a short paragraph pointing at this direction.
