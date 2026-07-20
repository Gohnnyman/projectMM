# Plan — Migrate MoonLight fixture presets as seeded built-ins

## Context

MoonLight's `DriverNode.cpp` (@ `6586921770`) defines 17 `lightPreset_*` fixture channel-maps via named offsets (`offsetRed`, `offsetPan`, `offsetRGBW`, `offsetBrightness`, …). projectMM's `LightPresetsModule` seeds only 5 (RGB/GRB/BGR/RGBW/GRBW) as read-only `locked` rows. The product owner selected which to migrate under three tiers, with these resolved decisions:

- **Tier A (colour orders): WRGB only.** MoonLight's own comments name real hardware only for WRGB ("rgbw ws2814 LEDs"). RBG/GBR/BRG are bare permutations with no named fixture → **not seeded** (a user adds a custom preset if they ever hit one).
- **Tier B (multi-channel LED/par): migrate GRB6, RGBWYP, RGBCCT, IRGB.**
- **Tier C (moving heads): migrate BeeEyes-15, BeTopper-32, 19x15W-24, tagging only channels whose role we support**; everything else `None`.
- **Intensity/Brightness master channel → `Dimmer`** (existing role; inert until moving-head effect writers land — a correct map that nothing animates yet).
- **Extend the colour vocabulary with `WarmWhite`, `Yellow`, `UV`** so RGBCCT (cold+warm white) and RGBWYP (adds Y+UV) migrate fully rather than half-dark. The existing `White` is kept as-is (a normal/cold white) — NOT renamed to ColdWhite — so existing RGBW/GRBW presets' persisted role bytes and labels are unchanged; the second white is the new `WarmWhite`. Industry naming (CW/WW), our PascalCase (`White`/`WarmWhite`), option strings `"W"`/`"WW"`.

Design record for the LightPresets library itself: [Plan-20260711 - LightPresets reusable named-preset library (shipped).md](Plan-20260711%20-%20LightPresets%20reusable%20named-preset%20library%20(shipped).md).

## Verified current state (file:line)

- **`ChannelRole` enum** (`src/light/ChannelRole.h:18-28`) — `None, Red, Green, Blue, White, Pan, Tilt, Zoom, Rotate, Gobo, Dimmer`, with `kChannelRoleOptions[]` index-aligned (`"—","R","G","B","W","Pan","Tilt","Zoom","Rotate","Gobo","Dimmer"`). A role byte IS a `ChannelRole` value (the pool stores these indices; `deriveCorrection` reinterpret_casts). Adding roles = append to BOTH the enum and the option array, same order.
- **`seedBuiltins()`** (`src/light/drivers/LightPresetsModule.h:370-389`) — loops a `LightPreset[]` (the OLD 5-value enum), names each from `kLightPresetOptions`, fills roles via `fillRolesFromPreset(lp, roles, 4, nCh)` with a fixed `ChannelRole roles[4]` — **caps at 4 channels**, so it can't express a 15/24/32-ch fixture.
- **`LightPreset` enum + `fillRolesFromPreset`** (`src/light/drivers/Correction.h:24, 52-61`) — the pre-library driver preset (`RGB,GRB,BGR,RGBW,GRBW,Custom`). Its ONLY consumers are `seedBuiltins` and `DriverBase`'s legacy seed. The new library supersedes it.
- **Storage is uncapped** (`rolePool_` ScratchBuffer, `Preset{poolOffset, channelCount}` slice) — a preset can already be any width. Only the *seed helper's* `roles[4]` is the limiter.
- **`presetHasWhite`** (`:79-86`) scans roles for `ChannelRole::White` to show/hide a driver's whiteMode. `White2` must count too (a CCT fixture has white to synthesise).

## Design

### 1. Extend the colour vocabulary — `src/light/ChannelRole.h`

Append three colour roles after `White`, before the fixture roles, so plain strips still use the low values and existing persisted role bytes are unchanged (append-only — never renumber):

