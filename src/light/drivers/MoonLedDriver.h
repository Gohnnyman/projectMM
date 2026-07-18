#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"

namespace mm {

/// Output driver: parallel WS2812B on the **LCD_CAM** peripheral (ESP32-S3 / -P4), driven by **our own
/// DMA code** instead of ESP-IDF's `esp_lcd`. Same peripheral, same pins and same wire contract as
/// [MultiPinLedDriver](MultiPinLedDriver.md) — the difference is underneath, and it buys two things
/// `esp_lcd` cannot give: a frame **streamed** rather than held whole, and a **74HCT595 pin expander**
/// (one GPIO driving 8 strands — see the last section; skip it if you drive strands directly).
///
/// **A frame STREAMED, not held.** The DMA refills a small pool of internal buffers behind the read head,
/// so **RAM stops scaling with strand length**: at one light per buffer the pool is ~18 KB whether a
/// strand is 128 lights or 1024. `useRing` picks this path; `ringRows`/`ringBufs` size it.
///
/// **Why `esp_lcd` cannot do it.** It re-arms the peripheral on every transaction:
/// `lcd_start_transaction()` does `lcd_ll_reset()` + `lcd_ll_fifo_reset()` + a hard-coded 4 µs busy-wait
/// before each one. An LCD panel does not care — it is addressed, not clocked continuously. WS2812 is one
/// unbroken self-clocked bit stream, so a reset mid-frame corrupts everything after it: a frame cannot be
/// split across transactions at ANY chunk size, which forces the whole frame into ONE transaction, from
/// ONE contiguous DMA-reachable block. That is the cap this driver exists to lift.
///
/// **The hardware never demanded it.** The LCD peripheral has **no data-length register**
/// (`lcd_ll_set_phase_cycles()` sets `lcd_dout` as a boolean *enable*; IDF's own comment reads "Number of
/// data phase cycles are controlled by DMA buffer length"). It clocks exactly what the DMA feeds it and
/// stops when the chain ends. So **one `gdma_start()` over an arbitrarily long descriptor chain plus one
/// `lcd_ll_start()` is a single gapless stream across as many buffers as we like.** This driver takes
/// that, on IDF's HAL + GDMA link-list APIs — one level below `esp_lcd`, not raw registers; IDF's own
/// drivers use the same APIs. Rationale + what we give up:
/// [ADR-0014](https://github.com/MoonModules/projectMM/blob/main/docs/adr/0014-own-i80-dma-driver-below-esp-lcd.md).
///
/// **What streaming costs.** The whole-frame path has no CPU deadline once a transfer is armed; the ring
/// does. Its refill runs from the DMA's end-of-buffer interrupt and must beat the wire — 576 B per light
/// at 26.67 MHz is **21.6 µs/light** — or the strands see a gap. That trade is the reason both paths ship
/// and `useRing` is a switch, not a constant.
///
/// **Both drivers ship, deliberately.** `MultiPinLedDriver` is the **reference**: correct, memory-capped,
/// and what this one is measured against. This is the **challenger**. Both are registered module types, so
/// switching is a swap in the UI — the A/B needs no reflash, same board, same effect. The reference is
/// retired only if and when the challenger demonstrably beats it.
///
/// Everything above the DMA is inherited unchanged from ParallelLedDriver: the slicing, the fused 3-slot
/// encode (ParallelSlots.h), the async double-buffer, the loopback self-test, the
/// `frameTime` KPI, and the dead-frame guard. This class adds only what is i80-specific — WR, the ring's
/// geometry, and the platform forwards — which is why it is nearly all one-liners. LCD_CAM only: the
/// classic ESP32's i80 is the I2S peripheral, a different backend entirely.
///
/// ---
///
/// ## The 74HCT595 pin expander (skip unless one is fitted)
///
/// **1 GPIO drives 8 strands**, so 6 data pins → **48 strands**. Off by default (`pinExpander`); with it
/// off, everything above is the whole story and WR never reaches a pad.
///
/// ```text
///   bus lane 0 ──────► SER    ┌── 74HCT595 ──┐ QA ──► strand 0
///                             │              │ QB ──► strand 1
///   WR (pixel clock) ──┬────► SRCLK (shift)  │ ..        ..
///                      │      │              │ QH ──► strand 7
///   bus lane N ──┬─────┼────► RCLK (latch)   └──────────────┘
///                │     │
///                │     └────► SRCLK of every other '595  (all shift in lockstep)
///                └──────────► RCLK  of every other '595  (all latch together)
/// ```
///
/// **How one lane becomes 8 strands.** A '595 is a serial-in / parallel-out shift register: 8 SRCLK edges
/// clock 8 bits down its chain, then one RCLK edge presents all 8 at once on QA..QH. So a lane's byte
/// becomes 8 strands' worth of one WS2812 bit, and the whole '595 bank presents that bit simultaneously.
///
/// **Why WR is free, and why the latch is not.** The peripheral toggles WR once per bus word in hardware —
/// exactly a shift clock — so SRCLK costs **zero DMA bytes**. But the peripheral has only ONE clock
/// output, and it is already spent on SRCLK; the latch has to come from somewhere else, and the only
/// thing left is a **data lane**, driven per word like pixel data — one lane that carries no strand.
///
/// **What bounds the strand count is the driver, not the bus:** every data pin fans out to 8, and
/// ParallelLedDriver refuses more than `kMaxStrands` (64), so **8 data pins is the ceiling** — 64 strands.
/// hpwit's board populates **6 → 48 strands**.
///
/// **The '595 is a one-slot pipeline.** Its outputs only change on the latch, so during slot N the strand
/// sees the byte latched at the START of slot N — i.e. what was shifted in during slot N−1. The encoder
/// therefore writes every value ONE SLOT EARLY (the rotation in ParallelSlots.h). Get that wrong and the
/// first LED survives while everything after it is noise.
///
/// **Why the expander needs the ring.** The fan-out costs 8 bus words per WS2812 bit, so a light is 576 B
/// and a **48 x 256** frame is ~144 KB — more contiguous internal RAM than an S3 has, and from PSRAM the
/// DMA cannot sustain the expander's 26.67 MHz clock (both measured). Streaming is the only route to that
/// target, which is why these two features are one story.
///
/// **Prior art.** The '595 expander and the per-light streaming ring are hpwit's ideas, from
/// [I2SClocklessVirtualLedDriver](https://github.com/hpwit/I2SClocklessVirtualLedDriver) and his expander
/// board — studied hard, credited, and then written fresh against our own architecture and layout (his
/// `putdefaultones()` prefill has our own counterpart; the transpose is our own SWAR). He runs a PLL240M
/// clock tree (19.2 MHz, a 30 µs/light encode budget); we run PLL160M, where the only in-spec integer
/// prescale is 26.67 MHz — a **21.6 µs/light** budget.
class MoonLedDriver : public ParallelLedDriver<MoonLedDriver> {
public:
    // Data pins + loopback pin default to UNSET, for the same reason as the sibling: they are
    // user-soldered, so a hard-coded default would be a guess that could drive a pin the user
    // committed elsewhere. The base declares pins="" / loopbackRxPin=-1, so nothing is needed here.

