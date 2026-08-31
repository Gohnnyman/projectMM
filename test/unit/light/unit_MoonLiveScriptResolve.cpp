// @module MoonLive
// @also MoonLiveLayout, MoonLiveEffect, MoonLiveModifier

// Which FILE a script name means.
//
// A device keeps factory scripts in `/.moonlive`, downloaded by the UI from the shipped catalog,
// and the user's own in `/moonlive`. A name can therefore exist in one, the other, or both, and
// which one wins is what makes editing a factory script a fork rather than a change to it: the
// user's copy shadows the factory one, and deleting that copy restores the original without
// needing a network.
//
// These pin the three cases plus the one that used to be a bug waiting to happen: both readers
// (the compiler and the change-detector) must resolve to the SAME file, or a fork would be
// compiled from one and hashed from the other and recompile on every prepare sweep forever.

#include "doctest.h"
#include "light/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {

/// Write `text` to `dir/name`, and remember it so the test can take it away again.
void put(const char* dir, const char* name, const char* text) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    platform::fsMkdir(dir);
    REQUIRE(platform::fsWriteAtomic(path, text, std::strlen(text)));
}

void drop(const char* dir, const char* name) {
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", dir, name);
    platform::fsRemove(path);
}

/// A script that compiles and is trivially told apart from another by its control name, so a test
/// can prove WHICH file was read rather than merely that something was.
std::string scriptWith(const char* controlName) {
    return std::string("class R { byte v = 1; defineControls() { addControl(\"") + controlName +
           "\", v, 0, 9); } tick() { fill(0, 0, 0); } }";
}

/// Both directories cleared of `name`, so one test cannot leak into the next.
struct Clean {
    const char* name;
    explicit Clean(const char* n) : name(n) { wipe(); }
    ~Clean() { wipe(); }
    void wipe() const {
        drop(moonlive::kScriptDir, name);
        drop(moonlive::kFactoryScriptDir, name);
    }
};

}  // namespace

// The ordinary case for a script nobody has edited: it lives only in the factory directory, and
// naming it is enough. Without the fallback every downloaded script would report "script not found".
TEST_CASE("a factory script resolves when the user has no copy of it") {
    const char* name = "resolve-factory.mle";
    Clean clean(name);
    put(moonlive::kFactoryScriptDir, name, scriptWith("factory").c_str());

    char path[96];
    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kFactoryScriptDir) + "/" + name);
}

// THE fork rule. The editor only ever saves to the user directory, so a copy there is the user's
// edit of a factory script, and it has to win or an edit would appear to do nothing.
TEST_CASE("a user's copy shadows the factory script of the same name") {
    const char* name = "resolve-both.mle";
    Clean clean(name);
    put(moonlive::kFactoryScriptDir, name, scriptWith("factory").c_str());
    put(moonlive::kScriptDir, name, scriptWith("mine").c_str());

    char path[96];
    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kScriptDir) + "/" + name);
}

// Un-editing, and the reason the two directories exist at all: deleting the fork restores the
// factory script with no network, where a single directory would need it downloaded again.
TEST_CASE("deleting a user's copy restores the factory script") {
    const char* name = "resolve-revert.mle";
    Clean clean(name);
    put(moonlive::kFactoryScriptDir, name, scriptWith("factory").c_str());
    put(moonlive::kScriptDir, name, scriptWith("mine").c_str());

    char path[96];
    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    REQUIRE(std::string(path) == std::string(moonlive::kScriptDir) + "/" + name);

    drop(moonlive::kScriptDir, name);

    REQUIRE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kFactoryScriptDir) + "/" + name);
}

// A name in neither directory is not found, and the path it reports is the USER one: a message
// naming a place a user would not write to sends them looking in the wrong folder.
TEST_CASE("a script in neither directory is not found") {
    const char* name = "resolve-absent.mle";
    Clean clean(name);

    char path[96];
    CHECK_FALSE(moonlive::resolveScript(name, path, sizeof(path)));
    CHECK(std::string(path) == std::string(moonlive::kScriptDir) + "/" + name);
}

// The two readers must agree. compileScriptFile reads the text and scriptFileHash answers "has it
// changed since I compiled it": resolve them differently and a fork compiles from one file while
// its hash comes from the other, so it looks changed on every prepare sweep and recompiles forever.
TEST_CASE("the compiler and the change-detector read the same file") {
    const char* name = "resolve-agree.mle";
    Clean clean(name);
    const std::string factory = scriptWith("factory");
    const std::string mine    = scriptWith("mine");
    put(moonlive::kFactoryScriptDir, name, factory.c_str());
    put(moonlive::kScriptDir, name, mine.c_str());

    uint32_t hash = 0;
    REQUIRE(moonlive::scriptFileHash(name, hash));
    // The hash of the USER's text, not the factory one, since that is the file that will compile.
    CHECK(hash == moonlive::scriptHash(mine.c_str(), mine.size()));
    CHECK(hash != moonlive::scriptHash(factory.c_str(), factory.size()));

    // And once the fork is gone, both follow to the factory copy together.
    drop(moonlive::kScriptDir, name);
    REQUIRE(moonlive::scriptFileHash(name, hash));
    CHECK(hash == moonlive::scriptHash(factory.c_str(), factory.size()));
}
