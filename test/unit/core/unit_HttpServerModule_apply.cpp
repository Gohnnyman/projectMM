// @module HttpServerModule

#include "doctest.h"
#include "core/HttpServerModule.h"
#include "core/Scheduler.h"
#include "core/ModuleFactory.h"
#include "core/MoonModule.h"
#include "core/JsonSink.h"

#include <cstring>

// Pins the transport-free apply-core that HttpServerModule exposes — applyAddModule
// / applySetControl / applyClearChildren / applyOp. These are the operations the
// HTTP /api/modules + /api/control handlers do, factored out of the TcpConnection so
// BOTH the HTTP path and the Improv-serial APPLY_OP path drive one shared
// implementation ("Improv = REST over serial"). Testing them directly here, without
// a socket, is the unit-test win of the extraction: the apply logic is now provable
// in isolation. Also exercises the robustness rule (the apply-core tolerates bad
// input — unknown module, unknown type, malformed op — without crashing, returning a
// typed result instead).

namespace {

// A leaf with one editable Uint8 control + a child-accepting container, so we can
// add, set, and clear-children without pulling in real light modules.
struct Knob : public mm::MoonModule {
    uint8_t value = 10;
    bool showExtra = false;   // toggling this ADDS/REMOVES a control → a real schema change
    void defineControls() override {
        controls_.addControl("value", value, 0, 100);
        if (showExtra) controls_.addControl("extra", value, 0, 100);
    }
};
struct Box : public mm::MoonModule {
    // accepts any child (the HTTP role gate lives above the apply-core).
};

// A leaf with a VALIDATED Text control — mirrors SystemModule.deviceModel: the printable-
// ASCII rule is a per-control validator, so a bad value is rejected on EVERY write path
// (including the APPLY_OP `set` the installer uses), not in a bespoke per-transport RPC.
struct Tag : public mm::MoonModule {
    char label[32] = "init";
    static bool printableAscii(const char* v) {
        if (!v) return false;
        size_t n = std::strlen(v);
        if (n == 0 || n >= 32) return false;
        for (size_t i = 0; i < n; i++) {
            unsigned char b = static_cast<unsigned char>(v[i]);
            if (b < 0x20 || b > 0x7E) return false;
        }
        return true;
    }
    void defineControls() override {
        controls_.addText("label", label, sizeof(label), printableAscii);
    }
};

// A stand-in for the real Drivers module the WLED shim targets: an `on` Bool + a `brightness`
// Uint8, the two controls applyWledState drives. Named "Drivers" so findModuleByName resolves it.
struct FakeDrivers : public mm::MoonModule {
    bool on = true;
    uint8_t brightness = 20;
    void defineControls() override {
        controls_.addControl("on", on);
        controls_.addControl("brightness", brightness, 0, 255);
    }
};

// Build a tree: scheduler root "Root" (a Box) with HttpServerModule wired to it.
// Returns via out-params so each case starts clean. Caller owns release via the
// scheduler.
void registerTestTypes() {
    static bool done = false;
    if (done) return;
    mm::ModuleFactory::registerType<Knob>("Knob");
    mm::ModuleFactory::registerType<Box>("Box");
    mm::ModuleFactory::registerType<Tag>("Tag");
    mm::ModuleFactory::registerType<FakeDrivers>("Drivers");
    done = true;
}

// Find a direct child of `parent` by name (the test inspects the tree directly
// rather than through HttpServerModule's private findModuleByName).
mm::MoonModule* childNamed(mm::MoonModule* parent, const char* name) {
    for (uint8_t i = 0; i < parent->childCount(); i++) {
        auto* c = parent->child(i);
        if (c && std::strcmp(c->name(), name) == 0) return c;
    }
    return nullptr;
}

} // namespace

