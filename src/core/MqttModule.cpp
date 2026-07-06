#include "core/MqttModule.h"

#include "core/Scheduler.h"     // setControl — the shared apply-core
#include "core/JsonUtil.h"      // json::hasKey/parseBool/parseInt/parseString — the inbound ha/set parse
                                // (same flat helpers HttpServerModule::applyWledState uses; no arena)
#include "light/Palette.h"      // Palettes::nearestForHue — a pure hue/sat→index CONVERSION with no
                                // light state or objects, the one narrow reach this core module makes
                                // into the light domain. PO-accepted: routing a HomeKit colour to a
                                // palette needs the palette set, which is inherently light-domain, and
                                // a format conversion is the least-coupling way to bridge it (the
                                // module still drives the palette via Scheduler::setControl, not a
                                // light object). Deliberate divergence from the plan's "no light
                                // include" line, made with the trade-off understood, not by precedent.

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

namespace {
// A scratch send buffer big enough for any control packet we build (CONNECT with auth is the
// largest at well under 200 bytes). Sends go through sendPacket (non-blocking writeSome).
constexpr size_t kSendBufLen = 256;
}  // namespace

// The topic prefix, derived live from a STABLE hardware id: projectMM/<last6-of-MAC> (lowercase
// hex), e.g. projectMM/563cfe. Not stored (no buffer). The MAC is fixed for the chip's life, so a
// device RENAME never changes the topics — external integrations (Homebridge, Home Assistant) stay
// pinned. This is the WLED / Tasmota / HA-discovery convention: the MQTT identity is the hardware
// id, and the friendly *display* name is a separate concern (published on the `name` topic, read
// from deviceName()). The last 6 hex is the same short-id WLED uses (`wled/<last6>`).
void MqttModule::topicPrefix(char* out, size_t cap) const {
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    std::snprintf(out, cap, "%s/%02x%02x%02x", prefixRoot_, mac[3], mac[4], mac[5]);
}

// A full topic: <prefix>/<suffix>, e.g. projectMM/563cfe/on/set.
void MqttModule::buildTopic(char* out, size_t cap, const char* suffix) const {
    char prefix[96];
    topicPrefix(prefix, sizeof(prefix));
    std::snprintf(out, cap, "%s/%s", prefix, suffix);
}

// The HA MQTT-discovery config topic: homeassistant/light/projectMM_<mac6>/config. Independent of
// topicPrefix() — the discovery prefix is HA's `homeassistant`, not our `projectMM` root, and the
// object id carries the projectMM_ prefix so the id is unique across vendors on a shared broker.
void MqttModule::buildDiscoveryTopic(char* out, size_t cap) const {
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    std::snprintf(out, cap, "homeassistant/light/%s_%02x%02x%02x/config",
                  prefixRoot_, mac[3], mac[4], mac[5]);
}

// The availability (LWT) topic: <prefix>/status. The broker publishes the retained "offline" Will
// here on an ungraceful drop; the module publishes retained "online" on connect. HA's avty_t points
// at it, so the entity greys out when the device disappears.
void MqttModule::buildStatusTopic(char* out, size_t cap) const {
    buildTopic(out, cap, "status");
}

// Lazily allocate the two discovery scratch buffers from the heap the first time discovery publishes,
// and report them via setDynamicBytes so the UI's per-module memory line accounts for them. A device
// that never enables HA discovery never calls this → zero bytes. Returns false on OOM (the caller then
// skips the publish rather than deref a null — the module keeps running, discovery just doesn't announce).
bool MqttModule::ensureDiscoveryBuffers() {
    if (discoveryBuf_ && discoveryPayload_) return true;
    if (!discoveryBuf_)     discoveryBuf_     = static_cast<uint8_t*>(platform::alloc(kDiscoveryBufLen));
    if (!discoveryPayload_) discoveryPayload_ = static_cast<char*>(platform::alloc(kDiscoveryPayloadLen));
    if (!discoveryBuf_ || !discoveryPayload_) { freeDiscoveryBuffers(); return false; }
    setDynamicBytes(kDiscoveryBufLen + kDiscoveryPayloadLen);
    return true;
}

void MqttModule::freeDiscoveryBuffers() {
    if (discoveryBuf_)     { platform::free(discoveryBuf_);     discoveryBuf_ = nullptr; }
    if (discoveryPayload_) { platform::free(discoveryPayload_); discoveryPayload_ = nullptr; }
    setDynamicBytes(0);
}

