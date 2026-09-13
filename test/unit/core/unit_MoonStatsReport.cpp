// @module MoonStatsReport

#include "doctest.h"
#include "core/MoonStatsModule.h"
#include "core/AudioService.h"

#include <cstring>
#include <string>

#include "core/SystemModule.h"

namespace {

/// A module carrying exactly the controls a real device would expose that MUST NEVER be reported:
/// the identifying ones (device name, MAC), the secret ones (SSID, password), and free text the
/// user typed. Built to look as much like the real System module as possible, because a builder
/// that walked the tree rather than naming its fields would happily emit all of these.
class LeakyModule : public mm::MoonModule {
public:
    LeakyModule() { setName("System"); }

    void defineControls() override {
        std::strncpy(deviceName_, "ewoud-livingroom", sizeof(deviceName_) - 1);
        std::strncpy(mac_, "A4:CF:12:9B:33:07", sizeof(mac_) - 1);
        std::strncpy(ssid_, "Travelrouter", sizeof(ssid_) - 1);
        std::strncpy(password_, "hunter2-secret", sizeof(password_) - 1);
        std::strncpy(note_, "my bedroom wall, do not touch", sizeof(note_) - 1);
        std::strncpy(chip_, "ESP32-S3", sizeof(chip_) - 1);
        std::strncpy(flash_, "16MB", sizeof(flash_) - 1);

        controls_.addText("deviceName", deviceName_, sizeof(deviceName_));
        controls_.addReadOnly("mac", mac_, sizeof(mac_));
        controls_.addText("ssid", ssid_, sizeof(ssid_));
        controls_.addPassword("password", password_, sizeof(password_));
        controls_.addText("note", note_, sizeof(note_));
        controls_.addReadOnly("chip", chip_, sizeof(chip_));
        controls_.addReadOnly("flash", flash_, sizeof(flash_));
    }

private:
    char deviceName_[32] = {};
    char mac_[24] = {};
    char ssid_[32] = {};
    char password_[32] = {};
    char note_[40] = {};
    char chip_[16] = {};
    char flash_[8] = {};
};

/// A scripted module, the shape MoonLiveEffect and friends have: a `script` FilePath control
/// holding a file NAME, plus whatever that script declared.
class ScriptedModule : public mm::MoonModule {
public:
    ScriptedModule(const char* name, const char* script, mm::ModuleRole role)
        : role_(role) {
        setName(name);
        std::snprintf(script_, sizeof(script_), "%s", script);
    }

    mm::ModuleRole role() const MM_NONBLOCKING override { return role_; }

    void defineControls() override {
        controls_.addFilePath("script", script_, sizeof(script_), mm::moonlive::kEffectPick);
    }

private:
    mm::ModuleRole role_;
    char script_[48] = {};
};

/// The report for a tree holding one scripted module.
std::string scriptedReport(const char* script, mm::ModuleRole role = mm::ModuleRole::Effect) {
    ScriptedModule mod("MoonLive", script, role);
    mod.defineControls();
    mm::MoonModule* tree[] = {&mod};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, mm::MoonStatsEvent::Install, nullptr, "4.0.0", nullptr);
    return std::string(sink.data(), sink.size());
}

std::string report(mm::MoonStatsEvent event = mm::MoonStatsEvent::Install,
                   const char* id = nullptr,
                   const char* version = "4.0.0",
                   const char* previous = nullptr) {
    LeakyModule sys;
    sys.defineControls();
    mm::MoonModule* tree[] = {&sys};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, event, id, version, previous);
    return std::string(sink.data(), sink.size());
}

}  // namespace

