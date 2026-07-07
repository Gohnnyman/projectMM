#pragma once

#include "light/drivers/ParallelLedDriver.h"   // shared CRTP body
#include "platform/platform.h"

#include <cstdint>
#include <cstring>  // std::strcmp

namespace mm {

/// Output driver: parallel WS2812B over the ESP32-S3 [LCD_CAM i80 bus](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/lcd/index.html).
/// The S3's scale path — RMT gives it 4 channels, this gives 8 lanes for the wall time of one. The
/// shared body (slicing, the single-shot DMA transfer, the fused encode, the loopback self-test)
/// lives in ParallelLedDriver; this class adds only the i80-specific pieces:
///  - The sacrificial WR (pixel clock) + DC GPIOs the peripheral mandates even though WS2812 ignores
///    both, and the "exactly 8 pins" rule (the i80 layer rejects a partial bus).
///  - **The 3-slot-per-bit wire contract:** each WS2812 bit becomes three bus slots at 2.67 MHz
///    (slot = 375 ns): all-active-lanes HIGH, the data bits, then all LOW — so a `1` is HIGH 750 ns
///    and a `0` 375 ns, approximating RMT's 700/350. The slot is deliberately NOT the lineage's
///    ~416 ns: newer WS2812B revisions spec T0H max ≈ 380 ns, and a longer `0` on a direct 3.3 V line
///    gets misread as `1` (the strip washes white). One 8-bit bus word per slot (bus bit L = the
///    L-th pin); unequal strands idle LOW once exhausted. Slot layout: LcdSlots.h.
///  - **Memory:** one internal-RAM DMA frame buffer (PSRAM deliberately unused — the peripheral
///    streams from internal SRAM): `longest lane × channels × 24 + latch` bytes, ~72 B/RGB light
///    (1000 lights across 8 lanes ≈ 9 KB; the boundary is ~1500+ lights on a SINGLE lane ≈ 110 KB).
///  - The platform::lcdWs2812* calls (ESP-IDF's esp_lcd i80 bus + GDMA).
///
/// Prior art: Adafruit's LCD_CAM discovery, hpwit's I2SClockless lineage, FastLED's S3 driver —
/// architecture studied, never copied.
class LcdLedDriver : public ParallelLedDriver<LcdLedDriver> {
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
    /// (esp_lcd_panel_io_i80.c: `wr_gpio_num >= 0 && dc_gpio_num >= 0`), yet the WS2812 strands
    /// ignore both — they are peripheral-fixed, not user-strand wiring, so a sensible overridable
    /// default cannot do harm (same class as the chip-fixed Ethernet pins). The data pins gate
    /// startup, so the bus stays idle until the user sets them regardless. (Dropping WR/DC entirely
    /// needs a direct-LCD_CAM driver that bypasses esp_lcd, hpwit-style — backlogged, not this
    /// increment.)
    int8_t clockPin = 10;
    int8_t dcPin = 11;

    // --- CRTP hooks the base calls (all non-virtual; no vtable) ---

    /// The number of i80 lanes this chip provides (0 = not this chip); the base's
    /// inert-on-wrong-chip guards key off it.
    static constexpr uint8_t lanesAvailable() { return platform::lcdLanes; }
    static constexpr bool kExactLaneCount = true;   // i80 needs all 8 data lanes
    static constexpr const char* kInitFailMsg = "LCD init failed — check pins / memory";

    /// Bind the i80-specific bus controls: the sacrificial WR (clockPin) and DC pins
    /// the peripheral mandates.
    void addBusControls() {
        controls_.addPin("clockPin", clockPin);
        controls_.addPin("dcPin", dcPin);
    }
    /// A clockPin or dcPin change triggers a bus rebuild via the onBuildState sweep.
    bool busControlTriggersBuild(const char* name) const {
        return std::strcmp(name, "clockPin") == 0 || std::strcmp(name, "dcPin") == 0;
    }

    /// Create the i80 bus + its DMA buffer sized for `frameBytes` on the current data
    /// lanes plus the WR/DC pins; returns whether init succeeded.
    bool busInit(size_t frameBytes) {
        return platform::lcdWs2812Init(lcd_, laneList_, laneCount_,
                                       static_cast<uint16_t>(clockPin),
                                       static_cast<uint16_t>(dcPin), frameBytes);
    }
    /// The bus's DMA buffer the base encodes into.
    uint8_t* busBuffer()                 { return platform::lcdWs2812Buffer(lcd_); }
    /// The DMA buffer's byte capacity (fixed at bus creation).
    size_t   busCapacity() const         { return platform::lcdWs2812BufferCapacity(lcd_); }
    /// Kick off the autonomous transfer of the first `bytes` of the DMA buffer;
    /// returns whether it started.
    bool     busTransmit(size_t bytes)   { return platform::lcdWs2812Transmit(lcd_, bytes); }
    /// Block up to `ms` for the in-flight transfer to complete.
    void     busWait(uint32_t ms)        { platform::lcdWs2812Wait(lcd_, ms); }
    /// Tear down the i80 bus and its DMA buffer.
    void     busDeinit()                 { platform::lcdWs2812Deinit(lcd_); }

    /// Run the loopback self-test. The i80 layer requires all 8 data GPIOs valid, so
    /// a 1-lane private bus is impossible; the loopback builds the full-width bus and
    /// carries the pattern on lane 0. Passes the WR/DC pins the init needs.
    platform::RmtLoopbackResult busLoopback(const uint8_t* frame, size_t frameBytes,
                                            size_t dataBytes, uint8_t rowBits) {
        return platform::lcdWs2812Loopback(laneList_, laneCount_,
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
    platform::LcdWs2812Handle lcd_;
    int8_t lastClockPin_ = -1;
    int8_t lastDcPin_ = -1;
};

} // namespace mm