    /// WR — the pixel clock — and it is needed **only by a 74HCT595 expander**, which is why it is the
    /// one bus control this driver keeps.
    ///
    /// WR toggles once per bus word in hardware, which is exactly what a '595's SRCLK needs: the pixel
    /// clock IS the shift clock. That is why the expander costs zero DMA bytes for its clock, and why
    /// the LATCH has to ride a *data lane* instead (the peripheral gives only one clock output).
    ///
    /// **In direct mode nothing reads WR** — WS2812 is self-clocked, so the strands ignore it — and the
    /// platform then leaves it unrouted, so the pin stays free for a strand. The value is still used to
    /// pick which GPIO WR would land on in shift mode, so a board with an expander sets it and a plain
    /// board can ignore it.
    ///
    /// There is no `dcPin`. DC exists so an LCD panel can separate command bytes from data bytes; a
    /// WS2812 strand has no such concept, the peripheral is configured to hold DC at a constant level,
    /// and (unlike `esp_lcd`, which mandates a valid DC GPIO) this backend simply never routes it to a
    /// pad. Spending a GPIO on it would buy nothing.
    int8_t clockPin = 10;

    /// Stream the frame as a RING of small internal DMA buffers (on, the default), or send it WHOLE from
    /// one contiguous buffer (off). Ignored in direct mode — that never rings. A prepare trigger (rebuilds
    /// the bus).
    ///
    /// Whole-frame is the simpler path and is proven under ~240 lights/strand, but it cannot reach the
    /// expander's target size: a 48x256 frame is ~144 KB contiguous internal, which does not exist, and
    /// from PSRAM the DMA cannot sustain the 26.67 MHz expander clock (measured: the identical frame runs
    /// from internal RAM and produces no output from PSRAM). So it stays as the A/B reference the ring is
    /// measured against, not as a fallback the ring degrades into.
    bool useRing = true;