void MqttModule::publishDiscovery(bool announce) {
    // Retract (OFF): free the buffers unconditionally — freeing local memory needs no socket, so this
    // runs even while disconnected/reconnecting (else a discovery-off toggle mid-reconnect would strand
    // the buffers until teardown, breaking "no memory when discovery is off"). When we happen to be
    // connected we ALSO send the empty retained payload so HA removes the entity; when not, the buffers
    // still free and the entity clears on the broker's own retained-message expiry / next connect.
    if (!announce) {
        if (state_ == Conn::Connected && discoveryBuf_) {
            char topic[96];
            buildDiscoveryTopic(topic, sizeof(topic));
            const size_t n = buildMqttPublish(topic, nullptr, 0, discoveryBuf_, kDiscoveryBufLen,
                                              /*retain=*/true);
            if (n != 0) sendPacket(discoveryBuf_, n);
        }
        freeDiscoveryBuffers();
        return;
    }

    // Announce path needs a live socket (there's nothing to publish to otherwise) and allocates.
    if (state_ != Conn::Connected) return;
    if (!ensureDiscoveryBuffers()) { setStatusLine("error: discovery alloc failed"); return; }

    char topic[96];
    buildDiscoveryTopic(topic, sizeof(topic));

    // Identity + display name. uniq_id/object_id derive from the stable MAC; name is the friendly
    // deviceName (a rename updates the label, never the id — see the topic-identity decision).
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    char id[24];
    std::snprintf(id, sizeof(id), "%s_%02x%02x%02x", prefixRoot_, mac[3], mac[4], mac[5]);
    const char* dn = systemModule_ ? systemModule_->deviceName() : nullptr;
    if (!dn || !dn[0]) dn = id;

    char cmd[80], stat[80], avty[80];
    buildTopic(cmd,  sizeof(cmd),  "ha/set");
    buildTopic(stat, sizeof(stat), "ha/state");
    buildStatusTopic(avty, sizeof(avty));

    // JSON-schema MQTT light. Abbreviated keys (HA's documented short forms). brightness at the
    // default 0-255 scale (no scale key needed). dev{} groups the entity under a device card in HA.
    const int pn = std::snprintf(discoveryPayload_, kDiscoveryPayloadLen,
        "{\"schema\":\"json\",\"name\":\"%s\",\"uniq_id\":\"%s\",\"cmd_t\":\"%s\","
        "\"stat_t\":\"%s\",\"avty_t\":\"%s\",\"brightness\":true,"
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"MoonModules\",\"mdl\":\"projectMM\"}}",
        dn, id, cmd, stat, avty, id, dn);
    if (pn <= 0 || static_cast<size_t>(pn) >= kDiscoveryPayloadLen) return;   // truncated → don't send a broken config

    const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(discoveryPayload_),
                                      static_cast<size_t>(pn), discoveryBuf_, kDiscoveryBufLen,
                                      /*retain=*/true);
    if (n == 0) { setStatusLine("error: discovery config too large"); return; }
    sendPacket(discoveryBuf_, n);
}

// SUBSCRIBE to <prefix>/ha/set — the HA-native JSON command topic. Called at CONNACK (with the
// mqttthing set-topics) and on a mid-session haDiscovery turn-on (subscriptions otherwise only happen
// once at CONNACK).
void MqttModule::subscribeHaSet() {
    if (state_ != Conn::Connected) return;
    char topic[96];
    buildTopic(topic, sizeof(topic), "ha/set");
    uint8_t buf[kSendBufLen];
    const size_t n = buildMqttSubscribe(nextPacketId_++, topic, buf, sizeof(buf));
    if (n != 0) sendPacket(buf, n);
}

void MqttModule::setup() {
    setStatusLine(enabled() ? "idle" : "disabled");
    MoonModule::setup();
}

// Release the lazily-allocated discovery buffers when the module is torn down (deleted from the tree,
// or on device shutdown). The socket is closed via the normal reset path; MoonModule::teardown()
// recurses to children (this module has none). No memory outlives the module.
void MqttModule::teardown() {
    freeDiscoveryBuffers();
    MoonModule::teardown();
}

