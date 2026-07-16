#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"

namespace mm {

/// Output driver: parallel 8-or-16-lane WS2812B on the **LCD_CAM** peripheral, driven by **our own
/// DMA code** instead of ESP-IDF's `esp_lcd` component. Same peripheral, same wire contract, same
/// pins as [MultiPinLedDriver](MultiPinLedDriver.md) — the difference is underneath (who programs the DMA), plus
/// what falls out of it: owning the GPIO matrix means this driver needs no DC pin at all and routes WR
/// only when a '595 expander reads it, so a direct-mode board spends its GPIOs on strands alone.
///
/// **Why this exists.** `esp_lcd` re-arms the peripheral on every transaction: `lcd_start_transaction()`
/// does `lcd_ll_reset()` + `lcd_ll_fifo_reset()` + a hard-coded 4 µs busy-wait before each one. An LCD
/// panel does not care — it is addressed, not clocked continuously. WS2812 is one unbroken self-clocked
/// bit stream, so a reset mid-frame corrupts everything after it. That makes a frame split across
/// several `esp_lcd` transactions impossible to send gaplessly at ANY chunk size, which forces the
/// whole frame into ONE transaction — and that is what caps the driver: the DMA must stream the entire
/// frame from a single contiguous, DMA-reachable block.
///
/// The hardware never demanded that. The LCD peripheral has **no data-length register**
/// (`lcd_ll_set_phase_cycles()` sets `lcd_dout` as a boolean *enable*; IDF's own comment reads
/// "Number of data phase cycles are controlled by DMA buffer length"). It clocks out exactly what the
/// DMA feeds it and stops when the chain ends. So **one `gdma_start()` over an arbitrarily long
/// descriptor chain plus one `lcd_ll_start()` is a single gapless stream across as many buffers as we
/// like** — which is what lifts the memory ceiling. This driver takes that, built on IDF's HAL and
/// GDMA link-list APIs (one level below `esp_lcd`, not raw registers; IDF's own drivers use the same
/// APIs). Rationale + what we give up: [ADR-0014](https://github.com/MoonModules/projectMM/blob/main/docs/adr/0014-own-i80-dma-driver-below-esp-lcd.md).
///
/// **Both drivers ship, and that is deliberate.** `MultiPinLedDriver` is the **reference**: correct,
/// memory-capped, and the thing this one is measured against. This is the **challenger**. Because both
/// are registered module types, switching between them is a swap in the UI — the A/B needs no reflash,
/// on the same board, on the same effect. The reference is retired only if and when the challenger
/// demonstrably beats it.
///
/// Everything above the DMA is inherited unchanged from ParallelLedDriver: the slicing, the fused
/// 3-slot encode ([ParallelSlots.h](ParallelSlots.md)), the async double-buffer, the 74HCT595
/// shift-register expander, the loopback self-test, the `frameTime` KPI, and the dead-frame guard. This
/// class adds only what is i80-specific — the WR pin (a '595 pin here, not an i80 tax: see clockPin)
/// and the platform forwards — which is why it is nearly all one-liners.
///
/// LCD_CAM only (ESP32-S3 / -P4). The classic ESP32's i80 is the I2S peripheral, a different backend
/// entirely, so this driver is not offered there.
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

    /// A/B path selector: which output path the shift-mode driver uses. 0 = AUTO (wantsRing() decides:
    /// ring when the whole frame won't fit internal DMA RAM, else whole-frame), 1 = force RING, 2 = force
    /// WHOLE-FRAME. The force modes exist to A/B the two paths on the same board/content/load — in
    /// particular to test whether the whole-frame PSRAM path actually holds at the shift clock (the premise
    /// the ring was built to work around), and to force the ring for its own diagnosis. AUTO is the shipping
    /// default; the forces are diagnostic. Ignored in direct mode (never rings). A prepare trigger (rebuilds
    /// the bus). Values match the Select option order below.
    uint8_t forceRing = 0;

    // --- CRTP hooks the base calls (all non-virtual; no vtable) ---