    /// Lights per DMA buffer — the ring's grain, and a control rather than a constant because the optimum
    /// is a measurement, not a derivation. RAM is the ONLY axis that wants it small: at 1 the ring is
    /// ~18 KB flat at ANY strand length (128, 256, 1024 all cost the same), which is what puts 48x256 in
    /// reach. Three axes want it big:
    ///
    /// | axis | why big wins |
    /// |---|---|
    /// | per-call overhead | the encode seam's fixed cost amortises over the rows in a buffer; at 1 it is paid per light, inside the ISR |
    /// | interrupt rate | one EOF per buffer: 256 lights at 100 fps is 25.6k int/s at 1 row vs 1.6k at 16 — and a busy ISR starves the network stack |
    /// | lap-time runway | `ringRows x ringBufs x 21.6 us` is how long a WiFi preemption may last before the DMA laps a buffer the ISR is still refilling |
    ///
    /// So the per-light ring is not "better" — it is the only geometry whose RAM is flat. Sweep this to
    /// find the knee. Pin-expander mode only; a prepare trigger (the buffers are sized and the DMA chain
    /// mounted at build time, so a change is a rebuild). Default = what this driver shipped with, so an
    /// existing config renders identically.
    uint8_t ringRows = platform::kRingRowsDefault;

    /// How many buffers the DMA circulates. Depth buys **lap time**, not capacity: the pool is a sliding
    /// window the encoder refills behind the read head, so `ringRows x ringBufs x 21.6 us` is the jitter
    /// the ring absorbs. Pin-expander mode only; a prepare trigger.
    uint8_t ringBufs = platform::kRingBufsDefault;

    // --- CRTP hooks the base calls (all non-virtual; no vtable) ---

    /// LCD_CAM lanes on this chip (0 = none, and then the base's guards make the driver inert).
    /// Unlike the sibling this does NOT add `i2sLanes`: the classic ESP32's i80 is the I2S peripheral,
    /// which this backend does not implement.
    static constexpr uint8_t lanesAvailable() { return platform::lcdLanes; }
    /// The i80 bus width is 8 or 16 — a hardware fact (`lcd_ll_set_data_wire_width` takes nothing else).
    /// The PIN count stays free: configure only the pins that drive something and the base rounds the bus
    /// up around them, parking the spare lanes on WR (which the peripheral already drives, and nothing
    /// reads). Parlio sets this false — its bus width IS its pin count.
    static constexpr bool kPowerOfTwoBus = true;
    /// The loopback cannot build a 1-lane private bus, so it rebuilds the full-width bus and carries
    /// the pattern on lane 0 — the test frame must therefore be encoded at the operational bus width.
    static constexpr bool kLoopbackFullWidth = true;
    /// Status text when the bus will not come up, so the cause is on screen rather than in a serial log.
    /// The two real causes are named: a pin the peripheral cannot route, or no DMA-reachable memory for
    /// the frame (or the ring's pool).
    static constexpr const char* kInitFailMsg = "MoonI80 bus init failed — check pins / memory";
    /// The expander needs a backend that can stream its ×8 frame; LCD_CAM is it, and this driver is
    /// LCD_CAM-only, so the answer is simply "wherever this driver runs at all".
    static constexpr bool kSupportsPinExpander = platform::lcdLanes > 0;

    /// The base pads spare bus lanes with this GPIO. Unrouted lanes cost nothing here, so the value is
    /// only ever *used* in shift mode — where WR is a real pad and the padding is genuinely inert.
    uint16_t clockPinForBus() const { return static_cast<uint16_t>(clockPin); }

    /// WR is a '595 pin here, so the control follows the expander toggle: bound always (a saved value
    /// survives a round-trip through direct mode) but shown only when a shift register can read it.
    void addBusControls() {
        controls_.addPin("clockPin", clockPin);
        controls_.setHidden(controls_.count() - 1, !pinExpanderMode());
    }

