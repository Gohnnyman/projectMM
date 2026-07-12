# Plan — 16-lane parallel LED output (LCD_CAM + Parlio, 8-or-16 bus)

## Context

The parallel LED drivers (LCD_CAM on S3/P4, Parlio on P4) cap at **8 lanes**, but the cap is *ours*, not the silicon's — both peripherals do 16 data lines. Two shipping catalog boards are **waiting on this feature**: **SE 16 V1** and **LightCrafter 16** (both ESP32-S3, both `pins: None` in `deviceModels.json` because 16 outputs can't be configured today). So this is demand-driven, not build-ahead.

**Why esp_lcd-16, not a direct-register driver (settled after research):** the "hpwit driver needs no clock pin and does 20 lanes" reputation is from his *classic-ESP32 I2S* driver — a different peripheral. His actual **S3 driver uses the same `esp_lcd` i80 component we do**, with the same WR/DC + 8-or-16 constraints, and pads unused lanes to a single "ghost pin" — exactly the trick this plan adopts. He documents abandoning his own direct-register version because it broke across an IDF major bump; we build on IDF v6 and already fight 5.5→6.x platform drift, so a register-level driver would be the most brittle file in the tree for the gain of *one* reclaimed pin. FastLED's S3 LCD path is also esp_lcd. So: **widen the esp_lcd driver we own** (net-neutral, IDF-clean, matches the whole field). >16 lanes is physically impossible on S3 (hardware cap 16); only reachable on P4 via the *RGB panel* peripheral (24 lanes) — backlogged separately, not this plan.

