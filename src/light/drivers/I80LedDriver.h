#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"


namespace mm {

/// Output driver: parallel 8-or-16-lane WS2812B over the ESP-IDF [esp_lcd i80 bus](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html)
/// — the parallel scale path on **all three i80-capable ESP32 families**. RMT gives a chip 4-8
/// channels; this gives 8-16 lanes for the wall time of one. The magic is that ESP-IDF exposes ONE
/// public i80 API (`esp_i80_new_i80_bus` / `esp_i80_panel_io_tx_color`) and routes it to whichever
/// peripheral the silicon has — so this single driver serves every chip:
///  - **ESP32-S3 / -P4:** backed by the dedicated **LCD_CAM** peripheral.
///  - **classic ESP32:** backed by the **I2S peripheral in i80/LCD mode** (the classic has no LCD_CAM;
///    I2S-i80 is its only >8-lane route). IDF's own CMake picks the backend by chip; the two are
///    mutually exclusive per silicon, so `lanesAvailable()` reads whichever lane-count constant is
///    non-zero. Named for the **i80 bus** (the shared API), not a peripheral, since it isn't one
///    peripheral — same reason its siblings are named `Rmt`/`Parlio` (their APIs).
///
/// The shared body (slicing, the whole-frame async double-buffer DMA, the fused encode, the loopback
/// self-test, the `wireUs` KPI) lives in ParallelLedDriver; this class adds only the i80-specific pieces:
///  - The sacrificial WR (pixel clock) + DC GPIOs the i80 bus mandates even though WS2812 ignores
///    both, and the "exactly 8 or 16 pins" rule (the i80 layer rejects a partial bus). A sub-16 board
///    parks unused lanes + WR/DC on one spare GPIO (the ghost-pin trick).
///  - **The 3-slot-per-bit wire contract:** each WS2812 bit becomes three bus slots at 2.67 MHz
///    (slot = 375 ns): all-active-lanes HIGH, the data bits, then all LOW — so a `1` is HIGH 750 ns
///    and a `0` 375 ns, approximating RMT's 700/350. The slot is deliberately NOT the lineage's
///    ~416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and a longer `0` on a direct 3.3 V line
///    gets misread as `1` (the strip washes white). One bus word per slot (bus bit L = the L-th pin);
///    unequal strands idle LOW once exhausted. Slot layout: ParallelSlots.h.
///  - Both backends do **whole-frame chained DMA** (autonomous, CPU out of the timing loop), so the
///    classic I2S path is WiFi-underrun-immune by construction — it does NOT need the ISR-refilled
///    ring / large `nbDmaBuffer` cushion the raw-register I2S-clockless lineage requires.
///  - The platform::i80Ws2812* calls (ESP-IDF's esp_lcd i80 bus + GDMA).
///
/// Prior art: Adafruit's LCD_CAM discovery, hpwit's I2SClockless lineage (classic-ESP32 I2S parallel),
/// FastLED's S3 driver — architecture studied, never copied. We build on IDF's maintained esp_lcd i80
/// abstraction rather than tracing the raw-register I2S driver (*Industry standards, our own code*).
class I80LedDriver : public ParallelLedDriver<I80LedDriver> {
public:
    // Data pins + loopback pin default to UNSET: they are user-soldered (the strand
    // runs to whatever GPIOs the user wired), so a hard-coded default would be a
    // guess that could drive a pin the user committed elsewhere — empty until set,
    // the driver idles meanwhile (the "default only when it cannot do harm" rule;
    // see lessons.md). The ESP32-S3 N16R8 Dev bench wiring is pins "1,2,4,5,6,7,8,9",
    // loopbackRxPin 12 (kept clear of the octal-PSRAM pins 26-37, USB 19/20, and
    // strapping pins) — set those again to reproduce the bench. (Base declares
    // pins="" and loopbackRxPin=0, so the empty default needs no code here.)

