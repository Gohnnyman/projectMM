#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"


namespace mm {

/// Output driver: parallel WS2812B over the ESP32-P4 Parlio (Parallel IO) TX peripheral — the P4's
/// scale path, sibling of I80LedDriver. The shared body (slicing, encode, single-shot DMA, loopback)
/// lives in ParallelLedDriver; Parlio is the SIMPLER peripheral, so this class adds LESS than the
/// i80 driver:
///  - NO clockPin/dcPin: Parlio generates the pixel clock itself (kClockHz), so there are no
///    sacrificial WR/DC lines (addBusControls is empty).
///  - kExactLaneCount = false: i80 rejects a partial bus; Parlio runs on 1..8 lanes — whatever
///    `pins` names.
///
/// Prior art: the ESP32-P4 Parlio peripheral, the hpwit/FastLED parallel-WS2812 lineage —
/// architecture studied, never copied.
class ParlioLedDriver : public ParallelLedDriver<ParlioLedDriver> {
public:
    // All controls default to UNSET — pins="", ledsPerPin="" (= all lights on the
    // first lane, even-split with one lane), loopbackRxPin=-1 — so no constructor is
    // needed (the base default-initialises them). Pins/loopback are unset because the
    // strand is user-soldered: a hard-coded pin would guess the user's wiring and
    // could drive a pin committed elsewhere ("default only when it cannot do harm",
    // see lessons.md). The P4-NANO bench uses pins "20,21,22,23,24,25,26,27",
    // loopbackRxPin 33 (clear of the NANO's strapping 34-38, Ethernet RMII
    // 28-31/49-52, C6 SDIO 14-19/54, I2C 7-8 — clear GPIOs are 20-27, 32-33, 39-48).

    // --- CRTP hooks the base calls (all non-virtual; no vtable) ---

    /// The number of Parlio lanes this chip provides (0 = not this chip); the base's
    /// inert-on-wrong-chip guards key off it.
    static constexpr uint8_t lanesAvailable() { return platform::parlioLanes; }
    static constexpr bool kExactLaneCount = false;   // 1..16 lanes all valid
    // Parlio builds a 1-lane private unit for the loopback, so the loopback frame stays 8-bit
    // regardless of the operational bus width (unlike i80, which needs a full-width bus).
    static constexpr bool kLoopbackFullWidth = false;
    static constexpr const char* kInitFailMsg = "Parlio init failed — check pins / memory";

    // The WS2812 slot rate (375 ns @ 2.67 MHz) — identical to the LCD driver's;
    // the P4 Parlio's 160 MHz PLL clock divides to it exactly (/60).
    static constexpr uint32_t kClockHz = 2'666'666;

    /// No bus controls: Parlio has no sacrificial clock/DC pins (the bus rebuilds on
    /// a data-pin change alone).
    void addBusControls() {}
    /// No extra bus controls, so none can trigger a rebuild.
    bool busControlTriggersBuild(const char*) const { return false; }
    /// No extra pins to record (Parlio has no WR/DC).
    void recordBusPins() {}
    /// No extra pins to track, so they are always current.
    bool extraBusPinsCurrent() const { return true; }

    /// Create the Parlio bus + its DMA buffer(s) sized for `frameBytes` on the current lanes, driving
    /// the pixel clock at kClockHz; `wantSecondBuffer` requests the async double-buffer's second frame
    /// buffer (allocated only if it fits). Returns whether init succeeded.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) {
        return platform::parlioWs2812Init(parlio_, laneList_, laneCount_,
                                          kClockHz, frameBytes, wantSecondBuffer);
    }
    /// DMA buffer `i` (0/1) the base encodes into; buffer 1 is null when the second
    /// buffer didn't fit (single-buffer mode). Both are the same size (busCapacity).
    uint8_t* busBuffer(uint8_t i)        { return platform::parlioWs2812Buffer(parlio_, i); }
    /// The per-buffer byte capacity (fixed at bus creation; both buffers equal).
    size_t   busCapacity() const         { return platform::parlioWs2812BufferCapacity(parlio_); }
    /// Kick off the autonomous transfer of the first `bytes` of DMA buffer `i`;
    /// returns whether it started.
    bool  busTransmit(uint8_t i, size_t bytes) { return platform::parlioWs2812Transmit(parlio_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    void  busWait(uint8_t i, uint32_t ms)      { platform::parlioWs2812Wait(parlio_, i, ms); }
    /// The most recent DMA transfer's wire time (µs) — the WS2812 output floor.
    uint32_t busLastTransmitUs() const         { return platform::parlioWs2812LastTransmitUs(parlio_); }
    /// Tear down the Parlio bus and its DMA buffer.
    void     busDeinit()                 { platform::parlioWs2812Deinit(parlio_); }

    /// Run the loopback self-test. Parlio runs on a single lane, so the loopback
    /// builds its own private 1-lane unit on lane 0 (no i80 full-bus workaround
    /// needed; no WR/DC to pass).
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) {
        return platform::parlioWs2812Loopback(laneList_, laneCount_,
                                              static_cast<uint16_t>(loopbackRxPin),
                                              frame, frameBytes, dataBytes, rowBits);
    }

private:
    platform::ParlioWs2812Handle parlio_;
};

} // namespace mm