TEST_CASE("apply-core: applyAddModule adds a child, idempotent on the id") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);

    using OpResult = mm::HttpServerModule::OpResult;

    // Add a Knob named "K" under "Root".
    CHECK(http.applyAddModule("Knob", "K", "Root") == OpResult::Ok);
    CHECK(childNamed(root, "K") != nullptr);

    // Idempotent: re-adding the same id is AlreadyExists (no duplicate) — a distinct
    // success the HTTP handler reports as {"ok":true,"note":"already exists"}.
    CHECK(http.applyAddModule("Knob", "K", "Root") == OpResult::AlreadyExists);
    CHECK(root->childCount() == 1);

    // Unknown type / missing parent / top-level add are typed failures, not crashes.
    CHECK(http.applyAddModule("NopeType", "X", "Root") == OpResult::UnknownType);
    CHECK(http.applyAddModule("Knob", "Y", "NoSuchParent") == OpResult::ModuleNotFound);
    CHECK(http.applyAddModule("Knob", "Z", "") == OpResult::BadRequest);  // no parent → top-level

    s.deleteTree(root);
}

// applyAddModule reports the created module's FINAL name (post-disambiguation) via outName, so the
// HTTP handler can return it and the UI can select + focus the new module (the "+" focus fix).
TEST_CASE("apply-core: applyAddModule reports the created name, disambiguated") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    // An explicit id comes back verbatim.
    char name[32] = {};
    REQUIRE(http.applyAddModule("Knob", "First", "Root", name, sizeof(name)) == OpResult::Ok);
    CHECK(std::strcmp(name, "First") == 0);

    // No explicit id → the type name is the base; a second same-type add disambiguates, and the
    // REPORTED name is the disambiguated one (what the UI must select, not the colliding base).
    char n1[32] = {}, n2[32] = {};
    REQUIRE(http.applyAddModule("Knob", "", "Root", n1, sizeof(n1)) == OpResult::Ok);
    REQUIRE(http.applyAddModule("Knob", "", "Root", n2, sizeof(n2)) == OpResult::Ok);
    CHECK(std::strcmp(n1, n2) != 0);                         // the two got distinct names
    CHECK(childNamed(root, n1) != nullptr);                  // each reported name resolves to a real child
    CHECK(childNamed(root, n2) != nullptr);

    // outName is optional — the APPLY_OP transport passes nullptr and must still succeed.
    CHECK(http.applyAddModule("Knob", "NoReport", "Root") == OpResult::Ok);

    s.deleteTree(root);
}

TEST_CASE("apply-core: applySetControl writes a value, rejects out-of-range / unknown") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    REQUIRE(http.applyAddModule("Knob", "K", "Root") == OpResult::Ok);

    // The value JSON is the same {"value":N} body the HTTP handler reads by key.
    CHECK(http.applySetControl("K", "value", "{\"value\":42}") == OpResult::Ok);
    auto* k = static_cast<Knob*>(childNamed(root, "K"));
    REQUIRE(k != nullptr);
    CHECK(k->value == 42);

    // Out of the 0..100 range → typed rejection, value left unchanged.
    CHECK(http.applySetControl("K", "value", "{\"value\":999}") == OpResult::OutOfRange);
    CHECK(k->value == 42);

    // Unknown module vs unknown control → distinct typed failures (each a 404 with
    // its own body on the HTTP path), no crash.
    CHECK(http.applySetControl("Nope", "value", "{\"value\":1}") == OpResult::ModuleNotFound);
    CHECK(http.applySetControl("K", "nope", "{\"value\":1}") == OpResult::ControlNotFound);

    s.deleteTree(root);
}

TEST_CASE("apply-core: applyClearChildren empties a container (replaceChildren)") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    REQUIRE(http.applyAddModule("Knob", "A", "Root") == OpResult::Ok);
    REQUIRE(http.applyAddModule("Knob", "B", "Root") == OpResult::Ok);
    CHECK(root->childCount() == 2);

    CHECK(http.applyClearChildren("Root") == OpResult::Ok);
    CHECK(root->childCount() == 0);

    // Clearing a non-existent parent is ModuleNotFound, not a crash. Clearing an
    // already-empty container is Ok.
    CHECK(http.applyClearChildren("Nope") == OpResult::ModuleNotFound);
    CHECK(http.applyClearChildren("Root") == OpResult::Ok);

    s.deleteTree(root);
}

