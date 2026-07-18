# Plan — MoonI80 ring: the read-head-trailing refill (reach 48×256), instrumented by the loopback

## Context

**The scatter is fully diagnosed.** Bench-bisected on the wall (PO), the ring is clean **iff `ringBufs − nSlices ≥ ~2`** — a producer/consumer HEADROOM margin, not a slice count, buffer count, encode, or `suc_eof` issue (all ruled out on the wall; `descErr=0` throughout because the chain has no owner handshake). This is the ORIGINAL `nSlices < ringBufs` analysis, off-by-one on the exact margin. The morning's "reuse works" and the mid-day "reuse is NOT the blocker" were both wrong — the latter from misreading a single `+1`-margin data point (`bufs=17`/16 slices) as "no reuse yet broken."

**Why "more buffers" cannot reach the target.** With the margin rule, the pool RAM for 48×256 is `~(256 + 2·ringRows)·576 ≈ 145 KB` **regardless of ringRows** — the 256 lights dominate, and 145 KB is the same wall as whole-frame. So headroom-by-buffers caps the driver at ~240 lights. **The fix must give headroom WITHOUT `ringBufs ≥ nSlices`.**

**hpwit's model is exactly that** (he reviewed our code): his refill index **trails the DMA read head by a fixed margin, by construction** — a small fixed pool (his `__NB_DMA_BUFFER=10`) that streams *any* strand length because the write is structurally always N slices behind the read, independent of nSlices. That is the target: **constant ~7–18 KB RAM at 256, 512, arbitrary lights.**

