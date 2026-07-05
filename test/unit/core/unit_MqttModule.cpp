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
