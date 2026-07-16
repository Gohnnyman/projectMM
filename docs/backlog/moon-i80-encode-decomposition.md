# MoonI80 shift encode — decomposition vs hpwit

Working note for the 1-light ring's remaining deficit. Delete when the encode meets its deadline.

## The deadline

The '595 shift bus clocks **576 B per light** at **26.67 MHz** → the DMA drains one light in **21.6 µs**.
The ring holds one light per DMA buffer, so the EOF ISR must encode one light inside that window. There
is no slower legal clock on our tree: prescale 3 of PLL160M/2 gives a 300 ns slot (in spec); prescale 4
gives 400 ns, past the ~380 ns T0H max, where a "0" reads as a "1".

**Measured: 50 µs/light. 2.3× over.**

## Per-light decomposition

Both drivers do the same 24 transposes per light (3 channels × 8 '595 taps). An 8×8 butterfly costs the
same whether 2 rows or 6 are non-zero, so **per-light cost is independent of pin count** — hpwit gets 48
strands out of the same work we spend on 16. Instruction counts below are executed-per-light, read off
the actual disassembly (`xtensa-esp32s3-elf-objdump`), timed at 1.3 cycles/instruction @ 240 MHz.

### Ours (2 data pins × 8 taps = 16 strands)

| stage | instr | µs |
|---|---:|---:|
| `encodeRows` prologue + member hoists | 40 | 0.22 |
| `memset(wire_, 0, wireCap_)` | 56 | 0.30 |
| 16 lanes × `Correction::apply` (inlined) | 224 | 1.21 |
| 3 × call overhead into `encodeWs2812ShiftData` | 18 | 0.10 |
| gather (3 ch × 8 cycles × 2 pins) | 312 | 1.69 |
| transpose (3 ch × 8 cycles × `transposeBits8x8Pair`) | 576 | 3.12 |
| emit (3 ch × 8 cycles × 8 bits) | 960 | 5.20 |
| **total** | **2186** | **11.8** |

### hpwit's `loadAndTranspose` (6 data pins × 8 taps = 48 strands)

| stage | instr | µs |
|---|---:|---:|
| prologue (hoist driver fields to locals, once) | 20 | 0.11 |
| gather: 8 taps × 6 pins, brightness LUT fused into the read | 456 | 2.47 |
| transpose **+ emit fused** (3 calls, each unrolled 8×, storing into the DMA buffer) | 576 | 3.12 |
| **total** | **1052** | **5.7** |

His 130 fps at 256 lights = 30 µs/light of wire, so his encode must fit well inside that. ~6 µs is
consistent with it.

## What the decomposition says

**The instruction-count model above says ~12 µs. A cycle-counter profile of the live ISR says otherwise —
and the profile is the one to believe:**

| stage | cycles/light | µs | share |
|---|---:|---:|---:|
| `memset(wire_)` | 131 | 0.5 | 2% |
| correction, 16 lanes | 1510 | 6.3 | 19% |
| transpose + emit | 6280 | 26.2 | 79% |
| cache msync | 95 | 0.4 | 1% |
| **accounted** | **~8000** | **~33** | |
| unaccounted (vs the 50 µs the ISR's own timer reports) | | ~17 | |

**The encode is 26 µs of real work — 4× the model, not a stall.** Both stages come out 4–7× the
instruction estimate, consistently, which is why six model-driven optimizations all measured null: there
was no bug to find. The model was wrong; the work is genuine. Treat the table above as a shape, not a
budget.

Structural differences, and their status:

- **His transpose IS his emit** — `transpose16x1_noinline2(firstPixel[ch].bytes, buff)` where `buff` is
  the DMA buffer; it stores bit-planes straight in at the `_BRIGHTNES_n` offsets (48 B apart). **Ours now
  does the same** — verified: GCC produced a byte-identical ELF from the fused and two-loop forms, so it
  was already fusing them. Closed, and it was not the cost.
- **His brightness LUT is fused into the gather** (`mapg[*(poli_b+1)]`); ours is a separate
  `Correction::apply` pass into `wire_` plus a `memset`. Worth ~1.5 µs by the table — real but small.
- **His `firstPixel[]` gather is once per light**, indexed `[tap<<4 | pin]`; ours re-reads `wire_` inside
  the cycle loop. Worth ~1.7 µs.

Together those are worth ~1.5 µs of plumbing (the LUT lookups themselves — 48 per light — are real work
that fusing does not remove). Neither is the lever.

## Ruled out (all measured on the S3, all null)

| hypothesis | test | result |
|---|---|---|
| PSRAM snapshot reads | `allocIsr` (internal RAM), retested at 1 light/buffer | 50 → 50 µs |
| cache msync per refill | split counter | 7 µs of 1118 (0.6%) |
| flash-resident ISR code | `IRAM_ATTR` on `encodeRingSlice` | no change |
| 16 KB instruction cache | 32 KB | no change |
| per-call setup overhead | 4 rows/buffer (¼ the calls, same RAM) | 48.3 vs 46 µs/light |
| `planes[]` staging spill | fused emit | byte-identical ELF |

## Won (measured, kept)

| change | effect |
|---|---|
| `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`) — IDF's `-Og` was inherited, never chosen | 1.52× |
| data-only encode + prefill | 4.4× host |
| **prefill once per buffer at frame arm** (was once per light: 384 constant stores against 192 data stores — the whole-slot encoder by another name) | **66 → 46 µs** |
| 32-bit pair transpose instead of 64-bit SWAR on a 32-bit CPU | 1790 → 1052 B of code, 77 → 66 µs |

## Open

**The clock is the largest untried lever.** hpwit's own `_BUFFER_TIMING = (_NB_BIT / 19.2) - 4` macro is
his encode deadline: **~30 µs/pixel** at his 19.2 MHz shift clock, and he ships a counter for pixels that
miss it plus an auto-tuner that pads the DMA when they do. He does not have a 6 µs encoder — he has a
bigger budget. Moving to PLL240M to unlock 19.2 MHz is **+39%** for us (21.6 → 30 µs). Necessary but not
sufficient at 50 µs; it is a peripheral-wide clock change and needs its own plan.

~17 µs sits between the profiled ~33 and the ISR timer's 50. `msync` is ruled out (0.4 µs) and ISR
entry/exit is outside that window, so the leading suspect is the two `esp_timer_get_time()` calls that
form the counter itself — measured at ~3% of frame time when sampled 1-in-16, which does not fully
explain it. Rebuild the stage profile behind a `constexpr bool` gate (not `#ifdef` — the platform
boundary rule forbids those outside `src/platform/`) before chasing it further.

## Open bug: whole-frame shift mode never delivers frames (PRE-EXISTING)

**Reproduced from a COLD BOOT with `forceRing=2` persisted** — the ring never allocates, so this is not
the ring, not the ring→whole-frame transition, and not heap fragmentation. Whole-frame shift mode is
broken on its own, at 128 lights and at 192.

Symptoms: `tickTimeUs` **210 ms** (≈4 fps, LEDs frozen), `frameTime` holds one boot-time value (3061 µs —
so exactly one frame transmitted, then nothing), `status` still reads "driving N of M lights" with no
error, and `ringDbg` correctly reports "not ring". Switching back to the ring recovers instantly (3 ms
/tick, 166 fps).

The 210 ms is two timeouts inside `tickAsync`: `busWaitIfBusy` (`waitBudgetMs()`, capped at 100 ms) plus
`moonI80Ws2812Transmit`'s `wireFree` wait (`kWireFreeTimeoutMs`). Both time out because **the transfer's
EOF never fires** — the DMA starts and never completes. `deadFrames_` never reaches
`kDeadFramesBeforeGiveUp`, which is why the give-up path never sets "output stalled" and the status stays
misleadingly healthy.

Ruled out, each measured: heap fragmentation (128 lights needs 72 KB and still stalls); a too-short wait
(20–100 ms budget against a 2.8–4.1 ms real transfer); today's stage profile (reproduced with it stripped);
`dynamicBytes: 0` (a red herring — that counter tracks core-allocated buffers, not the driver's DMA
buffer).

**Why it went unnoticed:** the `forceRing` A/B switch appears never to have worked, because until the
1-light ring landed the ring path never actually ran — so nobody ever compared the two. Whole-frame in
DIRECT mode is unaffected and is what every other board ships.

Next: instrument `moonI80Ws2812Transmit`'s return and the EOF callback for a whole-frame shift transfer.
The `bytes > st->cap` guard at the top of Transmit returns false silently, and `tickAsync` treats a false
as "skip this frame" — no error, no retry, no `inFlight_`. That is the shape to check first.
