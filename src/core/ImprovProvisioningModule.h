#pragma once

#include "core/MoonModule.h"
#include "core/NetworkModule.h"
#include "core/SystemModule.h"
#include "core/HttpServerModule.h"
#include "core/build_info.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace mm {

/// Browser-driven WiFi provisioning over USB-serial, using the Improv-WiFi protocol
/// (https://www.improv-wifi.com/, from Nabu Casa). Improv is an open standard for handing a
/// device its WiFi credentials over a *local* link — USB serial here — at the moment it has no
/// network yet, solving the bootstrap chicken-and-egg: a freshly-flashed ESP32 isn't on your
/// WiFi, so you can't reach it over the network to give it the password; Improv carries that
/// first handoff over the cable the browser is already connected to from flashing. projectMM
/// extends this past credentials with the `APPLY_OP` vendor RPC ("Improv = REST over serial")
/// to push the whole device config (including the deviceModel identity, which is just one of
/// the config controls) over the same already-there link.
///
/// This module is the status surface: one read-only `provision_status` control that reports
/// `listening` / `received credentials` / `connecting` / `connected: <ssid>` / `error: reason`
/// / `not supported on this platform` (desktop). The actual protocol parsing + UART task live
/// in the platform layer (`mm::platform::improvProvisioningInit`, platform.h). tick1s() polls a
/// `ready` flag the platform task sets when credentials arrive, then calls
/// NetworkModule::setWifiCredentials, which writes through to the same buffers the AP-fallback
/// UI flow uses. A code-wired child of NetworkModule (`markWiredByCode()`) so persistence-apply
/// preserves it across reboots even on devices whose `Network.json` predates the addition. On
/// desktop (`platform::hasImprov == false`) the module exists for UI uniformity; the
/// listener-install is skipped.
///
/// **Transports.** The listener serves both UART0 (external USB-to-UART bridges) and the S3's
/// native USB-Serial-JTAG port, so a native-USB-only board provisions over that port directly.
/// If neither serial path is available, the AP-mode flow remains (the device boots a SoftAP at
/// `4.3.2.1`, join from a phone, enter credentials). Both transports speak the same Improv-WiFi
/// serial protocol — frames of `IMPROV` + version byte + type + length + payload + checksum
/// (full spec: https://www.improv-wifi.com/serial/; the constants are authored in
/// ImprovFrame.h).
///
/// **RPCs.** Four standard commands plus two vendor extensions: `GET_CURRENT_STATE` (authorized
/// / provisioned), `GET_DEVICE_INFO` (`[firmware, version, chipFamily, deviceName]`),
/// `GET_WIFI_NETWORKS` (synchronous scan, up to 10 SSIDs — rejected while STA is connected),
/// `WIFI_SETTINGS` (writes SSID + password, polls `wifiStaConnected()` up to 30 s, replies with
/// `http://ip/` or `ERROR_UNABLE_TO_CONNECT`). Vendor `SET_TX_POWER` (`0xFD`) persists + applies
/// a pre-association TX-power cap *before* any association attempt — the escape hatch for boards
/// whose LDO browns out at full TX power, since the cap must land before the first association or
/// the board fails auth at 20 dBm before it is ever online. Payload `[1][dBm]` (0–21; 0 lifts the
/// cap); an out-of-range value replies with `error 0x81`. Vendor `APPLY_OP` (`0xFC`) carries
/// ONE REST op as JSON, routed to HttpServerModule's apply-core — the exact same code the HTTP
/// handlers call — so a REST call over the network and an `APPLY_OP` over serial execute
/// identically. The web installer pushes a device-model's whole catalog config this way during
/// provisioning (the deviceModel identity is just one `set System.deviceModel` op), so the
/// defaults apply over the serial port the installer already owns during the flash — which is
/// what lets the HTTPS installer page configure an `http://` device a browser fetch can't reach
/// (mixed-content). Ops are idempotent; a long value chunks across frames into a reassembly
/// buffer, applied on the device's main loop when `last=1`; single-buffered (a new op errors with
/// `0x82` while the previous is unconsumed).
///
/// **Frame payload shape.** `[0xFC][seq][last][chunk]`. The installer paces ops open-loop (a
/// fixed delay sized to the worst-case consume window) rather than reading the ack back, so a lost
/// op is improbable rather than impossible; each op is idempotent, so a re-flash re-applies
/// cleanly.
///
/// **Parent-key caveat.** The serial `add` op names the parent `parent`, while HTTP
/// `POST /api/modules` names it `parent_id`; both feed `applyAddModule()` but parse different
/// keys, so an HTTP payload is NOT a drop-in APPLY_OP — rename `parent_id` → `parent`. The serial
/// key stays terse because every byte counts against the frame.
///
/// **clearChildren pre-pass.** The installer does a `clearChildren` pre-pass for every add-parent
/// (not only replaceChildren containers) so "apply device defaults" lands the same with or without
/// an erase: the device-side `add` is idempotent on module id, so a persisted same-name child
/// would otherwise survive and the re-add be skipped.
///
/// **Eth-only builds.** The serial listener runs on every ESP32 target, including Ethernet-only
/// builds: they compile in the vendor RPCs plus `GET_CURRENT_STATE` / `GET_DEVICE_INFO`, so the
/// installer pushes config over serial to an eth device exactly as to a WiFi one. The
/// WiFi-provisioning RPCs (`WIFI_SETTINGS`, `GET_WIFI_NETWORKS`) build only on WiFi targets; on
/// eth, `GET_CURRENT_STATE` reports "provisioned" + the URL from the Ethernet link.
/// `WIFI_SETTINGS` and `GET_WIFI_NETWORKS` are both rejected while `platform::wifiStaConnected()`
/// — the scan gate protects large installs, since `esp_wifi_scan_start` puts the radio into scan
/// mode for 2-5 s, dropping inbound ArtNet packets (a visible glitch on a 16K-LED rig).
///
/// **Prior art:** Improv-WiFi is the standard ESPHome / Home Assistant use for cross-firmware
/// WiFi provisioning (spec: https://www.improv-wifi.com/serial/; reference C++:
/// https://github.com/improv-wifi/sdk-cpp). projectMM-v1's `deploy/wifi.py` +
/// `deploy/flashfs.py --wifi` covered the same rack-provisioning use case by baking credentials
/// into a LittleFS partition image and re-flashing; Improv replaces that with live serial
/// provisioning (devices stay running, no flash mode required).
/// @card ImprovProvisioningModule.png
class ImprovProvisioningModule : public MoonModule {
public:
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    void setNetworkModule(NetworkModule* n) { networkModule_ = n; }
    /// For the APPLY_OP vendor RPC — the module routes a pushed REST op to the
    /// HttpServerModule's apply-core (the same code /api/modules + /api/control use).
    void setHttpServerModule(HttpServerModule* h) { httpServerModule_ = h; }