/// The report never carries anything that identifies the person or their network, however much of
/// it the module tree holds.
///
/// This is the privacy policy made executable. The policy promises no device name, no network
/// addresses, no credentials and no free text the user typed, and the tree here holds all four
/// sitting beside the hardware fields that ARE reported. A builder that emitted what it found
/// rather than naming each field would fail this the first time it ran.
TEST_CASE("the usage report cannot carry identifying or secret values") {
    const std::string json = report();

    // The VALUES, which is what would actually harm someone.
    CHECK(json.find("ewoud-livingroom") == std::string::npos);
    CHECK(json.find("A4:CF:12:9B:33:07") == std::string::npos);
    CHECK(json.find("Travelrouter") == std::string::npos);
    CHECK(json.find("hunter2-secret") == std::string::npos);
    CHECK(json.find("my bedroom wall") == std::string::npos);

    // The KEYS, so a later refactor cannot reintroduce the field with an empty value and look
    // harmless while the next change fills it in.
    CHECK(json.find("deviceName") == std::string::npos);
    CHECK(json.find("\"mac\"") == std::string::npos);
    CHECK(json.find("ssid") == std::string::npos);
    CHECK(json.find("password") == std::string::npos);
    CHECK(json.find("note") == std::string::npos);
}

/// The hardware facts the report exists for do arrive, so the test above is not passing merely
/// because the builder emits nothing.
TEST_CASE("the usage report carries the hardware facts it exists to collect") {
    const std::string json = report();
    CHECK(json.find("ESP32-S3") != std::string::npos);
    CHECK(json.find("16MB") != std::string::npos);
    CHECK(json.find("\"chip\"") != std::string::npos);
    CHECK(json.find("\"flash\"") != std::string::npos);
}

/// An install and an upgrade are told apart by the report itself, with no identifier involved: a
/// previous version present means the firmware changed under an existing install.
TEST_CASE("an upgrade is distinguished from a fresh install by the previous version") {
    const std::string fresh = report(mm::MoonStatsEvent::Install, nullptr, "4.0.0", nullptr);
    CHECK(fresh.find("\"event\":\"install\"") != std::string::npos);
    CHECK(fresh.find("previousVersion") == std::string::npos);

    const std::string upgraded = report(mm::MoonStatsEvent::Upgrade, nullptr, "4.1.0", "4.0.0");
    CHECK(upgraded.find("\"event\":\"upgrade\"") != std::string::npos);
    CHECK(upgraded.find("\"previousVersion\":\"4.0.0\"") != std::string::npos);
}

/// The button's event. Install and Upgrade are decided by a version comparison, which cannot see a
/// setup that changed without one: someone who reported a bare board and then wired up the fixtures
/// they actually run. Distinct from the other two so the install count stays a count of installs.
TEST_CASE("a user-triggered refresh is its own event, carrying the same payload") {
    const std::string refreshed = report(mm::MoonStatsEvent::Refresh, nullptr, "4.0.0", nullptr);
    CHECK(refreshed.find("\"event\":\"refresh\"") != std::string::npos);
    // Same shape as any other report: the button re-sends, it does not send something smaller.
    CHECK(refreshed.find("\"chip\"") != std::string::npos);
    CHECK(refreshed.find("\"version\":\"4.0.0\"") != std::string::npos);
    // And it is none of the other two, so a legend cannot show it as an install.
    CHECK(refreshed.find("\"event\":\"install\"") == std::string::npos);
    CHECK(refreshed.find("\"event\":\"upgrade\"") == std::string::npos);
}

/// A report built without consent carries no installation id at all, rather than an empty or
/// placeholder one: nothing is generated until the user says yes.
TEST_CASE("no installation id appears until one is supplied") {
    CHECK(report().find("installationId") == std::string::npos);

    const std::string withId = report(mm::MoonStatsEvent::Install,
                                      "66b1706d30ff5c0fb1c6fdd7f6fe1151");
    CHECK(withId.find("\"installationId\":\"66b1706d30ff5c0fb1c6fdd7f6fe1151\"")
          != std::string::npos);
}

