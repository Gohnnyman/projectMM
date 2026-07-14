# 14. Our own i80 DMA driver, one level below esp_lcd

Date: 2026-07-14

## Status

Accepted

## Context

`I80LedDriver` drives parallel WS2812 output through ESP-IDF's `esp_lcd` i80 bus. It pre-encodes a whole frame and sends it as **one** `esp_lcd_panel_io_tx_color` transaction, which is gapless and correct — the peripheral streams the buffer and stops.

That single-transaction design is also the driver's ceiling. The DMA must read the whole frame in one unbroken stream, so the frame has to be somewhere the DMA can sustain, in one contiguous allocation. The measured consequences:

- The **74HCT595 expander** renders correctly only while its ×8 frame fits internal DMA RAM — about **96 lights per strand** on the ESP32-S3. Above that the frame lands in PSRAM and the strands garble.
- **Parlio** caps at ~**4,096 lights**: a hardware 65,535-byte single-transfer limit, and a contiguous-block limit below that.
- The **classic ESP32** caps at ~**2,048 lights**: its i80 backend is the I2S peripheral, whose DMA cannot address PSRAM at all.

The obvious fix is to split the frame into several transactions. **We built that, and it does not work.** On a dense frame the strands flash full-brightness white; on a sparse frame the fault hides. The cause is in IDF, not in our code — every transaction starts with `lcd_start_transaction` (`esp_lcd/i80/esp_lcd_panel_io_i80.c`):

```c
lcd_ll_reset(bus->hal.dev);        // reset the LCD peripheral
lcd_ll_fifo_reset(bus->hal.dev);   // flush the FIFO
gdma_start(...);
esp_rom_delay_us(4);               // hard-coded busy-wait
lcd_ll_start(bus->hal.dev);
```

**`esp_lcd` resets the peripheral between transactions.** For an LCD panel that is harmless — a panel is addressed, not clocked continuously. For WS2812 it is fatal: the protocol is one unbroken self-clocked bit stream, and a mid-frame reset corrupts everything after it. IDF's Parlio driver resets its FIFO in the same place (`esp_driver_parlio/src/parlio_tx.c`). So **no chunking strategy inside IDF's LED-adjacent drivers can be gapless**, at any chunk size or boundary. This is why hpwit's driver — the reference implementation for this class of LED output — hand-rolls its DMA rather than using IDF's.

There is, however, an opening. The LCD peripheral has **no data-length register**: `lcd_ll_set_phase_cycles()` sets `lcd_dout` as a *boolean enable*, and IDF's own comment reads *"Number of data phase cycles are controlled by DMA buffer length"*. The peripheral clocks out exactly what the DMA feeds it and stops when the chain ends. Therefore **one `gdma_start()` over an arbitrarily long descriptor chain, plus one `lcd_ll_start()`, is a single continuous gapless stream spanning as many buffers as we like.** `esp_lcd` discards that capability by re-arming per transaction; the hardware never required it.

The descriptors are almost free: a 144 KB frame (16 lanes × 1,024 lights) needs 37 descriptors — 444 bytes.

## Decision

**Build a second i80 implementation, `MoonI80`, on IDF's HAL and GDMA link-list APIs (`lcd_ll_*`, `gdma_link_*`) — one level below `esp_lcd` — and ship it alongside the existing driver rather than replacing it.**

Three parts to the decision:

1. **One level below `esp_lcd`, not down to the registers.** `gdma_link_*` and the LCD HAL are the APIs IDF's own drivers are built on. We are declining `esp_lcd`'s transaction *policy*, not its abstractions. No raw register pokes.

2. **The whole frame in one descriptor chain (phase 1).** We already pre-encode the frame, so the DMA can simply read it — no ISR refill, no ring, no real-time deadline, and therefore no underrun for WiFi to cause. This is strictly simpler than hpwit's design, which needs a CPU refill only because it transposes per-LED.

3. **Both drivers ship.** `I80LedDriver` remains the default and the **reference implementation**: correct, memory-capped, and the thing MoonI80 is measured against. MoonI80 is the challenger. It replaces the reference only when it demonstrably beats it on the same bench. Both are registered module types, so the A/B is a swap in the UI with no reflash.

An **internal-RAM ring with CPU refill** (hpwit's shape, and the only thing that can ever work on the classic ESP32) is deferred to a phase 2, and **gated on phase 1 measuring that the silicon — not `esp_lcd` — is the wall.** The ring is a superset of the same descriptor machinery (`GDMA_FINAL_LINK_TO_HEAD` closes the chain), so it is an extension, not a rewrite.

## Consequences

**What we gain.** A gapless multi-buffer transfer, which is what lifts the memory ceilings: the frame no longer has to be one contiguous DMA-reachable block. It also removes the per-transaction size caps that bound Parlio.

**What we give up, and it is real.** This diverges from *[Industry standards, our own code](../../CLAUDE.md#principles)* — we are leaving a maintained IDF driver for code we own. The justification is that the maintained driver **cannot express the behaviour the hardware supports and WS2812 requires**, and that is demonstrated from IDF's source, not assumed. But we now carry: the GPIO/clock/bus setup `esp_lcd` was doing for us, the interrupt plumbing, and the risk of drifting against future IDF versions. Keeping `I80LedDriver` as the reference is the mitigation — if MoonI80 rots, the working path is still there and still default.

**What phase 1 measured (2026-07-14, board B — the question this ADR was written to settle).**

MoonI80 renders correctly on real hardware: the SE16 at 4,096 lights (direct, 16 lanes) and board B through the 74HCT595 expander, both confirmed by eye, with **zero GDMA mount failures** and a wire time matching the `esp_lcd` reference to within 0.15% (19,646 µs vs 19,674 µs — so our own peripheral configuration produces the same waveform).

And it answered the open question, by removing the suspect rather than reasoning about it:

| pixel clock | frame in PSRAM | result |
|---|---|---|
| **2.67 MHz** (direct) | 2,048 lights | **drives** — 7,712 µs on the wire |
| **26.67 MHz** (expander) | *any* size — 54 KB or 144 KB | **never completes** |

Same board, same PSRAM, same descriptor chain, same driver. The only variable is the clock. **The S3's GDMA cannot sustain a PSRAM read at the expander's 10× rate** — a '595 is serial-in, so each WS2812 slot is shifted out over 8 bus words, and the bus must run ten times faster to keep the slot's duration.

This **kills the hypothesis that motivated the build**: the `esp_lcd` path failed with thousands of `lli full` descriptor-mount errors, which pointed hard at its descriptor handling. MoonI80 removes that mechanism entirely (own chain, mounted once, owner-checking off) — the mount errors are gone, and the transfer still never completes. The `lli full` storm was a **symptom, not the cause**. Six earlier hypotheses about this bug were proposed and refuted (see [lessons.md](../history/lessons.md)); this is the first one killed by a controlled experiment with a working control condition rather than by a story that stopped fitting.

**Consequence: phase 2 is now justified by measurement, and this driver is its foundation.** The fix is the internal-RAM ring — close the chain into a ring (`GDMA_FINAL_LINK_TO_HEAD`) over small *internal* buffers and refill them from the PSRAM frame in our own EOF callback, a bulk sequential CPU read, so the DMA never reads PSRAM at the expander's clock at all. Every piece of that (our own link list, our own EOF hook, one continuous `lcd_ll_start` that is never re-armed) exists *only* because we own the DMA; `esp_lcd` can express none of it. The ring extends this machinery rather than replacing it, exactly as the decision above anticipated.