void MqttModule::onBuildControls() {
    controls_.addText("broker", broker_, sizeof(broker_));
    controls_.addUint16("port", port_, 1, 65535);
    controls_.addText("username", username_, sizeof(username_));
    controls_.addPassword("password", password_, sizeof(password_));
    controls_.addBool("haDiscovery", haDiscovery_);   // announce a HA-discovery light (default on)
    controls_.addReadOnly("mqtt_status", statusStr_, sizeof(statusStr_));
    MoonModule::onBuildControls();
}

// A broker/port/credentials change re-homes the connection: drop the socket so loop1s reconnects
// with the new settings on the next tick. Scoped to THIS module's controls via onUpdate (not the
// whole-tree onBuildState sweep), so an unrelated change — a grid resize, a layout edit — never
// drops the MQTT connection. Live, no reboot.
void MqttModule::onUpdate(const char* controlName) {
    if (std::strcmp(controlName, "broker") == 0 || std::strcmp(controlName, "port") == 0 ||
        std::strcmp(controlName, "username") == 0 || std::strcmp(controlName, "password") == 0) {
        resetConnection(enabled() ? "reconnecting" : "disabled");
    } else if (std::strcmp(controlName, "haDiscovery") == 0) {
        // Announce or retract live — NO reset (bouncing the socket to change a discovery flag is
        // needless). On a turn-ON mid-session also SUBSCRIBE (subscriptions otherwise only fire at
        // CONNACK); on turn-OFF the retract clears the HA entity. Publishes only when connected.
        publishDiscovery(haDiscovery_);
        if (haDiscovery_) { subscribeHaSet(); publishState(true); }
    }
}

// Enable/disable transition. On disable we send a clean DISCONNECT + close (rather than leaving a
// dangling socket for the broker to time out) — loop1s stops being called once disabled, so this
// transition hook is the only place a disable can act.
void MqttModule::onEnabled(bool enabled) {
    if (!enabled && conn_.valid()) {
        uint8_t buf[4];
        const size_t n = buildMqttDisconnect(buf, sizeof(buf));
        if (n) sendPacket(buf, n);   // best-effort courtesy DISCONNECT
    }
    if (!enabled) freeDiscoveryBuffers();   // MQTT turned off → not used → reclaim the discovery heap
    resetConnection(enabled ? "idle" : "disabled");
}

// Send a whole MQTT packet without EVER blocking the render loop. Uses writeSome (non-blocking): a
// control packet is ≤256 B, far under the socket send buffer, so a healthy socket accepts it all in
// one call. A partial or zero write means the buffer is backing up (a zero-window / stalled broker) —
// return false so the caller resets the connection rather than spin-retrying forever, which is what
// the blocking write() would do inside loop1s (the hot-path violation this avoids).
bool MqttModule::sendPacket(const uint8_t* data, size_t len) {
    if (len == 0) return true;
    // Test seam: mirror the outbound bytes into the capture buffer (null in production) so a unit test
    // can assert what the module emits — there's no live socket in ctest, and writeSome returns -1.
    if (sendCapture_) {
        if (sendCaptureLen_ + len <= sendCaptureCap_) {
            std::memcpy(sendCapture_ + sendCaptureLen_, data, len);
            sendCaptureLen_ += len;
        }
        return true;   // capture mode always "succeeds" so the connect/publish flow proceeds in tests
    }
    const int sent = conn_.writeSome(data, len);
    return sent == static_cast<int>(len);   // all-or-fail; a partial/0/-1 is a connection problem
}

void MqttModule::enableSendCaptureForTest(uint8_t* buf, size_t cap) {
    sendCapture_ = buf; sendCaptureCap_ = cap; sendCaptureLen_ = 0;
}

// Close the socket and return to Idle with a status line — the single reset path so every caller
// (reconfig, disable, timeout, protocol error, peer close) leaves the same clean state.
void MqttModule::resetConnection(const char* status) {
    conn_.close();
    state_ = Conn::Idle;
    havePublished_ = false;
    setStatusLine(status);
}

