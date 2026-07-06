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
/// **Not auto-wired.** Factory-registered like AudioModule, so a board with an I2C bus adds it
/// through the installer device catalog (its `sda`/`scl` controls carrying that board's bus
/// pins) or the user adds it from the UI. The pins default to GPIO21/22, the Arduino-ESP32
/// core's conventional I2C pair, so the control pre-fills a sensible starting point on a classic
/// ESP32 (the pins route through the GPIO matrix, so they're a convention, not fixed hardware);
/// a board with a fixed bus overrides them in its catalog entry (such as the S31's `sda:51,
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
    /// A diagnostic, like FirmwareUpdateModule / DevicesModule — keeps its
    /// controls and the scan action available regardless of the `enabled` toggle.
    bool respectsEnabled() const override { return false; }

    ModuleRole role() const override { return ModuleRole::Peripheral; }

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
    // Default to GPIO21/22 — the Arduino-ESP32 core's default I2C pair, the pins a
    // contributor expects to try first on a classic ESP32. They're a *convention*,
    // not fixed hardware (I2C routes through the GPIO matrix to any pins), so they
    // pre-fill the control as a sensible starting point the user edits. A board
    // with a FIXED bus (the S31 codec, the P4) overrides them via its catalog entry.
    int8_t sda_ = 21;
    int8_t scl_ = 22;
    char resultStr_[64] = "";    // space-separated hex addresses, e.g. "0x18 0x3c"
    char statusBuf_[40] = "idle";

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
            // AudioModule is active) — say so instead of a misleading "0 found".
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

        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s found",
                      static_cast<unsigned>(n), n == 1 ? "" : "s");
        setStatus(statusBuf_);
        markDirty();   // push the updated result + status to the UI
    }

    static constexpr size_t kMaxAddrs = 16;  // plenty for one bus
};

} // namespace mm