```cpp
enum class ChannelRole : uint8_t {
    None,
    Red, Green, Blue, White, WarmWhite, Yellow, UV,   // colour roles (White = normal/cold)
    Pan, Tilt, Zoom, Rotate, Gobo, Dimmer,             // fixture roles
};
```

⚠️ **Append-only is load-bearing.** Persisted presets store role bytes as these indices. Inserting WarmWhite/Yellow/UV *between* the existing colour roles and the fixture roles shifts Pan…Dimmer up by 3 — which silently corrupts any persisted moving-head custom a user already made. Verified safe here because the fixture roles have **no seeded built-ins yet and no effect writers**, so no persisted data references them. `White` keeps index 4, so every existing RGBW/GRBW preset's persisted bytes are untouched. (If a fixture role were already persisted, the new roles would have to go at the END.) Note this reasoning at the enum.

`kChannelRoleOptions[]` gets the matching strings in the same slots: `"WW","Y","UV"` after `"W"`. Update `presetHasWhite` to also match `WarmWhite`.

### 2. Seed the built-ins directly as role arrays — `src/light/drivers/LightPresetsModule.h`

Replace the `LightPreset`-enum-driven `seedBuiltins()` with a table of `{name, roles[]}` literals so a preset of ANY width seeds directly (retires the `roles[4]` cap and the `fillRolesFromPreset` dependency — a subtraction). Shape:

```cpp
struct BuiltinPreset { const char* name; const ChannelRole* roles; uint8_t channelCount; };
// each roles array a file-scope constexpr, e.g.:
static constexpr ChannelRole kRGB[]  = {R::Red, R::Green, R::Blue};
static constexpr ChannelRole kWRGB[] = {R::White, R::Red, R::Green, R::Blue};
// … one per seeded preset …
```

`seedBuiltins()` loops the table: for each, claim a slot, set `locked=true`, name it, size the pool slice to `channelCount`, copy the roles. Same per-row mechanics as today, just data-driven and width-agnostic.

### 3. The seeded set (name → dense roles[], `—` = None)

Migrating MoonLight's offset maps to dense role arrays. A moving-head offset map (`offsetPan=0, offsetTilt=1, …`) becomes `roles[channel]=role`; every channel MoonLight doesn't name, or whose role we don't support, is `None`.

**Already seeded (unchanged):** RGB, GRB, BGR, RGBW, GRBW.

**Tier A — add:**
- `WRGB` (4): `W,R,G,B`

**Tier B — add:**
- `GRB6` (6): `G,R,B,—,—,—` (curtain; 3 spacer channels)
- `RGBWYP` (6): `R,G,B,W,Y,UV` (lightbar; Y+UV now real roles)
- `RGBCCT` (5): `R,G,B,W,WW` (cold white = W, warm = WW)
- `IRGB` (4): `Dimmer,R,G,B` (CH1 master intensity → Dimmer)

**Tier C — add (supported channels tagged, rest None):**
MoonLight offsets → dense array. Overlapping offsets (MoonLight sets `offsetRed=0` AND `offsetPan=0` on the same channel) resolve to the FIXTURE role (Pan/Tilt), since `offsetRGBW=N` says the drivable RGB block starts at channel N — the low-channel R/G/B offsets are MoonLight's internal aliases, not separate channels. So:
- `MH BeeEyes 150W-15` (15): ch0=Pan, ch1=Tilt, ch3=Dimmer(Brightness2), ch5=Gobo, ch7=Zoom, ch8=Dimmer(Brightness), ch10=R, ch11=G, ch12=B, rest `—`.
- `MH BeTopper 19x15W-32` (32): ch0=Pan, ch2=Tilt, ch5=Zoom, ch6=Dimmer, ch9=R, ch10=G, ch11=B, rest `—` (RGBW1/2/3 sub-cells at 13/17/24 stay `None` — no multi-cell role).
- `MH 19x15W-24` (24): ch0=Pan, ch1=Tilt, ch3=Dimmer, ch4=R, ch5=G, ch6=B, ch7=W, ch17=Zoom, rest `—` (RGBW1/2 at 8/12 stay `None`).

