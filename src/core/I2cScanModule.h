#pragma once

#include "core/MoonModule.h"
#include "platform/platform.h"  // i2cScan

#include <cstdint>
#include <cstdio>
#include <cstring>  // strcmp

namespace mm {

/// A core, domain-neutral diagnostic that scans an I2C bus and reports which device
/// addresses ACK — the standard `i2cdetect` operation, surfaced in the UI. It is the bring-up
/// tool for any I2C peripheral (an audio codec, a sensor, a port expander): set the bus pins,
/// press scan, read off the addresses present, confirming wiring before a driver tries to talk
/// to the device. Pressing `scan` probes the bus on the `sda` / `scl` pins and lists the 7-bit
/// addresses found in `result`.
///
/// Same shape as DevicesModule (a momentary `scan` button → results), one rung simpler: the bus
/// is local (no persisted list, no live age-out), so a single read-only `result` string suffices
/// instead of a ListSource. Scan state ("N devices found", "set sda + scl pins first") reports
/// through the standard `setStatus()` channel.
///
/// **Fixed System module.** Wired by code as a child of SystemModule in `main.cpp` — a
/// hardware-inspection tool that is always available on every board, not user-added (you don't
/// add a bus scanner, it's part of the device's bring-up toolkit). It stays passive until the
/// user presses scan: no bus is opened and no pins are driven at boot. The pins default to
/// GPIO21/22, the Arduino-ESP32 core's conventional I2C pair, so the control pre-fills a sensible
/// starting point on a classic ESP32 (the pins route through the GPIO matrix, so they're a
/// convention, not fixed hardware); a board with a fixed bus overrides them as a control-value
/// default in its catalog entry (such as the S31's `sda:51,
/// scl:50`).
///
/// **How it works:** the probe is `platform::i2cScan(sda, scl, out, maxOut)` (declared in
/// platform.h), a self-contained seam that opens a temporary I2C master bus on the given pins,
/// probes every 7-bit address (`0x01`–`0x77`), writes the ACKing addresses into the caller's
/// buffer, and tears the bus down. Opening its own short-lived bus (rather than borrowing one)
/// means the scan never conflicts with a bus another driver owns — the ES8311 codec on the
/// ESP32-S31 holds its own bus, and the scan probes the same pins independently between codec
/// operations. On a target without an I2C bus (an I2C-less ESP32, or desktop) the seam returns
/// `kI2cBusUnavailable`, so the scan reports "bus unavailable" rather than a misleading "0
/// devices found" — the 0 is reserved for a real scan where nothing ACKed.
///
/// **Prior art:** the bus-scan-as-a-feature mirrors MoonLight's I2C scan diagnostic; the seam
/// name and probe range follow the Linux `i2c-tools` `i2cdetect` convention
/// (https://manpages.debian.org/i2c-tools/i2cdetect.8.en.html).
/// @card I2cScanModule.png
class I2cScanModule : public MoonModule {
public:
    /// Respects the `enabled` toggle (the default): disabling it releases its bus pins in the pin
    /// ownership map (and re-claims them on enable), so a user can free GPIO21/22 for another module
    /// by switching the scanner off. Safe because I2cScan has no loop — it acts only on the `scan`
    /// button (onUpdate), which still works whether enabled or not; disabling only stops it *claiming*
    /// its pins, which is exactly what "off" should mean for a pin-holding diagnostic.

    void onBuildControls() override {
        controls_.addPin("sda", sda_);
        controls_.addPin("scl", scl_);
        controls_.addButton("scan");
        controls_.addReadOnly("result", resultStr_, sizeof(resultStr_));
        MoonModule::onBuildControls();
    }

    void onUpdate(const char* controlName) override {
        if (std::strcmp(controlName, "scan") == 0) scan();
    }

private:
    // Default to -1 (unused). I2cScan is present on every board as the bring-up tool, but a
    // board without an I2C device shouldn't claim any GPIO for it — the pin ownership map reads
    // a Pin value of -1 as "unclaimed" (the standard sentinel), so an idle scanner squats on
    // nothing. A board WITH a bus sets the real pins via its catalog entry (the S31 codec 51/50,
    // the P4 boards 7/8); for an ad-hoc scan on a bare board the user types the pins (the classic
    // Arduino-ESP32 pair is 21/22 — I2C routes through the GPIO matrix to any pins). The `scan`
    // button reports "set sda + scl pins first" while they're -1.
    int8_t sda_ = -1;
    int8_t scl_ = -1;
    char resultStr_[64] = "";    // space-separated hex addresses, e.g. "0x18 0x3c"
    // Backs the "N devices found" status only (setStatus stores the pointer, so it can't be a
    // local — it must outlive scan()). Sized for the longest such string: "255 devices found" =
    // 17 chars + NUL. The other statuses are string literals (static lifetime, no buffer).
    char statusBuf_[20] = "idle";

    void scan() {
        if (sda_ < 0 || scl_ < 0) {
            resultStr_[0] = '\0';
            setStatus("set sda + scl pins first", Severity::Warning);
            return;
        }
        uint8_t found[kMaxAddrs];
        const size_t n = platform::i2cScan(static_cast<uint16_t>(sda_),
                                           static_cast<uint16_t>(scl_),
                                           found, kMaxAddrs);
        if (n == platform::kI2cBusUnavailable) {
            // The bus is held by another driver (e.g. the ES8311 codec while
            // AudioService is active) — say so instead of a misleading "0 found".
            resultStr_[0] = '\0';
            setStatus("bus in use — free the I2C driver, then scan", Severity::Warning);
            markDirty();
            return;
        }
        // Build the "0x18 0x3c …" result string, truncating cleanly if the buffer
        // fills (more devices than fit is unusual on one bus, but stay bounded).
        int pos = 0;
        for (size_t i = 0; i < n; i++) {
            const int w = std::snprintf(resultStr_ + pos, sizeof(resultStr_) - pos,
                                        "%s0x%02x", i ? " " : "", found[i]);
            if (w <= 0 || pos + w >= static_cast<int>(sizeof(resultStr_))) break;
            pos += w;
        }
        if (n == 0) resultStr_[0] = '\0';

        // n is bounded by kMaxAddrs (a 7-bit bus has at most 128 addresses), but the compiler can't
        // see that through size_t — cast to uint8_t so it knows the count is <= 255 (max 3 digits),
        // which keeps "N devices found" inside statusBuf_[20] (format-truncation is an error here).
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s found",
                      static_cast<unsigned>(static_cast<uint8_t>(n)), n == 1 ? "" : "s");
        setStatus(statusBuf_);
        markDirty();   // push the updated result + status to the UI
    }

    static constexpr size_t kMaxAddrs = 16;  // plenty for one bus
};

} // namespace mm
