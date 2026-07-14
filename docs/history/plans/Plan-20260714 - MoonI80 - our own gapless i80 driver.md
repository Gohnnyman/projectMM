# Plan — MoonI80: our own gapless i80 driver, beside IDF's

## Context

The i80 LED driver is capped by memory, and today's investigation found *why* the obvious fix doesn't work.

**Where we are.** `I80LedDriver` pre-encodes a whole frame and hands it to IDF's `esp_lcd` as **one** `tx_color` transaction. That is gapless and correct, but the frame must be streamed by the DMA in one unbroken read — which runs into memory ceilings: the shift-register expander is capped at **~96 lights/strand** on the S3 (above that the frame lands in PSRAM and the strands garble), Parlio caps at **~4,096 lights** (contiguous-block + 65,535-byte hardware limits), and the classic ESP32 caps at **2,048 lights** (its DMA cannot reach PSRAM at all).

**The obvious fix — chunk the frame into several `tx_color` calls — is DEAD, and we proved it.** Built it, measured it on the SE16: sparse effects fine, dense effects flicker full-brightness white. The cause is in IDF's source, not ours (`esp_lcd_panel_io_i80.c:784-794`, `lcd_start_transaction`):

```c
lcd_ll_reset(bus->hal.dev);        // resets the LCD peripheral
lcd_ll_fifo_reset(bus->hal.dev);   // flushes the FIFO
gdma_start(...);
esp_rom_delay_us(4);               // hard-coded 4 µs busy-wait
lcd_ll_start(bus->hal.dev);
```

**`esp_lcd` resets the peripheral between every transaction.** LCD panels don't care; WS2812 does — a mid-frame reset garbles the stream. Parlio's driver does the same (`parlio_tx.c:505`). So *no* chunking strategy inside IDF's LED-adjacent drivers can be gapless, whatever the chunk size or boundary. That is why hpwit hand-rolls his DMA.

**The opening.** The LCD peripheral has **no data-length register** — `lcd_ll_set_phase_cycles()` only sets `lcd_dout` as a *boolean enable* (`esp_hal_lcd/esp32s3/include/hal/lcd_ll.h:411-416`), and IDF's own comment says "*Number of data phase cycles are controlled by DMA buffer length*". So the peripheral clocks out exactly as much as the DMA feeds it and stops when the chain ends. **One `gdma_start()` over an arbitrarily long descriptor chain + one `lcd_ll_start()` = one continuous gapless stream, spanning as many buffers as we like.** `esp_lcd` throws that away; we don't have to.

And the descriptors are nearly free: a 144 KB frame (16 lanes × 1024 lights) needs **37 descriptors = 444 bytes**.

**What we are building.** `MoonI80` — a second i80 implementation behind the *same* platform interface, using IDF's **HAL + GDMA link-list APIs** (`lcd_ll_*`, `gdma_link_*`) one level below `esp_lcd`. Not raw register pokes. IDF's own drivers are built on exactly these APIs.

**Both drivers ship side by side.** IDF's is the *reference*: guaranteed-correct, memory-capped, and the thing we A/B against. MoonI80 is the *challenger*. We retire the reference only when the challenger beats it on the same bench — and having both permanently is the instrument we lacked all of today.

## Phasing (the PO's sequencing: phase 1, then phase 2 only if needed)

### Phase 1 — whole-frame descriptor chain, no ring, no CPU in the loop

Own the descriptor list; point it at the existing pre-encoded frame buffer (wherever it lives, PSRAM included); fire once.

