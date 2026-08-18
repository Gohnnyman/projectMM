#pragma once

#include <cstdio>
#include <cstring>

// Wrap a bare statement body in the class and entry point a MoonLive script needs.
//
// A script is a class: `class T { tick() { … } }`. Nearly every test here is about ONE behavior
// inside that body (a loop counter surviving a call, a control keeping its value, a golden byte
// sequence), and spelling the enclosing class out at every call site would bury the assertion under
// four lines of identical ceremony. This puts the ceremony in one place so a test reads as what it
// is about.
//
// A test that is ABOUT the class shape (the grammar accepting or refusing a form, the class name in
// a diagnostic) passes its own complete source and does not use this.
/// As mmScript, but naming the ENTRY POINT the binding under test will ask for: a layout calls
/// `placeLights`, a modifier `modifyLogical`, an effect `tick`. A test for one binding has to emit
/// the name that binding looks up, or the script compiles and the binding then finds nothing.
inline const char* mmScriptAs(const char* entry, const char* body);

inline const char* mmScriptAs(const char* entry, const char* body) {
    // A RING of buffers, thread-local. Thread-local because the concurrency test compiles from two
    // threads at once and a shared buffer would hand each the other's source. A ring because tests
    // build TABLES of scripts (`{mmScript(a), …}, {mmScript(b), …}`) that hold the pointers and use
    // them later: with a single buffer every row would alias the last one written, and the test
    // would quietly check the same script N times instead of failing.
    //
    // Both bounds are measured, not guessed: the longest script passed here is 116 bytes and the
    // deepest table is 8 rows, so this is ~4x headroom on each and 8 KB per thread rather than the
    // 64 KB a round 16x4096 would have cost. A caller past either bound owns a std::string instead,
    // which the oversize refusal below makes loud rather than silent.
    static constexpr size_t kSlots = 12;
    static constexpr size_t kSlotBytes = 640;
    thread_local char ring[kSlots][kSlotBytes];
    thread_local unsigned next = 0;
    char* wrapped = ring[next++ % kSlots];

    // LEADING DECLARATIONS GO TO CLASS SCOPE. A test body often opens with `uint8_t speed = 7;`,
    // which is a member (or a declared control), and a member inside a function body is not what
    // the script means. Splitting here rather than at 60 call sites keeps every test reading as the
    // body it is about, and matches how the shipped scripts are laid out.
    const char* p = body;
    const char* declEnd = body;
    while (true) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (std::strncmp(p, "uint8_t", 7) != 0) break;
        const char* semi = std::strchr(p, ';');
        if (!semi) break;
        const char* eol = std::strchr(semi, '\n');
        declEnd = eol ? eol : semi + 1;
        p = declEnd;
    }

    // A body past the buffer would be SILENTLY CLIPPED by snprintf, and the test would then check a
    // script nobody wrote, which is how the runaway-script case stopped being a runaway. Refuse
    // instead: a caller with a script this long builds its own std::string.
    if (std::strlen(body) + 64 >= kSlotBytes) return "";

    if (declEnd != body) {
        const int declLen = static_cast<int>(declEnd - body);
        std::snprintf(wrapped, kSlotBytes, "class T {\n%.*s\n  %s() {\n%s\n  }\n}\n",
                      declLen, body, entry, declEnd);
    } else {
        std::snprintf(wrapped, kSlotBytes, "class T {\n  %s() {\n%s\n  }\n}\n", entry, body);
    }
    return wrapped;
}

inline const char* mmScript(const char* body) { return mmScriptAs("tick", body); }
