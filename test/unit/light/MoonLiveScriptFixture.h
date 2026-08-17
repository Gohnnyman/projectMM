#pragma once

#include "doctest.h"
#include "light/moonlive/MoonLiveScriptFile.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

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
/// Every script this fixture wrote, removed when the test process exits.
///
/// The scripts go in the SAME directory a real install keeps its scripts in — that is what makes the
/// test meaningful — so leaving them behind drops a hundred `t*.mlv` files among the user's own, and
/// the next run adds another hundred. Deleting each file at the end of its test would be wrong: a
/// test compiles the script and then re-reads it through the module, so the file has to outlive the
/// call. Process exit is the first moment they are all certainly finished with.
inline std::vector<std::string>& mmScriptRegistry() {
    static std::vector<std::string> paths;
    static const bool once = [] {
        std::atexit([] {
            for (const auto& p : mmScriptRegistry()) mm::platform::fsRemove(p.c_str());
        });
        return true;
    }();
    (void)once;
    return paths;
}

inline const char* mmWriteScript(const char* text) {
    static std::atomic<int> counter{0};
    thread_local char name[32];
    std::snprintf(name, sizeof(name), "t%d.mlv", ++counter);

    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", mm::moonlive::kScriptDir, name);
    mm::platform::fsMkdir(mm::moonlive::kScriptDir);
    REQUIRE(mm::platform::fsWriteAtomic(path, text, std::strlen(text)));
    {
        // The concurrency test writes from two threads, so the shared registry needs a lock even
        // though each thread's NAME is its own.
        static std::mutex m;
        std::lock_guard<std::mutex> lock(m);
        mmScriptRegistry().emplace_back(path);
    }
    return name;
}