TEST_CASE("apply-core: applyOp dispatches each op type and tolerates bad input") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    // The op JSON shapes are exactly what the installer pushes over APPLY_OP.
    CHECK(http.applyOp("{\"op\":\"add\",\"type\":\"Knob\",\"id\":\"K\",\"parent\":\"Root\"}") == OpResult::Ok);
    CHECK(childNamed(root, "K") != nullptr);

    CHECK(http.applyOp("{\"op\":\"set\",\"module\":\"K\",\"control\":\"value\",\"value\":7}") == OpResult::Ok);
    CHECK(static_cast<Knob*>(childNamed(root, "K"))->value == 7);

    CHECK(http.applyOp("{\"op\":\"clearChildren\",\"parent\":\"Root\"}") == OpResult::Ok);
    CHECK(root->childCount() == 0);

    // Unknown op verb and a malformed (no "op") object are BadRequest, not crashes —
    // the robustness rule: any pushed bytes are tolerated.
    CHECK(http.applyOp("{\"op\":\"frobnicate\"}") == OpResult::BadRequest);
    CHECK(http.applyOp("{\"nope\":1}") == OpResult::BadRequest);

    s.deleteTree(root);
}

// A per-control validator (like SystemModule.deviceModel's printable-ASCII rule) is
// enforced THROUGH the apply-core — so the APPLY_OP `set` the installer pushes over
// serial is guarded exactly like an HTTP write, with no per-transport special-casing.
// This is the point of moving validation onto the control: one backend check, every path.
TEST_CASE("apply-core: a control validator rejects bad input on the set/APPLY_OP path") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;

    REQUIRE(http.applyAddModule("Tag", "T", "Root") == OpResult::Ok);
    auto* tag = static_cast<Tag*>(childNamed(root, "T"));
    REQUIRE(tag != nullptr);

    // Valid value applies — via applySetControl (HTTP path) ...
    CHECK(http.applySetControl("T", "label", "{\"value\":\"LOLIN D32\"}") == OpResult::Ok);
    CHECK(std::strcmp(tag->label, "LOLIN D32") == 0);

    // ... and via applyOp (the APPLY_OP-over-serial path) — same shape the installer sends.
    CHECK(http.applyOp("{\"op\":\"set\",\"module\":\"T\",\"control\":\"label\",\"value\":\"Living Room\"}")
          == OpResult::Ok);
    CHECK(std::strcmp(tag->label, "Living Room") == 0);

    // A raw control byte in the value → Malformed on the APPLY_OP path, prior value kept.
    const char badOp[] = {'{','"','o','p','"',':','"','s','e','t','"',',',
                          '"','m','o','d','u','l','e','"',':','"','T','"',',',
                          '"','c','o','n','t','r','o','l','"',':','"','l','a','b','e','l','"',',',
                          '"','v','a','l','u','e','"',':','"','x', 0x01, '"','}', 0};
    CHECK(http.applyOp(badOp) == OpResult::Malformed);
    CHECK(std::strcmp(tag->label, "Living Room") == 0);   // unchanged — no partial write

    // Empty string → Malformed too (the validator rejects 0-length), prior value kept.
    CHECK(http.applySetControl("T", "label", "{\"value\":\"\"}") == OpResult::Malformed);
    CHECK(std::strcmp(tag->label, "Living Room") == 0);

    s.deleteTree(root);
}