    /// The output path + the ring's geometry and instrument. A separate hook from addBusControls() so the
    /// base can place these AFTER latchPin — clockPin and latchPin are one '595 wiring pair and belong
    /// together in the UI, not split by a mode selector.
    void addRingControls() {
        // Path selector (pin-expander mode only): the ring, or the whole frame. A distinct axis from
        // ringSnapshot — this picks the PATH, ringSnapshot tunes how the RING reads its source; they
        // compose. Whole-frame is the A/B reference: it is how the whole-frame-PSRAM-at-the-expander-clock
        // question stays testable on the same board and content.
        controls_.addBool("useRing", useRing);
        controls_.setHidden(controls_.count() - 1, !pinExpanderMode());
        // The geometry + the instrument, shown only when the RING is the chosen path — all three are
        // meaningless on the whole-frame one. (Gating on wantsRing() is safe here, unlike ringSnapshot's:
        // it reads `useRing`, a plain member, not frameBytes_, which is still 0 when the schema is first
        // built.)
        controls_.addUint8("ringRows", ringRows, 1, 64);
        controls_.setHidden(controls_.count() - 1, !wantsRing());
        controls_.addUint8("ringBufs", ringBufs, 2, 32);
        controls_.setHidden(controls_.count() - 1, !wantsRing());
        // Ring internals, so the streaming can be diagnosed by polling /api/state (reliable) rather than
        // scraping serial: "sl<slices>/bf<bufs> dn<done> de<descErr> enc<worst refill µs> gap<worst EOF-to-EOF µs>".
        controls_.addReadOnly("ringDbg", ringDbgStr_, sizeof(ringDbgStr_));
        controls_.setHidden(controls_.count() - 1, !wantsRing());
    }

    /// Refresh the ringDbg diagnostic string once a second (base tick1s chains here via refreshBusKpi).
    /// Nothing to report on the whole-frame path — the control is hidden there, so leave it untouched
    /// rather than writing a "no ring" status that only repeats what useRing says.
    void refreshBusKpi() {
        const platform::MoonI80RingStats s = platform::moonI80Ws2812RingStats(bus_);
        if (!s.isRing) return;
        // The extended fields (ld/tx/ipb/ci/tn) are the LAPPING-phase instruments — the same readouts that
        // isolated the prime-only bugs (ld = drain progress, tx = real wire time vs the physical frame
        // minimum, ipb/ci/tn = node accounting + the terminator). Kept until 256+/strand ships (see the
        // backlog's ring entry for the removal list).
        std::snprintf(ringDbgStr_, sizeof(ringDbgStr_), "sl%u/bf%u dn%u ld%u tx%u ipb%u ci%u tn%d de%u enc%u gap%u",
                      static_cast<unsigned>(s.nSlices), static_cast<unsigned>(s.ringBufs),
                      static_cast<unsigned>(s.doneGiven), static_cast<unsigned>(s.lastDrain),
                      static_cast<unsigned>(platform::moonI80Ws2812LastTransmitUs(bus_)),
                      static_cast<unsigned>(s.itemsPerBuf), static_cast<unsigned>(s.consumedItems),
                      static_cast<int>(s.termNodeDiag), static_cast<unsigned>(s.descErr),
                      static_cast<unsigned>(s.maxEncodeUs), static_cast<unsigned>(s.maxIsrGapUs));
                      // enc = worst ISR refill-encode µs (producer); gap = worst EOF-to-EOF µs (deadline).
                      // enc >= gap == the refill can't keep pace (PACE); enc << gap but still fails == CURSOR/logic.
    }
    /// Which of this driver's controls need the BUS rebuilt (not just a re-encode) when they change:
    /// the WR pin, the output path, and the ring's geometry — buffers are sized and the DMA chain mounted
    /// at build time, so each of these is a rebuild.
    bool busControlTriggersBuild(const char* name) const {
        return std::strcmp(name, "clockPin") == 0
            || std::strcmp(name, "useRing") == 0      // path switch: rebuild the bus on the new path
            || std::strcmp(name, "ringRows") == 0     // geometry: buffers are sized and the chain mounted
            || std::strcmp(name, "ringBufs") == 0;    // at build time, so a change is a rebuild
    }