void MqttModule::loop1s() {
    if constexpr (!platform::hasNetwork) { MoonModule::loop1s(); return; }

    if (!enabled() || broker_[0] == '\0') {
        if (conn_.valid()) resetConnection(enabled() ? "idle" : "disabled");
        MoonModule::loop1s();
        return;
    }
    if (!platform::networkReady()) { MoonModule::loop1s(); return; }

    const uint32_t now = platform::millis();
    switch (state_) {
        case Conn::Idle: {
            // Backoff between connect attempts so a down broker isn't hammered every tick. A prior
            // FAILURE (unreachable broker, bad hostname → a synchronous getaddrinfo each try) backs
            // off harder to keep the recurring DNS stall rare.
            const uint32_t backoff = lastConnectFailed_ ? kFailedBackoffMs : kReconnectBackoffMs;
            if (now - lastConnectTry_ >= backoff || lastConnectTry_ == 0) {
                lastConnectTry_ = now;
                startConnect();
            }
            break;
        }
        case Conn::ConnectingTcp: {
            // Poll the non-blocking TCP connect — never blocks the tick.
            const auto r = conn_.connectPoll();
            if (r == platform::TcpConnection::ConnectResult::Connected) sendConnectPacket();
            else if (r == platform::TcpConnection::ConnectResult::Failed) resetConnection("error: connect failed");
            else if (now - connectStartedMs_ >= kConnectTimeoutMs) resetConnection("error: connect timeout");
            break;
        }
        case Conn::Connecting:
            // TCP up, CONNECT sent, waiting for CONNACK. A broker that accepts TCP but never CONNACKs
            // (finding: the silent-broker wedge) is bounded by the same connect timeout.
            serviceConnected();
            if (state_ == Conn::Connecting && now - connectStartedMs_ >= kConnectTimeoutMs)
                resetConnection("error: no CONNACK");
            break;
        case Conn::Connected:
            serviceConnected();
            break;
    }
    MoonModule::loop1s();
}

// Begin a NON-BLOCKING TCP connect (getaddrinfo + connect kicked off, returns immediately). loop1s
// polls it in ConnectingTcp so the render loop never stalls on an unreachable broker.
void MqttModule::startConnect() {
    setStatusLine("connecting");
    // Assume failure until a full connect succeeds (cleared in the CONNACK-accepted path). Every
    // failure route resets to Idle without clearing this, so the next Idle uses the longer backoff.
    lastConnectFailed_ = true;
    if (!conn_.connectStart(broker_, port_)) {   // immediate failure (DNS / socket)
        resetConnection("error: connect failed");
        return;
    }
    connectStartedMs_ = platform::millis();
    state_ = Conn::ConnectingTcp;
}

// TCP is up — send CONNECT and wait for CONNACK.
void MqttModule::sendConnectPacket() {
    uint8_t buf[kSendBufLen];
    const char* user = username_[0] ? username_ : nullptr;
    const char* pass = password_[0] ? password_ : nullptr;
    // A stable, slash-free clientId (MQTT-3.1.3-5 allows only [0-9a-zA-Z], and a broker may reject a
    // '/'): "projectMM-<last6-of-MAC>", alphanumeric + one hyphen. NOT topicPrefix() — that carries a
    // slash. Same MAC identity as the topics, just without the path separator.
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    char clientId[32];
    std::snprintf(clientId, sizeof(clientId), "projectMM-%02x%02x%02x", mac[3], mac[4], mac[5]);
    // Last Will: retained "offline" on <prefix>/status. The broker publishes it if we drop
    // ungracefully (power cut, WiFi loss), so HA's avty_t greys the entity out. Declared here at
    // CONNECT; we publish the retained "online" ourselves once CONNACK lands (handleInboundByte).
    char willTopic[96];
    buildStatusTopic(willTopic, sizeof(willTopic));
    const size_t n = buildMqttConnect(clientId, user, pass, kKeepaliveSec, buf, sizeof(buf),
                                      willTopic, "offline", /*willRetain=*/true);
    if (n == 0 || !sendPacket(buf, n)) {
        resetConnection("error: connect send failed");
        return;
    }
    parser_ = MqttInboundParser{};       // fresh parser per connection
    state_ = Conn::Connecting;           // waiting for CONNACK
    connectStartedMs_ = platform::millis();
    lastActivity_ = connectStartedMs_;
    lastPingSent_ = connectStartedMs_;
}

