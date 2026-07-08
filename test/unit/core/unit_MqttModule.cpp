// @module MqttModule
// @also Scheduler

// Pins MqttModule's inbound routing: a PUBLISH arriving on a <prefix>/…/set topic drives the
// matching Drivers control through the shared Scheduler::setControl primitive — the same seam IR and
// the WLED bridge use. The socket is not involved: feedForTest() injects raw MQTT bytes (built with
// the tested MqttPacket builders) exactly as the broker would deliver them, so the routing is
// provable with no broker (mirrors IrModule::injectCodeForTest). A FakeDrivers stands in for the
// real Drivers with the on / brightness / palette controls MQTT targets.

#include "doctest.h"
#include "core/MqttModule.h"
#include "core/MqttPacket.h"
#include "core/Scheduler.h"
#include "core/MoonModule.h"
#include "core/SystemModule.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {

// Stands in for Drivers: on (Bool), brightness (Uint8 0-255), palette (Select). Named "Drivers" so
// MqttModule's setControl("Drivers", …) resolves to it.
struct FakeDrivers : public MoonModule {
    bool on = true;
    uint8_t brightness = 100;
    uint8_t palette = 0;
    void onBuildControls() override {
        controls_.addBool("on", on);
        controls_.addUint8("brightness", brightness, 0, 255);
        // A Uint8 palette with the real built-in range (0..255 is a superset of the ~60 built-ins),
        // so a nearest-palette index the MQTT map returns isn't clamped away by an artificially small
        // Select — the real Drivers.palette binds 0..kCount-1.
        controls_.addUint8("palette", palette, 0, 255);
    }
};

// Build a scheduler with FakeDrivers + a SystemModule + an MqttModule, run setup so
// Scheduler::instance() is live and controls are bound. The topic prefix is STABLE + MAC-derived
// (projectMM/<last6-of-MAC>), NOT from deviceName — so it's rename-proof. On desktop the fake MAC is
// DE:AD:BE:EF:CA:FE (platform_desktop.cpp), so last-6 = "efcafe".
struct Rig {
    static constexpr const char* kPrefix = "projectMM/efcafe";   // desktop fake MAC last-6
    Scheduler scheduler;
    FakeDrivers* drivers = new FakeDrivers();
    SystemModule* system = new SystemModule();
    MqttModule* mqtt = new MqttModule();
    Rig() {
        drivers->setName("Drivers");
        system->setName("System");
        mqtt->setName("Mqtt");
        mqtt->setSystemModule(system);   // for the published friendly-name (not the topic identity)
        scheduler.addModule(drivers);
        scheduler.addModule(system);
        scheduler.addModule(mqtt);
        scheduler.setup();   // binds controls, sets Scheduler::instance()
    }
    ~Rig() { scheduler.teardown(); }

    // Deliver a PUBLISH to `suffix` (under the derived prefix, e.g. "on/set") with a string payload,
    // as the broker socket would. Pass a leading "/" to send an ABSOLUTE topic (for the wrong-prefix
    // test); otherwise the derived prefix is prepended.
    void publish(const char* suffix, const char* payload) {
        char topic[128];
        if (suffix[0] == '/') std::snprintf(topic, sizeof(topic), "%s", suffix + 1);   // absolute
        else std::snprintf(topic, sizeof(topic), "%s/%s", kPrefix, suffix);
        uint8_t buf[160];
        const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload), buf, sizeof(buf));
        REQUIRE(n > 0);
        mqtt->feedForTest(buf, n);
    }
};

// Walk the concatenated MQTT packet stream the capture holds and return the fixed-header first byte
// (type nibble + flags) of the first PUBLISH whose topic equals `wantTopic`, or -1 if none. Lets a
// test assert the RETAIN bit (bit 0, §3.3.1.3) rather than only string-matching the payload — a
// regression dropping retain=true flips this bit but leaves every substring intact.
int publishFlagsForTopic(const uint8_t* buf, size_t len, const char* wantTopic) {
    size_t i = 0;
    while (i < len) {
        const uint8_t first = buf[i];
        // Remaining Length: a 1–4 byte varint (§2.2.3).
        size_t j = i + 1, mult = 1, remLen = 0;
        for (int b = 0; b < 4 && j < len; b++, j++) {
            remLen += (buf[j] & 0x7F) * mult;
            if (!(buf[j] & 0x80)) { j++; break; }
            mult *= 128;
        }
        const size_t body = j;                 // first byte after the fixed header
        if ((first & 0xF0) == 0x30 && body + 2 <= len) {   // PUBLISH
            const size_t topicLen = (size_t(buf[body]) << 8) | buf[body + 1];
            if (body + 2 + topicLen <= len &&
                std::strncmp(reinterpret_cast<const char*>(buf + body + 2), wantTopic, topicLen) == 0 &&
                std::strlen(wantTopic) == topicLen) {
                return first;
            }
        }
        i = body + remLen;
    }
    return -1;
}

}  // namespace