    /// WR (pixel clock) and DC: the IDF i80 bus *requires* both on real GPIOs
    /// (esp_i80_panel_io_i80.c: `wr_gpio_num >= 0 && dc_gpio_num >= 0`), yet the WS2812 strands
    /// ignore both — they are peripheral-fixed, not user-strand wiring, so a sensible overridable
    /// default cannot do harm (same class as the chip-fixed Ethernet pins). The data pins gate
    /// startup, so the bus stays idle until the user sets them regardless. (Dropping WR/DC entirely
    /// needs a direct-LCD_CAM driver that bypasses esp_lcd, hpwit-style — backlogged, not this
    /// increment.)
    int8_t clockPin = 10;
    int8_t dcPin = 11;

    // --- CRTP hooks the base calls (all non-virtual; no vtable) ---

    /// The number of i80 lanes this chip provides (0 = no i80 bus on this chip); the base's
    /// inert-on-wrong-chip guards key off it. Reads whichever backend the silicon has —
    /// `lcdLanes` (LCD_CAM, S3/P4) or `i2sLanes` (I2S-i80, classic ESP32) — which are mutually
    /// exclusive per chip (at most one is non-zero), so the sum picks the right one.
    static constexpr uint8_t lanesAvailable() { return platform::lcdLanes + platform::i2sLanes; }
    static constexpr bool kExactLaneCount = true;   // i80 needs exactly 8 or 16 data lanes
    // The i80 loopback can't build a 1-lane private bus, so it rebuilds the FULL-WIDTH bus and
    // carries the pattern on lane 0 — the loopback frame must be encoded at the operational bus
    // width (16-bit for a 16-lane driver) to match. (Parlio can do a 1-lane unit, so it sets false.)
    static constexpr bool kLoopbackFullWidth = true;
    static constexpr const char* kInitFailMsg = "i80 bus init failed — check pins / memory";

    /// Bind the i80-specific bus controls: the sacrificial WR (clockPin) and DC pins
    /// the peripheral mandates.
    void addBusControls() {
        controls_.addPin("clockPin", clockPin);
        controls_.addPin("dcPin", dcPin);
    }
    /// A clockPin or dcPin change triggers a bus rebuild via the prepare sweep.
    bool busControlTriggersBuild(const char* name) const {
        return std::strcmp(name, "clockPin") == 0 || std::strcmp(name, "dcPin") == 0;
    }

    /// Reject a data lane that collides with the WR (clockPin) or DC pin. The i80
    /// peripheral routes a distinct output signal to each of the 8 data lanes plus
    /// WR + DC via the GPIO matrix; IDF does NOT check that they differ, so a data
    /// pin equal to clockPin/dcPin gets two signals on one GPIO and that lane emits
    /// the clock/DC waveform instead of pixel data (silent corruption — the strip on
    /// that lane shows garbage). Fail loud + idle instead, same shape as the other
    /// parse errors. (Hides the base's no-op default; the base calls this via CRTP.)
    // Returns a WARNING string (not an error) if a data lane sits on clockPin (WR) or
    // dcPin: that lane emits the bus-control waveform instead of pixel data. It's a
    // warning because on a board that wires all 8/16 lanes but drives fewer strands,
    // parking WR/DC on an unused data pin is a valid choice — only a lane driving a
    // real strand shows garbage. The base routes this to setConfigWarn; the driver
    // keeps running. null when the WR/DC pins are clear of the data set.
    /// FATAL bus-pin check → routed to the ERROR path (idles the driver), unlike validateBusPins'
    /// per-lane WARNINGS. WR and DC on the SAME GPIO breaks the i80 bus outright (it needs two
    /// distinct control lines — the bus won't init), so it can't be a warn-and-run like a data-lane
    /// collision (which only corrupts that one lane). null = no fatal condition. (CRTP hook; the
    /// base's default returns null, Parlio has no WR/DC and keeps the default.)
    const char* validateBusFatal() const {
        if (clockPin >= 0 && clockPin == dcPin)
            return "clockPin (WR) and dcPin are the same GPIO — they must differ";
        return nullptr;
    }

