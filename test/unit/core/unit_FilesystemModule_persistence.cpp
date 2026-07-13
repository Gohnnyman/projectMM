// @module FilesystemModule
// @also Scheduler, Layer

#include "doctest.h"
#include "core/FilesystemModule.h"
#include "core/ModuleFactory.h"
#include "core/Scheduler.h"
#include "core/SystemModule.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/RegionModifier.h"
#include "light/layers/Layer.h"
#include "light/drivers/LightPresetsModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

// Persistence round-trip: set deviceName → save → recreate Scheduler+modules → load → assert.
// Uses fsSetRoot to isolate the test from any real /.config/ on disk.
// A control change (deviceName) saved with flush() reappears on the next boot once a fresh Scheduler loads the same path.
TEST_CASE("FilesystemModule round-trip") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_persist_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(tmpRoot);

    // Scheduler::release() deletes its modules — they must be heap-allocated.
    // --- First run: set deviceName, save ---
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        auto* sys = new mm::SystemModule();
        sys->setTypeName("SystemModule");
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        sys->setScheduler(&scheduler);
        scheduler.addModule(fs);
        scheduler.addModule(sys);
        scheduler.setup();

        // setup() derived MAC-based default; mutate it as if the UI did:
        for (uint8_t i = 0; i < sys->controls().count(); i++) {
            auto& c = sys->controls()[i];
            if (std::strcmp(c.name, "deviceName") == 0) {
                std::snprintf(static_cast<char*>(c.ptr), 9, "%s", "MM-ROUND");
                sys->markDirty();
                mm::FilesystemModule::noteDirty();
                break;
            }
        }

        // flush() does the same work as tick1s() does once the debounce expires, but
        // synchronously — used here to keep the test deterministic without wall-clock waits.
        fs->flush();

        char path[512];
        std::snprintf(path, sizeof(path), "%s/.config/SystemModule.json", tmpRoot);
        CHECK(std::filesystem::exists(path));

        scheduler.release();
    }

    // --- Second run: fresh modules, load from disk ---
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        auto* sys = new mm::SystemModule();
        sys->setTypeName("SystemModule");
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        sys->setScheduler(&scheduler);
        scheduler.addModule(fs);
        scheduler.addModule(sys);
        scheduler.setup();

        CHECK(std::strcmp(sys->deviceName(), "MM-ROUND") == 0);

        scheduler.release();
    }

    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot("."); // restore default
}

// Structural persistence: hand-write a Layer.json describing a different tree shape
// than the one main.cpp builds, then load and verify the live tree reconciles to
// match the JSON — type swap at position 0, trim of position 1.
// On load, a Layer's children are reconciled against the saved JSON: position 0 swaps to the saved type, extras at later positions are trimmed.
TEST_CASE("FilesystemModule structural reconciliation") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_struct_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    // ModuleFactory must know the types before reconciliation can construct them.
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier");

    // Write a Layer.json that asks for one child (RainbowEffect at position 0).
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"channelsPerLight\":3,\"enabled\":true,"
             "\"0.type\":\"RainbowEffect\",\"0.enabled\":true}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    // Build a live tree: Layer with NoiseEffect at pos 0 and MultiplyModifier at pos 1.
    // The JSON wants RainbowEffect at pos 0 and nothing at pos 1 — so we expect a swap
    // and a trim.
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    auto* noise = new mm::NoiseEffect();
    noise->setTypeName("NoiseEffect");
    auto* mirror = new mm::MultiplyModifier();
    mirror->setTypeName("MultiplyModifier");
    layer->addChild(noise);
    layer->addChild(mirror);

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    REQUIRE(layer->childCount() == 1);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Pins the wiredByCode-preserves-child contract that lets a new firmware revision
// add a code-created child (e.g. ImprovProvisioning under NetworkModule) without
// the child getting trimmed on every boot for users whose saved Network.json
// predates the addition.
//
// Setup: an on-disk file describes Layer with zero children. Live tree has Layer
// with a RainbowEffect child that main.cpp would have wired and marked. After
// scheduler.setup() runs the persistence load, the wired child must survive.
// A code-wired child (markWiredByCode) survives a load from older JSON that doesn't mention it — new firmware additions aren't trimmed for existing users.
TEST_CASE("FilesystemModule preserves code-wired children when JSON predates them") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_wired_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");

    // Saved file: Layer with no children — the "old release" state.
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"channelsPerLight\":3,\"enabled\":true}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    // Live tree: Layer with a code-wired RainbowEffect child. This mirrors
    // what main.cpp does for NetworkModule + ImprovProvisioningModule.
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    auto* rainbow = new mm::RainbowEffect();
    rainbow->setTypeName("RainbowEffect");
    layer->addChild(rainbow);
    rainbow->markWiredByCode();

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // The code-wired RainbowEffect must still be there after persistence load.
    REQUIRE(layer->childCount() == 1);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);
    CHECK(layer->child(0)->isWiredByCode() == true);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Companion to the wiredByCode case above: when the JSON describes a different
