// @module NetworkModule
// @also FilesystemModule

// MoonBase reads the WiFi credentials with a bounded 1024-byte prefix read of
// /.config/NetworkModule.json (moonbase/main/moonbase_main.cpp loadCredentials): a tiny image
// has no JSON parser and no room for the whole file, which also carries every child module's
// config. That bound is a cross-image contract with NetworkModule's control registration order,
// and nothing else pins it: a control added ABOVE ssid/password would push them out of the
// prefix and silently break MoonBase's network join on every deployed 4 MB device. This test is
// the pin.

#include "doctest.h"
#include "core/FilesystemModule.h"
#include "core/NetworkModule.h"
#include "core/Scheduler.h"
#include "platform/platform.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

// The credentials a provisioned device saves sit inside the first kilobyte of NetworkModule.json, where MoonBase's bounded prefix read finds them.
TEST_CASE("NetworkModule.json carries ssid and password within MoonBase's 1024-byte prefix read") {
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

    const auto ssidEnd = content.find("\"ssid\":\"bench-ssid\"");
    const auto pwKey = content.find("\"password\":");
    REQUIRE(ssidEnd != std::string::npos);
    REQUIRE(pwKey != std::string::npos);
    // The whole password VALUE must fit too: key position + key + a worst-case 64-char
    // passphrase escaped to at most twice its length stays under the bound.
    CHECK(ssidEnd < 1024);
    CHECK(pwKey + 12 + 2 * 64 + 2 < 1024);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}