    /// Diagnostics keep ticking; matches FirmwareUpdateModule / SystemModule.
    bool respectsEnabled() const override { return false; }

    /// Apparatus, not swappable content — provisioning is a fixed device service.
    /// Not deletable (matches Board / Preview); can still be disabled.
    bool userEditable() const override { return false; }

    void setup() override {
        if constexpr (platform::hasImprov) {
            // Strings borrowed; platform task copies them into its own storage
            // on init, so locals going out of scope here is fine.
            const char* deviceName = systemModule_ ? systemModule_->deviceName() : "projectMM";
            platform::ImprovDeviceInfo info{
                deviceName,
                platform::chipModel(),
                kVersion,
            };
            platform::improvProvisioningInit(
                info,
                pendingSsid_, sizeof(pendingSsid_),
                pendingPassword_, sizeof(pendingPassword_),
                &pendingCredentials_,
                statusStr_, sizeof(statusStr_),
                &pendingTxPower_, &pendingTxPowerReady_,
                pendingOp_, sizeof(pendingOp_), &pendingOpReady_);
        } else {
            std::strncpy(statusStr_, "not supported on this platform", sizeof(statusStr_) - 1);
        }
    }

    void defineControls() override {
        controls_.addReadOnly("provision_status", statusStr_, sizeof(statusStr_));
    }

