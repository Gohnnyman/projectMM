// @module NetworkModule
// @also FilesystemModule

// MoonBase reads the WiFi credentials, the Ethernet wiring and the TX cap with a bounded
// 2048-byte prefix read of /.config/NetworkModule.json (moonbase/main/moonbase_main.cpp
// loadCredentials): a tiny image has no JSON parser and no room for the whole file, which also
// carries every child module's config. That bound is a cross-image contract with
// NetworkModule's control registration order, and nothing else pins it: a control added ABOVE
// these keys would push them out of the prefix and silently break MoonBase's network join (or
// its eth wiring) on every deployed 4 MB device. This test is the pin.
//
// The eth controls exist only on Ethernet-capable builds, so this desktop-run test pins the
// bound indirectly: every top-level Network control serializes BEFORE the first child module
// key ("0.<name>"), so end-of-top-level plus a stated worst case for the ESP32-only keys must
// sit inside the prefix.

#include "doctest.h"
#include "core/FilesystemModule.h"
#include "core/NetworkModule.h"
#include "core/Scheduler.h"
#include "platform/platform.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

// The keys MoonBase scrapes sit inside its 2048-byte prefix read of NetworkModule.json, with room for the ESP32-only eth block.
TEST_CASE("NetworkModule.json keeps MoonBase's scraped keys inside its 2048-byte prefix read") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_moonbase_contract_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* net = new mm::NetworkModule();
    net->setTypeName("NetworkModule");
    scheduler.addModule(fs);
    scheduler.addModule(net);
    scheduler.setup();

    net->setWifiCredentials("bench-ssid", "bench-password");
    net->markDirty();
    fs->flush();

    std::ifstream f(std::string(tmpRoot) + "/.config/NetworkModule.json");
    REQUIRE(f.good());
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();

    constexpr size_t kPrefixRead = 2048;   // moonbase_main.cpp loadCredentials buf size
    // The ESP32-only additions to the top-level block that this desktop file cannot contain:
    // 8 eth keys plus values (~260 bytes serialized) with margin.
    constexpr size_t kEsp32OnlyBudget = 400;

    const auto ssidEnd = content.find("\"ssid\":\"bench-ssid\"");
    const auto pwKey = content.find("\"password\":");
    REQUIRE(ssidEnd != std::string::npos);
    REQUIRE(pwKey != std::string::npos);
    CHECK(ssidEnd < kPrefixRead - kEsp32OnlyBudget);
    // The whole password VALUE fits too: a worst-case 64-char passphrase escaped to twice
    // its length.
    CHECK(pwKey + 12 + 2 * 64 + 2 < kPrefixRead - kEsp32OnlyBudget);

    // Every top-level control precedes the first child-module key; with the ESP32-only budget
    // on top, the whole scraped block stays inside the prefix.
    const auto firstChild = content.find("\"0.");
    if (firstChild != std::string::npos) {
        CHECK(firstChild < kPrefixRead - kEsp32OnlyBudget);
    } else {
        // No children on this build: the whole file must fit with the budget to spare.
        CHECK(content.size() < kPrefixRead - kEsp32OnlyBudget);
    }

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}