- **No ISR refill, no ring, no real-time deadline, no WiFi-underrun risk.** Strictly simpler than hpwit's design — he needs a CPU refill because he transposes per-LED; we already pre-encode the whole frame, so the DMA can just read it.
- This **directly tests the open question**: is the S3's PSRAM shift-mode failure caused by something `esp_lcd` does (its per-transaction descriptor pool + mount), or by the silicon (GDMA/PSRAM bandwidth)? If MoonI80's PSRAM frame works → `esp_lcd` was the problem and we are done. If it still fails → **the silicon is the limit**, proven, and phase 2 is justified rather than assumed.
- **Honest risk:** if it *is* a bandwidth limit, phase 1 changes nothing on the S3 (same DMA, same memory, same rate). Phase 1 is cheap enough that finding this out definitively is worth it either way — and it still removes the *transaction-size* caps (Parlio's 65,535 B) regardless.

### Phase 2 — internal-RAM ring + CPU refill (only if phase 1 proves the silicon is the wall)

The same descriptor machinery, but the chain points at a small ring of **internal-RAM** buffers that the CPU refills from the PSRAM frame (a bulk sequential read, which PSRAM is good at). The DMA never touches PSRAM.

- This is hpwit's shape, and the **only** thing that can ever work on the classic ESP32 (whose DMA cannot reach PSRAM at all).
- **Cost, stated plainly:** the CPU is back in the timing loop with a hard refill deadline — the thing WiFi can disturb, and what hpwit defends with `_DMA_EXTENSTION` padding and a level-3 IRAM ISR. We traded that away when we chose whole-frame DMA; phase 2 trades it back. That is exactly why we keep both drivers.
- `GDMA_FINAL_LINK_TO_HEAD` makes the chain circular, so the ring is a superset of phase 1's machinery, not a rewrite.

## The seam — why this is cheap

The platform layer is *already* the interface. `I80LedDriver` talks to **8 functions** and knows nothing about `esp_lcd`:

```text
i80Ws2812Init / Buffer / BufferCapacity / Transmit / Wait / LastTransmitUs / Deinit / Loopback
```

A second implementation is a second `.cpp` behind the same 8 functions. `ParlioLedDriver.h` is **99 lines** — the existence proof that a sibling driver is nearly free.

## Implementation

### 1. Platform: the MoonI80 backend

**New file `src/platform/esp32/platform_esp32_moon_i80.cpp`** + one explicit `SRCS` line in `esp32/main/CMakeLists.txt` (the list is explicit, no GLOB; must be warning-clean under `-Wall -Wextra -Werror`, and self-inerting on chips without LCD_CAM).

Mirror the `i80Ws2812*` family as `moonI80Ws2812*` (same 8 signatures, same `MoonI80Ws2812Handle { void* impl; }` opaque handle) in `src/platform/platform.h`, next to the existing block.

Internals, built on IDF HAL + GDMA link-list (both reachable: `esp_hal_lcd` publishes its includes; `gdma_link.h` is under `esp_private/` and linkable — IDF's own drivers use it this way):
- **Init**: claim the LCD_CAM peripheral + a GDMA TX channel; configure clock/bus width/GPIO matrix (the same GPIO routing `lcd_i80_bus_configure_gpio` does); allocate the frame buffer(s) exactly as today (`esp_lcd_i80_alloc_draw_buffer`'s job, but ours); create ONE `gdma_link_list` sized for the whole frame (`gdma_new_link_list`, `num_items = ceil(bytes/4095)`).
- **Transmit**: `gdma_link_mount_buffers()` the whole frame (one call, `mark_eof` on the last node, `GDMA_FINAL_LINK_TO_NULL`), then `gdma_start()` + `lcd_ll_start()`. **No peripheral reset per frame beyond the one-time-per-transfer reset IDF also does at the start** — the point is one transaction, not many.
- **Done**: GDMA EOF callback (or the LCD `TRANS_DONE` interrupt) gives the same per-buffer semaphore the current backend uses, so the driver's wait/reuse contract is unchanged.
- **Deinit**: reverse, with the same drain-before-free discipline (a live DMA reading a buffer about to be freed is the use-after-free that bit the chunking attempt).

### 2. Domain: the sibling driver

**New file `src/light/drivers/MoonI80LedDriver.h`** — the CRTP surface is 12 methods + 5 constants, and `I80LedDriver.h` is the template. Every `bus*` hook is a one-line forward to `moonI80Ws2812*`. Reuses `ParallelLedDriver` for *everything* (slicing, encode, double-buffer, shift-register, loopback, `wireUs` KPI, the dead-frame guard).

**Register it** in `src/main.cpp`: one gated `#include` + one `registerType<mm::MoonI80LedDriver>("MoonI80LedDriver", "light/drivers.md#mooni80led")`, inside the existing `#if defined(CONFIG_SOC_LCD_I80_SUPPORTED)` block.

This makes the A/B a **module swap in the UI** — both drivers are offered, the user picks. No reflash to compare, which is the whole point.

### 3. ADR

**New `docs/adr/NNNN-own-i80-dma-driver.md`** (Nygard). Records: IDF's per-transaction peripheral reset makes gapless multi-transaction output impossible (with the source citation); the LCD data phase is DMA-length-driven, which is the opening; we go one level below `esp_lcd` to IDF's HAL+GDMA (not raw registers); both drivers ship until the challenger wins. This is a deliberate divergence from *Industry standards, our own code* and must be recorded, not slipped in.

## Verification

**Host** — free, and this is the payoff of the CRTP base: `MoonI80LedDriver` compiles on desktop (`lcdLanes == 0` → `lanesAvailable() == 0` → every bus call inert), so it gets the existing `unit_I80LedDriver.cpp`-style config/lane/pin/control coverage by copying that file's shape (+ one line in `test/CMakeLists.txt`). The base's tick/double-buffer/shift-register behaviour is *already* covered via the Mock drivers and is inherited unchanged.

**Hardware — the A/B, on the SE16 (S3, 16-lane), which is the rig with a known-good reference:**
1. **Correctness first, unshifted.** Swap `I80LedDriver` → `MoonI80LedDriver` in the UI, same pins, same effect. **Dense effect (the one that exposed the chunking flicker) must be flicker-free** — that is the acceptance test, and it is the PO's eyes, not a log line. The GDMA error count is *not* a success metric (today it reported "0 errors" on a state the PO called broken, and 121 errors on a state he called clean).
2. **Then the real question — PSRAM.** Enable the shift-register expander on board B and push past 96 lights/strand. If MoonI80 renders where IDF's cannot, phase 1 is the answer. If it fails identically, the silicon is the wall and phase 2 is justified.
3. **Loopback** as the closed-loop check once it renders (`loopbackTest`, and note its RX path is still unproven — see §7.5).
4. **No regression**: `I80LedDriver` untouched, still selectable, still the default.

## Scope guards

Phase 1 only (whole-frame chain). Phase 2 (internal-RAM ring) is DEFERRED and gated on phase 1 *measuring* that the silicon is the limit — not assumed. **NOT** touching `I80LedDriver` (it is the reference; it stays the default and stays correct). **NOT** Parlio yet — it has the same per-transaction reset problem (`parlio_tx.c:505`) and the same fix likely generalises, but it is a separate increment on separate hardware. **NOT** raw register pokes — IDF HAL + GDMA link-list only. **NOT** removing the IDF driver until the challenger demonstrably beats it.

## Files

- `src/platform/platform.h` — the `moonI80Ws2812*` seam (8 fns + handle), mirroring the existing i80 block.
- `src/platform/esp32/platform_esp32_moon_i80.cpp` (new) + `esp32/main/CMakeLists.txt` SRCS line.
- `src/light/drivers/MoonI80LedDriver.h` (new) — CRTP sibling, `I80LedDriver.h` is the template.
- `src/main.cpp` — gated `#include` + one `registerType`.
- `docs/adr/NNNN-own-i80-dma-driver.md` (new) — the divergence, recorded.
- `docs/moonmodules/light/drivers.md` — a `#mooni80led` section (the `registerType` docPath must resolve; `check_specs.py` enforces it).
- `test/unit/light/unit_MoonI80LedDriver.cpp` (new) + `test/CMakeLists.txt` line.
