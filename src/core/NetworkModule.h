#pragma once

#include "core/MoonModule.h"
#include "core/Scheduler.h"
#include "core/SystemModule.h"
#include "core/FilesystemModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Manages all device connectivity with an automatic priority cascade: Ethernet → WiFi
/// STA → WiFi AP. One MoonModule, one UI card — the user sees "Network", not three
/// separate technologies. ESP32-specific (and Teensy later); desktop and RPi use OS-level
/// networking and load no NetworkModule.
///
/// **Priority cascade:** Ethernet is always preferred (hardware detected, cable plugged),
/// WiFi STA is next (SSID configured, Ethernet unavailable), WiFi AP is the last resort
/// (STA fails or no SSID). When a higher-priority connection becomes available, lower ones
/// are torn down to reclaim memory; when a higher-priority connection drops, the next
/// activates automatically. The cascade tries each interface unconditionally and relies on
/// the platform init calls to fail fast when hardware is absent — `platform::ethInit()`
/// returns false without a PHY, and the WiFi paths return false on chips without a radio,
/// so no interface hangs waiting on missing hardware.
///
/// **AP shutdown delay:** when STA connects successfully, AP stays active for ~10 s (with
/// a UI message) before tearing down, giving the user time to reconnect via STA. AP always
/// uses the fixed IP `4.3.2.1` — easy to remember, avoids 192.168.x.x conflicts with home
/// routers.
///
/// **State machine:** `State` (Idle, WaitingEth, WaitingSta, ConnectedEth, ConnectedSta,
/// AP) is driven from `tick1s()`. The `mode` control mirrors the state in plain language
/// and is always present, even on the Ethernet-only build. A late-appearing interface
/// (slow DHCP, cable plugged in after boot, saved WiFi credentials) is promoted from Idle
/// / AP / ConnectedSta by the periodic upgrade checks — no reboot.
///
/// **Ethernet:** which PHY *driver* is compiled in is per chip (classic/P4 carry the
/// internal-EMAC RMII driver, the S3 the W5500 SPI driver; a `MM_NO_ETH` build stubs
/// `ethInit()` to return false). *Which* PHY a board uses and *on which pins* is runtime
/// config — the `ethType` + pin controls, set per board in the device-model catalog and
/// seeded from the per-chip default in `platform_config.h`. A W5500 change applies live
/// (the SPI driver tears down and re-inits, no reboot); an RMII change saves and applies
/// on the next boot (the EMAC/clock release is fiddlier). The eth controls, bound only on
/// an Ethernet-capable build (`platform::hasEthernet`) and shown per PHY type:
///  - `ethType` — PHY dropdown; the stored index maps 0=None, 1=LAN8720 (RMII),
///    2=IP101 (RMII), 3=W5500 (SPI), 4=YT8531 (RGMII, the S31's on-chip 1 Gb EMAC),
///    matching the `EthPhyType` enum order. 0 shows no pin rows; a type reveals only its set.
///  - `ethPhyAddr` — SMI/PHY MDIO address: -1 = auto-detect (scan the bus, the RGMII default), else 0..31 (typically 0 or 1).
///  - `ethRstGpio` — PHY reset GPIO (−1 = none / module self-resets).
///  - `ethMdcGpio` / `ethMdioGpio` — RMII SMI clock / data GPIOs (−1 = IDF default). RMII only.
///  - `ethClockGpio` — RMII 50 MHz reference-clock GPIO; `ethClockExtIn` = clock direction
///    (on = fed IN by the board, off = chip drives it OUT). RMII only.
///  - `ethSpiMiso` / `ethSpiMosi` / `ethSpiSck` / `ethSpiCs` / `ethSpiIrq` — W5500 SPI pins
///    (`ethSpiIrq` −1 = polling). W5500 only.
///
/// **mDNS:** included here (not a separate module). Registers the deviceName on whichever
/// interface is active and re-registers when the active interface changes or the name is
/// renamed live. Uses ESP-IDF's `mdns_init()` / `mdns_hostname_set()`.
///
/// **Addressing (DHCP / Static):** the `addressing` selector chooses how the active client
/// interface — WiFi STA *or* Ethernet — gets its IPv4 address. DHCP (the default) runs the
/// interface's DHCP client. Static pins the `ip` / `gateway` / `subnet` / `dns` controls onto the
/// netif via `platform::netSetStaticIPv4` (which stops the DHCP client and sets the address); the
/// same octets that on DHCP would come from the server. Applied at each interface's bring-up
/// (`applyStaticIfConfigured`) and live on a change (`syncAddressingLive`, no reboot — toggling
/// back to DHCP restarts the client and re-leases). On Static there is no DHCP `GOT_IP` event, so
/// the platform marks the interface connected when the static IP is set. A static IP on Ethernet
/// bypasses DHCP entirely — useful where a DHCP handshake is unreliable but the link is fine.
///
/// **Device name:** the network name is owned solely by SystemModule; this module only
/// READS it (see `readDeviceName`), and it is the single identity behind the mDNS
/// `<name>.local`, the SoftAP SSID, and the DHCP hostname — so a device shows one name
/// everywhere.
///
/// **`MM_IP=` boot token:** `currentIp()` writes the device's current LAN IP as octets;
/// main.cpp formats it and appends a machine-parseable `MM_IP=<ip>` token to its
/// once-per-second tick line — gated to the first 60 s of uptime (the installer reads at
/// ~3–15 s after boot; afterwards the IP comes from the REST API, so a permanent token would
/// just be noise on the perf line). The web installer reads this from the boot serial log
/// right after flashing to auto-add the device to "Your devices" — timing-independent because
/// the token rides the already-periodic tick line. Deliberately IP-only; once the installer
/// has the IP it reads everything else from the live REST API.
///
/// **Memory:** the network stack cost varies by mode (Ethernet ~20 KB, STA ~40 KB, AP
/// ~30 KB, STA+AP during the shutdown delay ~60 KB, fully reclaimed after release). This
/// is why NetworkModule registers with the Scheduler BEFORE the light pipeline: network
/// memory is claimed first so the light pipeline's adaptive allocation sees the real
/// remaining heap. On a mode change the transition sequence checks heap, tears down light
/// buffers first if heap is tight (display goes dark temporarily — acceptable, a crash is
/// not), starts the new mode, then re-runs `scheduler_->prepareTree()` so allocation uses
/// whatever heap remains. Reported via the standard per-module system; dynamicBytes updates
/// after each mode change.
///
/// **Ethernet-only build:** `esp32-eth` compiles WiFi out entirely
/// (`platform::hasWiFi == false`), branched via `if constexpr`. The cascade is
/// Ethernet-only (no STA/AP states reachable), the `ssid` / `password` controls are not
/// bound, but the `addressing` selector, static-IP controls, and `mDNS` toggle remain. The
/// `ssid_` / `password_` buffers still exist (unconditional struct layout keeps persistence
/// stable), simply never displayed or used.
///
/// **Security:** AP mode is open (no password) — a fallback for initial setup only. The
/// STA password is stored in the controls. No HTTPS — an embedded device on the local
/// network only.
///
/// **Prior art:** MoonLight — mDNS hostname advertising, REST API for network config,
/// credentials persisted to SPIFFS. ESP-IDF — `esp_wifi.h`, `mdns.h`, `esp_netif.h`,
/// `esp_event.h`.
/// @card NetworkModule.png
class NetworkModule : public MoonModule {
public:
    void setScheduler(Scheduler* s) { scheduler_ = s; }
    void setSystemModule(SystemModule* s) { systemModule_ = s; }

    /// External entry-point for setting WiFi credentials at runtime — used by
    /// ImprovProvisioningModule when the browser/CLI pushes new credentials over
    /// USB-serial. Writes the same buffers the AP-fallback UI flow writes via
    /// POST /api/control on `ssid` / `password`, then drives a clean transition
    /// into `State::WaitingSta` so tick1s() takes over and either reports
    /// connected (onConnected) or falls back to AP after the 10 s timeout.
    ///
    /// Why the explicit AP→STA tear-down (rather than just calling wifiStaInit
    /// and letting esp_wifi_set_mode handle the mode change): in AP-mode the
    /// platform layer's wifiInitDone_ flag is true, which makes ensureWifiInit
    /// return early without registering the IP_EVENT_STA_GOT_IP handler. Without
    /// that handler the wifiStaConnected_ flag never flips, the WaitingSta
    /// state never sees the STA come up, and the device sits in limbo with
    /// STA mode active but the state machine still thinking it's in AP.
    /// wifiApStop() drops wifiInitDone_=false so the next ensureWifiInit
    /// registers handlers cleanly.

