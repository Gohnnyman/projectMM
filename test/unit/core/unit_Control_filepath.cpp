// @module Control

// Pins ControlType::FilePath: a control whose VALUE is the name of a file, while the file's
// CONTENTS are edited in the UI and travel over /api/file.
//
// Why the type exists as its own type: a file body cannot ride /api/control at all (every route but
// /api/file and the firmware upload returns 413 once the request exceeds the server's buffer), so a
// control that means "this file" has to store a reference and leave the bytes to the streaming
// route. TextArea is its opposite: there the value IS the body. That is a difference in what the
// value MEANS, which no rendering flag expresses.

#include "doctest.h"
#include "core/Control.h"
#include "core/JsonSink.h"

#include <cstring>
#include <string>

namespace {
// What a module declares: where its files live and which of them to offer. Borrowed by the
// descriptor, so it has to outlive the control, exactly like addSelect's options array.
const mm::FilePathPick kScriptPick = {"/moonlive", ".mle", nullptr};
}  // namespace

TEST_CASE("a file-path control carries the directory and extension the module declared") {
    char script[41] = "plasma.mle";
    mm::ControlList controls;
    controls.addFilePath("script", script, sizeof(script), kScriptPick);
    REQUIRE(controls.count() == 1);
    CHECK(controls[0].type == mm::ControlType::FilePath);
    CHECK(std::strcmp(mm::controlTypeName(controls[0].type), "filepath") == 0);

    // The UI lists a directory and filters it without knowing what a script is: the module supplies
    // both facts, which is what keeps the control domain-neutral.
    mm::JsonSink sink;
    mm::writeControlMetadata(sink, controls[0]);
    const std::string meta = sink.data();
    CHECK(meta.find("\"dir\":\"/moonlive\"") != std::string::npos);
    CHECK(meta.find("\"ext\":\".mle\"") != std::string::npos);
}

TEST_CASE("a file-path control with no directory offers no picker rather than a broken one") {
    char path[41] = "";
    mm::ControlList controls;
    controls.addFilePath("file", path, sizeof(path));      // no picker: an editor with a fixed path
    mm::JsonSink sink;
    mm::writeControlMetadata(sink, controls[0]);
    const std::string meta = sink.data();
    CHECK(meta.find("\"dir\"") == std::string::npos);
}

TEST_CASE("a file-path control listing every file omits the extension filter") {
    static const mm::FilePathPick anyFile = {"/presets", nullptr, nullptr};
    char path[41] = "";
    mm::ControlList controls;
    controls.addFilePath("file", path, sizeof(path), anyFile);
    mm::JsonSink sink;
    mm::writeControlMetadata(sink, controls[0]);
    const std::string meta = sink.data();
    CHECK(meta.find("\"dir\":\"/presets\"") != std::string::npos);
    CHECK(meta.find("\"ext\"") == std::string::npos);
}

// The value is a NAME, never a body. A control write that tried to carry a file's contents would
// arrive here, and it must fill the buffer and stop rather than run off the end of it: any input,
// any size, degrade visibly (the robustness rule).
TEST_CASE("a file-path control stores a name, never a file body") {
    char script[41] = "";
    mm::ControlList controls;
    controls.addFilePath("script", script, sizeof(script), kScriptPick);

    const std::string big = "{\"script\":\"" + std::string(5000, 'x') + "\"}";
    const mm::ApplyResult r = mm::applyControlValue(controls[0], big.c_str(),
                                                    "script", mm::ApplyPolicy::Clamp);
    CHECK(r == mm::ApplyResult::Ok);   // accepted, and bounded by the buffer rather than refused
    CHECK(std::strlen(script) == sizeof(script) - 1);   // filled, and no further
    CHECK(script[sizeof(script) - 1] == '\0');          // still a valid C string
}

// It persists like the text control it is: a device that reboots comes back pointing at the same
// file, which is what makes "the script survives a power cycle" true.
TEST_CASE("a file-path control persists and reloads its name") {
    char script[41] = "ember.mle";
    mm::ControlList controls;
    controls.addFilePath("script", script, sizeof(script), kScriptPick);
    CHECK(mm::isPersistable(controls[0]));

    mm::JsonSink sink;
    mm::writeControlValue(sink, controls[0]);
    CHECK(std::strcmp(sink.data(), "\"ember.mle\"") == 0);

    std::snprintf(script, sizeof(script), "%s", "something-else.mle");
    mm::applyControlValue(controls[0], "{\"script\":\"ember.mle\"}", "script", mm::ApplyPolicy::Clamp);
    CHECK(std::strcmp(script, "ember.mle") == 0);
}