    /// WR only reaches a pad in shift mode, so it can only COLLIDE in shift mode. In direct mode the
    /// signal never leaves the peripheral, so `clockPin` naming a strand's GPIO is harmless — and
    /// rejecting it would forbid a perfectly good config for the sake of a signal nobody reads.
    const char* validateBusFatal() const {
        if (pinExpanderMode()) {
            // The '595 needs WR on a real GPIO (it is the SRCLK). Unset (-1) would route the
            // peripheral's WR signal to GPIO 65535 — reject it before busInit reaches the pad.
            if (clockPin < 0) return "the 74HCT595 expander needs a clockPin (its shift clock)";
            if (latchPin >= 0 && latchPin == clockPin)
                return "latchPin is on clockPin (WR) — the latch needs its own GPIO";
        }
        return nullptr;
    }
    /// A data lane sharing WR's GPIO is silent corruption — the matrix routes both signals to the one
    /// pad and that strand emits the shift clock instead of pixel data. Only possible in shift mode.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const {
        if (!pinExpanderMode()) return nullptr;
        for (uint8_t i = 0; i < n; i++)
            if (lanes[i] == static_cast<uint16_t>(clockPin)) return "a data pin is on clockPin (WR)";
        return nullptr;
    }

    /// Create the bus + its DMA buffer(s) for `frameBytes`. `busPinList()`/`busPinCount()` come from
    /// the base (in shift mode the list appends the latch — it is a bus lane), and
    /// `busClockMultiplier()` tells the platform how many bus words one WS2812 slot is shifted out
    /// over, so it can scale the pixel clock and the slot keeps its wire duration.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) {
        return platform::moonI80Ws2812Init(bus_, this->busPinList(), this->busPinCount(),
                                           static_cast<uint16_t>(clockPin), frameBytes,
                                           wantSecondBuffer, this->busClockMultiplier());
    }

    /// Should reinit build a RING for this config instead of the whole-frame path? Only in pin-expander
    /// mode — direct mode never rings, it drives PSRAM fine at the 10x-slower clock — and then it is the
    /// user's choice via `useRing`, defaulting to on.
    ///
    /// There is no auto-router. It would have exactly one right answer at the size the expander exists for
    /// (a 48x256 frame never fits internal DMA RAM, so it would always pick the ring) while presenting
    /// itself as a decision, and its silent fallback made the ACTIVE path invisible — the driver reported
    /// "driving N lights" while quietly running whole-frame from PSRAM, which does not clock at the
    /// expander's 26.67 MHz. The switch says what runs.
    bool wantsRing() const {
        if (!pinExpanderMode()) return false;   // direct mode never rings (drives PSRAM fine)
        return useRing;
    }

    /// Bring the bus up as a streaming RING (the phase-2 path): the platform loops a few small internal
    /// buffers and calls back per drained buffer to refill it, so a frame too big for internal RAM never
    /// materialises (see platform.h). `rowBytes`/`padBytes` come from the base's frame arithmetic; the
    /// trampoline below is the encode seam. Returns false if even the small ring won't fit — the base
    /// then falls back to busInit (whole-frame), which idles with a status if IT can't fit either.
    bool busInitRing(size_t rowBytes, uint32_t totalRows) {
        return platform::moonI80Ws2812InitRing(bus_, this->busPinList(), this->busPinCount(),
                                               static_cast<uint16_t>(clockPin), rowBytes, totalRows,
                                               ringRows, ringBufs, this->busClockMultiplier(),
                                               &MoonLedDriver::ringEncodeTrampoline, this);
    }
    /// Send one frame on the ring: prime the pool, fire the DMA, and let the EOF ISR refill behind it.
    bool busTransmitRing()          { return platform::moonI80Ws2812TransmitRing(bus_); }
    /// Did the bus actually come up as a ring? The base routes tick() on this, so it reports what the
    /// platform BUILT, not what was asked for — a ring that would not fit falls back to whole-frame.
    bool busIsRing() const          { return platform::moonI80Ws2812IsRing(bus_); }

    /// The platform's `MoonI80EncodeFn` seam: a plain function pointer (there is no CRTP hook for it), so
    /// this static trampoline recovers `this` from `user` and encodes one slice into the ring buffer the
    /// platform hands it. `dst` is the buffer; `firstRow`/`rowCount` name the slice; `closeFrame` gates
    /// the trailing latch pad (only the last slice emits it).
    ///
    /// **It prefills the shift constants FIRST, then the data — because a ring buffer is recycled, not
    /// re-zeroed.** The whole-frame path prefills once at reinit and `encodeRows` then writes only the
    /// DATA word (two-thirds of the stores hoisted out); but a ring buffer holds a DIFFERENT slice each
    /// time it drains, so its constants (pulse-start/tail per this slice's active mask, and the latch pad
    /// on the last slice) must be re-laid on every refill or the buffer keeps the previous slice's
    /// constants and renders wrong on the second frame (pinned by the recycled==fresh host test). Direct
    /// mode writes every slot word in encodeRows, so it needs no prefill.
    static void ringEncodeTrampoline(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                     bool closeFrame) {
        auto* self = static_cast<MoonLedDriver*>(user);
        const uint8_t outCh = self->correction_.outChannels;
        const auto first = static_cast<nrOfLightsType>(firstRow);
        const auto count = static_cast<nrOfLightsType>(rowCount);
        if (self->slotBytes() == 1) {
            if (self->pinExpanderMode()) self->prefillShiftRows<uint8_t>(outCh, dst, first, count);
            self->encodeRows<uint8_t>(outCh, dst, first, count, closeFrame);
        } else {
            if (self->pinExpanderMode()) self->prefillShiftRows<uint16_t>(outCh, dst, first, count);
            self->encodeRows<uint16_t>(outCh, dst, first, count, closeFrame);
        }
    }
    /// The whole-frame path's DMA buffer `i` (0, or 1 with doubleBuffer on) — where the base encodes a
    /// frame. Null on a ring handle, which has no whole-frame buffer to hand out.
    uint8_t*  busBuffer(uint8_t i)              { return platform::moonI80Ws2812Buffer(bus_, i); }
    /// Bytes that buffer holds — the base's guard against encoding past the end after a live resize.
    size_t    busCapacity() const               { return platform::moonI80Ws2812BufferCapacity(bus_); }
    /// Clock buffer `i` out: one gapless DMA transfer of `bytes`, returning as soon as it is armed.
    bool      busTransmit(uint8_t i, size_t bytes) { return platform::moonI80Ws2812Transmit(bus_, i, bytes); }
    /// Block until buffer `i` has finished clocking (or `ms` elapses) — how the async double-buffer
    /// defers its wait to the NEXT frame instead of stalling this one.
    bool      busWait(uint8_t i, uint32_t ms)   { return platform::moonI80Ws2812Wait(bus_, i, ms); }
    /// Measured wire time of the last frame, in µs — the `frameTime` KPI's source, and the output floor
    /// the encode is compared against.
    uint32_t  busLastTransmitUs() const         { return platform::moonI80Ws2812LastTransmitUs(bus_); }
    /// Tear the bus down: stop the DMA before freeing anything it could still read. Safe on a
    /// half-built bus, so a failed init and a live one release through the same path.
    void      busDeinit()                       { platform::moonI80Ws2812Deinit(bus_); }

    /// Drive `frame` on a private bus and capture the wire back on `loopbackRxPin` (jumpered), so the
    /// self-test bit-verifies what the peripheral ACTUALLY emitted — the one instrument that does not
    /// take the driver's word for it.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) {
        return platform::moonI80Ws2812Loopback(this->busPinList(), this->busPinCount(),
                                               static_cast<uint16_t>(clockPin),
                                               static_cast<uint16_t>(loopbackRxPin),
                                               frame, frameBytes, dataBytes, rowBits,
                                               this->busClockMultiplier(),
                                               ringRows, ringBufs, useRing);
    }

    /// WR is part of the bus identity, so a change to it rebuilds the bus — not just a data-pin edit.
    void recordBusPins() { lastClockPin_ = clockPin; }
    /// Do this driver's extra bus pins still match the live bus? WR is bus identity here, so the base
    /// rebuilds when this goes false rather than routing a stale clock.
    bool extraBusPinsCurrent() const { return lastClockPin_ == clockPin; }

private:
    platform::MoonI80Ws2812Handle bus_;
    int8_t lastClockPin_ = -1;
    char ringDbgStr_[112] = "—";   // TEMP DIAGNOSTIC: ring counters incl. the lapping-phase fields (refreshed in refreshBusKpi via tick1s)
};

} // namespace mm