void MqttModule::serviceConnected() {
    // Drain whatever the socket has (non-blocking). A bounded read per tick keeps this cheap.
    uint8_t rx[256];
    for (int pass = 0; pass < 8; pass++) {
        const int n = conn_.read(rx, sizeof(rx));
        if (n > 0) {
            lastActivity_ = platform::millis();
            for (int i = 0; i < n; i++) handleInboundByte(rx[i]);
            if (state_ == Conn::Idle) return;   // handleInboundByte reset us (refused / malformed)
        } else if (n == 0) {                    // peer closed
            resetConnection("disconnected");
            return;
        } else {
            break;                              // -1 = nothing pending right now
        }
    }

    if (state_ == Conn::Connected) {
        publishState(false);                 // emit any changed get topics
        // Re-publish the friendly name if the device was renamed while connected (topics are stable,
        // but the display label should follow). Cheap change-detect via a rolling signature — no
        // stored name buffer. publishName is a no-op if unchanged.
        maybeRepublishName();

        // Keepalive: PINGREQ at keepalive/2. If the broker goes silent past ~keepalive*1.5, drop.
        const uint32_t now = platform::millis();
        if (now - lastPingSent_ >= (kKeepaliveSec * 1000u) / 2) {
            uint8_t ping[2];
            const size_t pn = buildMqttPingreq(ping, sizeof(ping));
            if (pn == 0 || !sendPacket(ping, pn)) { resetConnection("error: ping failed"); return; }
            lastPingSent_ = now;
        }
        if (now - lastActivity_ >= kKeepaliveSec * 1500u)    // 1.5× keepalive with no traffic
            resetConnection("timeout");
    }
}

void MqttModule::handleInboundByte(uint8_t byte) {
    const MqttFeedResult r = parser_.feed(byte);
    // A malformed / oversize packet desyncs the byte stream for the connection's life (MQTT 3.1.1
    // §4.8: a protocol violation MUST close the connection). Drop and reconnect rather than reinterpret
    // mid-body garbage as fixed headers.
    if (r == MqttFeedResult::Malformed) { resetConnection("error: bad packet"); return; }
    if (r != MqttFeedResult::PacketReady) return;

    const uint8_t type = parser_.lastType();
    if (type == static_cast<uint8_t>(MqttPacketType::Connack)) {
        // CONNACK body is [session-present][return-code] (§3.2). A short body is a protocol violation
        // — treat as a failed connect, don't fall through and subscribe on a malformed accept.
        if (parser_.bodyLen() < 2) { resetConnection("error: bad CONNACK"); return; }
        // Non-zero return code = the broker refused (bad auth, unavailable, …).
        if (parser_.body()[1] != 0) {
            resetConnection("error: broker refused");
            return;
        }
        // Subscribe to <prefix>/+/set with three explicit filters (one SUBSCRIBE each — simple).
        static const char* kSets[] = {"on/set", "brightness/set", "hsv/set"};
        char topic[128];
        for (const char* suffix : kSets) {
            buildTopic(topic, sizeof(topic), suffix);
            uint8_t buf[kSendBufLen];
            const size_t n = buildMqttSubscribe(nextPacketId_++, topic, buf, sizeof(buf));
            if (n == 0 || !sendPacket(buf, n)) { resetConnection("error: subscribe failed"); return; }
        }
        state_ = Conn::Connected;
        lastConnectFailed_ = false;          // full success → next reconnect uses the short backoff
        setStatusLine("connected");
        havePublished_ = false;
        publishName();                       // retained friendly name so a hub shows the display name
        // Availability: publish retained "online" to <prefix>/status (the LWT's counterpart — the
        // broker publishes "offline" if we drop). Must precede the discovery announce so HA sees the
        // entity available the instant its config lands.
        {
            char st[96]; buildStatusTopic(st, sizeof(st));
            uint8_t sb[kSendBufLen];
            const size_t sn = buildMqttPublish(st, reinterpret_cast<const uint8_t*>("online"), 6,
                                               sb, sizeof(sb), /*retain=*/true);
            if (sn != 0) sendPacket(sb, sn);
        }
        if (haDiscovery_) { publishDiscovery(true); subscribeHaSet(); }
        publishState(true);                  // publish initial state so mqttthing + HA show it
    } else if (type == static_cast<uint8_t>(MqttPacketType::Publish)) {
        const char* topic = nullptr; const uint8_t* payload = nullptr; size_t plLen = 0;
        if (parser_.publish(&topic, &payload, &plLen)) routePublish(topic, payload, plLen);
    }
    // PINGRESP / SUBACK: nothing to do beyond the activity timestamp already stamped.
}