    /// Improv SET_TX_POWER path: persist + apply the TX-power cap (whole dBm,
    /// 0 = lift). Must run BEFORE setWifiCredentials when both arrive from one
    /// provisioning flow — a weak-powered board / WiFi module (thin LDO, marginal
    /// USB supply) browns out and fails WiFi auth at full power, so the cap has to
    /// be in place for the association attempt.
    void setTxPowerSetting(uint8_t dBm) {
        if (dBm > 21) return;
        txPowerSetting_ = dBm;
        markDirty();
        FilesystemModule::noteDirty();   // same persist arming as setWifiCredentials
        syncTxPower();                   // applies now if the radio is up; the
                                         // STA-start path re-applies otherwise
    }

    void setWifiCredentials(const char* ssid, const char* password) {
        if (!ssid) return;
        // snprintf, not strncpy+manual-NUL: strncpy does not terminate when the source fills the
        // buffer, so it always needs that second line — and forgetting it is a classic bug, which is
        // why GCC flags the pattern. snprintf terminates and truncates identically. (A 99-char SSID
        // into a 32-byte buffer DOES truncate — but an over-long SSID is invalid anyway, and a
        // truncated-but-terminated string is what the old code already produced.)
        std::snprintf(ssid_, sizeof(ssid_), "%s", ssid);
        std::snprintf(password_, sizeof(password_), "%s", password ? password : "");
        markDirty();
        FilesystemModule::noteDirty();   // start the debounce so the change actually flushes
                                         // (markDirty alone only sets the bit; the save scheduler
                                         // needs noteDirty to arm — Improv-pushed creds would
                                         // otherwise persist only if some other control changed)
        if constexpr (platform::hasWiFi) {
            // Tear down any prior WiFi state (AP-fallback, mid-flight STA
            // attempt, or stale init from a previous reconfigure) so the
            // platform's event-handler registration runs fresh.
            if (state_ == State::AP) {
                platform::wifiApStop();
                noteRadioStopped();
                apShutdownPending_ = false;
            }
            if (state_ == State::WaitingSta || state_ == State::ConnectedSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
            if (platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                stateChangeTime_ = platform::millis();
                // Apply the TX-power cap NOW, before the radio's first
                // probe / auth / assoc burst — that's the window the
                // weak-power brown-out cap exists to protect. Waiting for the
                // next tick1s() tick to syncTxPower would leave up to
                // 1 s of full-power TX during association, the exact
                // failure mode the cap defends against. syncTxPower
                // itself is cheap and idempotent.
                syncTxPower();
                std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi STA: %s", ssid_);
                setStatus(statusBuf_, Severity::Status);
                // Re-evaluate control visibility — rssi was visible while
                // state_ was ConnectedSta (any prior call to wifiStaConnected)
                // and would otherwise stay rendered with a now-stale reading
                // until the cascade either reconnects (onConnected rebuilds)
                // or falls back to AP (startAP rebuilds). Match those paths.
                rebuildControls();
            } else {
                // STA init failed (OOM, GPIO conflict). Try to recover via
                // AP so the user can re-enter credentials manually.
                startAP();
            }
        }
    }

    /// Networking is infrastructure — keep the cascade ticking even when the user
    /// toggled "enabled" off, otherwise the device would silently drop off the LAN
    /// and become unreachable.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    void setup() override {
        // Push the DHCP hostname (option 12) before any bring-up so the device shows
        // its name — not "Unknown" — in the router's client list. Stored once; every
        // netif the platform creates (eth, the wifi cascade, a later reconnect) reads
        // it. Same name as mDNS/SoftAP: deviceName, default MM-XXXX.
        //
        // Live-rename boundary: setHostname() is single-writer-before-readers by
        // contract (see platform_esp32.cpp) — NOT safe to re-call after bring-up from
        // tick1s without platform-side synchronization. And the DHCP hostname only
        // rides the DISCOVER, so it can't change until the next lease renewal regardless.
        // So a live deviceName rename updates mDNS immediately (syncMdns re-registers)
        // and the SoftAP SSID on its next start; the DHCP/router-list name follows on the
        // next renewal or reconnect, picking up the new value here. That lag is inherent
        // to DHCP, not a bug; forcing a reconnect to refresh it would drop the LAN link.
        platform::setHostname(readDeviceName());
        // Push the board's eth config (persisted controls, loaded before setup)
        // into the platform layer before ethInit reads it.
        syncEthConfig();
        // Baseline the addressing signature so syncAddressingLive only fires on a *later* change:
        // the initial static apply is done by the bring-up path (WaitingEth / onConnected), and a
        // DHCP-mode device must not have its client needlessly restarted on the first tick.
        appliedAddressingSig_ = addressingSig();
        addressingSigApplied_ = true;
        // Try Ethernet first (non-blocking)
        if (platform::ethInit()) {
            state_ = State::WaitingEth;
            std::printf("NetworkModule: Ethernet init started\n");
        } else if constexpr (platform::hasWiFi) {
            // Ethernet not available, fall back to WiFi (STA → AP).
            if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                syncTxPower();  // see setWifiCredentials's syncTxPower comment
                std::printf("NetworkModule: WiFi STA init started, SSID: %s\n", ssid_);
            } else {
                startAP();
            }
        } else {
            // Ethernet-only build: no WiFi fallback. Stay Idle until a cable
            // appears (WaitingEth is only entered on a successful ethInit()).
            state_ = State::Idle;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
        }

        stateChangeTime_ = platform::millis();

