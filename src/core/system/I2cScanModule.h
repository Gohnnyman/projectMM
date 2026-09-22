#pragma once

#include "core/module/MoonModule.h"
#include "platform/platform.h"  // i2cScan

#include <cstdint>
#include <cstdio>
#include <cstring>  // strcmp

namespace mm {

/// Scan an I2C bus and report which addresses answer, the standard detect operation in the UI.
///
/// It is the bring-up tool for any I2C peripheral: set the pins, press scan, read what is there.
/// A fixed System module, wired by code, and passive until the button is pressed.
/// @card I2cScanModule.png
///
/// @moreinfo
///
/// ## How it probes
///
/// The platform opens a temporary bus, probes every 7-bit address, and tears it down.
/// Its own short-lived bus means a scan never conflicts with one another driver owns.
/// A target without a bus reports as unavailable rather than as an empty scan.
///
/// ## The pins
///
/// They default to unused, so a board with no I2C device claims no GPIO for the scanner.
/// A board with a fixed bus sets them from its catalog entry.
/// For an ad-hoc scan the user types them, the conventional pair being 21 and 22.
///
/// Prior art: the scan mirrors MoonLight's, and the probe range follows Linux i2c-tools.
class I2cScanModule : public MoonModule {
public:
    // It respects the enabled toggle, so switching it off frees its pins for another module.

    /// Declare the two bus pins, the scan button and the result readout.
    void defineControls() override {
        controls_.addPin("sda", sda_);
        controls_.addPin("scl", scl_);
        controls_.addButton("scan");
        controls_.addReadOnly("result", resultStr_, sizeof(resultStr_));
        MoonModule::defineControls();
    }

    /// Run a scan when the button is pressed.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "scan") == 0) scan();
    }

private:
    // Unused by default, so an idle scanner claims no GPIO in the pin map.
    int8_t sda_ = -1;            ///< the data pin, or -1 for unused
    int8_t scl_ = -1;            ///< the clock pin, or -1 for unused
    char resultStr_[64] = "";    ///< the addresses found, space-separated hex
    // It backs the "N devices found" status, which must outlive the call that sets it.
    char statusBuf_[20] = "idle";

    /// Probe the bus and report what answered, or why it could not.
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
            // Held by another driver, so say so rather than report a misleading zero.
            resultStr_[0] = '\0';
            setStatus("bus in use — free the I2C driver, then scan", Severity::Warning);
            markDirty();
            return;
        }
        // Truncate cleanly if the buffer fills, which one bus rarely manages.
        int pos = 0;
        for (size_t i = 0; i < n; i++) {
            const int w = std::snprintf(resultStr_ + pos, sizeof(resultStr_) - pos,
                                        "%s0x%02x", i ? " " : "", found[i]);
            if (w <= 0 || pos + w >= static_cast<int>(sizeof(resultStr_))) break;
            pos += w;
        }
        if (n == 0) resultStr_[0] = '\0';

        // Cast so the compiler can see the count fits, since format truncation is an error here.
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u device%s found",
                      static_cast<unsigned>(static_cast<uint8_t>(n)), n == 1 ? "" : "s");
        setStatus(statusBuf_);
        markDirty();   // push the updated result + status to the UI
    }

    static constexpr size_t kMaxAddrs = 16;  ///< plenty for one bus
};

} // namespace mm
