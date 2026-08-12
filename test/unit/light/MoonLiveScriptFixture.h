#pragma once

#include "doctest.h"
#include "light/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

/// Put a script on the filesystem and return its NAME.
///
/// A scripted module holds a file name now, not the text — so a test that wants to compile
/// something has to write the file first, which is the same path the UI takes when it saves an
/// edit. Testing through the file is the point: it exercises what actually ships, rather than a
/// text-only path production never uses.
///
/// Each call gets a fresh name, so tests that compile several scripts do not collide.
/// Returns a name valid for the caller's thread until its next call. THREAD-LOCAL, not static: the
/// concurrency test compiles scripts from two threads at once, and a shared counter and buffer would
/// hand both threads the same name — each would then compile the other's script.
inline const char* mmWriteScript(const char* text) {
    static std::atomic<int> counter{0};
    thread_local char name[32];
    std::snprintf(name, sizeof(name), "t%d.mlv", ++counter);

    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    mm::platform::fsMkdir(mm::moonlive::kScriptDir);
    REQUIRE(mm::platform::fsWriteAtomic(path, text, std::strlen(text)));
    return name;
}