    const char* validateBusPins(const uint16_t* lanes, uint8_t n) const {
        for (uint8_t i = 0; i < n; i++) {
            // clockPin/dcPin are int8_t (-1 = unset); only a real GPIO can collide.
            if (clockPin >= 0 && lanes[i] == static_cast<uint16_t>(clockPin))
                return "a LED pin is on clockPin (WR) — that lane carries the clock, not pixels";
            if (dcPin >= 0 && lanes[i] == static_cast<uint16_t>(dcPin))
                return "a LED pin is on dcPin — that lane carries DC, not pixels";
        }
        return nullptr;
    }

    /// Create the i80 bus + its DMA buffer(s) sized for `frameBytes` on the current data lanes plus
    /// the WR/DC pins; `wantSecondBuffer` requests the async double-buffer's second frame buffer
    /// (allocated only if it fits — else single-buffer). Returns whether init succeeded.
    bool busInit(size_t frameBytes, bool wantSecondBuffer) {
        return platform::i80Ws2812Init(i80_, laneList_, laneCount_,
                                       static_cast<uint16_t>(clockPin),
                                       static_cast<uint16_t>(dcPin), frameBytes, wantSecondBuffer);
    }
    /// DMA buffer `i` (0/1) the base encodes into; buffer 1 is null when the second
    /// buffer didn't fit (single-buffer mode). Both are the same size (busCapacity).
    uint8_t* busBuffer(uint8_t i)        { return platform::i80Ws2812Buffer(i80_, i); }
    /// The per-buffer byte capacity (fixed at bus creation; both buffers equal).
    size_t   busCapacity() const         { return platform::i80Ws2812BufferCapacity(i80_); }
    /// Kick off the autonomous transfer of the first `bytes` of DMA buffer `i`;
    /// returns whether it started.
    bool  busTransmit(uint8_t i, size_t bytes) { return platform::i80Ws2812Transmit(i80_, i, bytes); }
    /// Block up to `ms` for buffer `i`'s in-flight transfer to complete.
    void  busWait(uint8_t i, uint32_t ms)      { platform::i80Ws2812Wait(i80_, i, ms); }
    /// The most recent DMA transfer's wire time (µs) — the WS2812 output floor.
    uint32_t busLastTransmitUs() const         { return platform::i80Ws2812LastTransmitUs(i80_); }
    /// Tear down the i80 bus and its DMA buffer.
    void     busDeinit()                 { platform::i80Ws2812Deinit(i80_); }

    /// Run the loopback self-test. The i80 layer requires all 8 data GPIOs valid, so
    /// a 1-lane private bus is impossible; the loopback builds the full-width bus and
    /// carries the pattern on lane 0. Passes the WR/DC pins the init needs.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) {
        return platform::i80Ws2812Loopback(laneList_, laneCount_,
                                           static_cast<uint16_t>(clockPin),
                                           static_cast<uint16_t>(dcPin),
                                           static_cast<uint16_t>(loopbackRxPin),
                                           frame, frameBytes, dataBytes, rowBits);
    }

    /// Store WR/DC alongside the data pins, so a clockPin/dcPin edit rebuilds the
    /// bus too (not just a data-pin change).
    void recordBusPins() { lastClockPin_ = clockPin; lastDcPin_ = dcPin; }
    /// Whether the live bus's WR/DC pins still match the current clockPin/dcPin (so
    /// the base can skip a rebuild).
    bool extraBusPinsCurrent() const {
        return lastClockPin_ == clockPin && lastDcPin_ == dcPin;
    }

private:
    platform::I80Ws2812Handle i80_;
    int8_t lastClockPin_ = -1;
    int8_t lastDcPin_ = -1;
};

} // namespace mm