/// Memory and light count ride the report as RAW numbers, for the server to bucket into ranges.
///
/// Nothing pinned them, and the worker's own `clean()` drops anything that is not a string unless a
/// field has a branch of its own: exactly the regression that stored three zeros for every device.
TEST_CASE("the report carries memory and light count as numbers") {
    mm::SystemModule system;
    system.setName("System");

    mm::MoonModule* tree[] = {&system};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr, 256, 282152, 84788);
    const std::string json = sink.data();

    CHECK(json.find("\"lightCount\":256") != std::string::npos);
    // Unquoted: a JSON number, not a string, which is what the server's numeric branch accepts.
    CHECK(json.find("\"lightCount\":\"") == std::string::npos);
    // The VALUES, unquoted: the server's numeric branch accepts a JSON number and its generic
    // string test drops anything else, which is how three zeros were stored for every device.
    CHECK(json.find("\"totalHeap\":282152") != std::string::npos);
    CHECK(json.find("\"freeHeap\":84788") != std::string::npos);
}

/// The report names what the user ADDED, not the boot tree every device shares.
///
/// Counting main.cpp's wired modules made every slice read "2 of 2 devices", which says only that
/// both booted. What varies between installations is what someone chose to run, so a wired module
/// is skipped while its children are still walked: a user's effect hangs under a wired parent.
TEST_CASE("the report names modules by ROLE, not by how they were wired") {
    // A plain container. NOT wired by code, so only the ROLE rule excludes it: under the older
    // isWiredByCode() test this one would have been reported.
    mm::MoonModule container;
    container.setName("Container");

    // Wired by code AND a real role: the mirror case, reported under the role rule and dropped
    // under the old one. Together these two fail if the filter ever switches back.
    mm::AudioService added;
    added.setName("SomeService");
    added.markWiredByCode();

    mm::AudioService off;
    off.setName("Disabled");
    off.setEnabled(false);

    mm::MoonModule* tree[] = {&container, &added, &off};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 3, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr);
    const std::string json = sink.data();

    CHECK(json.find("service:SomeService") != std::string::npos);   // a real role: reported
    CHECK(json.find("Container") == std::string::npos);             // generic: a structural container
    CHECK(json.find("Disabled") == std::string::npos);              // switched off
}

/// A user's module hangs UNDER a wired parent, so skipping the parent must not skip the child.
TEST_CASE("a module added under a wired parent is still reported") {
    mm::SystemModule parent;
    parent.setName("Effects");
    parent.markWiredByCode();

    mm::AudioService child;
    child.setName("Lissajous");
    parent.addChild(&child);

    mm::MoonModule* tree[] = {&parent};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr);
    const std::string json = sink.data();

    CHECK(json.find("service:Lissajous") != std::string::npos);
    CHECK(json.find("Effects") == std::string::npos);
}

/// A scripted module reports WHICH script it runs, because "MoonLive" alone says nothing: the
/// interesting fact is that a device is running `aurora.mle`.
TEST_CASE("a scripted module reports the shipped script it runs") {
    const std::string json = scriptedReport("aurora.mle");
    CHECK(json.find("effect:MoonLive/aurora.mle") != std::string::npos);
}

/// The other half, and the one that matters: a script a USER wrote is a name they invented, which
/// is text they typed. The module still counts, under its bare type name.
///
/// Without this the feature would be a privacy regression wearing a usage-statistics hat: a script
/// called "ewoud-bedroom-test.mle" would travel to the server exactly like a shipped name.
TEST_CASE("a script the user wrote is counted but never named") {
    const std::string json = scriptedReport("ewoud-bedroom-test.mle");
    CHECK(json.find("ewoud-bedroom-test") == std::string::npos);
    CHECK(json.find("effect:MoonLive") != std::string::npos);
    CHECK(json.find("effect:MoonLive/") == std::string::npos);
}

/// A shipped name under the WRONG extension is not a shipped script: the catalogs are per role, so
/// a lookup that scanned them all would let `aurora.mle` through on a layout and, worse, would make
/// "is this ours" depend on a name rather than a name plus its kind.
TEST_CASE("a catalog name is matched against its own role's catalog") {
    const std::string json = scriptedReport("grid.mll", mm::ModuleRole::Layout);
    CHECK(json.find("layout:MoonLive/grid.mll") != std::string::npos);
}
