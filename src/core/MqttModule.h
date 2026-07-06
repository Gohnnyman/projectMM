#pragma once

#include "core/MoonModule.h"
#include "core/MqttPacket.h"
#include "core/SystemModule.h"
#include "platform/platform.h"

#include <cstdint>

namespace mm {

/// MQTT client service: bridges the device's light controls (on / brightness / palette) to an MQTT
/// broker so home-automation hubs can drive it. The headline consumer is **Homebridge** (via the
/// `homebridge-mqttthing` "lightbulb" accessory), which publishes to `set` topics and reads `get`
/// topics — a bare on/off + brightness + colour surface. It is a network sub-service, a code-wired
/// child of NetworkModule alongside Improv/Devices, and it drives the shared apply-core exactly as
/// IR and the WLED bridge do: every command routes through `Scheduler::setControl("Drivers", …)`,
/// so MQTT adds a transport, not new control plumbing.
///
/// **The client is our own.** MQTT 3.1.1 is a small, standard protocol (the same framing mosquitto
/// and mqttthing speak), so the wire format lives in a dependency-free, golden-vector-tested header
/// (MqttPacket.h) and this module owns only the socket lifecycle over `platform::TcpConnection`
/// (`connect` + the non-blocking `read`/`writeSome`). No library — the framing is pinned by tests
/// the way the Improv frames are.
///
/// **Topics** (prefix `projectMM/<last6-of-MAC>` — a STABLE id, so a rename never repoints topics;
/// the friendly deviceName rides the separate retained `<prefix>/name` topic):
///   `<prefix>/on/set`         ← "true"/"false"        → Drivers.on
///   `<prefix>/on/get`         → publish current on
///   `<prefix>/brightness/set` ← 0..100 (mqttthing)    → *255/100 → Drivers.brightness
///   `<prefix>/brightness/get` → publish brightness*100/255
///   `<prefix>/hsv/set`        ← "h,s,v"               → hue+sat → nearest palette → Drivers.palette
///   `<prefix>/hsv/get`        → publish the chosen palette's representative "h,s,v"
///
/// **Home Assistant MQTT Discovery** (the `haDiscovery` control, default on). On connect the device
/// publishes a RETAINED JSON-schema light config to `homeassistant/light/projectMM_<mac6>/config`, so
/// HA (and any Discovery-aware hub — the Tasmota/ESPHome/Zigbee2MQTT convention) **auto-creates a
/// wired light entity** — no hand-matching topics. It then speaks HA's own schema alongside the
/// mqttthing topics above:
///   `<prefix>/ha/set`         ← `{"state":"ON"|"OFF"[,"brightness":0-255]}` → Drivers.on/brightness
///   `<prefix>/ha/state`       → retained `{"state":…,"brightness":0-255}` on change (HA-scale, no rescale)
///   `<prefix>/status`         → retained "online"; the CONNECT **Last-Will** publishes "offline" here
///                               on an ungraceful drop, so HA greys the entity out (`avty_t`)
/// Toggling `haDiscovery` re-announces / retracts live (an empty retained config removes the entity),
/// no reconnect. `uniq_id` is the MAC-stable `projectMM_<mac6>`, never the editable name. JSON schema
/// (not the default schema) so future controls add a key — HA's native `effect`/`effect_list` maps a
/// preset/effect picker with no new topic.
///
/// **Lifecycle** (all on loop1s(), off the render hot path — MQTT is slow control): connect lazily
/// once `networkReady() && enabled`, CONNECT → CONNACK → SUBSCRIBE to the `set` topics, PINGREQ every
/// keepalive/2, drain inbound bytes through MqttInboundParser and route PUBLISHes to Drivers, and
/// publish the `get` topics whenever the local value changes (and on connect, so mqttthing never
/// reads "No Response"). A dropped socket reconnects with a backoff.
///
/// **Prior art:** the OASIS MQTT 3.1.1 standard, homebridge-mqttthing's topic conventions, and Home
/// Assistant's MQTT-discovery format (the same retained-`homeassistant/…/config` announce Tasmota /
/// ESPHome / Zigbee2MQTT use). projectMM writes its own lean client over the platform socket
/// primitive rather than a framework MQTT library. See docs/moonmodules/core/services.md#mqtt for the
/// Homebridge accessory config; docs/usecases/home-automation.md for the HA setup.
/// @card MqttModule.png
class MqttModule : public MoonModule {
public:
    void setSystemModule(SystemModule* s) { systemModule_ = s; }

    void setup() override;
    void teardown() override;                          // free the lazily-allocated discovery buffers
    void onBuildControls() override;
    void onUpdate(const char* controlName) override;   // a broker/port/cred change re-homes the socket
    void onEnabled(bool enabled) override;             // enable/disable → connect / clean DISCONNECT
    void loop1s() override;

    /// Feed inbound bytes as if they arrived from the broker socket — the entry the host unit tests
    /// drive (there's no live broker in ctest). Mirrors IrModule::injectCodeForTest.
    void feedForTest(const uint8_t* bytes, size_t len);

    /// Test seam: capture every outbound packet sendPacket() writes, so a unit test can assert what
    /// the module emits (e.g. the retained discovery config on CONNACK) — there's no live socket in
    /// ctest. Enable before the exercise; read back the concatenated bytes. Off in production (the
    /// capture buffer is null).
    void enableSendCaptureForTest(uint8_t* buf, size_t cap);
    size_t sentCaptureLenForTest() const { return sendCaptureLen_; }

private:
    // Connection state machine — advanced by loop1s(). ConnectingTcp = a non-blocking TCP connect is
    // in flight (polled, never blocks the tick); Connecting = TCP up, CONNECT sent, awaiting CONNACK.
    enum class Conn : uint8_t { Idle, ConnectingTcp, Connecting, Connected };