void MqttModule::routePublish(const char* topic, const uint8_t* payload, size_t payloadLen) {
    // Match the topic suffix after our (derived) prefix. A short fixed payload is copied
    // NUL-terminated so the parsers below (strcmp / atoi) are safe on the non-terminated socket slice.
    char prefix[96];
    topicPrefix(prefix, sizeof(prefix));
    const size_t prefixLen = std::strlen(prefix);
    if (std::strncmp(topic, prefix, prefixLen) != 0 || topic[prefixLen] != '/') return;
    const char* suffix = topic + prefixLen + 1;

    // HA-native JSON command: {"state":"ON"|"OFF"[,"brightness":0-255]}. Parsed with the same flat
    // mm::json helpers HttpServerModule::applyWledState uses (key-order-independent, whitespace-safe);
    // needs a bigger NUL-terminated buffer than the scalar `value[32]` below. HA brightness is already
    // 0-255, so no rescale (unlike the mqttthing brightness/set 0-100 path).
    if (std::strcmp(suffix, "ha/set") == 0) {
        char body[128];
        const size_t blen = payloadLen < sizeof(body) - 1 ? payloadLen : sizeof(body) - 1;
        std::memcpy(body, payload, blen);
        body[blen] = '\0';
        if (json::hasKey(body, "state")) {
            char st[8] = "";
            json::parseString(body, "state", st, sizeof(st));
            const bool on = (std::strcmp(st, "ON") == 0);
            setControlValue("on", on ? "{\"value\":true}" : "{\"value\":false}");
        }
        if (json::hasKey(body, "brightness")) {
            int bri = json::parseInt(body, "brightness");
            if (bri < 0) bri = 0;
            if (bri > 255) bri = 255;
            char json[24];
            std::snprintf(json, sizeof(json), "{\"value\":%d}", bri);
            setControlValue("brightness", json);
        }
        // Future: an "effect" key → setControlValue("palette"/"effect", …) with effect_list in the config.
        return;
    }

    char value[32];
    const size_t vlen = payloadLen < sizeof(value) - 1 ? payloadLen : sizeof(value) - 1;
    std::memcpy(value, payload, vlen);
    value[vlen] = '\0';

    if (std::strcmp(suffix, "on/set") == 0) {
        const bool on = (std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0);
        setControlValue("on", on ? "{\"value\":true}" : "{\"value\":false}");
    } else if (std::strcmp(suffix, "brightness/set") == 0) {
        // mqttthing sends 0..100; rescale to 0..255.
        int pct = std::atoi(value);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        const int bri = (pct * 255) / 100;
        char json[24];
        std::snprintf(json, sizeof(json), "{\"value\":%d}", bri);
        setControlValue("brightness", json);
    } else if (std::strcmp(suffix, "hsv/set") == 0) {
        // "h,s,v" — hue 0..359, sat 0..100, val 0..100 (mqttthing HSV). Hue+sat pick the nearest
        // palette; value maps to brightness so the color wheel's brightness ring still dims.
        int h = 0, s = 0, v = -1;
        std::sscanf(value, "%d,%d,%d", &h, &s, &v);
        const uint8_t idx = Palettes::nearestForHue(static_cast<uint16_t>(h < 0 ? 0 : h),
                                                    static_cast<uint8_t>(s < 0 ? 0 : (s > 100 ? 255 : s * 255 / 100)));
        char json[24];
        std::snprintf(json, sizeof(json), "{\"value\":%u}", static_cast<unsigned>(idx));
        setControlValue("palette", json);
        if (v >= 0) {
            int bri = (v > 100 ? 100 : v) * 255 / 100;
            std::snprintf(json, sizeof(json), "{\"value\":%d}", bri);
            setControlValue("brightness", json);
        }
    }
}

// Publish the friendly display name (deviceName) on the retained `<prefix>/name` topic. The topic
// identity is the stable MAC (rename-proof); this is the separate human-facing label a hub reads for
// its accessory title (the WLED serverDescription / HA discovery `name` role). Retained so a hub
// connecting later still gets it. Published on connect + on a deviceName change.
// A cheap rolling signature of the current deviceName (djb2), so a rename can be detected without
// storing the name string.
static uint32_t nameSignature(const char* s) {
    uint32_t h = 5381;
    for (; s && *s; s++) h = h * 33u + static_cast<uint8_t>(*s);
    return h;
}