TEST_CASE("MqttModule: on/set drives Drivers.on") {
    Rig r;
    r.drivers->on = true;
    r.publish("on/set", "false");
    CHECK(r.drivers->on == false);
    r.publish("on/set", "true");
    CHECK(r.drivers->on == true);
    // "1"/"0" are accepted too (mqttthing integerValue mode).
    r.publish("on/set", "0");
    CHECK(r.drivers->on == false);
    r.publish("on/set", "1");
    CHECK(r.drivers->on == true);
}

TEST_CASE("MqttModule: brightness/set rescales 0-100 to 0-255") {
    Rig r;
    r.publish("brightness/set", "0");
    CHECK(r.drivers->brightness == 0);
    r.publish("brightness/set", "100");
    CHECK(r.drivers->brightness == 255);
    r.publish("brightness/set", "50");
    CHECK(r.drivers->brightness == 127);          // 50*255/100
    // Out-of-range clamps, not wraps.
    r.publish("brightness/set", "250");
    CHECK(r.drivers->brightness == 255);
}

TEST_CASE("MqttModule: hsv/set maps a hue to the nearest palette + value to brightness") {
    Rig r;
    // A blue-ish hue at full saturation should pick a blue-family palette (a non-zero index — not
    // Rainbow at 0). We assert it moved off the default and that value drove brightness.
    r.drivers->palette = 0;
    r.publish("hsv/set", "210,100,40");      // blue, sat 100%, value 40%
    CHECK(r.drivers->palette != 0);               // snapped to some blue-family palette
    CHECK(r.drivers->brightness == (40 * 255) / 100);   // value → brightness
}

// --- Home Assistant MQTT Discovery: the HA-native ha/set command topic ---
// HA drives the JSON-schema light with {"state":"ON"|"OFF"[,"brightness":0-255]} on <prefix>/ha/set.
// Unlike the mqttthing brightness/set (0-100), HA brightness is already 0-255 — no rescale.

TEST_CASE("MqttModule: ha/set {state} drives Drivers.on") {
    Rig r;
    r.drivers->on = true;
    r.publish("ha/set", "{\"state\":\"OFF\"}");
    CHECK(r.drivers->on == false);
    r.publish("ha/set", "{\"state\":\"ON\"}");
    CHECK(r.drivers->on == true);
}

TEST_CASE("MqttModule: ha/set {brightness} maps 0-255 with no rescale") {
    Rig r;
    r.publish("ha/set", "{\"state\":\"ON\",\"brightness\":128}");
    CHECK(r.drivers->on == true);
    CHECK(r.drivers->brightness == 128);          // HA is already 0-255 — no *255/100
    r.publish("ha/set", "{\"brightness\":255}");
    CHECK(r.drivers->brightness == 255);
    // Out-of-range clamps.
    r.publish("ha/set", "{\"brightness\":999}");
    CHECK(r.drivers->brightness == 255);
}

TEST_CASE("MqttModule: ha/set is key-order-independent") {
    Rig r;
    r.drivers->on = false;
    // brightness before state — mm::json's strstr lookup is order-independent (HA emits compact JSON;
    // the flat helpers match `"key":` / `"key": `, so we feed the same no-inner-space shape HA sends).
    r.publish("ha/set", "{\"brightness\":64,\"state\":\"ON\"}");
    CHECK(r.drivers->on == true);
    CHECK(r.drivers->brightness == 64);
}

TEST_CASE("MqttModule: ha/set with only state leaves brightness untouched") {
    Rig r;
    r.drivers->brightness = 200;
    r.publish("ha/set", "{\"state\":\"OFF\"}");   // no brightness key
    CHECK(r.drivers->on == false);
    CHECK(r.drivers->brightness == 200);          // unchanged (hasKey guard)
}

// The discovery announce: on CONNACK the module publishes a RETAINED config to
// homeassistant/light/projectMM_<mac6>/config. Assert via the outbound-capture seam (no live socket).
TEST_CASE("MqttModule: CONNACK publishes a retained HA discovery config") {
    Rig r;
    uint8_t cap[1024];
    r.mqtt->enableSendCaptureForTest(cap, sizeof(cap));
    // haDiscovery defaults OFF (opt-in; the WLED /json shim covers HA), so enable it first — this
    // test asserts the announce shape when the user opts into MQTT discovery.
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":true}");
    // A CONNACK-accept: fixed header 0x20, len 2, session-present 0, return-code 0 (accepted).
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    r.mqtt->feedForTest(connack, sizeof(connack));
    const size_t len = r.mqtt->sentCaptureLenForTest();
    REQUIRE(len > 0);
    // The captured stream must contain the discovery topic + the key config fields.
    std::string sent(reinterpret_cast<const char*>(cap), len);
    CHECK(sent.find("homeassistant/light/projectMM_efcafe/config") != std::string::npos);
    CHECK(sent.find("\"schema\":\"json\"") != std::string::npos);
    CHECK(sent.find("\"uniq_id\":\"projectMM_efcafe\"") != std::string::npos);
    CHECK(sent.find("projectMM/efcafe/ha/set") != std::string::npos);    // cmd_t
    CHECK(sent.find("projectMM/efcafe/ha/state") != std::string::npos);  // stat_t
    CHECK(sent.find("projectMM/efcafe/status") != std::string::npos);    // avty_t
    CHECK(sent.find("online") != std::string::npos);                     // the retained availability publish
    // The discovery config AND the availability publish must carry the RETAIN bit (bit 0 of the PUBLISH
    // fixed header) — a late-joining HA reads the retained config/state, so dropping retain breaks it.
    const int cfgFlags = publishFlagsForTopic(cap, len, "homeassistant/light/projectMM_efcafe/config");
    REQUIRE(cfgFlags >= 0);
    CHECK((cfgFlags & 0x01) == 0x01);                                    // discovery config retained
    const int avtyFlags = publishFlagsForTopic(cap, len, "projectMM/efcafe/status");
    REQUIRE(avtyFlags >= 0);
    CHECK((avtyFlags & 0x01) == 0x01);                                   // availability "online" retained
}