// The WLED shim's `{on,bri}` apply drives the real `on` + `brightness` controls independently:
// turning off must NOT clobber the brightness value (the whole point of the shared `on` control,
// replacing the old bri=0 fudge). Home Assistant + the WLED app both post through this path.
TEST_CASE("apply-core: applyWledState sets on + bri independently (no brightness clobber)") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    auto* drivers = new FakeDrivers();
    drivers->setName("Drivers");
    drivers->defineControls();       // register on + brightness so applyWledState can find them
    s.addModule(drivers);
    mm::HttpServerModule http;
    http.setScheduler(&s);

    // Set a real brightness first.
    http.applyWledState("{\"bri\":200}");
    CHECK(drivers->brightness == 200);
    CHECK(drivers->on == true);

    // Turn OFF → on flips false, brightness value is PRESERVED (the fudge would have zeroed it).
    http.applyWledState("{\"on\":false}");
    CHECK(drivers->on == false);
    CHECK(drivers->brightness == 200);   // preserved — this is the subtraction's payoff

    // Turn back ON → on true, brightness still the level the user had.
    http.applyWledState("{\"on\":true}");
    CHECK(drivers->on == true);
    CHECK(drivers->brightness == 200);

    // Combined {on,bri} applies both.
    http.applyWledState("{\"on\":true,\"bri\":50}");
    CHECK(drivers->on == true);
    CHECK(drivers->brightness == 50);

    s.deleteTree(root);
    s.deleteTree(drivers);
}

// Diff-on-the-wire (the 1 Hz-stutter fix): the periodic WS push sends only CHANGED control values,
// not the whole ~34 KB tree every second. buildStatePatch value-hashes each leaf against a baseline
// and emits only the ones that differ. These pin the core guarantees: an unchanged tree → EMPTY patch
// (the whole point — no per-second re-serialise of static config), and a single value change → a
// one-entry patch addressed by "<module>/<control>".
TEST_CASE("buildStatePatch: unchanged tree yields an empty patch") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box(); root->setName("Root");
    auto* k = new Knob(); k->setName("K");
    root->addChild(k);
    s.addModule(root);
    s.setup();                                  // defineControls so K has its "value" control
    mm::HttpServerModule http; http.setScheduler(&s);

    http.baselineLeafHashesForTest();           // snapshot current values as the baseline
    mm::JsonSink sink;
    const uint16_t changed = http.buildStatePatchForTest(sink);
    CHECK(changed == 0);                         // nothing changed since baseline → empty
    CHECK(std::strcmp(sink.data(), "{\"patch\":[]}") == 0);

    s.deleteTree(root);
}

// A schema change (rebuildControls — hidden flags / option sets) can't be seen by the value-hash
// patch, so any module's rebuildControls() flips the WS full-resync flag through the static
// schema-changed hook. This is what carries a metadata-only change (WiFi addressing hides fields,
// a preset Select gains an option) to connected clients. Pins the hook wiring + the subtraction of
// the old per-call-site resyncs.
TEST_CASE("schema-changed hook: rebuildControls() resyncs ONLY on a real schema change") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box(); root->setName("Root");
    auto* k = new Knob(); k->setName("K");
    root->addChild(k);
    s.addModule(root);
    s.setup();
    mm::HttpServerModule http; http.setScheduler(&s);
    http.installSchemaHookForTest();                // hook only, no TCP listener (a port bind is flaky)

    // A value-only rebuild (same control set) must NOT resync — that's the common slider-drag path,
    // carried by the per-leaf value patch, not a full metadata resend.
    http.clearFullResyncForTest();
    CHECK(!http.fullResyncPendingForTest());
    k->rebuildControls();                           // schema unchanged (showExtra still false)
    CHECK(!http.fullResyncPendingForTest());        // …so the hook does NOT fire

    // A real schema change (a control appears) MUST resync — the value patch can't carry it.
    k->showExtra = true;
    k->rebuildControls();
    CHECK(http.fullResyncPendingForTest());         // …flips the resync flag via the hook

    // A CHILD's schema change caught by a rebuild on the PARENT — rebuildControls() rebuilds the
    // whole subtree, so the signature must recurse into children or the resync is dropped (the
    // preset-library case: adding a preset grows every child driver's Select while the parent's own
    // controls are unchanged). Toggle the child, rebuild the parent, expect a resync.
    http.clearFullResyncForTest();
    k->showExtra = false;
    root->rebuildControls();                        // parent rebuild cascades to the child
    CHECK(http.fullResyncPendingForTest());         // child's control vanished → caught via recursion

    http.release();                                 // unwires the hook (clears instance_)
    s.deleteTree(root);
}

