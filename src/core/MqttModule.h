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
/// **Lifecycle** (all on loop1s(), off the render hot path — MQTT is slow control): connect lazily
/// once `networkReady() && enabled`, CONNECT → CONNACK → SUBSCRIBE to the `set` topics, PINGREQ every
/// keepalive/2, drain inbound bytes through MqttInboundParser and route PUBLISHes to Drivers, and
/// publish the `get` topics whenever the local value changes (and on connect, so mqttthing never
/// reads "No Response"). A dropped socket reconnects with a backoff.
///
/// **Prior art:** the OASIS MQTT 3.1.1 standard + homebridge-mqttthing's topic conventions (see
/// docs/moonmodules/core/ui/ui.md#mqtt for the Homebridge accessory config). The MoonLight sibling
/// bridges the same on/off+brightness surface through a full framework MQTT client + HA discovery;
/// projectMM writes its own lean client over the platform socket primitive instead.
class MqttModule : public MoonModule {
public:
    void setSystemModule(SystemModule* s) { systemModule_ = s; }

    void setup() override;
    void onBuildControls() override;
    void onUpdate(const char* controlName) override;   // a broker/port/cred change re-homes the socket
    void onEnabled(bool enabled) override;             // enable/disable → connect / clean DISCONNECT
    void loop1s() override;

    /// Feed inbound bytes as if they arrived from the broker socket — the entry the host unit tests
    /// drive (there's no live broker in ctest). Mirrors IrModule::injectCodeForTest.
    void feedForTest(const uint8_t* bytes, size_t len);

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

    static constexpr uint16_t kKeepaliveSec = 30;
    static constexpr uint32_t kReconnectBackoffMs = 5000;       // after a clean disconnect
    static constexpr uint32_t kFailedBackoffMs    = 30000;      // after a failed attempt — a bad
        // hostname re-runs a synchronous getaddrinfo each try (a brief tick stall), so back off harder
    static constexpr uint32_t kConnectTimeoutMs = 8000;   // overall TCP-connect + CONNACK-wait bound
};

} // namespace mm