**The loopback works** (PO: the "never captured" backlog note is stale — verified fixed by commit `2873ec9d`, 2026-07-15: "captures it back off the strand, bit-verifies 2304/2304 bits"; R14's strand-15 measurement is independent proof). My `0 sym` was a config/wiring mismatch, not a dead path. A working loopback = **an instrument that bit-verifies the fix without the PO's eyes** — the thing missing all session.

## Step A — Two loopback modes; the INTRUSIVE one is the instrument this bug needs (do FIRST)

**The current loopback cannot see the scatter, by construction.** It builds a PRIVATE test frame and transmits it via `moonI80Ws2812Loopback` — a self-contained transmit+capture that does NOT go through the render ring. So a PASS proves only "the '595 encode + a single-shot DMA is correct" (already known); it can never observe a ring-scatter, because it doesn't use the ring. That is why it was useless for this bug, and why my `0 sym` said nothing about the ring.

**The PO's two-mode design (discussed before) is the fix:**
- **Non-intrusive** (what exists): private test frame on a SPARE '595 output. Proves the peripheral. Safe, does not disturb the render. Keep as-is.
- **Intrusive** (new): capture what the **live render pipeline / ring** actually put on the wire, off a strand the pipeline drives, and bit-verify it against the frame we asked the ring to send. **THIS mode sees the scatter** — it measures whether the frame reached the LEDs intact, which is exactly the missing instrument.

**The capture primitive already exists and is decoupled:** `captureAndVerifyFrame(rxGpio, frameBytes, dataBytes, ...)` (`platform_esp32_rmt.cpp:285`) captures on `rxGpio` and bit-verifies against an expected frame — independent of who transmitted. Intrusive mode = point that at a live-pipeline strand while the ring renders, and compare against the ring's own source snapshot. No new capture code; a new *wiring* of the existing one.

Steps:
1. **Correct the stale doc**: `docs/backlog/shift-register-driver-analysis.md:385` ("never captured a single symbol") — the RX path was fixed 2026-07-15 (`2873ec9d`, "captures it back off the strand, bit-verifies 2304/2304"). Present-tense: the loopback captures and bit-verifies in expander mode; R14's bit-0 settling is the one tolerated artifact.
2. **Confirm non-intrusive still PASSes** on a spare-strand jumper (reproduce `2873ec9d`) — proves the wiring + capture are sound before building intrusive.
3. **Add intrusive mode** — the smallest shape: a control (e.g. `loopbackIntrusive`, or a mode on the existing `loopbackTest`) that, instead of a private transmit, arms `captureAndVerifyFrame` on `loopbackRxPin` for ONE live render frame and verifies the captured wire against the ring's source snapshot (or a known test pattern the effect is set to). Design the seam with the PO: which strand it taps, whether it pauses the effect or captures a live one, how it reports `firstBadBit` + the slice it falls in.
4. **Acceptance**: with intrusive mode, a `+1`-margin config (scattered on the wall) must report a bit-fault AT a slice boundary, and a `+3` config a clean PASS — the instrument reproduces the PO's eye observations. Then Step B's fix is measurable by machine.

## Step B — The read-head-trailing refill ring (the real fix, reaches 48×256)

The design agent's Shape A (self-terminating sentinel) plus the **critical addition** the margin rule demands: the refill must **trail the DMA read head by a fixed margin**, so headroom is structural, not bought with buffers.

### The core change — refill trails the read head
Today the ISR refills a free-running counter that marches WITH the read head (the bug). hpwit's fix: the refill targets a buffer the DMA has provably passed by ≥margin. Concretely:
- The EOF event gives `tx_eof_desc_addr` (which node just drained) — map it (or the drain count) to the just-drained buffer index `d`.
- Refill the buffer at `d` (the one just freed), and **only advance the encode cursor when the pool has ≥`kHeadroom` free buffers** between the write and the read. With a fixed small pool and one refill per drain, the write stays exactly one lap behind — which IS the trailing guarantee, provided the encode keeps pace (it does: `gap` >> encode time in every measured config).
- The margin rule says `kHeadroom ≈ 2`. Size the pool `ringBufs = kHeadroom + <enough to cover encode jitter>` — a small constant (~12), NOT `nSlices`.

### Self-terminating frame end (hpwit point 1, design-agent Shape A)
- Keep the closed `GDMA_FINAL_LINK_TO_HEAD` body loop (never stalls). Add a dedicated **zero sentinel run** (`GDMA_FINAL_LINK_TO_NULL`), pool grown by its node count.
- When the last real slice is refilled (in `startRingTransfer` priming if `nSlices ≤ ringBufs`, else in the ISR), `gdma_link_concat` that buffer's final node → the sentinel head. The DMA walks the body, into the sentinel's LOW, hits NULL, **self-terminates** — no `gdma_stop` racing the prefetcher.
- Sentinel EOF gives `done[0]`; the ISR stops calling `gdma_stop`/`lcd_ll_stop`.
- **Reset sizing**: the sentinel must clock ≥300 µs LOW, sized to `kResetLowBytes` from the slot rate — INDEPENDENT of `ringRows` (at `ringRows=1` one row ≈21.6 µs is far too short; today's `kTailBufs=1` reset is marginal there — a real contributor).
- Re-arm restores the spliced node's `next` in `startRingTransfer` (DMA stopped between frames — safe).
- **REJECT** rebuilding a linear `nSlices`-node chain per frame — that is the documented prior design that stalled at `nSlices > ringBufs`. Flag in-comment.

### Why it reaches 48×256
The pool is a small fixed size (~12 buffers, ~7–110 KB depending on `ringRows`), the refill trails the read head by construction, and the sentinel ends the frame cleanly. Headroom no longer scales with `nSlices`, so 256 / 512 / arbitrary lights all stream at constant RAM. This is the "unlimited lights at constant RAM" the ring was always for.

## Files
- `src/platform/esp32/platform_esp32_moon_i80.cpp` — the ISR refill (trailing cursor + sentinel splice), `initRingDma`/`createRingState` (sentinel node, pool sizing), `startRingTransfer` (re-arm), `destroyState`, the heap guard, `MoonI80State`. Also the stale mount comment (~945-960, describes a removed latch pad).
- `test/unit/light/unit_ParallelLedDriver_ring.cpp` — the mock must mirror the new termination + trailing-refill order (its `driveRingFrameWithTermination` pins the drain-count stop, which changes). **Run ASan** (this session shipped a heap overflow only ASan caught).
- `docs/backlog/shift-register-driver-analysis.md` — Step A doc fix.
- `docs/backlog/backlog-light.md` — replace the "8 vs 16 slices, no mechanism" entry with the margin rule + the trailing-refill fix.

## Verification
- **Step A**: loopback PASS on the PO's strand-0 wiring, reproducing `2873ec9d`.
- **Step B**:
  - Host `ctest` + **ASan** (the ring/termination tests; the mock changes).
  - **The margin acceptance test, MEASURED not eyeballed**: sweep `bufs − nSlices` across its range and confirm the clean/scatter boundary MOVES — with the trailing-refill fix, a small pool (`ringBufs=12`) must render clean at `nSlices` WELL beyond 10 (the old +2 boundary), i.e. at 256 lights / `ringRows=8` (32 slices, pool of 12). That is the fix working: headroom without `bufs ≥ nSlices`.
  - Bit-verify via the now-working loopback where possible.
  - PO's eyes remain the final gate; `de=0` proves nothing.
- **Before commit**: build clean, ctest, ASan, scenarios, ESP32 ×3. (The pending ASan+CodeRabbit fixes are still uncommitted — decide with the PO whether they ride this commit or precede it.)

## Out of scope
- Encode speed (hpwit: fill constants, unroll loops) — 65 µs vs 21.6 µs, unmeasurable until the wall is clean; revisit AFTER the ring streams.
- `_DMA_EXTENSTION` / PLL240M — deadline headroom, not this bug.

---

## Session update 2026-07-18 — findings + refined design

**Step A3 shipped + flashed (shiffy, S3-n16r8):** the loopback now rides the ring when `useRing` is on (not only when the frame overflows internal RAM), at the driver's LIVE `ringRows`/`ringBufs`. Threaded `useRing` + `ringRows`/`ringBufs` through `moonI80Ws2812Loopback` (platform.h decl, ESP32 impl, desktop stub, `MoonLedDriver::busLoopback`).

**Jumper identified — strand 8 (0-indexed).** The non-intrusive loopback (whole-frame private bus) PASSes on strand 8 from a clean heap, proving jumper + capture + peripheral + bit-verify all sound. (The earlier `0 sym` on strand 0 was the wrong strand, not a dead path.) Strand 8 = data pin 1's '595 (GPIO 10), shift position 0 → Q7.

**Root cause of the intrusive `bus init failed`: the loopback builds a PRIVATE ring.** `runLoopbackSelfTest` calls `deinit()` (frees the live ~150 KB render ring) then rebuilds a private ring of the SAME size. On a fragmented heap (measured `maxBlock=34 KB` with 237 KB free at idle) the 20×7.5 KB pool can't be placed → `bus init failed`. This is the 48×256 RAM wall reproduced by machine: the ring needs a large contiguous pool, which the heap can't always give.

**Design correction (PO):** the intrusive loopback must **RIDE the live render ring**, not build a private one. No `deinit`, no private alloc — so no fragmentation, AND it verifies the ACTUAL render output (a truer test). This is built FIRST, as the instrument that makes every Step B iteration machine-verifiable (the contaminated `enc`/`gap` counters are not trustworthy — `enc0 gap94802` at 256 lights is frame-boundary contaminated, the classic "counters are worthless, the wall is the instrument" trap).

### Ride-the-live-ring intrusive loopback — mechanism (verify against a FORCED known pattern)
1. Do NOT `deinit()`. The live ring keeps rendering.
2. Pin the driver's SOURCE to a known pattern for the tapped strand (write the known RGB into `sourceBuffer_`, or force a Solid) so strand 8's expected wire is deterministic — reusing the existing `0xA5/00/0xFF` bit-verify.
3. Arm the RMT-RX capture on `loopbackRxPin` (GPIO 16) — a capture-only entry point that builds NO i80 bus.
4. Let the running ring clock one frame; capture; bit-verify strand 8's wire against the known pattern. A scattered margin shows as a bit-fault AT a slice boundary; a clean margin PASSes.

### Step B — trailing-refill, OPTION 1 chosen (looping chain kept)
Keep the proven `GDMA_FINAL_LINK_TO_HEAD` loop (never stalls). Key the refill off the drained descriptor (`tx_eof_desc_addr`) so the write provably trails the read head by construction; shrink the pool to a small fixed size (encode-jitter runway only, ~12). The self-terminating NULL-sentinel (hpwit's exact model) is the FALLBACK if option 1 doesn't clean up on the wall.

### Backlog surfaced this session
- Loopback teardown leak (~80 KB/cycle under rapid repeat) + idle heap fragmentation (`maxBlock=34 KB` with 237 KB free). Instrument reliable for a single clean run only until fixed.
- Flash scripts must persist the used port to `moondeck.json` `last_port` (MoonDeck's dropdown resolves it live but doesn't write it back, so CLI flashes can't find it).

### Ride-the-live-ring: PARKED (kept, default-off, hot-path-clean) — the RX can't share the pin
Built the driver-agnostic ride (platform::ws2812LoopbackRide + the snapshot pattern-hold + loopbackIntrusive
control). It compiles clean, flashes, and PROVES the no-fragmentation goal (heap healthier after a run, since
it builds no private ring). But it captures `0 sym idle=0` — confirmed across strand 8, strand 15, and a
fully-lit Solid strand (ruling out sparse content). Root cause: an RMT-RX cannot capture on a GPIO net the
LCD_CAM peripheral is actively driving for output — the non-intrusive path only works because it deinit()s the
peripheral first, freeing the pin's input path. This is an ESP32 GPIO-matrix reality, not a code bug. Moving to
a spare strand (15) does NOT help: on the '595 the spare strand rides the SAME live ring (one shift stream per
data pin), but the blocker is the pin's INPUT path, independent of strand.

Decision (PO): PARK the ride, keep the code. It is default-off (`loopbackIntrusive=false`, `patternHoldStrand_
=-1`) and hot-path-clean (one leading short-circuit `if (patternHoldStrand_ >= 0 ...)` per ring frame; skipped
entirely when off). Step B is verified by the PROVEN non-intrusive loopback (PASS on strand 8, 2304/2304) + the
PO's eyes. Once Step B shrinks the ring to a small fixed pool, the non-intrusive path's fragmentation flakiness
(its only flaw) largely vanishes too. Future revival path if wanted: a brief GPIO-matrix output-detach of just
the RX pin for the capture window (no deinit, no realloc) — the one way to free the pin's input while the ring
keeps running. Not pursued now; the goal is 48×256 (Step B), which the existing loopback + eyes can verify.

### Step B bisect (2026-07-18, PO eyes) — the "scatter" is THREE distinct bugs, not one
Careful margin bisect on the wall (128 lights, shiffy) split the problem apart:

1. **Bug 1 — reset tail sized in BUFFERS not TIME (a FREEZE at small ringRows).** `kTailBufs=1` = one zero
   buffer = `ringRows × 21.6us`. Below ~150us the WS2812 reads it as a PAUSE not a reset (hpwit: "less than
   150us ... like it was sent just after") → the strand never latches → frozen frame. rows=6 (130us) froze;
   rows=16 (346us) clean. Isolated: froze at 10 slices / +8 margin (rules out slice-count AND margin), and
   130 lights with a SHORT last slice was clean (rules out ragged division). FIX SHIPPED: guarantee >=350us
   idle-LOW between the frame's stop (new `lastStopUs`) and the next arm, in `startRingTransfer` —
   time-based, ZERO extra RAM, pool-size-safe (a tail-buffer count would re-lap a small pool). Verified: it
   moved rows=6 from instant-freeze to "runs a few ms then freezes."

2. **Bug 1b — CONTENT freeze ("frozen except pixel 0") still at small ringRows AFTER the reset fix.** KEY
   discriminator: `dn` (doneGiven) KEEPS ADVANCING while the wall is frozen → the ring is NOT wedged (frames
   complete, DMA runs); it re-transmits STALE buffer rows. Pixel 0 alone updates. Signature points at the
   prefill/encode split leaving rows 1+ stale at small ringRows (the latent bug flagged in
   [[prefill-once-per-frame-not-per-slice]]). NEXT: trace WHY encodeRows/prefill produces stale rows 1+ when
   ringRows is small — a code investigation, not more bench sweeps. This is the true small-pool blocker.

3. **Bug 2 — tight-margin SCATTER.** `enc0 de0` while scattered = not pace, not corruption; structural.
   +2 clean / +1 scattered at 16 slices. Deferred until Bug 1b clears (can't test a small pool until small
   ringRows renders fresh content). Fix direction unchanged: self-terminating chain + refill trailing the
   read head.

Counters confirmed WORTHLESS again: enc0/de0 identical across clean/scatter/freeze; `gap` contaminated by
inter-frame idle. The WALL is the only instrument.

### IRAM policy — checked (PO raised it), a documented FOLLOW-UP not the current fix
Our policy (platform_esp32_moon_i80.cpp ~303): the EOF ISR ENTRY is IRAM_ATTR (dispatch + semaphore give),
but the heavy encode it tail-calls (encodeRingSlice) stays in FLASH. Rationale: the channel does NOT set
isr_cache_safe (not ESP_INTR_FLAG_IRAM), so a flash-resident callback is permitted and only faults when the
flash cache is disabled (a SPI-flash write — OTA/NVS), which never overlaps rendering. Mirrors IDF's own
RGB-LCD bounce-buffer refill (IRAM-forced only under opt-in CONFIG_LCD_RGB_ISR_IRAM_SAFE, default off) — a
recognized pattern, not a bespoke shortcut.

hpwit keeps MORE in IRAM (his encode + descriptor ops). Two separate angles:
- CORRECTNESS (cache-disabled fault): CANNOT cause the steady flicker — no flash writes during render.
- JITTER: our ISR tail-calls a flash-resident encode, so a cold path is a flash-fetch (cache-miss) latency
  spike in the ISR. If that makes the ISR miss the DMA timing on some frames, it COULD contribute to glitches.
  This is the plausible relevance of "hpwit has more IRAM."

Decision (PO): fix the gdma_stop-mid-frame RACE (self-terminating chain) FIRST — it's the confirmed
structural root of both the flicker and the scatter. If flicker PERSISTS after (pure jitter), THEN move the
encode/descriptor ops to IRAM as a targeted jitter fix — but check IRAM headroom first (16 KB region, was
~94% full; the flash-resident encode may not FIT, which is likely why the original decision kept it in flash).

### Self-terminating chain — the design (hpwit Point 1, verified; option 1 approved)
hpwit: "I let the DMA stop at the end of the frame" — the chain self-terminates via a NULL sentinel, NO
mid-frame gdma_stop. Our counter-based `gdma_stop`+`lcd_ll_stop` in the EOF ISR races the GDMA prefetcher AND
the render thread's next-frame re-prime = the residual flicker (prime-only) + the tight-margin scatter
(lapping). Same root. Fix = self-terminate. Complements (does NOT replace) the shipped reset-tail-by-time
(hpwit: "if I wait long enough it will restart with a new frame" = the >=300us LOW makes the strand latch
before the next frame).

Mechanism (keep the FIXED ringBufs-node looping pool — NOT a per-frame linear chain, which stalled):
- `gdma_link_concat(link, idx, NULL, -1)` sets node idx's `next = NULL` — verified a SINGLE ISR-safe
  pointer write (gdma_link.c), no locks/alloc. Restoring the loop is `gdma_link_concat(link, tailIdx, link,
  0)` (tail->head), also one write.
- Node math: itemsPerBuf = esp_dma_calculate_node_count(rowsPerBuf*rowBytes, align, 4095) — can be >1 (rows=13
  ->2, rows=16 ->3 at kDmaNodeMaxBytes=4095), so node != buffer in general. STORE itemsPerBuf on MoonI80State
  so the ISR can map buffer b -> its last node = (b+1)*itemsPerBuf - 1.
- FRAME END (ISR): when `drained` reaches the stop point (nSlices + the reset-tail buffers), splice NULL onto
  the last node of the just-drained buffer so the DMA self-terminates after finishing it — REMOVE gdma_stop /
  lcd_ll_stop. The DMA's final-node EOF gives `done` + sets lastStopUs (reset clock still starts here).
- RE-ARM (startRingTransfer): restore tail->head BEFORE gdma_start. The DMA is genuinely halted between
  frames (it self-terminated), so the restore is race-free — this is the whole point vs the old mid-frame
  stop.
- MOCK + ASan: the host ring test must mirror the NULL-splice termination + re-arm order (the mock caught the
  earlier ASan overflow). Update driveRingFrameWithTermination.

Risk: a wrong node index = a hang (DMA walks into a NULL early, or never terminates). Verify on the wall at
rows=6 (prime-only, itemsPerBuf=1) FIRST, then rows=13/16 (itemsPerBuf>1), then the lapping 256-light case.

### hpwit's ACTUAL termination — read from his source (de-risks the splice)
Read /Users/ewoud/Developer/GitHub/hpwit/I2SClocklessVirtualLedDriver/src/I2SClocklessVirtualLedDriver.h.
His structure:
- `__NB_DMA_BUFFER`(=10) circular working buffers [0->1->..->9->0], PLUS two extra: [N] a prime/arm node
  (next=[0], suc_eof=0 so no interrupt), and [N+1] a PERMANENT NULL-terminator node (next=NULL), which
  nothing points at during normal running.
- FRAME END, in his IRAM ISR (line ~2255), VERBATIM:
    if (ledToDisplay_out == (num_led_per_strip - __NB_DMA_BUFFER))
        DMABuffersTampon[dmaBufferActive % __NB_DMA_BUFFER]->next = DMABuffersTampon[__NB_DMA_BUFFER + 1];
  i.e. he splices the pre-built NULL node onto the CURRENT buffer's `next` **__NB_DMA_BUFFER buffers BEFORE
  the last LED** — his comment: "not -1 because it takes time to have the change into account and it rereads
  the buffer." THAT is the GDMA-prefetch answer: splice the terminator a FULL POOL DEPTH ahead of the read
  head, never at the last slice. He ALSO keeps a hard fallback: `if (ledToDisplay >= NUM_LEDS + N - 1)
  i2sStop`.
- IRAM: his interrupt is ESP_INTR_FLAG_IRAM and transpose/loadAndTranspose are IRAM_ATTR — the whole encode
  is IRAM-resident (confirms the PO's IRAM instinct; our flash-resident encode is the jitter follow-up).

OUR TRANSLATION (esp_lcd link API, our own code): pre-build a NULL-terminator node in the link list (one
extra item). In the EOF ISR, when `drained == nSlices - ringBufs` (a full pool depth before the last real
slice), gdma_link_concat(link, <current buffer's last node>, <terminator node>) so the DMA self-terminates a
pool-depth later, AFTER clocking the remaining slices + reset tail. Keep gdma_stop ONLY as a timeout fallback
(remove once the wall proves the NULL terminates). Re-arm: restore the loop before gdma_start (DMA halted
between frames). This is hpwit's exact mechanism, written against our API — splice-ahead-by-pool-depth is the
de-risk.

### Self-terminate attempt 1 — WEDGED, reverted. The missing piece: the NULL-node EOF event.
Built the prime-only arm-time NULL splice (gdma_link_concat(link, termNode, NULL, -1) in startRingTransfer,
skip gdma_stop in the ISR when termNode>=0, restore the loop on next arm). Node math verified correct
(rows=13: nSlices=10, itemsPerBuf=2, termNode=(10+1)*2-1=21 = buffer 10's last node). Flashed shiffy: it
BOOTED fine (no hang) but the RENDER WEDGED — dn stuck, "no LED output", done never fired. Cleanly reverted
(kept reset-tail + prime-only gate, which render clean); left the termNode/itemsPerBuf state fields in place.

ROOT of the wedge: when the GDMA walks into a node whose `next == NULL`, it evidently does NOT raise the
`on_trans_eof` callback our ISR is registered on (esp_lcd's on_trans_eof) — so `done` is never given and the
render loop times out. hpwit does NOT use esp_lcd's on_trans_eof: he drives the I2S/GDMA descriptors directly
and keys off `suc_eof` bits on his own nodes (his [N] node has suc_eof=0; his interrupt is on the descriptor
EOF, not a peripheral callback). So his termination fires his interrupt in a way ours won't.

NEXT (before re-flashing): determine EXACTLY which event a NULL-terminated GDMA node raises on the S3
(read IDF esp_driver_dma / the LCD_CAM DMA docs, or hpwit's suc_eof descriptor setup). Options once known:
(a) register the correct event (on_trans_eof may need a mark_eof=true on the terminator node so the LAST node
raises EOF even though next=NULL — the mount currently sets mark_eof on every node, but the SPLICED-in NULL
may drop it); (b) keep a short-timeout gdma_stop fallback that fires done if the NULL EOF doesn't within N us
(hpwit keeps exactly such a hard-stop fallback: `if (ledToDisplay >= NUM_LEDS + N - 1) i2sStop`). The likely
fix is (a)+(b): ensure the terminator node has mark_eof, AND keep a fallback. Do NOT re-flash until the EOF
event is understood — a wedged render each attempt costs a reflash.

### Self-terminate attempt 2 — MECHANISM PROVEN, one GDMA-indexing puzzle blocks it
Diagnostic-driven this time (added ld/eof/tn/ci to ringDbg). Findings:
- The mechanism WORKS: with a "splice ONCE per geometry, never per-frame" arm (per-frame restore+resplice
  raced the still-walking DMA — that was the intermittent ld=7 wedge), rows=13 ran SUSTAINED clean: ld=11
  (=nSlices+1), dn climbing over 8s, dead=false. hpwit's self-termination is right for us.
- THE REMAINING PUZZLE, pinned by data: the splice node is CORRECT (tn=10 = buffer 10 at rows=6 itemsPerBuf=1;
  tn=21 = buffer 10 at rows=13 itemsPerBuf=2, ci=40=20*2). But the DMA terminates EARLY: ld=5 when it should
  reach ld=11. So `gdma_link_concat(link, 10, NULL)` makes the DMA stop around node 5, NOT node 10 —
  **gdma_link_concat's item_index is NOT the DMA's walk position.** Splicing NULL onto "index 10" terminates
  the chain much earlier than buffer 10.
- Also unstable across geometry SWITCHES: a fresh rows=6 (or rows=13 after a reflash) wedges at low ld; the
  earlier clean rows=13 was a lucky arming. So there is a real ordering/indexing bug, not just a race.

NEXT (study, NOT another flash — 3 cycles hit the anti-stalling limit): read IDF esp_driver_dma
`gdma_link_mount_buffers` + `gdma_link_concat` to learn how the item_index maps to the WALKED chain order
(the mount may not lay node i at list-index i; alignment padding / internal reordering). The fix is to splice
on the index that is actually the DMA's Nth walked node. Candidates once understood: (a) walk the chain via
the link API to find the real terminator index; (b) use `gdma_link_get_head_addr` + node addresses to map
tx_eof_desc_addr back to a buffer (the ISR already gets the drained descriptor addr); (c) hpwit sidesteps
this entirely by pointing at a PERMANENT separate NULL node ([N+1]) via his OWN descriptor array, not IDF's
link-list indices — consider building our own descriptor array like his rather than fighting the IDF index
abstraction. Scaffolding left in the tree: bufLastNode[], termNode, itemsPerBuf, and the ld/tn/ci ringDbg
diag. Board reverted to known-good (reset-tail + prime-only gate render clean).

### RESOLVED: prime-only self-termination SHIPS — verified on the wall at 60/128/192 lights per strand
The "GDMA index puzzle" was never an index-mapping problem. It was THREE stacked bugs, each found by a
targeted diagnostic (ld/tx/ipb/ci/tn in ringDbg) and each fixed structurally:

1. **Per-buffer mount calls re-linked the terminator away.** `gdma_link_mount_buffers` links `node[start-1]
   -> node[start]` on every call, so mounting buffer termBuf+1 overwrote the NULL just placed on termBuf —
   the chain looped ~23x per "frame" (ld=230, tx=24ms). Fix: mount only up to and including the terminator.
2. **Multi-node buffers break the walk.** With a buffer spanning 2+ descriptor nodes (rows>=8 at 576 B/row),
   the NULL sat on the right node (tn=33) yet the DMA stopped mid-chain (~node 25). hpwit never enters this
   case: his buffer struct IS a single lldesc_t. Fix: clamp rowsPerBuf so one buffer = ONE node (<= 4095 B),
   deleting the bug class. Lossless — small buffers are the small-pool direction anyway.
3. **EOF counting undercounts.** The GDMA interrupt is a latch bit, not a queue: two EOFs during an ISR delay
   (an /api/state serialise) coalesce into one invocation, the drain count comes up short, `done` never
   fires, the driver gives up (every big-frame config died within ~20 frames, ld stuck a few short). Fix:
   in prime-only, mark_eof ONLY on the terminator — ONE interrupt per frame, no counting, undercount
   impossible, ~nSlices-fold fewer interrupts (hpwit's suc_eof=0 on his arm node is the same instinct).

End state: prime-only frames (nSlices <= ringBufs) mount a NULL-terminated single-node-per-buffer chain at
BUILD time, arm with plain gdma_start(head), interrupt once at the terminator, and never gdma_stop — no race
exists by construction. Wall-verified clean at 60, 128, and 192 lights/strand (rows=7, 28 slices, 29 nodes),
stable under heavy API polling. NOTE: the old "+2 margin" rule is obsolete for prime-only — no reuse, no
refill, so bufs = nSlices + 1 (tail) suffices.

**Remaining: LAPPING (256+/strand, nSlices > ringBufs max 32) — 256 still scatters on the old looping path**
(verified on the wall alongside the clean 192). The next phase applies the SAME principles to the lap:
hpwit's ISR splice of the terminator a POOL-DEPTH ahead of the read head, and no load-bearing EOF counting
(the lapping refill still needs per-buffer EOFs, but frame-end must key off the terminator, not a count).
The platform mount/EOF contract is below the busInitRing seam, so it is hardware-verified (the host mock pins
the driver-side contract above the seam; 27/27 ring tests green throughout).