TEST_CASE("buildStatePatch: a changed control value yields a one-entry patch") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box(); root->setName("Root");
    auto* k = new Knob(); k->setName("K");
    root->addChild(k);
    s.addModule(root);
    s.setup();
    mm::HttpServerModule http; http.setScheduler(&s);

    http.baselineLeafHashesForTest();
    k->value = 77;                               // a device-side value change (no setControl) — the
                                                 // value-compare catches it, which a dirty flag wouldn't
    mm::JsonSink sink;
    const uint16_t changed = http.buildStatePatchForTest(sink);
    CHECK(changed == 1);                          // exactly the one changed control
    // The entry addresses it by "<module>/<control>" and carries the new value.
    CHECK(std::strstr(sink.data(), "\"path\":\"K/value\"") != nullptr);
    CHECK(std::strstr(sink.data(), "\"value\":77") != nullptr);

    // Building again with nothing further changed → empty (the cache updated on the first emit).
    mm::JsonSink sink2;
    CHECK(http.buildStatePatchForTest(sink2) == 0);

    s.deleteTree(root);
}

// A module's STATUS must ride the 1 Hz value-diff, not the full state alone. A driver can fault at any
// moment (a bus that won't init, a loopback verdict, a Hue pairing result) with no schema change and no
// structural change — so nothing triggers a resync, and a status carried only by the full state would
// sit stale indefinitely. It is worse with the tabbed UI: a module whose card is behind a collapsed tab
// would surface no fault at all. This pins @status/@severity as patch leaves.
TEST_CASE("buildStatePatch: a status change rides the patch (no resync needed)") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box(); root->setName("Root");
    auto* k = new Knob(); k->setName("K");
    root->addChild(k);
    s.addModule(root);
    s.setup();
    mm::HttpServerModule http; http.setScheduler(&s);

    http.baselineLeafHashesForTest();            // baseline: K has no status
    {
        mm::JsonSink sink;
        CHECK(http.buildStatePatchForTest(sink) == 0);   // quiet tree → empty patch
    }

    // A fault appears — no control changed, no schema change, no structural change.
    k->setStatus("bus init failed", mm::MoonModule::Severity::Error);

    mm::JsonSink sink;
    const uint16_t changed = http.buildStatePatchForTest(sink);
    CHECK(changed > 0);                                          // it must reach the client…
    CHECK(std::strstr(sink.data(), "\"K/@status\"") != nullptr); // …addressed as a header leaf
    CHECK(std::strstr(sink.data(), "bus init failed") != nullptr);
    CHECK(std::strstr(sink.data(), "\"K/@severity\"") != nullptr);
    CHECK(std::strstr(sink.data(), "error") != nullptr);
    // The status value must be VALID JSON: singly-quoted, not `""bus init failed""`. The double-quote bug
    // (writeJsonString self-quotes, plus a manual quote wrap) produced invalid JSON that made the browser
    // drop the WHOLE patch — so the status never updated live. A strstr for the text alone missed it (the
    // text is still a substring of the malformed form), so assert the value's exact quoting here.
    CHECK(std::strstr(sink.data(), "\"value\":\"bus init failed\"") != nullptr);  // correct single quotes
    CHECK(std::strstr(sink.data(), "\"\"bus init failed\"\"") == nullptr);        // NOT double-quoted

    s.deleteTree(root);
}

// A spy for the schema-changed hook: HttpServerModule wires this hook to requestFullResync(), so counting its
// fires is exactly "how many full-state resyncs would this control change request." A static counter because
// the hook is a plain function pointer (MoonModule::setSchemaChangedHook), mirroring the production wiring.
static int g_schemaFires = 0;
static void countSchemaFire() { g_schemaFires++; }