void MqttModule::publishName() {
    if (state_ != Conn::Connected) return;
    // Always the device's own name — SystemModule guarantees it non-empty and per-device unique
    // (it falls back to MM-<last4MAC>, never a shared literal), so N devices never collide as one
    // name in the hub. No systemModule_ (a wiring bug) → skip; don't invent a non-unique fallback.
    const char* dn = systemModule_ ? systemModule_->deviceName() : nullptr;
    if (!dn || !dn[0]) return;
    char topic[128];
    buildTopic(topic, sizeof(topic), "name");
    uint8_t buf[kSendBufLen];
    const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(dn), std::strlen(dn),
                                      buf, sizeof(buf), /*retain=*/true);
    // Stamp the signature only on a SUCCESSFUL send — else a failed name publish is retried next
    // tick (maybeRepublishName sees the mismatch) rather than being lost until the next reconnect.
    if (n && sendPacket(buf, n)) nameSig_ = nameSignature(dn);
}

// Re-publish the name only if it changed since the last publish (a rename while connected).
void MqttModule::maybeRepublishName() {
    const char* dn = systemModule_ ? systemModule_->deviceName() : nullptr;
    if (dn && dn[0] && nameSignature(dn) != nameSig_) publishName();
}

void MqttModule::publishState(bool force) {
    if (state_ != Conn::Connected) return;
    Scheduler* s = Scheduler::instance();
    const bool on = driversOn(s);
    const uint8_t bri = driversBrightness(s);
    const uint8_t pal = driversPalette(s);
    if (!force && havePublished_ && on == lastOn_ && bri == lastBri_ && pal == lastPalette_) return;

    char topic[128];
    uint8_t buf[kSendBufLen];

    // A publish here is one of the three get-topics. On ANY send failure, reset the connection and
    // DON'T commit last*/havePublished_ — so after the reconnect the change is republished, not lost
    // (a committed-but-unsent state would leave the hub showing stale values forever). Same "stamp
    // only on success" rule as publishName / the ping path.
    auto publish = [&](const char* suffix, const char* payload) -> bool {
        buildTopic(topic, sizeof(topic), suffix);
        const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload), buf, sizeof(buf));
        return n != 0 && sendPacket(buf, n);
    };

    char briStr[8];
    std::snprintf(briStr, sizeof(briStr), "%d", (bri * 100) / 255);
    // hsv/get — the chosen palette's representative hue, full sat, value = brightness%.
    char hsvStr[16];
    std::snprintf(hsvStr, sizeof(hsvStr), "%u,100,%d",
                  static_cast<unsigned>(Palettes::representativeHue(pal)), (bri * 100) / 255);

    if (!publish("on/get", on ? "true" : "false") ||
        !publish("brightness/get", briStr) ||
        !publish("hsv/get", hsvStr)) {
        resetConnection("error: state publish failed");
        return;
    }

    // HA-native state on <prefix>/ha/state (retained, so a late-joining HA gets current state). One
    // JSON message with 0-255 brightness (no rescale, unlike brightness/get's 0-100). Only when
    // discovery is on, and inside this change-gated block so it emits once per change, not per tick.
    if (haDiscovery_) {
        char haState[48];
        std::snprintf(haState, sizeof(haState), "{\"state\":\"%s\",\"brightness\":%u}",
                      on ? "ON" : "OFF", static_cast<unsigned>(bri));
        char haTopic[128];
        buildTopic(haTopic, sizeof(haTopic), "ha/state");
        const size_t n = buildMqttPublish(haTopic, reinterpret_cast<const uint8_t*>(haState),
                                          std::strlen(haState), buf, sizeof(buf), /*retain=*/true);
        if (n == 0 || !sendPacket(buf, n)) { resetConnection("error: state publish failed"); return; }
    }

    lastOn_ = on; lastBri_ = bri; lastPalette_ = pal;
    havePublished_ = true;
}

void MqttModule::setControlValue(const char* control, const char* valueJson) {
    if (Scheduler* s = Scheduler::instance()) s->setControl("Drivers", control, valueJson);
}

void MqttModule::feedForTest(const uint8_t* bytes, size_t len) {
    for (size_t i = 0; i < len; i++) handleInboundByte(bytes[i]);
}

void MqttModule::setStatusLine(const char* msg) {
    std::snprintf(statusStr_, sizeof(statusStr_), "%s", msg);
}

} // namespace mm
