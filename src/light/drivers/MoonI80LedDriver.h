#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"

namespace mm {

/// Output driver: parallel 8-or-16-lane WS2812B on the **LCD_CAM** peripheral, driven by **our own
/// DMA code** instead of ESP-IDF's `esp_lcd` component. Same peripheral, same wire contract, same
/// pins as [I80LedDriver](I80LedDriver.md) — the difference is underneath (who programs the DMA), plus
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
/// **Both drivers ship, and that is deliberate.** `I80LedDriver` is the **reference**: correct,
/// memory-capped, and the thing this one is measured against. This is the **challenger**. Because both
/// are registered module types, switching between them is a swap in the UI — the A/B needs no reflash,
/// on the same board, on the same effect. The reference is retired only if and when the challenger
/// demonstrably beats it.
///
/// Everything above the DMA is inherited unchanged from ParallelLedDriver: the slicing, the fused
/// 3-slot encode ([ParallelSlots.h](ParallelSlots.md)), the async double-buffer, the 74HCT595
/// shift-register expander, the loopback self-test, the `wireUs` KPI, and the dead-frame guard. This
/// class adds only what is i80-specific — the WR pin (a '595 pin here, not an i80 tax: see clockPin)
/// and the platform forwards — which is why it is nearly all one-liners.
///
/// LCD_CAM only (ESP32-S3 / -P4). The classic ESP32's i80 is the I2S peripheral, a different backend
/// entirely, so this driver is not offered there.
class MoonI80LedDriver : public ParallelLedDriver<MoonI80LedDriver> {
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
    /// The expander needs a backend that can stream its ×8 frame; LCD_CAM is it, and this driver is
    /// LCD_CAM-only, so the answer is simply "wherever this driver runs at all".
    static constexpr bool kSupportsShiftRegister = platform::lcdLanes > 0;

    /// The base pads spare bus lanes with this GPIO. Unrouted lanes cost nothing here, so the value is
    /// only ever *used* in shift mode — where WR is a real pad and the padding is genuinely inert.
    uint16_t clockPinForBus() const { return static_cast<uint16_t>(clockPin); }

    /// WR is a '595 pin here, so the control follows the expander toggle: bound always (a saved value
    /// survives a round-trip through direct mode) but shown only when a shift register can read it.
    void addBusControls() {
        controls_.addPin("clockPin", clockPin);
        controls_.setHidden(controls_.count() - 1, !shiftMode());
    }
    bool busControlTriggersBuild(const char* name) const {
        return std::strcmp(name, "clockPin") == 0;
    }

    /// WR only reaches a pad in shift mode, so it can only COLLIDE in shift mode. In direct mode the
    /// signal never leaves the peripheral, so `clockPin` naming a strand's GPIO is harmless — and
    /// rejecting it would forbid a perfectly good config for the sake of a signal nobody reads.
    const char* validateBusFatal() const {
        if (shiftMode() && latchPin >= 0 && latchPin == clockPin)
            return "latchPin is on clockPin (WR) — the latch needs its own GPIO";
        return nullptr;
    }
    /// A data lane sharing WR's GPIO is silent corruption — the matrix routes both signals to the one
    /// pad and that strand emits the shift clock instead of pixel data. Only possible in shift mode.
    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const {
        if (!shiftMode()) return nullptr;
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
};

} // namespace mm