// REGRESSION GUARD for the front/back-end sync class of bug that recurred several times: a control change the
// UI can only learn from the FULL state (never the per-second value patch) MUST request a full resync, or the
// client's cached state keeps the stale value and reverts the change ~1 s later. The canonical case is the
// module `enabled` toggle (it rides the full state, not the patch). The counterpart is equally load-bearing:
// an ORDINARY value change must NOT request a resync, or every slider drag nukes+rebuilds the whole UI (the
// "expander collapses / picker closes" symptom). This test pins BOTH directions at the one seam where they
// broke — Scheduler::setControl, via applySetControl — using the schema-changed hook as the resync signal.
TEST_CASE("apply-core: enabled toggle requests a full resync; a plain value change does not") {
    registerTestTypes();
    mm::Scheduler s;
    auto* root = new Box();
    root->setName("Root");
    s.addModule(root);
    mm::HttpServerModule http;
    http.setScheduler(&s);
    using OpResult = mm::HttpServerModule::OpResult;
    REQUIRE(http.applyAddModule("Knob", "K", "Root") == OpResult::Ok);

    mm::MoonModule::setSchemaChangedHook(&countSchemaFire);

    // (1) A plain value change must NOT fire the resync (its value rides the patch — a resync here would be
    // the over-rebuild that collapses open UI state). Knob.value has no conditional-visibility schema effect.
    g_schemaFires = 0;
    REQUIRE(http.applySetControl("K", "value", "{\"value\":42}") == OpResult::Ok);
    CHECK(g_schemaFires == 0);

    // (2) Toggling `enabled` MUST fire the resync (the fix): enabled rides the full state only, so without a
    // resync the client never learns the new value and reverts the toggle. Both directions of the toggle.
    g_schemaFires = 0;
    REQUIRE(http.applySetControl("K", "enabled", "{\"value\":false}") == OpResult::Ok);
    CHECK(g_schemaFires >= 1);   // disable → resync requested
    g_schemaFires = 0;
    REQUIRE(http.applySetControl("K", "enabled", "{\"value\":true}") == OpResult::Ok);
    CHECK(g_schemaFires >= 1);   // re-enable → resync requested

    mm::MoonModule::setSchemaChangedHook(nullptr);   // don't leak the spy into other tests
    s.deleteTree(root);
}

// --- a file write re-derives what was built from the file -------------------------------------
//
// Persistent state changes two ways: a control write, and a file write. Core enforced "re-derive
// what depends on it" for the first only, so saving a file's CONTENTS under an unchanged name
// changed nothing on the device: a scripted module kept running the program compiled from the
// PREVIOUS text, and the only way to make it notice was to re-name the file. Editing a script and
// seeing the fixture change is the loop this closes.
namespace {
// A module that counts its own prepare() calls. Observing the COUNT rather than the scheduler's
// private request flag keeps the test on the behavior (something re-derived) instead of on the
// mechanism (a bool was set), so a future change of mechanism does not have to rewrite it.
struct Prepares : public mm::MoonModule {
    uint8_t prepared = 0;
    void prepare() override { prepared++; }
};
}  // namespace

TEST_CASE("a written file asks the tree to re-derive") {
    mm::Scheduler s;
    auto* root = new Prepares();
    root->setName("Root");
    s.addModule(root);
    s.setup();

    mm::HttpServerModule http;
    http.setScheduler(&s);
    const uint8_t afterSetup = root->prepared;

    // A tick with nothing pending must not re-derive: the request is what triggers it, not the
    // passage of time. Without this the next check would pass even if the write did nothing.
    s.tick();
    REQUIRE(root->prepared == afterSetup);

    http.applyFileChanged("/moonlive/plasma.mle");
    s.tick();
    CHECK(root->prepared == afterSetup + 1);
}