// type at the position where a code-wired child lives, the position-replacement
// must NOT kill the code-wired child. Stop reconciliation at that index instead
// and let the next save re-write the file with the actual tree shape.
// When the saved JSON wants a different type at the position where a code-wired child lives, reconciliation stops at that index instead of destroying the wired child.
TEST_CASE("FilesystemModule does not replace code-wired child on type mismatch") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_wired_replace_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    // Saved file: Layer with a NoiseEffect at position 0 — a stale shape from
    // before the firmware moved a code-wired effect into that slot.
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"channelsPerLight\":3,\"enabled\":true,"
             "\"0.type\":\"NoiseEffect\",\"0.enabled\":true}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    auto* rainbow = new mm::RainbowEffect();
    rainbow->setTypeName("RainbowEffect");
    layer->addChild(rainbow);
    rainbow->markWiredByCode();

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // Code-wired child stays — type mismatch did not trigger a replacement.
    REQUIRE(layer->childCount() == 1);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);
    CHECK(layer->child(0)->isWiredByCode() == true);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Round-trip persistence with children: write a Layer subtree that contains both
// controls and child modules with controls of their own, then read the file back as
// text and verify it parses as valid JSON. Regresses the missing-comma bug between
// each child's "N.type" field and that child's first control (e.g. "0.type":"X""0.foo":1
// instead of "0.type":"X","0.foo":1).
// Saving a Layer with multiple children produces valid JSON — comma separators between child `N.type` and the child's first control field are present.
TEST_CASE("FilesystemModule writes valid JSON with children") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_write_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier");

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    auto* mirror = new mm::MultiplyModifier();
    mirror->setTypeName("MultiplyModifier");
    auto* noise = new mm::NoiseEffect();
    noise->setTypeName("NoiseEffect");
    layer->addChild(mirror);
    layer->addChild(noise);

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // Mark dirty and flush so the file appears immediately.
    layer->markDirty();
    fs->flush();

    // Read back the raw file and verify both child "type" fields are followed by
    // a comma before the next field — the previously-broken serializer emitted
    // "0.type":"MultiplyModifier""0.mirrorX":true with no separator.
    std::ifstream f(std::string(tmpRoot) + "/.config/Layer.json");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();  // Windows holds an exclusive lock on open files — close before remove_all.
    CHECK(content.find("\"MultiplyModifier\",") != std::string::npos);
    CHECK(content.find("\"NoiseEffect\",") != std::string::npos);
    // And the catastrophic "}{ or "X""Y syntactic shape must not appear.
    CHECK(content.find("\"\"") == std::string::npos);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// No size cap: a config LARGER than the old fixed 2 KB save buffer round-trips in full. The save
// serializes into a growable JsonSink and the load reads a file-sized heap buffer, so neither side
// truncates. Built from a LightPresetsModule with many custom presets — its persisted array of
// role wirings comfortably exceeds 2048 bytes, which the old fixed buffer would have silently
// dropped (returning false → nothing written → config lost on reboot).
TEST_CASE("FilesystemModule round-trips a config larger than the old 2 KB cap") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_bigcfg_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);
    mm::ModuleFactory::registerType<mm::LightPresetsModule>("LightPresetsModule");

    uint32_t markerId = 0;
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        auto* lp = new mm::LightPresetsModule();
        lp->setTypeName("LightPresetsModule");
        scheduler.addModule(fs);
        scheduler.addModule(lp);
        scheduler.setup();   // seeds the 13 built-ins

        // Add 15 wide (24-channel) custom presets — the serialized array is well over 2 KB. (15,
        // not 20: the seeded built-ins now number 13, and 13 + 20 would exceed kMaxPresets=32; 13 +
        // 15 = 28 fits, and 15 wide presets still serialise far past the old 2 KB cap being tested.)
        for (int k = 0; k < 15; k++) {
            uint32_t id = 0;
            REQUIRE(lp->addListRow(id));
            lp->setListRowField(id, "channels", "{\"value\":24}");
            if (k == 0) {
                markerId = id;
                lp->setListRowField(id, "name", "{\"value\":\"MARKER\"}");
                lp->setListRowField(id, "ch5", "{\"value\":5}");   // Pan at ch5 — a distinctive pick
            }
        }
        lp->markDirty();
        mm::FilesystemModule::noteDirty();
        fs->flush();

        // The saved file must exist AND be larger than the old 2048 cap (proving the cap is gone).
        const std::string path = std::string(tmpRoot) + "/.config/LightPresetsModule.json";
        REQUIRE(std::filesystem::exists(path));
        CHECK(std::filesystem::file_size(path) > 2048u);
        scheduler.release();
    }
    {
        // Fresh boot: load the big file (file-sized heap read) and confirm the wiring survived.
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        auto* lp = new mm::LightPresetsModule();
        lp->setTypeName("LightPresetsModule");
        scheduler.addModule(fs);
        scheduler.addModule(lp);
        scheduler.setup();

        CHECK(lp->listRowCount() == 28);   // 13 built-ins + 15 custom, all restored
        mm::Correction c;
        REQUIRE(lp->deriveCorrection(markerId, 255, c));   // the marker preset resolves after reload
        CHECK(c.outChannels == 24);                        // its 24-channel width survived
        scheduler.release();
    }

    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Singleton survives probe lifecycle: /api/types factory-creates a probe of every