**The hardware truth this plan is shaped by:** IDF restricts LCD_CAM i80 `bus_width` to **exactly 8 or 16** (line 128-130) and **rejects NC data pins**. So on LCD, a board wires *exactly 8 or exactly 16 real GPIOs*; a sub-16 board parks unused data lanes (+ WR/DC) on dead S3 GPIOs and those lanes idle LOW harmlessly (the `activeMask` rule). Parlio is more flexible (1..16, unused lanes NC). The bus width is **derived from the pin count**: ≤8 pins → 8-bit bus (today's path, unchanged), 9..16 pins → 16-bit bus (the new path).

Design record to save on approval: `docs/history/plans/Plan-YYYYMMDD - 16-lane parallel LED output.md`.

## The crux: 8→16 makes the bus 16-bit

Everything is built around "one bus byte per slot, bit L = data line L", 8 bits wide today. At 16 lanes the slot becomes a **uint16** (16 data lines clock per pixel-clock): the transpose output plane, `activeMask`, the tick-loop `mask`, and the DMA-buffer *element* all double to 16-bit, and the DMA byte-size doubles. The 8-bit path stays for ≤8 lanes; the 16-bit path engages only when >8 pins are configured (a compile-time template instantiation selected by one runtime branch in `tick()`).

## Increments

Ship as **PSRAM-first, then the width** (the memory change lands cleanly under the doubled footprint, re-proven at 8 lanes so it's isolated):

### Increment 0 — DMA buffer to PSRAM (its own commit)
The 16-bit frame doubles the internal-SRAM footprint; the S3 already hits an LCD single-DMA-buffer init ceiling between 8192–12288 lights at 8 lanes (performance.md), which halves at 16-bit. Move both buffers PSRAM-first-with-internal-fallback.
- `src/platform/esp32/platform_esp32_lcd.cpp:140` (`esp_lcd_i80_alloc_draw_buffer`) and `src/platform/esp32/platform_esp32_parlio.cpp:125` (`heap_caps_aligned_alloc`): `MALLOC_CAP_INTERNAL` → `MALLOC_CAP_SPIRAM` first, internal fallback (allocate-and-degrade, matching `platform::alloc`). Both peripherals' GDMA set `access_ext_mem = true` already.
- Alignment: replace the fixed-64 with the ext-mem constraint (`gdma_get_alignment_constraints` → `ext_mem_align`), round `bufferBytes` up to it. Wrong alignment = silent DMA corruption the loopback catches.
- **Re-prove at 8 lanes** on S3 + P4 loopback (isolates the memory change), and confirm a previously-too-big frame now inits.

### Increment 1 — the 8-or-16 widening
Everything else, one coherent change (the uint16 slot, transpose, and frameBytes doubling can't be split without a broken intermediate).

## Design

### The new 16×8 transpose (`src/light/drivers/LcdSlots.h`)
A uint16 plane where bit L = lane L splits at the byte boundary: low byte = lanes 0..7, high byte = lanes 8..15 — two **independent** 8-lane transposes. So reuse the existing, already-bit-perfect-pinned `transposeLanes8x8`:
```cpp
inline void transposeLanes16x8(const uint8_t* in /*16*/, uint16_t* out /*8*/) {
    uint8_t lo[8], hi[8];
    transposeLanes8x8(in,     lo);   // lanes 0..7  → low byte
    transposeLanes8x8(in + 8, hi);   // lanes 8..15 → high byte
    for (int b = 0; b < 8; b++)
        out[b] = uint16_t(lo[b]) | (uint16_t(hi[b]) << 8);
}
```
Textbook, branch-free, zero new magic constants, reuses the pinned SWAR core. (A fused 128-bit SWAR is a later drop-in behind the same signature + test if profiling ever demands it.) **Byte-order caveat:** which byte carries lanes 8..15 is decided by the loopback bit-verify on a high lane; if the peripheral maps the high byte first, swap the `lo`/`hi` shift (one line).

### The encoder, templated on slot type (`LcdSlots.h`)
Template `encodeWs2812LcdSlots` on `Slot` (uint8_t or uint16_t); lane count = `sizeof(Slot)*8`; dispatch the transpose with `if constexpr`. Argument deduction keeps every existing 8-bit call site source-unchanged (`encodeWs2812LcdSlots(wire, mask8, ch, out8)` still resolves to the uint8 instantiation). The per-row hot loop stays branch-free per instantiation.

### The driver (`src/light/drivers/ParallelLedDriver.h`)
- `kMaxLanes = 8` → `16` (the `laneList_`/`laneCounts_`/`laneStart_`/`busPins_`/`wire[]` arrays auto-resize by element count).
- `tick()`: one runtime branch `laneCount_ <= 8 ? encodeRows<uint8_t>() : encodeRows<uint16_t>()`, where `encodeRows<Slot>` is the current row loop with `mask`, the encode call, and the `out` advance parameterized on `Slot` (advance in *elements*, so byte math is automatic). `dmaBuf_` stays `uint8_t*` at the seam; the uint16 path reinterpret-casts.
- `frameBytesFor(maxLights, outCh, slotBytes)`: multiply the per-light bytes AND the latch pad by `slotBytes` (`= laneCount_ > 8 ? 2 : 1`). **The pad must scale too** — it's a count of idle bus *words*, and a 16-bit word is 2 bytes; an unscaled pad halves the latch LOW duration.
- **Drop `kExactLaneCount`** (LcdLedDriver.h:54 + the check at ~283). LCD now requires the pin count to be exactly 8 or exactly 16 (derive `bus_width` from it); the "needs exactly 8 pins" literal becomes an "8 or 16 pins" check. Parlio stays 1..16.
- Grow the text buffers: `pins[24]` → `pins[64]`, `ledsPerPin[48]` → `ledsPerPin[96]` (16 pins overflow the current sizes).
- Loopback `runLoopbackSelfTest`: `perLightBytes`/`dataBytes` scale by `slotBytes`. **Highest-risk thread:** `captureAndVerifyFrame`'s `kBits = dataBytes / 3` (platform_esp32_rmt.cpp:292) is width-blind (it verifies one RX pin's wire signal, same bit-count regardless of bus width) but is fed width-scaled bytes — add a `slotBytes` param so `kBits = dataBytes / (3 * slotBytes)`.

### The platform seam
- `platform_esp32_lcd.cpp:100`: `busCfg.bus_width = laneCount <= 8 ? 8 : 16`; pin loop `:104` `i < laneCount && i < 8` → `< 16`.
- **Ghost-pin consolidation (the bonus win):** point `wr_gpio_num`, `dc_gpio_num`, and every unused `data_gpio_nums[i]` (below `bus_width`) at *one* caller-supplied sacrificial GPIO instead of two separate WR/DC pins — takes LCD from 2 sacrificial pins to 1, no register code (hpwit's exact trick). The driver's `validateBusPins` collision guard already rejects a data lane colliding with the sacrificial pin.
- `platform_esp32_parlio.cpp:49`: `kBusWidth` → derived `data_width = laneCount <= 8 ? 8 : 16`; pin loops widen. The `parlio_tx_unit_transmit(..., bytes*8, ...)` bit-length is **coincidentally width-invariant** (buffer-bits = bytes*8 regardless of data_width) — leave it, add a comment so it isn't "fixed" into a bug. The 65535-byte ceiling (`:149`) is also width-invariant (a total-buffer-bit cap); update only the *prose* (16-bit halves lights/lane: ~448 RGB vs 897 — reaching 16×2048 needs the separate Parlio-chunked-transfer backlog item).
- `platform_config.h:85,97`: `lcdLanes`/`parlioLanes` `8` → `16`, update comments.

## Files
- **`src/light/drivers/LcdSlots.h`** — add `transposeLanes16x8`; template `encodeWs2812LcdSlots` on slot type.
- **`src/light/drivers/ParallelLedDriver.h`** — `kMaxLanes`→16; uint16 encode path in `tick()`; `frameBytesFor`+`slotBytes`(+pad); drop `kExactLaneCount`; grow `pins`/`ledsPerPin`; loopback `dataBytes` scaling.
- **`src/light/drivers/LcdLedDriver.h`** — drop `kExactLaneCount=true`; the "8 or 16 pins" rule; ghost-pin sacrificial-GPIO control.
- **`src/platform/esp32/platform_esp32_lcd.cpp`** — derive `bus_width`; pin loop `<16`; ghost-pin consolidation; (Inc 0) PSRAM buffer.
- **`src/platform/esp32/platform_esp32_parlio.cpp`** — derive `data_width`; pin loops; ceiling prose; (Inc 0) PSRAM buffer.
- **`src/platform/esp32/platform_config.h`** — `lcdLanes`/`parlioLanes`→16.
- **`src/platform/esp32/platform_esp32_rmt.cpp`** — `captureAndVerifyFrame` `kBits` denominator gains `slotBytes` (16-bit loopback).
- **`test/unit/light/unit_LcdLedEncoder.cpp`** — template the exhaustive SWAR-equals-naive test to 16 lanes (≥4096 trials incl. high-lane-only masks); add 16-lane golden encoder cases (lane 0, lane 15, lane 7+8 across the byte boundary, empty mask, RGBW 96×2 size).
- **`test/unit/light/unit_{Lcd,Parlio}LedDriver.cpp`** — invert the "exactly 8" / "rejects >8" cases to "8 or 16" / "rejects >16"; add 9..16-lane frameBytes-doubling + lane-slicing cases.
- **Docs** — `backlog-light.md` (mark the 16-lane item shipping; record the hpwit-direct + P4-RGB-24 alternatives as *not chosen* with the IDF-fragility reason); `performance.md` (16-lane multi-pin results); `deviceModels.json` (SE16 + LightCrafter16 gain their 16 LED pins).

## Verification
1. **Host:** `cmake --build build` 0-warn; the exhaustive 16-lane transpose test (SWAR == naive over all patterns × masks, incl. high-lane masks) + golden encoder cases + driver frameBytes-doubling cases; `ctest` + scenarios green.
2. **Increment 0 (PSRAM) on hardware:** S3 + P4 loopback PASS at 8 lanes with the buffer in PSRAM; a frame that previously failed internal-SRAM init now inits.
3. **Increment 1 on hardware:** S3 LCD (16 real pins from the clean-16 set `4..18,21`; WR/DC/pad on a ghost GPIO) — loopback on lane 0, then **re-jumper to a high lane (e.g. lane 12)** via `loopbackTxPin` to prove the high byte / `transposeLanes16x8` high half; then a real 16-strip rig. Repeat on P4 Parlio (8 base pins + 6 more from the P4 clear set). Confirm frame timing matches 16-bit expectations.
4. **KPI:** re-measure the P4 tick at 16 lanes (the transpose ~doubles; confirm it stays within budget with the SWAR two-pass).

## Scope guard
- **Not** a direct-register LCD_CAM driver (rejected: one-pin gain for the most IDF-brittle file in the tree; the author himself abandoned it). **Not** >16 lanes (S3 hardware-capped at 16; P4-RGB-panel 24-lane is a separate future backlog item).
- Parlio-chunked-transfer (to actually drive 16×2048 past the 65535-byte ceiling) is the **separate** existing backlog item, not this plan.
- The shared lane-driver scaffolding extraction (the ~245 duplicated lines across LCD/Parlio) is triggered by "the 3rd parallel backend" — that's the parallel-I2S driver, not this widening; keep it separate.