        // Chain to base so children (ImprovProvisioningModule on ESP32) get setup()
        // after we've claimed the network resources we care about.
        MoonModule::setup();
    }

    /// The EMAC's data pads while Ethernet is the running interface. Not controls: the chip fixed
    /// them, so there is nothing to set, and a board that is not using Ethernet leaves them free for
    /// anything else (three of four classic boards in the catalog have no PHY at all).
    uint8_t fixedPins(FixedPin* out, uint8_t max) const override {
        if (!out || ethType_ == static_cast<uint8_t>(platform::ethNone)) return 0;
        uint8_t n = 0;
        for (uint8_t i = 0; i < platform::ethFixedPadCount && n < max; i++)
            out[n++] = FixedPin{platform::ethFixedPads[i].gpio, platform::ethFixedPads[i].name};
        return n;
    }

    void defineControls() override {
        // Chain to base FIRST so children (Improv on ESP32) register their
        // controls before NetworkModule appends its own — per the override-
        // and-chain convention in docs/coding-standards.md § Override-and-
        // chain ("defineControls — chain first, then parent work").
        // Earlier shape called this at the end, which inverted the order
        // (parent's controls landed before children's).
        MoonModule::defineControls();

        setStatus(statusBuf_);

        // Refresh the live-readout values (mode label + rssi + txPower) so a
        // rebuild triggered mid-state-transition shows the up-to-date numbers.
        updateMetrics();

        // `mode` reflects the state-machine state in plain language. Always
        // present (every firmware variant has a mode, even Ethernet-only).
        controls_.addReadOnly("mode", modeStr_, sizeof(modeStr_));

        // WiFi credential controls are absent in the Ethernet-only build.
        if constexpr (platform::hasWiFi) {
            controls_.addText("ssid", ssid_, sizeof(ssid_));
            controls_.addPassword("password", password_, sizeof(password_));
            // RSSI is meaningful only while associated as a STA. Hide on
            // Ethernet / AP / Idle to avoid showing a stale 0 dBm reading.
            controls_.addReadOnlyInt("rssi", rssi_, "dBm");
            controls_.setHidden(controls_.count() - 1, state_ != State::ConnectedSta);
            // TX power applies whenever the WiFi radio is active (STA or AP).
            // Hide on Ethernet / Idle where the radio is off.
            // Expert-only: a radio-tuning readout, not something a normal install reads.
            controls_.addReadOnlyInt("txPower", txPower_, "dBm");
            const bool radioOn = (state_ == State::ConnectedSta
                                  || state_ == State::WaitingSta
                                  || state_ == State::AP);
            controls_.setHidden(controls_.count() - 1, !radioOn);
            controls_.setAdvanced(controls_.count() - 1);
            // Writable TX-power cap (the weak-power / brown-out WiFi cap). Range 0..21 dBm.
            // 0 = "no override" (sentinel — syncTxPower then writes the
            // ESP-IDF ceiling, ~20 dBm, to actively lift any prior cap;
            // setting back to 0 truly restores default power). 1 is in
            // the bound but the platform layer clamps it up to 2 dBm
            // (ESP-IDF's minimum) — write 2 or higher for predictable
            // behavior. Always bound on radio-capable builds; the
            // deviceModels.json catalog injects 8 dBm for brown-out-prone boards.
            // Hidden with the same radioOn gate as the txPower readout above — a WiFi
            // TX-power cap is meaningless on Ethernet / Idle where the radio is off.
            controls_.addInt16("txPowerSetting", txPowerSetting_, 0, 21);
            controls_.setHidden(controls_.count() - 1, !radioOn);
            controls_.setAdvanced(controls_.count() - 1);
        }
        // Expert-only: discovery works without it, and the projectMM UI finds devices over UDP.
        controls_.addBool("mDNS", mdnsEnabled_);
        controls_.setAdvanced(controls_.count() - 1);

        // addressing goes immediately before the static-IP fields it conditions, so
        // the dropdown and the fields it reveals stay adjacent (mDNS, unrelated,
        // sits above rather than wedged between them).
        controls_.addSelect("addressing", addressing_, addressingOptions_, 2);

        // Static-IP fields are always bound (so persistence can load them at any time),
        // but visibility flips based on addressing mode. Toggling the Select triggers a
        // rebuildControls() in HttpServerModule which re-runs this method and re-evaluates
        // the hidden flags.
        const bool hideStatic = (addressing_ != kAddressingStatic);
        controls_.addIPv4("ip", staticIp_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("gateway", staticGateway_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("subnet", staticSubnet_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("dns", staticDns_);
        controls_.setHidden(controls_.count() - 1, hideStatic);

        // Ethernet pin/PHY config — only on builds with an Ethernet driver. The
        // board's deviceModels.json eth block writes these; an un-provisioned board keeps
        // the per-chip default. ethType picks the PHY (and which pin set applies):
        // 1=LAN8720(RMII), 2=IP101(RMII), 3=W5500(SPI), 4=YT8531(RGMII). The RMII/SPI pin
        // rows are shown by type so the UI isn't cluttered with the inapplicable set.
        if constexpr (platform::hasEthernet) {
            // ethType is the switch (always shown on an eth-capable build). When it
            // is 0 (no Ethernet) NO pin rows show; choosing LAN8720/IP101 reveals
            // the RMII rows, W5500 the SPI rows — only the applicable set is ever
            // visible. (Same "show only what's relevant" shape as the LED drivers.)
            controls_.addSelect("ethType", ethType_, ethTypeOptions_, 5);
            const bool isRmii  = (ethType_ == 1 || ethType_ == 2);
            const bool isSpi   = (ethType_ == 3);
            const bool isRgmii = (ethType_ == 4);
            // RGMII (S31): the data/clock pins are the chip's fixed IO_MUX pads, set in ethInitEmac()
            // and reported through fixedPins() rather than as controls, since nobody can choose them.
            // MDC/MDIO are NOT fixed: they ride the shared smi_gpio path on every interface and a
            // carrier can wire them anywhere, so they stay ordinary controls here.
            const bool isEth   = isRmii || isSpi || isRgmii;
            // GPIO controls use addPin → a plain number input (ControlType::Pin),
            // not a slider: a GPIO has no meaningful range to drag. -1 = unused.
            // phyAddr is a PHY MDIO address (-1 = auto-detect, else 0..31), NOT a GPIO —
            // so it uses a signed int control (addInt16), not addPin. (A Pin here would
            // make the pin ownership map report it as a false GPIO claim, since that map
            // reads every ControlType::Pin as a claimed GPIO.) It must be signed: -1 is
            // IDF's ESP_ETH_PHY_ADDR_AUTO sentinel (scan the MDIO bus), the S31's RGMII
            // default — a uint8 mangled -1 to 31 and the PHY never answered.
            controls_.addInt16("ethPhyAddr", ethPhyAddr_, -1, 31);
            controls_.setNumberField(controls_.count() - 1);   // an MDIO address is an identity, not a magnitude — a number field, not a slider
            controls_.setHidden(controls_.count() - 1, !isEth);
            controls_.addPin("ethRstGpio", ethRstGpio_);
            controls_.setHidden(controls_.count() - 1, !isEth);
            // MDC/MDIO are the PHY management pair, and every wired PHY needs them: ethInitEmac sets
            // smi_gpio outside the RMII/RGMII branch, so an RGMII board drives them too. Shown for
            // both (they are real board wiring a carrier can route differently), which is also what
            // gets them counted by the pin map: it skips a hidden pin control on the rule that hidden
            // means unused, so hiding a pin the MAC drives is how a pad goes missing from the map.
            controls_.addPin("ethMdcGpio", ethMdcGpio_);
            controls_.setHidden(controls_.count() - 1, !isRmii && !isRgmii);
            controls_.addPin("ethMdioGpio", ethMdioGpio_);
            controls_.setHidden(controls_.count() - 1, !isRmii && !isRgmii);
            controls_.addPin("ethClockGpio", ethClockGpio_);
            controls_.setHidden(controls_.count() - 1, !isRmii);
            // Clock direction is a boolean (true = clock IN / board feeds it,
            // false = chip drives it OUT) — a toggle, not a 0..1 slider.
            controls_.addBool("ethClockExtIn", ethClockExtIn_);
            controls_.setHidden(controls_.count() - 1, !isRmii);
            controls_.addPin("ethSpiMiso", ethSpiMiso_);
            controls_.setHidden(controls_.count() - 1, !isSpi);
            controls_.addPin("ethSpiMosi", ethSpiMosi_);
            controls_.setHidden(controls_.count() - 1, !isSpi);
            controls_.addPin("ethSpiSck", ethSpiSck_);
            controls_.setHidden(controls_.count() - 1, !isSpi);
            controls_.addPin("ethSpiCs", ethSpiCs_);
            controls_.setHidden(controls_.count() - 1, !isSpi);
            controls_.addPin("ethSpiIrq", ethSpiIrq_);
            controls_.setHidden(controls_.count() - 1, !isSpi);
        }
        // Chain to base is at the top of this method — see comment there.
    }

    void tick1s() MM_NONBLOCKING override {
        uint32_t now = platform::millis();
        uint32_t elapsed = now - stateChangeTime_;

        // The Ethernet-degraded warning is held only while the leaseless cable is still plugged in.
        // Once the link drops (cable out), reset the retry clock and clear the warning so the normal
        // connected status returns; the next re-plug then gets a fresh DHCP window (see ConnectedSta).
        if (!platform::ethLinkUp() && (ethDegraded_ || ethLinkUpAt_ != 0)) {
            ethLinkUpAt_ = 0;
            if (ethDegraded_) {
                ethDegraded_ = false;
                updateStatusIP();   // reverts to the WiFi/AP IP line (or leaves prior status if none)
            }
        }

        switch (state_) {
            case State::WaitingEth:
                // Static mode: as soon as the link is up, pin the IP (no DHCP round to wait for).
                // netSetStaticIPv4 marks eth connected, so the ethConnected() check below promotes
                // to Ethernet on the same or next tick. DHCP mode: this is a no-op, cascade as usual.
                if (addressing_ == kAddressingStatic && platform::ethLinkUp() && !platform::ethConnected())
                    applyStaticIfConfigured(platform::NetIface::Eth);
                if (platform::ethConnected()) {
                    onConnected("Ethernet");
                } else if ((elapsed > 3000 && !platform::ethLinkUp()) || elapsed > kEthDhcpWaitMs) {
                    // Surface WHY Ethernet is being abandoned. Two distinct cases: the link never
                    // came up (no cable, or the PHY didn't negotiate), vs. the link is up but no
                    // address was assigned (the S31-at-100M DHCP gap — see docs/backlog-core.md).
                    // When the link IS up but leaseless, remember it (ethDegraded_) so the warning
                    // is not buried under the WiFi-fallback "connected" status the cascade is about
                    // to set: a live-but-unusable Ethernet cable is worth the user's attention until
                    // they unplug it (which drops ethLinkUp → the warning clears, see tick1s/onConnected).
                    // Unless something is driving the link directly (raw L2, no IP wanted) — then
                    // writeEthDegradedStatus reports it as normal rather than as a fault.
                    if (platform::ethLinkUp()) {
                        ethDegraded_ = true;
                        writeEthDegradedStatus();
                    } else {
                        ethDegraded_ = false;
                        std::snprintf(statusBuf_, sizeof(statusBuf_), "Ethernet not detected: no cable/link");
                        setStatus(statusBuf_, Severity::Warning);
                    }
                    if constexpr (platform::hasWiFi) {
                        // No cable after 3s, or link up but no IP after 15s — cascade to WiFi
                        std::printf("NetworkModule: Ethernet %s, cascading\n",
                                    platform::ethLinkUp() ? "no IP (DHCP timeout)" : "no link (no cable)");
                        if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            startAP();
                        }
                    } else {
                        // Ethernet-only build: no fallback. Keep polling for a cable.
                        std::snprintf(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
                        stateChangeTime_ = now;
                    }
                }
                break;

            case State::WaitingSta:
                if constexpr (platform::hasWiFi) {
                    // Static mode: pin the IP during bring-up, the WaitingEth mirror — a DHCP-less
                    // network never fires a lease event, so waiting for "connected" before applying
                    // static would strand the STA into the AP fallback. netSetStaticIPv4 marks the
                    // STA connected once it is associated (platform-gated), so the check below
                    // promotes on the same or next tick. DHCP mode: no-op.
                    if (addressing_ == kAddressingStatic && !platform::wifiStaConnected())
                        applyStaticIfConfigured(platform::NetIface::Sta);
                    if (platform::wifiStaConnected()) {
                        onConnected("WiFi STA");
                    } else if (elapsed > kStaGraceMs) {
                        // WiFi STA didn't connect within the grace window, start AP
                        platform::wifiStaStop();
                        noteRadioStopped();
                        startAP();
                    }
                }
                break;

            case State::ConnectedEth:
                if (!platform::ethConnected()) {
                    if constexpr (platform::hasWiFi) {
                        std::printf("NetworkModule: Ethernet dropped, cascading\n");
                        platform::mdnsStop();
                        if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            startAP();
                        }
                    } else {
                        // Ethernet-only build: drop back to polling for the cable.
                        std::printf("NetworkModule: Ethernet dropped\n");
                        platform::mdnsStop();
                        std::snprintf(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
                        state_ = State::WaitingEth;
                        stateChangeTime_ = now;
                    }
                }
                updateStatusIP();
                break;

            case State::ConnectedSta:
                if constexpr (platform::hasWiFi) {
                    // Ethernet outranks WiFi: if a cable comes up while we are on
                    // WiFi STA, promote to Ethernet. onConnected() then shuts the
                    // WiFi STA down. Gated on ethConnected() (link + DHCP IP), not
                    // bare link-up, so WiFi is never dropped for a not-yet-working
                    // Ethernet — matches the State::AP upgrade check.
                    // Static mode: an Ethernet cable that is up needs no DHCP lease — pin its IP
                    // directly (netSetStaticIPv4 marks eth connected), so the promotion below fires
                    // this tick. Ethernet ALWAYS outranks WiFi when a cable is present (the AP → STA
                    // → ETH cascade is the architecture's contract, never conditional on a per-board
                    // quirk). This lets Ethernet win on a link where DHCP can't complete (the S31 at
                    // 100M): static bypasses the handshake entirely. DHCP mode skips this and relies
                    // on the lease (the ethConnected() check).
                    if (addressing_ == kAddressingStatic && platform::ethLinkUp() && !platform::ethConnected())
                        applyStaticIfConfigured(platform::NetIface::Eth);
                    if (platform::ethConnected()) {
                        std::printf("NetworkModule: Ethernet up, switching from WiFi STA\n");
                        platform::mdnsStop();
                        onConnected("Ethernet");
                    } else if (platform::ethLinkUp()) {
                        // DHCP mode, cable plugged while on WiFi. IDF restarts the eth DHCP client
                        // automatically on each link-up, so Ethernet IS being retried — the check above
                        // promotes to it the moment a lease lands (eth outranks WiFi). Only if the link
                        // stays up WITHOUT a lease past the DHCP window (the S31-at-100M gap) do we raise
                        // the "no address assigned" warning; the grace period lets a healthy cable that
                        // just needs a few seconds to lease get promoted, not flashed as failed.
                        // (In Static mode this branch is unreachable: the apply above already connected eth.)
                        if (ethLinkUpAt_ == 0) ethLinkUpAt_ = now;   // link just (re)appeared: start the clock
                        if (!ethDegraded_ && now - ethLinkUpAt_ > kEthDhcpWaitMs) {
                            ethDegraded_ = true;
                            writeEthDegradedStatus();
                        }
                    } else if (!platform::wifiStaConnected()) {
                        // **A dropout is not a divorce.** The radio reconnects itself (the platform's
                        // STA_DISCONNECTED handler calls esp_wifi_connect), and the common causes — a
                        // router rebooting, a roam, a few lost beacons — heal within seconds. So a
                        // link that reads down gets a grace window to come back before we give up on
                        // the network; only if it stays down do we fall back to AP. The window is
                        // WaitingSta's kStaGraceMs: the question ("has STA had its chance?") is the
                        // same for an initial connect and a mid-session drop, so the answer is too.
                        if (staLostTime_ == 0) {
                            staLostTime_ = now;
                            std::printf("NetworkModule: WiFi STA dropped, reconnecting\n");
                            std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi reconnecting…");
                            setStatus(statusBuf_, Severity::Warning);
                        } else if (now - staLostTime_ > kStaGraceMs) {
                            std::printf("NetworkModule: WiFi STA gone for %us, starting AP\n",
                                        (unsigned)(kStaGraceMs / 1000));
                            platform::mdnsStop();
                            platform::wifiStaStop();
                            noteRadioStopped();
                            staLostTime_ = 0;
                            startAP();
                        }
                    } else {
                        staLostTime_ = 0;   // reconnected within the grace window: back to normal
                        updateStatusIP();
                    }
                }
                break;

            case State::AP:
                if constexpr (platform::hasWiFi) {
                    // Check if higher-priority connection became available
                    if (platform::ethConnected()) {
                        onConnected("Ethernet");
                    } else if (ssid_[0] != 0 && platform::wifiStaConnected()) {
                        onConnected("WiFi STA");
                    } else if (ssid_[0] != 0 && now - stateChangeTime_ > kApRetryStaMs
                               && platform::wifiApClientCount() == 0) {
                        // **AP is a fallback, not a destination.** Falling back stops the STA radio, so
                        // wifiStaConnected() cannot become true on its own and the promote check above
                        // would wait forever — a device that lost its network would stay on its own AP
                        // until power-cycled. The canonical case is a router rebooting: the network
                        // comes back, and the device must find its way home unattended. So go and look
                        // periodically: re-init STA and let WaitingSta run its normal grace. A failure
                        // drops straight back here and retries later — an idle loop, not a dead end.
                        //
                        // **Gated on the AP being EMPTY, because the retry is not free.** The platform
                        // has no concurrent AP+STA mode: wifiStaInit() puts the radio in STA mode,
                        // which drops the SoftAP. Retrying while somebody is on the captive portal
                        // would kick them off every interval. So we only look for the network when
                        // nobody is using the AP — a user mid-setup is never interrupted, and an
                        // unattended device (nobody connected, which is the router-reboot case) still
                        // heals itself.
                        std::printf("NetworkModule: AP — retrying WiFi STA (%s)\n", ssid_);
                        if (platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();   // see setWifiCredentials's syncTxPower comment
                        } else {
                            stateChangeTime_ = now;   // init refused; wait out another interval
                        }
                    }
                }
                break;

            case State::Idle:
                // Recovery from a terminal-looking state. We land in Idle when
                // every bring-up path failed: Ethernet didn't appear within the
                // boot timeout, WiFi STA wasn't configured (or wasn't reachable),
                // and AP fallback failed to init. In Ethernet-only builds we
                // also land here when setup() can't ethInit(). The network
                // stack keeps running in the background though — if Ethernet
                // later acquires a DHCP lease (slow DHCP server, cable plugged
                // in after boot), ethConnected() flips true. Promote when we
                // see it; symmetric with the State::AP and State::ConnectedSta
                // upgrade checks above. Same for late WiFi STA in builds with
                // saved credentials.
                if (platform::ethConnected()) {
                    std::printf("NetworkModule: Ethernet up (recovered from Idle)\n");
                    onConnected("Ethernet");
                } else if constexpr (platform::hasWiFi) {
                    if (platform::wifiStaConnected()) {
                        std::printf("NetworkModule: WiFi STA up (recovered from Idle)\n");
                        onConnected("WiFi STA");
                    }
                }
                break;
        }

        syncMdns();
        syncTxPower();
        syncEthLive();          // hot-apply a W5500 eth config change (no reboot)
        syncAddressingLive();   // hot-apply a DHCP↔Static change (no reboot)

        // Refresh the live-readout values every tick — the UI polls /api/state
        // for them, so writing the same storage addresses is enough; no
        // control rebuild needed. (Hidden-flag changes happen on state
        // transitions via rebuildControls(), not here.)
        updateMetrics();

        // Tick children after our own state machine — option A: parent prepares,
        // children consume. ImprovProvisioningModule (when present) polls a
        // ready-flag here and may call back into setWifiCredentials().
        MoonModule::tick1s();
    }

    void release() override {
        // Tear down children first (Improv on ESP32) so the platform-side
        // Improv task stops touching UART0 before we drop the network state.
        MoonModule::release();
        platform::mdnsShutdown();
        if constexpr (platform::hasWiFi) {
            if (state_ == State::AP) { platform::wifiApStop(); noteRadioStopped(); }
            if (state_ == State::ConnectedSta || state_ == State::WaitingSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
        }
    }

private:
    Scheduler* scheduler_ = nullptr;
    SystemModule* systemModule_ = nullptr;

    enum class State : uint8_t {
        Idle,
        WaitingEth,
        WaitingSta,
        ConnectedEth,
        ConnectedSta,
        AP
    };

    State state_ = State::Idle;
    uint32_t stateChangeTime_ = 0;
    /// When the STA link was first seen down while in ConnectedSta (0 = up). The radio reconnects
    /// itself; this is how long we let it try before giving up on the network and falling back to AP.
    uint32_t staLostTime_ = 0;
    /// How long WiFi STA gets to (re)connect before we fall back to AP. One constant for both the
    /// initial connect (WaitingSta) and a mid-session dropout (ConnectedSta) — the question is the
    /// same in both places, so the answer should be too.
    static constexpr uint32_t kStaGraceMs = 10000;
    /// How long an Ethernet link may sit up without a DHCP lease before we give up on it — the
    /// WaitingEth timeout, and the same window a re-plugged cable's automatic DHCP retry gets in
    /// ConnectedSta before the "no address assigned" warning is raised. DHCP can take several
    /// seconds (~7 s measured on the P4-NANO), so the window is comfortably above that.
    static constexpr uint32_t kEthDhcpWaitMs = 15000;
    /// How often the AP fallback goes back and retries WiFi STA. Long, because each attempt
    /// re-inits the radio and briefly bounces the AP (a user mid-setup on 4.3.2.1 sees a blip), and
    /// because the causes it recovers from — a rebooting router, a device carried back into range —
    /// play out over minutes, not seconds. The device heals itself without anyone noticing; it just
    /// does not do it instantly.
    static constexpr uint32_t kApRetryStaMs = 60000;
    bool apShutdownPending_ = false;
    // Ethernet link is up but never got an address, so we cascaded to WiFi/AP. Kept set so the
    // "Ethernet detected: no address assigned" warning outranks the fallback's connected status
    // (updateStatusIP), until the cable is unplugged (ethLinkUp drops → cleared in tick1s).
    bool ethDegraded_ = false;
    // While on WiFi/AP: millis() when the Ethernet link most recently came up (0 = link down).
    // A re-plugged cable makes IDF retry DHCP automatically; we give that retry the same window as
    // first boot (kEthDhcpWaitMs) before declaring the cable degraded, so a healthy cable that just
    // needs a few seconds to lease is promoted to Ethernet, not flashed as failed. See ConnectedSta.
    uint32_t ethLinkUpAt_ = 0;
    bool mdnsRunning_ = false;
    // The device name last registered with mDNS, so syncMdns() can detect a live
    // rename (deviceName changed in SystemModule) and re-advertise — without it,
    // the .local name would keep announcing the old name until a reconnect. 24 =
    // SystemModule's deviceName_ capacity (the source of hostName()).
    char lastMdnsName_[24] = {};

    // Controls
    char ssid_[33] = {};
    char password_[64] = {};
    // Addressing mode: the `addressing` Select's stored index. Named so the `== kAddressingStatic`
    // checks read as intent, not a magic literal (matches the addressingOptions_ order).
    static constexpr uint8_t kAddressingDhcp = 0;
    static constexpr uint8_t kAddressingStatic = 1;
    uint8_t addressing_ = kAddressingDhcp;
    bool mdnsEnabled_ = true;
    // Module-owned backing store for the status slot inherited from MoonModule.
    // The base class only holds a const char* into this buffer (see
    // MoonModule::status_); the named "Buf" suffix makes the ownership clear
    // and distinguishes it from MoonModule's own status accessors.
    char statusBuf_[48] = {};

    // Static IP fields. uint8_t[4] octets, not strings — saves 12 bytes per
    // address vs char[16] dotted-quad, and the wire/persistence layers
    // (ControlType::IPv4) handle the string conversion at the boundary.
    // Only shown in the UI when addressing_==1 (Static); always bound for
    // persistence so toggling DHCP↔Static doesn't lose user-set values.
    uint8_t staticIp_[4]      = {0, 0, 0, 0};
    uint8_t staticGateway_[4] = {0, 0, 0, 0};
    uint8_t staticSubnet_[4]  = {255, 255, 255, 0};
    uint8_t staticDns_[4]     = {0, 0, 0, 0};

    // Read-only metrics surfaced to the UI.
    // - modeStr_ stays a buffer (state labels are short strings, no
    //   precedent for pointer-to-literal controls today).
    // - rssi_ / txPower_ are int8 — addReadOnlyInt stores them directly
    //   instead of formatting "<value> dBm" into per-control buffers
    //   (saves ~22 bytes vs the prior char[12] approach).
    char modeStr_[20] = {};   // longest label "Ethernet (waiting)" = 19+NUL
    int8_t rssi_ = 0;
    int8_t txPower_ = 0;

    // User-settable TX-power cap in whole dBm (0..21). Default 0 = "no
    // override". Persisted via the control binding. The platform setter
    // takes quarter-dBm (ESP-IDF's native unit), so syncTxPower() multiplies
    // by 4 at the call site. appliedTxPowerSetting_ tracks the last value
    // pushed to the radio so syncTxPower() in tick1s() detects changes (UI
    // write or board-injected value) and re-applies without needing a
    // per-control change callback.
    int16_t txPowerSetting_ = 0;
    int16_t appliedTxPowerSetting_ = -1;   // -1 = never applied, forces first sync

    // Ethernet pin/PHY config — runtime, seeded from the per-chip default
    // (platform::ethConfigDefault) so an un-provisioned board still comes up on
    // its historical pins; a board's deviceModels.json eth block overrides via these
    // controls. Pushed into the platform layer by syncEthConfig() before ethInit.
    // Bound only on builds that have an Ethernet driver (platform::hasEthernet).
    // -1 = "leave at IDF default / unused". ethType: 0=none,1=LAN8720,2=IP101,3=W5500.
    // ethType_ is uint8_t (not int16_t like the pins) so it binds as a Select
    // dropdown via addSelect — the value is the option index, which matches the
    // EthPhyType enum order (None/LAN8720/IP101/W5500).
    //
    // Defaults to 0 (None) — Ethernet is OPT-IN per board, set explicitly by the
    // deviceModels.json eth block (ethType: 1/2/3/4). A WiFi-only board (no eth block)
    // must NOT try to bring up a PHY it doesn't have — that wasted RMII/SPI init is the
    // bug this default avoids. The pins below stay seeded from the per-chip
    // ethConfigDefault so a board that DOES opt in gets its chip's historical pins without
    // re-listing them; only the PHY *selection* defaults off. Matches the installer UI,
    // whose Ethernet pill is "active" (green) only when ethType is set (ethConfigured()).
    // 0 = None; a board opts in via its catalog eth block. Left at None even where the platform
    // FIXES the interface (ethPhyIsFixed): syncEthConfig overrides the value on those targets, so
    // seeding it here would buy nothing and would put a value outside this Select's own option list
    // into a persisted control, which the settings loader then clamps to a DIFFERENT, real PHY.
    uint8_t ethType_       = static_cast<uint8_t>(platform::ethNone);
    // GPIO/address members are int8_t (one byte; -1 = unused). A GPIO never exceeds
    // ~54 on any ESP32-family chip, so int8 is ample — bound via addPin (Pin control
    // → number input). ethConfigDefault's fields are plain int; the values are all
    // small (≤52 / -1) so the copy into int8_t is lossless.
    int16_t ethPhyAddr_    = static_cast<int16_t>(platform::ethConfigDefault.phyAddr);  // PHY MDIO addr 0..31, or -1 = auto-detect (scan the bus). Signed (int16, via addInt16) so -1 round-trips — a uint8 cast the platform's -1 to 255 and the 0..31 control showed 31, a fixed address no PHY answered, so the S31's RGMII never linked. NOT a GPIO (deliberately not addPin, or the pin-map would false-claim it).
    int8_t  ethMdcGpio_    = static_cast<int8_t>(platform::ethConfigDefault.mdcGpio);
    int8_t  ethMdioGpio_   = static_cast<int8_t>(platform::ethConfigDefault.mdioGpio);
    int8_t  ethRstGpio_    = static_cast<int8_t>(platform::ethConfigDefault.rstGpio);
    // The RGMII data pads, mirrored from the platform's one list so they can be PUBLISHED as controls
    // (read-only): the controls are the registry the pin map reads, so a pad that is not a control is
    // a pad the map cannot see. Seeded once; nothing writes them.
    int8_t  ethClockGpio_  = static_cast<int8_t>(platform::ethConfigDefault.rmiiClockGpio);
    bool    ethClockExtIn_ = platform::ethConfigDefault.rmiiClockExtIn;
    int8_t  ethSpiMiso_    = static_cast<int8_t>(platform::ethConfigDefault.spiMiso);
    int8_t  ethSpiMosi_    = static_cast<int8_t>(platform::ethConfigDefault.spiMosi);
    int8_t  ethSpiSck_     = static_cast<int8_t>(platform::ethConfigDefault.spiSck);
    int8_t  ethSpiCs_      = static_cast<int8_t>(platform::ethConfigDefault.spiCs);
    int8_t  ethSpiIrq_     = static_cast<int8_t>(platform::ethConfigDefault.spiIrq);
    // Signature of the eth controls last applied, so tick1s() detects a UI/board
    // change (same pattern as appliedTxPowerSetting_). ethSigApplied_ guards the
    // "never applied yet" case rather than a sentinel value, since any uint32 is a
    // valid hash output. setup()'s syncEthConfig() sets it before any compare.
    uint32_t appliedEthSig_ = 0;
    bool ethSigApplied_ = false;
    // Last-applied addressing signature (mode + static octets), same guard shape as ethSig — so
    // syncAddressingLive re-applies only on a real DHCP↔Static / static-field change.
    uint32_t appliedAddressingSig_ = 0;
    bool addressingSigApplied_ = false;

    // A cheap order-sensitive hash of the eth control members — changes whenever
    // any eth control does, so tick1s() can detect a live reconfigure. uint32_t so
    // the rolling multiply wraps deterministically (signed overflow is UB).
    uint32_t ethSig() const {
        uint32_t h = ethType_;
        for (int16_t v : {ethRstGpio_, ethMdcGpio_, ethMdioGpio_,
                          ethClockGpio_, ethSpiMiso_, ethSpiMosi_,
                          ethSpiSck_, ethSpiCs_, ethSpiIrq_}) {
            h = h * 131u + static_cast<uint32_t>(v);
        }
        h = h * 131u + static_cast<uint32_t>(ethPhyAddr_ & 0xFF);   // PHY addr (int16, -1=auto; not a GPIO), folded in separately
        h = h * 131u + (ethClockExtIn_ ? 1u : 0u);   // bool, folded in separately
        return h;
    }

    // Build an EthPinConfig from the control members and push it to the platform
    // layer. Called in setup() before ethInit() so persisted / board-pushed values
    // take effect on init. (Eth bring-up is boot-time; this is not a live re-init.)
    void syncEthConfig() {
        if constexpr (platform::hasEthernet) {
            platform::EthPinConfig cfg{};
            // Where the platform FIXES the interface (ethPhyIsFixed), its type wins over the stored
            // control: a persisted value would otherwise select hardware that does not exist there.
            // On real silicon the flag is false and the board's own catalog value wins, as before.
            cfg.phyType        = platform::ethPhyIsFixed
                               ? static_cast<uint8_t>(platform::ethConfigDefault.phyType) : ethType_;
            cfg.phyAddr        = ethPhyAddr_;
            cfg.mdcGpio        = ethMdcGpio_;
            cfg.mdioGpio       = ethMdioGpio_;
            cfg.rstGpio        = ethRstGpio_;
            cfg.rmiiClockGpio  = ethClockGpio_;
            cfg.rmiiClockExtIn = ethClockExtIn_;
            cfg.spiMiso        = ethSpiMiso_;
            cfg.spiMosi        = ethSpiMosi_;
            cfg.spiSck         = ethSpiSck_;
            cfg.spiCs          = ethSpiCs_;
            cfg.spiIrq         = ethSpiIrq_;
            platform::setEthConfig(cfg);
            appliedEthSig_ = ethSig();   // mark this config as applied
            ethSigApplied_ = true;
        }
    }

    // Live eth reconfigure — called each tick from tick1s(). When an eth control
    // changed since the last apply AND the (new) type is W5500, tear the SPI driver
    // down and re-init on the spot — no reboot (W5500 is just an SPI device, clean
    // stop/uninstall/re-init). For RMII a live change only updates the stored config
    // + flags a status hint; the EMAC/clock release is fiddlier and applies on the
    // next boot (backlog: live RMII reconfigure). Same change-detect shape as
    // syncTxPower's appliedTxPowerSetting_.
    void syncEthLive() {
        if constexpr (platform::hasEthernet) {
            if (ethSigApplied_ && ethSig() == appliedEthSig_) return;   // nothing changed
            // Hot re-init only when the new type is W5500 AND this firmware actually
            // carries the W5500 driver (S3). Crucially NOT on a classic/P4 RMII board:
            // there ethInit() can't bring up W5500, so a hot ethStop()+ethInit() would
            // tear down the live RMII interface for a type it can't init, stranding the
            // device with no network (and killing the very connection that set the
            // control). On those boards — and for RMII/none everywhere — just save the
            // config and apply on next boot (backlog: live RMII reconfigure). The
            // EMAC/clock release is fiddlier and isn't hot-swappable yet anyway.
            const bool hotReinit = (ethType_ == 3) && platform::hasEthW5500;
            if (hotReinit) {
                platform::ethStop();
                syncEthConfig();                       // pushes cfg + records the new sig
                if (platform::ethInit()) {
                    state_ = State::WaitingEth;
                    stateChangeTime_ = platform::millis();
                    std::printf("NetworkModule: W5500 re-init (live config change)\n");
                } else {
                    std::snprintf(statusBuf_, sizeof(statusBuf_),
                                  "W5500 re-init failed — check pins"); setStatus(statusBuf_, Severity::Error);
                }
            } else {
                // RMII / none, or W5500 selected on a board without the SPI driver:
                // record the new config so the next boot uses it; don't disturb the
                // running interface.
                syncEthConfig();
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              "Ethernet config saved — restart to apply"); setStatus(statusBuf_);
            }
        }
    }

    // Hot-apply a DHCP↔Static change (or an edit to the static fields while in Static mode) on the
    // active interface, no reboot — the "every setting takes effect live" principle. Same
    // change-detect shape as syncTxPower/syncEthLive: a signature over addressing_ + the static
    // octets, compared to the last applied one. Static → pin the config; DHCP → restart the client
    // so it re-leases. Only touches whichever interface is currently connected.
    void syncAddressingLive() {
        uint32_t sig = addressingSig();
        if (addressingSigApplied_ && sig == appliedAddressingSig_) return;   // nothing changed
        appliedAddressingSig_ = sig;
        addressingSigApplied_ = true;

        platform::NetIface iface;
        if (state_ == State::ConnectedEth) iface = platform::NetIface::Eth;
        else if constexpr (platform::hasWiFi) {
            if (state_ != State::ConnectedSta) return;   // not on a client interface: nothing to apply
            iface = platform::NetIface::Sta;
        } else return;

        if (addressing_ == kAddressingStatic) {
            applyStaticIfConfigured(iface);
        } else {
            platform::netSetDhcp(iface);   // Static → DHCP: re-lease live
        }
        updateStatusIP();   // reflect the new address (static IP, or the re-leased one once it lands)
    }

    // Signature folding addressing mode + the four static octets, so an edit to any of them (in
    // Static mode) re-triggers syncAddressingLive. Cheap FNV-style roll, same idea as ethSig().
    uint32_t addressingSig() const {
        uint32_t h = 2166136261u;
        auto fold = [&](uint8_t b) { h = (h ^ b) * 16777619u; };
        fold(addressing_);
        for (int i = 0; i < 4; i++) { fold(staticIp_[i]); fold(staticGateway_[i]); fold(staticSubnet_[i]); fold(staticDns_[i]); }
        return h;
    }

    static constexpr const char* addressingOptions_[] = {"DHCP", "Static"};
    // ethType dropdown options — index order MUST match the EthPhyType enum
    // (None=0, LAN8720=1, IP101=2, W5500=3, YT8531=4) since the Select stores the index.
    static constexpr const char* ethTypeOptions_[] = {"None", "LAN8720", "IP101", "W5500", "YT8531"};

    void startAP() {
        // Same identity as the DHCP hostname and the mDNS .local name — all three read
        // SystemModule's deviceName, so a device shows ONE name everywhere. (Previously
        // had a separate "MM-AP" fallback, which could diverge when the name was empty.)
        const char* apName = readDeviceName();
        if (platform::wifiApInit(apName, "4.3.2.1")) {
            state_ = State::AP;
            stateChangeTime_ = platform::millis();
            apShutdownPending_ = true;
            syncTxPower();  // see setWifiCredentials's syncTxPower comment
            std::snprintf(statusBuf_, sizeof(statusBuf_), "AP: %s @ 4.3.2.1", apName); setStatus(statusBuf_, Severity::Status);
            // The address is what a user needs: AP mode exists so they can open the UI and
            // enter credentials. Naming the network without it sends them looking for a URL.
            std::printf("NetworkModule: AP started: %s → join it and open http://4.3.2.1\n", apName);
        } else {
            state_ = State::Idle;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "No network"); setStatus(statusBuf_, Severity::Error);
        }
        // statusBuf_ is the buffer MoonModule::status_ points at — no control
        // rebuild needed for status itself, but rssi/txPower visibility depends
        // on state_ so rebuildControls() re-evaluates their hidden flags.
        rebuildControls();
        if (scheduler_) scheduler_->prepareTree();
    }

    void onConnected(const char* via) {
        if (std::strcmp(via, "Ethernet") == 0) {
            state_ = State::ConnectedEth;
            ethDegraded_ = false;   // Ethernet itself got a lease — no longer degraded
        } else {
            state_ = State::ConnectedSta;
            // Static mode on WiFi: the STA is associated (wifiStaConnected) but its address comes
            // from us, not DHCP — pin it now, before updateStatusIP reads the netif. (Ethernet's
            // static apply already happened in WaitingEth, where it also set ethConnected.)
            if constexpr (platform::hasWiFi) applyStaticIfConfigured(platform::NetIface::Sta);
        }
        stateChangeTime_ = platform::millis();

        // Shut down lower-priority WiFi connections (no-op in the Ethernet-only build).
        if constexpr (platform::hasWiFi) {
            if (apShutdownPending_ || platform::wifiApConnected()) {
                std::printf("NetworkModule: Shutting down AP (higher priority connected)\n");
                platform::wifiApStop();
                noteRadioStopped();
                apShutdownPending_ = false;
            }
            if (state_ == State::ConnectedEth && platform::wifiStaConnected()) {
                std::printf("NetworkModule: Shutting down WiFi STA (Ethernet connected)\n");
                platform::wifiStaStop();
                noteRadioStopped();
            }
        }

        updateStatusIP();
        std::printf("NetworkModule: Connected via %s — %s\n", via, statusBuf_);

        syncMdns();

        // statusBuf_ is the buffer MoonModule::status_ points at — no control
        // rebuild needed for status itself, but rssi/txPower visibility depends
        // on state_ so rebuildControls() re-evaluates their hidden flags.
        rebuildControls();
        if (scheduler_) scheduler_->prepareTree();
    }

public:
    /// Write the current LAN IP as octets into out[0..3] (all-zero = not connected).
    /// Octets, not a string: the IP's canonical form is `uint8_t[4]` (matching the
    /// static-IP controls and formatDottedQuad), and no IP string is held as state —
    /// the IP already lives as the netif's binding, so duplicating it into a member
    /// would just waste RAM. Callers that need text format with formatDottedQuad at
    /// their boundary. Read by main.cpp's per-second tick line, which appends it as a
    /// stable `MM_IP=<ip>` token for the web installer's post-flash serial read —
    /// riding the already-periodic tick line means the IP re-emits every second for the
    /// first 60 s of uptime (timing-independent: DHCP can take several seconds — measured
    /// ~7s on the P4-NANO — and the installer reopens the port at its own pace, so a
    /// one-shot connect-time line is easy to miss; the 60 s cap lives in main.cpp).
    void currentIp(uint8_t out[4]) const {
        out[0] = out[1] = out[2] = out[3] = 0;
        if (state_ == State::ConnectedEth) platform::ethGetIPv4(out);
        else if constexpr (platform::hasWiFi) {
            if (state_ == State::ConnectedSta) platform::wifiStaGetIPv4(out);
        }
    }

private:
    /// If the user selected Static addressing, pin the configured IP/gw/mask/dns onto the given
    /// interface (the platform stops its DHCP client and sets the address); a no-op in DHCP mode.
    /// Called at each interface's bring-up transition — the one place that turns the `addressing`
    /// dropdown + static-IP controls into an actually-applied config, for both STA and Ethernet.
    void applyStaticIfConfigured(platform::NetIface iface) {
        if (addressing_ != kAddressingStatic) return;   // DHCP mode: leave the client running
        platform::netSetStaticIPv4(iface, staticIp_, staticGateway_, staticSubnet_, staticDns_);
    }

    /// The device's network name is owned solely by SystemModule; NetworkModule only
    /// READS it. This is the single identity behind every network name — the mDNS
    /// `<name>.local`, the SoftAP SSID, and the DHCP hostname are all this exact string,
    /// so a device shows one name everywhere. SystemModule guarantees it is a valid,
    /// non-empty hostname (sanitised + MAC-fallback in its setup/tick1s). Read through
    /// this one null-guard (systemModule_ is wired at boot; "" if somehow unwired — the
    /// platform name setters no-op on an empty string). NOT a deviceName of our own:
    /// it's SystemModule's, fetched.
    const char* readDeviceName() const {
        return systemModule_ ? systemModule_->deviceName() : "";
    }
    // The Ethernet-degraded warning: link is up but no address was assigned. Shown (Warning
    // severity) in preference to a WiFi/AP-fallback "connected" line, because a live-but-unusable
    // cable is worth flagging until the user unplugs it. Reports any address that WAS captured
    // (a partial/lost lease) so they have an IP to work with. Cleared when ethLinkUp() drops
    // (cable out) in tick1s, at which point updateStatusIP falls through to the normal IP line.
    void writeEthDegradedStatus() {
        // A driver has claimed the link for direct L2 use, so it is doing its job rather than
        // failing: a raw-L2 sender addresses the wire below IP and never wants a lease. Report it as Status rather than Warning — the "no address" line would otherwise
        // tell the user to unplug the very cable that is driving their panels.
        if (platform::ethRawL2Claimed()) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "Ethernet: link up, no IP (L2 in use)");
            setStatus(statusBuf_, Severity::Status);
            return;
        }
        uint8_t ip[4] = {};
        platform::ethGetIPv4(ip);
        if (ip[0] || ip[1] || ip[2] || ip[3]) {
            char ipStr[16]; formatDottedQuad(ipStr, ip);
            std::snprintf(statusBuf_, sizeof(statusBuf_), "Ethernet detected (%s): no lease", ipStr);
        } else {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "Ethernet detected: no address assigned");
        }
        setStatus(statusBuf_, Severity::Warning);
    }

    void updateStatusIP() {
        // A live-but-leaseless Ethernet cable outranks the WiFi/AP-fallback IP line: keep the
        // warning up so it isn't buried the moment the cascade connects WiFi. (On Ethernet itself,
        // ethDegraded_ is false — a real ConnectedEth means a lease succeeded.)
        if (ethDegraded_ && state_ != State::ConnectedEth) { writeEthDegradedStatus(); return; }
        uint8_t ip[4];
        currentIp(ip);   // same eth/wifi getter dispatch, in one place
        if (!ip[0] && !ip[1] && !ip[2] && !ip[3]) return;   // not connected — keep prior status
        char ipStr[16];
        formatDottedQuad(ipStr, ip);
        if (state_ == State::ConnectedEth) {
            // Carry the NEGOTIATED speed, not just the address. A gigabit PHY that fell back to
            // 100M still gets a lease and looks identical here, while the panel-card driver needs
            // the higher rate to hold its frame timing, so the one line answers both "am I on the
            // network" and "at what rate".
            const uint16_t mbps = platform::ethLinkSpeedMbps();
            if (mbps > 0) std::snprintf(statusBuf_, sizeof(statusBuf_), "Eth: %s (%u Mbit)",
                                        ipStr, static_cast<unsigned>(mbps));
            else          std::snprintf(statusBuf_, sizeof(statusBuf_), "Eth: %s", ipStr);
        } else {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi: %s", ipStr);
        }
        setStatus(statusBuf_, Severity::Status);
    }

    // Apply txPowerSetting_ to the radio whenever it changes (UI write,
    // board-injected value, or first time it lands after STA/AP comes up).
    // Mirrors syncMdns()'s shape: cheap idempotent check, called from
    // tick1s(). esp_wifi_set_max_tx_power requires the WiFi stack started
    // — wifiSetTxPower() guards on that and returns false otherwise, which
    // leaves appliedTxPowerSetting_ untouched so the next tick (post-STA-
    // up) retries cleanly.
    void syncTxPower() {
        if constexpr (!platform::hasWiFi) return;
        if (txPowerSetting_ == appliedTxPowerSetting_) return;
        // "No override" (0) with nothing ever applied is a genuine no-op: the
        // radio is already at its default ceiling, so there is nothing to push.
        // Skipping it is not just an optimisation — calling
        // esp_wifi_set_max_tx_power inside the radio-start call stack (this runs
        // right after wifiStaInit/startAP) hangs the classic ESP32 on IDF
        // v6.1-dev with an interrupt-watchdog reset, boot-looping the device. A
        // default board must never touch TX power; a real cap (1..21) still does,
        // and lifting a prior cap back to 0 still pushes the ceiling because
        // appliedTxPowerSetting_ is then > 0.
        if (txPowerSetting_ == 0 && appliedTxPowerSetting_ <= 0) {
            appliedTxPowerSetting_ = 0;   // mark synced so we don't re-check every tick
            return;
        }
        const bool radioUp = (state_ == State::ConnectedSta
                              || state_ == State::WaitingSta
                              || state_ == State::AP);
        if (!radioUp) return;
        // Convert dBm (user-facing) → quarter-dBm (ESP-IDF native). The
        // 0 sentinel ("no override") needs to actively undo any prior cap
        // — esp_wifi_set_max_tx_power has no "reset to default" call, so
        // we push the ceiling (80 = 20 dBm) instead. Without this the
        // cap would be sticky until reboot: setting back to 0 in the UI
        // would silently leave the radio at the prior cap.
        const int8_t quarterDbm = (txPowerSetting_ == 0)
                                  ? static_cast<int8_t>(80)
                                  : static_cast<int8_t>(txPowerSetting_ * 4);
        if (platform::wifiSetTxPower(quarterDbm)) {
            appliedTxPowerSetting_ = txPowerSetting_;
        }
    }

    // Invalidate the "last applied" tracker so the next syncTxPower()
    // re-applies the cap. Must be called every time the WiFi stack stops
    // (wifiStaStop / wifiApStop / release): ESP-IDF resets the radio's
    // TX-power state on stop, so our cached `applied` value no longer
    // reflects what the radio thinks. Without this, the equality check
    // in syncTxPower() short-circuits and the cap never lands on the
    // restarted radio — a brown-out-prone board would associate at full power
    // (brown-out hazard) until the user touched the control to force a
    // resync.
    void noteRadioStopped() { appliedTxPowerSetting_ = -1; }

    void syncMdns() {
        bool shouldRun = mdnsEnabled_ && (state_ == State::ConnectedEth || state_ == State::ConnectedSta);
        const char* devName = readDeviceName();
        if (shouldRun && !mdnsRunning_) {
            // Only mark running on success — leave false so tick1s retries next tick.
            if (platform::mdnsInit(devName)) {
                mdnsRunning_ = true;
                std::strncpy(lastMdnsName_, devName, sizeof(lastMdnsName_) - 1);
                lastMdnsName_[sizeof(lastMdnsName_) - 1] = 0;
            }
        } else if (shouldRun && mdnsRunning_ && std::strcmp(devName, lastMdnsName_) != 0) {
            // Live rename: the device name changed (SystemModule deviceName) while
            // mDNS is already up. Re-register so the .local name follows immediately —
            // no reconnect needed (the "no reboot to apply config" rule). mdnsInit is
            // idempotent: it just resets the hostname + _http instance name.
            if (platform::mdnsInit(devName)) {
                std::strncpy(lastMdnsName_, devName, sizeof(lastMdnsName_) - 1);
                lastMdnsName_[sizeof(lastMdnsName_) - 1] = 0;
            }
        } else if (!shouldRun && mdnsRunning_) {
            platform::mdnsStop();
            mdnsRunning_ = false;
        }
    }

    // Map State → human label for the `mode` control. Kept here (not a static
    // table) so a new State enumerator forces a compiler error rather than
    // silently falling back to "Unknown" in the UI.
    const char* modeLabel() const {
        switch (state_) {
            case State::Idle:         return "Idle";
            case State::WaitingEth:   return "Ethernet (waiting)";
            case State::WaitingSta:   return "WiFi STA (waiting)";
            case State::ConnectedEth: return "Ethernet";
            case State::ConnectedSta: return "WiFi STA";
            case State::AP:           return "WiFi AP";
        }
        return "Unknown";
    }

    void updateMetrics() {
        std::snprintf(modeStr_, sizeof(modeStr_), "%s", modeLabel());
        if constexpr (platform::hasWiFi) {
            // rssi_ / txPower_ are hidden in non-WiFi states but we still
            // refresh them so a transition back to a WiFi state shows fresh
            // data without a one-tick stale read. Zeroing on non-WiFi states
            // avoids leaving a stale 5-minute-old reading visible if the
            // user toggles the hidden flag off via DevTools.
            rssi_ = (state_ == State::ConnectedSta)
                    ? static_cast<int8_t>(platform::wifiStaRssi()) : 0;
            const bool radioOn = (state_ == State::ConnectedSta
                                  || state_ == State::WaitingSta
                                  || state_ == State::AP);
            txPower_ = radioOn ? static_cast<int8_t>(platform::wifiTxPower()) : 0;
        }
    }

};

} // namespace mm
