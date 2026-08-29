#pragma once

// OSC control ingest: turns a fader move in Resolume, TouchDesigner, TouchOSC or a DIY
// Arduino-over-Ethernet rig into a control write on this device.
//
// It owns no surface of its own. ControlModule already has the pads, encoders and faders, laid out
// to match a Mackie-style desk, and everything here lands in Scheduler::setControl, the same entry
// point the HTTP API and the UI use. So OSC gains no privilege: every validator still runs, and
// there is no second copy of the device's state to keep in step.
//
// Addresses (the public contract, so they stay small and boring):
//
//   /mm/fader/1                     f 0..1 or i 0..255  ->  ControlModule fader1
//   /mm/enc/3                       f 0..1 or i 0..255  ->  ControlModule enc3
//   /mm/control/Drivers/brightness  f 0..1 or i 0..255  ->  that module's control directly
//
// The /mm/control/ form is what makes projectMM useful to TouchDesigner on day one, without
// waiting for someone to bind a fader.
//
// NOT here: sending OSC (nothing we own consumes it: the X-Touch and QCon are Mackie desks, see
// reference/control-surfaces.md), bundles, and address wildcards. See the plan.
// Author: projectMM original

#include "core/MoonModule.h"
#include "core/OscPacket.h"
#include "core/Scheduler.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

/// Service: receives OSC and writes it onto the device's controls.
/// @card OscModule.png
class OscModule : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Off by default: this opens an unauthenticated UDP port that writes controls, so it is a
    /// capability a user turns on rather than one every device carries.
    bool enabledOsc = false;
    uint16_t port = osc::kDefaultPort;

    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("listen", enabledOsc);
        controls_.addControl("port", port, 1, 65535);
        controls_.addReadOnly("status", statusStr_, sizeof(statusStr_));
    }

    void onControlChanged(const char* name) override {
        // A port or listen change reopens the socket: the setting applies live, no reboot
        // (architecture.md's live-reconfiguration rule).
        if (std::strcmp(name, "port") == 0 || std::strcmp(name, "listen") == 0) closeSocket();
    }

    void release() override { closeSocket(); }

    void tick() MM_NONBLOCKING override {
        if constexpr (!platform::hasNetwork) return;
        if (!enabledOsc) { if (open_) closeSocket(); return; }
        if (!ensureSocket()) return;

        // Bounded non-blocking drain, the shape AudioService::syncReceive uses: a desk moves a
        // handful of controls per frame, so a small cap keeps a flood from owning the tick.
        uint8_t pkt[kMaxPacket];
        for (int i = 0; i < kMaxPerTick; i++) {
            const int n = sock_.recvFrom(pkt, sizeof(pkt));
            if (n <= 0) break;                       // -1 = nothing pending
            handle(pkt, static_cast<size_t>(n));
        }
    }

private:
    static constexpr long   kSurfaceWidth = 8;    // ControlModule's fader/encoder count
    static constexpr size_t kMaxPacket   = 256;   // an address plus a few args; controllers send far less
    static constexpr int    kMaxPerTick  = 16;
    static constexpr uint32_t kOpenRetryMs = 2000;

    /// Route one datagram. Unknown addresses are ignored rather than reported: a controller
    /// blasting its whole layout at us must not fill the log or slow the tick.
    void handle(const uint8_t* pkt, size_t len) {
        osc::Message m;
        if (!osc::parse(pkt, len, m)) return;
        const char* a = m.address;

        if (std::strncmp(a, "/mm/fader/", 10) == 0) {
            writeSurface("fader", a + 10, m);
        } else if (std::strncmp(a, "/mm/enc/", 8) == 0) {
            writeSurface("enc", a + 8, m);
        } else if (std::strncmp(a, "/mm/control/", 12) == 0) {
            writeControl(a + 12, m);
        }
        received_++;
    }

    /// `/mm/fader/N` and `/mm/enc/N`: write ControlModule's own control, so the surface reacts
    /// exactly as it does to a click in the UI and driveFader routes it onward.
    void writeSurface(const char* prefix, const char* indexText, const osc::Message& m) {
        // strtol, not atoi: this is unvalidated network input, and atoi cannot tell "0" from
        // "not a number at all". `end` also rejects trailing junk, so /mm/fader/1x is not a fader.
        char* end = nullptr;
        const long idx = std::strtol(indexText, &end, 10);
        if (end == indexText || *end != '\0') return;
        if (idx < 1 || idx > kSurfaceWidth) return;  // the surface is 8 wide; anything else is not ours
        char control[16];
        std::snprintf(control, sizeof(control), "%s%ld", prefix, idx);
        setValue("Control", control, osc::toByte(m));
    }

    /// `/mm/control/<Module>/<control>`: reach any control directly. The module and control names
    /// are taken verbatim, so a typo simply does not resolve, exactly as it would over HTTP.
    void writeControl(const char* rest, const osc::Message& m) {
        const char* slash = std::strchr(rest, '/');
        if (!slash || slash == rest || !slash[1]) return;
        char module[24];
        const size_t n = static_cast<size_t>(slash - rest);
        if (n >= sizeof(module)) return;
        std::memcpy(module, rest, n);
        module[n] = '\0';
        setValue(module, slash + 1, osc::toByte(m));
    }

    void setValue(const char* module, const char* control, uint8_t value) {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(value));
        sched->setControl(module, control, body);
    }

    /// Open + bind, deferred to the tick path and throttled on failure: the same shape
    /// AudioService uses, so a boot-present module cannot touch lwip before the stack is up and a
    /// busy port cannot burn a socket per tick.
    bool ensureSocket() {
        if (open_) return true;
        if (!platform::networkReady()) return false;
        const uint32_t now = platform::millis();
        if (lastFailMs_ != 0 && now - lastFailMs_ < kOpenRetryMs) return false;
        if (sock_.open() && sock_.bind(port)) {
            open_ = true;
            lastFailMs_ = 0;
            std::snprintf(statusStr_, sizeof(statusStr_), "listening on %u", static_cast<unsigned>(port));
            return true;
        }
        sock_.close();
        lastFailMs_ = now == 0 ? 1 : now;
        std::snprintf(statusStr_, sizeof(statusStr_), "port %u busy", static_cast<unsigned>(port));
        return false;
    }

    void closeSocket() {
        if (open_) sock_.close();
        open_ = false;
        lastFailMs_ = 0;
        std::snprintf(statusStr_, sizeof(statusStr_), enabledOsc ? "opening" : "off");
    }

    platform::UdpSocket sock_;
    bool     open_ = false;
    uint32_t lastFailMs_ = 0;
    uint32_t received_ = 0;
    char     statusStr_[24] = "off";
};

} // namespace mm