That's **5 existing + 8 new = 13 seeded built-ins** (WRGB, GRB6, RGBWYP, RGBCCT, IRGB, BeeEyes-15, BeTopper-32, 19x15W-24). Under `kMaxPresets = 32`, leaving 19 custom slots.

### 4. What is explicitly NOT migrated (documented, per no-silent-caps)

- **RBG, GBR, BRG** — no real fixtures; a comment at the seed table names them as intentionally skipped.
- **RGBW-cell sub-lights** (BeTopper's 4 cells, 19x15W's 2 cells) — no "multiple RGB cells in one fixture" role; those channels are `None` until the fixture model adds the concept. Named in the moving-head backlog item.
- **RGB2040 curtain** — its preset part is plain RGB, but it needs a dual-channel-group *layout* remap (MoonLight does this in VirtualLayer, not the preset). Not a preset migration; skip.
- **Effect-side writers** (`setPan/setZoom/setGobo/setDimmer`) — still absent. Every fixture role a Tier-C preset tags is an inert map until the moving-head effect increment. This migration is the channel *maps* only, by design.

## Files

- **`src/light/ChannelRole.h`** — +3 colour roles (White2, Yellow, UV) + option strings; append-only note.
- **`src/light/drivers/LightPresetsModule.h`** — data-driven `seedBuiltins()` (role-array table, width-agnostic); `presetHasWhite` also matches White2; skip-comment for the un-migrated orders.
- **`src/light/drivers/Correction.h`** — no change needed for seeding (the library owns its own seeds now). *If* `fillRolesFromPreset`/`LightPreset` end up with zero remaining callers after step 2, remove them (subtraction) — verify with grep first; out of scope if `DriverBase` still uses them.
- **`test/unit/light/unit_LightPresetsModule.cpp`** — assert the new built-in count + names, WRGB/RGBCCT/IRGB role arrays, White2 counts as white in `presetHasWhite`, a moving-head preset's width + tagged channels (Pan@0 etc.) + that unsupported channels are `None`, and that built-ins stay `locked`/unmovable.
- **`test/unit/light/unit_Correction.cpp`** — if roles were appended, assert an existing persisted role byte still resolves to the same colour (append-didn't-shift regression).
- **Docs:** `docs/moonmodules/light/supporting.md` — refresh the LightPresets card's built-in list (it names "RGB, GRB, BGR, RGBW, GRBW"). The `///` on the module + the generated technical page follow.

## Verification

1. `cmake --build build` clean (0-warn); `ctest` + scenarios green.
2. New tests pin: 13 built-ins seeded, names + role arrays correct, White2 in `presetHasWhite`, moving-head widths + tagged/None channels, locked+unmovable.
3. **Append-only regression:** an RGBW preset's persisted `[R,G,B,W]` bytes still resolve to R/G/B/W after the enum grew (proves no renumber).
4. Flash P4, confirm the new presets appear as locked rows, selectable on a driver; a 15-ch moving-head preset resolves without crashing (Robust-to-any-input at odd widths) — output is a valid RGB(W) fixture even though Pan/Tilt/Zoom are inert.
5. `check_specs.py` passes (built-in names in `///`/doc agree).

## Scope guard

Channel-map migration + colour-vocabulary extension ONLY. Do NOT add effect-side fixture writers (`setPan`…), the multi-RGBW-cell concept, or the RGB2040 layout remap — those are the deferred moving-head *effect/fixture-model* increment this seeds toward. Seeding a moving-head preset gives a correct DMX map that current effects drive only on its R/G/B/W channels; Pan/Tilt/Zoom/Gobo/Dimmer stay inert until writers land. That inertness is expected, not a bug.