// Regression (found live on P4/S31 hardware): turning haDiscovery OFF must free the discovery buffers
// EVEN when the socket is not currently Connected (mid-reconnect). The original guard bailed on
// `state_ != Connected` before reaching the free, so a discovery-off toggle during a reconnect stranded
// the 768 B until teardown — breaking "no memory when discovery is off". Freeing local memory needs no
// socket, so the retract path frees unconditionally; only the empty-retained PUBLISH needs a live link.
TEST_CASE("MqttModule: retract frees the discovery buffers even while disconnected") {
    Rig r;
    uint8_t cap[1024];
    r.mqtt->enableSendCaptureForTest(cap, sizeof(cap));
    // haDiscovery defaults OFF; opt in so CONNACK allocates the discovery buffers this test tracks.
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":true}");
    // CONNACK-accept → Connected → announce allocates the discovery buffers (448 + 320 = 768).
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    r.mqtt->feedForTest(connack, sizeof(connack));
    CHECK(r.mqtt->dynamicBytes() == MqttModule::kDiscoveryDynamicBytes);
    // A broker change re-homes the socket → resetConnection drops state to Idle, buffers still held
    // (a reconnect must not churn the heap). Now we're "allocated but not Connected".
    Scheduler::instance()->setControl("Mqtt", "broker", "{\"value\":\"10.0.0.9\"}");
    CHECK(r.mqtt->dynamicBytes() == MqttModule::kDiscoveryDynamicBytes);          // reset kept the buffers (correct)
    // Toggle discovery OFF while disconnected — must free despite no live socket.
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":false}");
    CHECK(r.mqtt->dynamicBytes() == 0);            // the fix: retract freed even while not Connected
}

TEST_CASE("MqttModule: a PUBLISH on an unrelated topic is ignored, not a crash") {
    Rig r;
    const uint8_t beforeBri = r.drivers->brightness;
    const bool beforeOn = r.drivers->on;
    r.publish("unknown/set", "whatever");    // no matching suffix
    r.publish("/otherdevice/on/set", "false");     // wrong prefix
    CHECK(r.drivers->brightness == beforeBri);
    CHECK(r.drivers->on == beforeOn);
}

TEST_CASE("MqttModule: a PUBLISH split across feeds still routes (fragment reassembly)") {
    Rig r;
    r.drivers->on = true;
    uint8_t buf[128];
    const char* payload = "false";
    const size_t n = buildMqttPublish("projectMM/efcafe/on/set", reinterpret_cast<const uint8_t*>(payload),
                                      std::strlen(payload), buf, sizeof(buf));
    REQUIRE(n > 0);
    // Feed one byte at a time — the parser holds partial state until the packet completes.
    for (size_t i = 0; i < n; i++) r.mqtt->feedForTest(&buf[i], 1);
    CHECK(r.drivers->on == false);
}

// Regression (reviewer): the topic identity is the STABLE MAC (projectMM/<last6>), NOT the device
// name — so renaming the device must NOT change which topics the module listens on. A command on the
// MAC-based topic keeps working after a rename; a command on a name-based topic never matched.
TEST_CASE("MqttModule: topic identity is MAC-stable, not affected by a device rename") {
    Rig r;
    r.drivers->on = true;
    // Command on the MAC topic works.
    r.publish("on/set", "false");
    CHECK(r.drivers->on == false);
    // Rename the device — topics must stay on the MAC prefix.
    Scheduler::instance()->setControl("System", "deviceName", "{\"value\":\"LivingRoom\"}");
    r.drivers->on = true;
    r.publish("on/set", "false");                 // still the MAC prefix (Rig::kPrefix)
    CHECK(r.drivers->on == false);                // rename didn't break routing
    // A command on a name-derived topic never matches (proves identity isn't the name).
    r.drivers->on = true;
    r.publish("/projectMM/LivingRoom/on/set", "false");   // absolute, name-based
    CHECK(r.drivers->on == true);                 // ignored — not our (MAC) prefix
}