// registered type (including FilesystemModule) to capture defaults, then deletes it.
// The probe's destructor must NOT clear the singleton — otherwise every save path
// (noteDirty, debounced tick1s, flushPending on reboot) silently no-ops for the rest
// of the device's life. The fix is to register the singleton in setScheduler(), not
// in the constructor. This test catches that singleton-clear regression.
// /api/types factory-creates a temporary FilesystemModule probe; its destruction must NOT clear the static singleton (otherwise every later save silently no-ops).
TEST_CASE("FilesystemModule singleton survives probe construct+destruct") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_singleton_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    mm::ModuleFactory::registerType<mm::FilesystemModule>("FilesystemModule");
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    mm::Scheduler scheduler;

    // 1) Real FS instance, registered via setScheduler (this is the singleton-binding path).
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // 2) Mimic /api/types: factory-construct a probe FilesystemModule, then delete it.
    //    Before the fix, the probe's destructor cleared the static singleton because
    //    `instance_ == this` for the probe at destruction time.
    {
        auto* probe = mm::ModuleFactory::create("FilesystemModule");
        REQUIRE(probe != nullptr);
        delete probe;
    }

    // 3) After the probe died, noteDirty() must still reach the real singleton. We
    //    verify it indirectly: mark Layer dirty + flush, and observe that the Layer.json
    //    file appears on disk. If the singleton was lost, flush would be a no-op
    //    (flushPending() returns early when instance_ is null) and the file would not
    //    exist.
    layer->markDirty();
    mm::FilesystemModule::flushPending();

    std::ifstream f(std::string(tmpRoot) + "/.config/Layer.json");
    CHECK(f.is_open());
    f.close();  // Windows holds an exclusive lock on open files — close before remove_all.

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Regression: Int16 controls (GridLayout's width/height/depth, Layer's start/end)
// round-tripped through the filesystem load path were clamped to c.min/c.max,
// which default to 0,0 because ControlDescriptor.min/max are uint8_t and can't
// represent an int16 range. Every Int16 control loaded as 0 — so a 128×128 grid
// became 0×0×0 after restart and the whole pipeline allocated no buffers.
// Int16 controls (GridLayout width/height, RegionModifier start/end) preserve their saved value across load — no zero-clamping from uint8 min/max bounds.
TEST_CASE("FilesystemModule Int16 controls round-trip preserves the saved value") {
    char tmpRoot[256];
    std::snprintf(tmpRoot, sizeof(tmpRoot), "/tmp/mm_int16_test_%u",
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(tmpRoot);
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    mm::platform::fsSetRoot(tmpRoot);

    // Hand-write a RegionModifier.json with non-zero Int16 values (including
    // negatives, which are legal on the wire) so the load path is exercised
    // without needing a save-side step.
    std::ofstream out(std::string(tmpRoot) + "/.config/RegionModifier.json");
    out << "{\"enabled\":true,\"startX\":42,\"startY\":-17,\"startZ\":0,"
        << "\"endX\":100,\"endY\":-100,\"endZ\":0}";
    out.close();

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* region = new mm::RegionModifier();
    region->setTypeName("RegionModifier");
    scheduler.addModule(fs);
    scheduler.addModule(region);
    scheduler.setup();

    CHECK(region->startX == 42);
    CHECK(region->startY == -17);
    CHECK(region->endX == 100);
    CHECK(region->endY == -100);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}