    /// Poll the SET_TX_POWER cap then the WiFi credentials the Improv task published (an
    /// acquire-load pairs each atomic flag with the platform task's release-store so the buffer
    /// writes are visible before we read them). The TX-power cap is applied first on purpose (see
    /// the inline note), and the deviceModel arrives like any other catalog default via the
    /// APPLY_OP poll below rather than here.
    void tick1s() override {
        // Vendor SET_TX_POWER RPC — handled BEFORE the credentials on purpose:
        // when an installer sends the cap and the credentials back-to-back,
        // both flags can land within one tick, and the cap must be persisted
        // before the STA attempt starts or a brown-out-prone board (a weak LDO /
        // marginal supply) fails auth at full power — the exact hole this RPC closes.
        if (pendingTxPowerReady_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setTxPowerSetting(pendingTxPower_);
            pendingTxPowerReady_.store(false, std::memory_order_release);
        }
        // The platform task writes credentials into pendingSsid_/pendingPassword_
        // then publishes via a release-store on pendingCredentials_. We do an
        // acquire-load here so the buffer writes are visible before we read
        // them. Pairs with the release-store in platform_esp32.cpp.
        if (pendingCredentials_.load(std::memory_order_acquire) && networkModule_) {
            networkModule_->setWifiCredentials(pendingSsid_, pendingPassword_);
            // Wipe the on-stack-ish password buffer; status string keeps any
            // error message the platform layer wrote. SSID is non-sensitive,
            // leave it for the next poll if a re-provision arrives.
            std::memset(pendingPassword_, 0, sizeof(pendingPassword_));
            pendingCredentials_.store(false, std::memory_order_release);
        }
        // deviceModel arrives like any other catalog default: an APPLY_OP
        // `set System.deviceModel` op, routed through the apply-core and the
        // control's per-control validator (handled in the APPLY_OP poll below).
    }

    /// Apply a pending APPLY_OP through HttpServerModule::applyOp. Polled per-TICK (not tick1s)
    /// because the installer pushes a burst of ops during provisioning and single-buffers them:
    /// the Improv task refuses a new op until this consumes the previous (clears
    /// pendingOpReady_), so a fast poll keeps the busy-window to ~one tick and the install
    /// snappy. Applying on the main loop here (not the Improv task) keeps the factory/tree
    /// mutation off the serial task — the same discipline the credentials/deviceModel paths
    /// follow. A failed op can't travel back on the already-spent frame ack, so it's surfaced
    /// over serial + in provision_status rather than looking like a clean install.
    void tick() override {
        if (pendingOpReady_.load(std::memory_order_acquire) && httpServerModule_) {
            // The Improv task already acked frame RECEIPT; the op is APPLIED here. A
            // failed op (UnknownType, OutOfRange, a not-found target) can't travel back
            // on that spent ack, so surface it: log it over serial and park it in
            // provision_status, so a silently-misconfigured device is visible on a
            // monitor and via /api/state rather than looking like a clean install.
            // Ok and AlreadyExists are both success (a re-pushed op is idempotent);
            // only a genuine failure is surfaced.
            auto r = httpServerModule_->applyOp(pendingOp_);
            if (r != HttpServerModule::OpResult::Ok &&
                r != HttpServerModule::OpResult::AlreadyExists) {
                std::printf("Improv APPLY_OP failed (result=%d): %s\n",
                            static_cast<int>(r), pendingOp_);
                std::snprintf(statusStr_, sizeof(statusStr_), "error: apply failed (%d)",
                              static_cast<int>(r));
            }
            std::memset(pendingOp_, 0, sizeof(pendingOp_));
            pendingOpReady_.store(false, std::memory_order_release);
        }
        MoonModule::tick();   // tick children (none today, but keep the contract)
    }

private:
    SystemModule*     systemModule_     = nullptr;
    NetworkModule*    networkModule_    = nullptr;
    HttpServerModule* httpServerModule_ = nullptr;
    char statusStr_[64] = "listening";

    // Buffers the platform task writes; sized to NetworkModule's storage.
    // std::atomic<bool> for the ready flag — read by tick1s() on the
    // scheduler thread, written by the Improv task. Acquire/release fencing
    // ensures the buffer writes are ordered against the flag publication,
    // which matters on dual-core Xtensa where the producer and consumer can
    // run on different cores.
    char pendingSsid_[33] = {};
    char pendingPassword_[64] = {};
    std::atomic<bool> pendingCredentials_{false};

    // Vendor SET_TX_POWER RPC — the pre-association TX-power cap (whole dBm)
    // for brown-out-prone boards; same producer/consumer shape as the above.
    uint8_t pendingTxPower_ = 0;
    std::atomic<bool> pendingTxPowerReady_{false};

    // Vendor APPLY_OP RPC — one REST op as JSON, reassembled by the Improv task and
    // applied here on the main loop via HttpServerModule::applyOp. Sized for the
    // largest op (a long pins list fits comfortably in 512 bytes).
    char pendingOp_[512] = {};
    std::atomic<bool> pendingOpReady_{false};
};

} // namespace mm