    /// LCD_CAM lanes on this chip (0 = none, and then the base's guards make the driver inert).
    /// Unlike the sibling this does NOT add `i2sLanes`: the classic ESP32's i80 is the I2S peripheral,
    /// which this backend does not implement.
    static constexpr uint8_t lanesAvailable() { return platform::lcdLanes; }
    static constexpr bool kPowerOfTwoBus = true;   // the BUS rounds to 8/16; the pin count is free
    /// The loopback cannot build a 1-lane private bus, so it rebuilds the full-width bus and carries
    /// the pattern on lane 0 — the test frame must therefore be encoded at the operational bus width.
    static constexpr bool kLoopbackFullWidth = true;
    static constexpr const char* kInitFailMsg = "MoonI80 bus init failed — check pins / memory";
    /// forceRing Select options (index = the forceRing member value): 0 auto, 1 ring, 2 whole-frame.
    static constexpr const char* kForceRingOptions[3] = {"auto", "ring", "wholeFrame"};
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
        // A/B path selector (shift mode only): AUTO / force ring / force whole-frame. Options match the
        // forceRing member (0/1/2). Distinct axis from ringSnapshot — this picks the PATH, ringSnapshot
        // tunes how the RING reads its source; they compose (force ring, then snapshot on/off). Shown only
        // in shift mode (direct never rings). Force whole-frame is how the whole-frame-PSRAM-at-the-shift-
        // clock question gets tested deliberately rather than left to the auto router.
        controls_.addSelect("forceRing", forceRing, kForceRingOptions, 3);
        controls_.setHidden(controls_.count() - 1, !pinExpanderMode());
        // TEMP DIAGNOSTIC: ring internals as a read-only control, so the ≥256 stall can be diagnosed by
        // polling /api/state (reliable) instead of scraping serial. Shows "slices/bufs eof/N done drain
        // items(cap/used)". Remove once the reuse boundary is fixed.
        controls_.addReadOnly("ringDbg", ringDbgStr_, sizeof(ringDbgStr_));
    }
    /// Refresh the ringDbg diagnostic string once a second (base tick1s chains here via refreshBusKpi).
    ///
    /// Reads: `sl<nSlices>/bf<ringBufs> dn<frames> de<descErr>` then the PACE half, which is per-FRAME:
    ///   - `enc:none` — the ISR encode branch never ran (nSlices <= ringBufs: every buffer was primed on
    ///     the render thread before gdma_start, so the ISR only memsets). **There is no pace question in
    ///     this regime** — printing a number here invites reading a leftover as if it meant something.
    ///   - `enc<mean>/<max>us gap<mean>/<min>us` — the refill (producer) against the drain (deadline).
    ///     Compare MEAN to MEAN for "does it keep up?"; `max` vs `min` is the worst case. The gap extreme
    ///     shown is the MIN because the tightest deadline is what a refill has to beat — a max gap is the
    ///     most slack, which flatters.
    /// The per-frame reset + mean are deliberate: a lifetime max outlives the config that produced it and
    /// gets read back under another one.
    void refreshBusKpi() {
        const platform::MoonI80RingStats s = platform::moonI80Ws2812RingStats(bus_);
        // STAGE PROFILE (temporary): cycles/light for each stage of the encode, at 240 MHz.
        uint32_t cm = 0, cc = 0, ce = 0, cr = 0;
        this->dbgProfile(cm, cc, ce, cr);
        char prof[48] = "";
        if (cr) {
            std::snprintf(prof, sizeof(prof), " | ms%u co%u en%u cyc/l",
                          static_cast<unsigned>(cm / cr), static_cast<unsigned>(cc / cr),
                          static_cast<unsigned>(ce / cr));
            this->dbgProfileReset();
        }
        if (!s.isRing) {
            std::snprintf(ringDbgStr_, sizeof(ringDbgStr_), "not ring%s", prof);
            return;
        }
        char pace[56];
        if (s.encCount == 0)
            std::snprintf(pace, sizeof(pace), "enc:none");   // ISR never encoded — no pace question here
        else
            // **The COUNTS are printed, and they are not decorative.** `enc` averages only the EOFs that
            // took the encode branch; `gap` averages EVERY EOF (most are memset-only and fast). Comparing
            // the two means without their denominators is the error that produced a phantom "the refill is
            // 2.3x too slow". They are also not independent: `gap` is measured ISR-entry to
            // ISR-entry with the encode running INSIDE it, so gap >= enc by construction for the same EOF.
            std::snprintf(pace, sizeof(pace), "enc%u/%uus(n%u) gap%u/%uus(n%u)",
                          static_cast<unsigned>(s.encMeanUs), static_cast<unsigned>(s.encMaxUs),
                          static_cast<unsigned>(s.encCount),
                          static_cast<unsigned>(s.gapMeanUs), static_cast<unsigned>(s.gapMinUs),
                          static_cast<unsigned>(s.gapCount));
        // rows<refilledRow>/<totalRows> is the raw refill cursor: it says directly whether the ISR had any
        // slice left to encode, which `enc:none` alone cannot distinguish from "the ISR never fired".
        std::snprintf(ringDbgStr_, sizeof(ringDbgStr_), "sl%u/bf%u rows%u/%u dn%u de%u %s%s",
                      static_cast<unsigned>(s.nSlices), static_cast<unsigned>(s.ringBufs),
                      static_cast<unsigned>(s.refilledRow), static_cast<unsigned>(s.totalRows),
                      static_cast<unsigned>(s.doneGiven), static_cast<unsigned>(s.descErr), pace, prof);
    }
    bool busControlTriggersBuild(const char* name) const {
        return std::strcmp(name, "clockPin") == 0
            || std::strcmp(name, "forceRing") == 0;   // A/B path switch: rebuild the bus on the new path
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

    /// Should reinit build a RING for this config instead of the whole-frame path? Only when the '595
    /// expander is engaged AND the whole frame would NOT fit internal DMA RAM — in which case the
    /// whole-frame path would put it in PSRAM and the S3's GDMA stalls reading PSRAM at the expander's
    /// 26.67 MHz clock (ADR-0014). A shift frame that DOES fit internal keeps the proven whole-frame path
    /// (and stays an A/B control against the ring at the same size). Direct mode never rings — it drives
    /// PSRAM fine at the 10×-slower clock.
    bool wantsRing() const {
        if (!pinExpanderMode()) return false;                  // direct mode never rings (drives PSRAM fine)
        if (forceRing == 1) return true;                 // A/B: force the ring
        if (forceRing == 2) return false;                // A/B: force whole-frame (test PSRAM at the shift clock)
        return !platform::moonI80Ws2812InternalFits(this->frameBytes_);   // AUTO
    }

    /// Bring the bus up as a streaming RING (the phase-2 path): the platform loops a few small internal
    /// buffers and calls back per drained buffer to refill it, so a frame too big for internal RAM never
    /// materialises (see platform.h). `rowBytes`/`padBytes` come from the base's frame arithmetic; the
    /// trampoline below is the encode seam. Returns false if even the small ring won't fit — the base
    /// then falls back to busInit (whole-frame), which idles with a status if IT can't fit either.
    bool busInitRing(size_t rowBytes, uint32_t totalRows, size_t padBytes) {
        return platform::moonI80Ws2812InitRing(bus_, this->busPinList(), this->busPinCount(),
                                               static_cast<uint16_t>(clockPin), rowBytes, totalRows,
                                               padBytes, this->busClockMultiplier(),
                                               &MoonLedDriver::ringEncodeTrampoline,
                                               &MoonLedDriver::ringPrefillTrampoline, this);
    }
    bool busTransmitRing()          { return platform::moonI80Ws2812TransmitRing(bus_); }
    bool busIsRing() const          { return platform::moonI80Ws2812IsRing(bus_); }

    /// The platform's `MoonI80EncodeFn` seam: a plain function pointer (there is no CRTP hook for it), so
    /// this static trampoline recovers `this` from `user` and encodes one slice into the ring buffer the
    /// platform hands it. `dst` is the buffer; `firstRow`/`rowCount` name the slice; `closeFrame` gates
    /// the trailing latch pad (only the last slice emits it).
    ///
    /// **It encodes DATA words only (`wholeSlots=false`), the same as the whole-frame path** — the ring's
    /// buffer already holds this slice's pulse-start/tail constants, laid by `encodeRingSlice` immediately
    /// before this call. Writing all three words per slot costs 576 stores per light against 192; measured
    /// on the host at the bench geometry (2 pins × 8 taps, 3 channels), whole-slot is **5.64× slower**, and
    /// the ring's whole budget is the 21.6 µs/light the wire takes to drain a buffer. That factor is the
    /// difference between an encode that keeps ahead of the DMA and one that cannot.
    ///
    /// The prefill+data pair is safe *here*, though it would not be on a live buffer: the ring refills a
    /// buffer **from the GDMA EOF ISR, on the buffer that just drained** — at EOF "the DMA is provably
    /// finished reading a buffer" (see `moonI80EofCb`), so both passes run while the peripheral is clocking
    /// a *different* buffer. Nothing half-written is ever on the wire. Prefilling per slice (rather than
    /// once at init) is also what keeps a strand that ends mid-slice correct: the active mask is a function
    /// of the row, so each refill re-derives it for the rows it is about to write. Direct mode has no
    /// constants and writes every slot word in one pass regardless.
    static void ringEncodeTrampoline(void* user, uint8_t* dst, uint32_t firstRow, uint32_t rowCount,
                                     bool closeFrame) {
        auto* self = static_cast<MoonLedDriver*>(user);
        const uint8_t outCh = self->correction_.outChannels;
        const auto first = static_cast<nrOfLightsType>(firstRow);
        const auto count = static_cast<nrOfLightsType>(rowCount);
        if (self->slotBytes() == 1) self->encodeRows<uint8_t>(outCh, dst, first, count, closeFrame);
        else                        self->encodeRows<uint16_t>(outCh, dst, first, count, closeFrame);
    }
    /// The platform's `MoonI80PrefillFn` seam's other half: lay one ring buffer's frame constants. Called
    /// once per buffer when a frame arms (render thread, never the ISR), so `ringEncodeTrampoline` writes
    /// only data words — see the platform.h contract for why that is two thirds of the stores.
    ///
    /// **Row 0's active-strand mask is used for every buffer, which is exact only while the strands are
    /// the same length.** The mask is a function of the row (a strand that runs out clears its bit), and a
    /// ring buffer holds a different light on every lap, so with RAGGED strands a buffer would carry the
    /// pulse-start of row 0 while clocking a light past a short strand's end — that strand would be driven
    /// HIGH instead of idle. Uniform strands (the '595 expander's normal wiring — one panel chain per tap)
    /// have one mask for the whole frame, so this is correct there. Ragged support needs the constants
    /// re-laid per lap at the row the buffer is about to hold, which costs back the stores this hoists;
    /// the honest fix is to prefill per RUN of equal-mask rows. Not yet done — see the backlog.
    static void ringPrefillTrampoline(void* user, uint8_t* dst, uint32_t rows) {
        auto* self = static_cast<MoonLedDriver*>(user);
        if (!self->pinExpanderMode()) return;   // direct mode has no constants
        const uint8_t outCh = self->correction_.outChannels;
        const auto count = static_cast<nrOfLightsType>(rows);
        if (self->slotBytes() == 1) self->prefillShiftRows<uint8_t>(outCh, dst, 0, count);
        else                        self->prefillShiftRows<uint16_t>(outCh, dst, 0, count);
    }
    uint8_t*  busBuffer(uint8_t i)              { return platform::moonI80Ws2812Buffer(bus_, i); }
    size_t    busCapacity() const               { return platform::moonI80Ws2812BufferCapacity(bus_); }
    bool      busTransmit(uint8_t i, size_t bytes) { return platform::moonI80Ws2812Transmit(bus_, i, bytes); }
    bool      busWait(uint8_t i, uint32_t ms)   { return platform::moonI80Ws2812Wait(bus_, i, ms); }
    uint32_t  busLastTransmitUs() const         { return platform::moonI80Ws2812LastTransmitUs(bus_); }
    void      busDeinit()                       { platform::moonI80Ws2812Deinit(bus_); }

    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) {
        return platform::moonI80Ws2812Loopback(this->busPinList(), this->busPinCount(),
                                               static_cast<uint16_t>(clockPin),
                                               static_cast<uint16_t>(loopbackRxPin),
                                               frame, frameBytes, dataBytes, rowBits,
                                               this->busClockMultiplier());
    }

    /// WR is part of the bus identity, so a change to it rebuilds the bus — not just a data-pin edit.
    void recordBusPins() { lastClockPin_ = clockPin; }
    bool extraBusPinsCurrent() const { return lastClockPin_ == clockPin; }

private:
    platform::MoonI80Ws2812Handle bus_;
    int8_t lastClockPin_ = -1;
    // TEMP DIAGNOSTIC: ring counters (refreshed in refreshBusKpi via tick1s). Sized for the worst case
    // "sl16/bf16 dn99999 de0 enc2456/2456us gap2460/2460us" (~56) — a truncated diagnostic is a lying one.
    char ringDbgStr_[160] = "—";
};

} // namespace mm