    void startConnect();                    // begin a non-blocking TCP connect (ConnectingTcp)
    void sendConnectPacket();               // TCP up → send CONNECT, go to Connecting
    void serviceConnected();                // drain inbound, keepalive, publish-on-change
    bool sendPacket(const uint8_t* data, size_t len);   // non-blocking atomic send (true = fully sent)
    void resetConnection(const char* status);   // close socket + back to Idle with a status line
    void handleInboundByte(uint8_t byte);   // feed the parser, route a completed PUBLISH
    void routePublish(const char* topic, const uint8_t* payload, size_t payloadLen);
    void publishState(bool force);          // publish get topics when local values changed
    void publishName();                     // publish the friendly deviceName on the retained name topic
    void maybeRepublishName();              // re-publish the name if it changed (rename while connected)
    void setControlValue(const char* control, const char* valueJson);   // → Scheduler::setControl
    void setStatusLine(const char* msg);

    // Home Assistant MQTT Discovery (JSON schema). When haDiscovery_ is on, the device announces a
    // retained `homeassistant/light/<id>/config` so HA auto-creates a wired light entity; it then
    // speaks HA's own JSON schema on <prefix>/ha/{set,state} alongside the mqttthing on/set etc.
    void buildDiscoveryTopic(char* out, size_t cap) const;   // homeassistant/light/projectMM_<mac6>/config
    void buildStatusTopic(char* out, size_t cap) const;      // <prefix>/status — the LWT availability topic
    void publishDiscovery(bool announce);   // announce=false publishes an empty retained config (retract)
    void subscribeHaSet();                  // SUBSCRIBE to <prefix>/ha/set (at CONNACK + on live toggle-on)

    SystemModule* systemModule_ = nullptr;

    // The topic prefix is DERIVED from a STABLE hardware id: projectMM/<last6-of-MAC>. Not stored (no
    // buffer). The MAC is fixed for the chip's life, so a device rename never changes the topics —
    // external integrations stay pinned (the WLED/Tasmota/HA convention). The friendly display name
    // is a separate concern, published on the retained `name` topic (publishName). buildTopic writes
    // <prefix>/<suffix>.
    const char* prefixRoot_ = "projectMM";
    void topicPrefix(char* out, size_t cap) const;
    void buildTopic(char* out, size_t cap, const char* suffix) const;

    // Controls (persisted).
    char     broker_[64]   = "";          // hostname or IP of the broker
    uint16_t port_         = 1883;
    char     username_[48] = "";
    char     password_[48] = "";
    bool     haDiscovery_  = true;        // announce a HA MQTT-discovery light (opt-out); see publishDiscovery
    char     statusStr_[64] = "disabled";

    platform::TcpConnection conn_;
    MqttInboundParser parser_;
    Conn     state_ = Conn::Idle;
    uint32_t lastPingSent_   = 0;
    uint32_t lastActivity_   = 0;         // last inbound byte or successful send (for keepalive)
    uint32_t lastConnectTry_ = 0;         // backoff clock for reconnects
    uint32_t connectStartedMs_ = 0;       // when the current TCP-connect / CONNACK-wait began
    uint32_t nameSig_        = 0;          // signature of the last-published friendly name (rename detect)
    uint16_t nextPacketId_   = 1;

    // Last-published state, so publishState only emits on change.
    bool    lastOn_    = false;
    uint8_t lastBri_   = 0;
    uint8_t lastPalette_ = 0xFF;          // 0xFF = "never published" → force first publish
    bool    havePublished_ = false;

    bool     lastConnectFailed_ = false;  // widen the backoff after a failure (esp. a slow DNS lookup)

    // Discovery-config scratch — HEAP, allocated lazily only when discovery actually publishes
    // (connected + haDiscovery on), freed in teardown() and when discovery is turned off, per the
    // pay-for-what-you-use rule (architecture.md § Memory strategy): a device that has the MQTT module
    // but never enables HA discovery pays ZERO for it. Heap (not a fixed member) also keeps the ~360 B
    // config frame off the shared 8 KB main-task stack (the P4 registerType-stack lesson). Two regions
    // because buildMqttPublish needs payload + output separate: the JSON builds into discoveryPayload_,
    // the framed packet into discoveryBuf_.
    static constexpr size_t kDiscoveryPayloadLen = 320;
    static constexpr size_t kDiscoveryBufLen     = 448;
    char*    discoveryPayload_ = nullptr;
    uint8_t* discoveryBuf_     = nullptr;
    bool ensureDiscoveryBuffers();   // lazily alloc both; false on OOM. Sets dynamicBytes.
    void freeDiscoveryBuffers();     // free both + dynamicBytes(0). Called on teardown / discovery-off.

    // Test-only outbound capture (null in production). sendPacket appends every emitted packet here.
    uint8_t* sendCapture_    = nullptr;
    size_t   sendCaptureCap_ = 0;
    size_t   sendCaptureLen_ = 0;

    static constexpr uint16_t kKeepaliveSec = 30;
    static constexpr uint32_t kReconnectBackoffMs = 5000;       // after a clean disconnect
    static constexpr uint32_t kFailedBackoffMs    = 30000;      // after a failed attempt — a bad
        // hostname re-runs a synchronous getaddrinfo each try (a brief tick stall), so back off harder
    static constexpr uint32_t kConnectTimeoutMs = 8000;   // overall TCP-connect + CONNACK-wait bound
};

} // namespace mm