TEST_CASE("a burst of file writes costs one re-derive, not one per file") {
    mm::Scheduler s;
    auto* root = new Prepares();
    root->setName("Root");
    s.addModule(root);
    s.setup();

    mm::HttpServerModule http;
    http.setScheduler(&s);
    const uint8_t before = root->prepared;

    // The File Manager's multi-file upload writes several files back to back. Each asks, and the
    // request coalesces into the single sweep the next tick performs, so a ten-file upload does not
    // rebuild the tree ten times on the render thread.
    for (int i = 0; i < 10; i++) http.applyFileChanged("/moonlive/x.mle");
    s.tick();
    CHECK(root->prepared == before + 1);
}

TEST_CASE("a file write with no scheduler is a no-op, not a crash") {
    // HttpServerModule is constructed before it is wired, and the Improv path builds one without a
    // tree at all. Degrade visibly, never crash (the robustness rule).
    mm::HttpServerModule http;
    http.applyFileChanged("/moonlive/plasma.mle");   // must simply return
    CHECK(true);
}

// The preview channel's one uplink message: a masked client frame whose payload is [0x51][hint].
// These are network bytes, so the parser's job is mostly refusal: wrong opcode, unmasked, wrong
// length, truncated, all -1, never a read past the buffer.
TEST_CASE("preview uplink parser accepts exactly the one defined message") {
    // [0x82 binary][0x82 masked len2][mask 4][0x51^m0][7^m1]
    uint8_t good[] = {0x82, 0x82, 0x11, 0x22, 0x33, 0x44, static_cast<uint8_t>(0x51 ^ 0x11),
                      static_cast<uint8_t>(7 ^ 0x22)};
    CHECK(mm::HttpServerModule::parsePreviewUplink(good, sizeof(good)) == 7);

    uint8_t text[] = {0x81, 0x82, 0x11, 0x22, 0x33, 0x44, static_cast<uint8_t>(0x51 ^ 0x11),
                      static_cast<uint8_t>(2 ^ 0x22)};
    CHECK(mm::HttpServerModule::parsePreviewUplink(text, sizeof(text)) == 2);   // text frames too

    uint8_t unmasked[] = {0x82, 0x02, 0x51, 0x07};
    CHECK(mm::HttpServerModule::parsePreviewUplink(unmasked, sizeof(unmasked)) == -1);

    uint8_t wrongOp[] = {0x82, 0x82, 0, 0, 0, 0, 0x99, 0x07};
    CHECK(mm::HttpServerModule::parsePreviewUplink(wrongOp, sizeof(wrongOp)) == -1);

    uint8_t ping[] = {0x89, 0x80, 0, 0, 0, 0};
    CHECK(mm::HttpServerModule::parsePreviewUplink(ping, sizeof(ping)) == -1);

    CHECK(mm::HttpServerModule::parsePreviewUplink(good, 5) == -1);             // truncated
}

// TCP coalesces: two hints sent in quick succession can land in ONE read. The parser reports how
// many bytes a frame occupied so the caller can walk the whole buffer; stopping at the first would
// leave the device serving a stale request until the next window.
TEST_CASE("the preview uplink parser reports a frame's length so a coalesced read can be walked") {
    uint8_t two[] = {
        0x82, 0x82, 0x11, 0x22, 0x33, 0x44, static_cast<uint8_t>(0x51 ^ 0x11),
        static_cast<uint8_t>(2 ^ 0x22),                       // first hint: stride 2
        0x82, 0x82, 0x55, 0x66, 0x77, 0x88, static_cast<uint8_t>(0x51 ^ 0x55),
        static_cast<uint8_t>(8 ^ 0x66),                       // second hint: stride 8
    };
    int used = 0;
    CHECK(mm::HttpServerModule::parsePreviewUplink(two, sizeof(two), &used) == 2);
    CHECK(used == 8);
    CHECK(mm::HttpServerModule::parsePreviewUplink(two + used, static_cast<int>(sizeof(two)) - used,
                                                   &used) == 8);
    CHECK(used == 8);

    // A partial tail consumes nothing, so the walk stops instead of spinning or reading past it.
    int tail = 0;
    CHECK(mm::HttpServerModule::parsePreviewUplink(two, 5, &tail) == -1);
    CHECK(tail == 0);
}
