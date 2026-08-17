#pragma once

#include "core/moonlive/MoonLive.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm::moonlive {

/// Where a scripted module's `.mlv` file lives. One fixed directory, the way `/.config` holds
/// persisted state: a module stores a NAME, not a path, so it cannot reach outside this folder and
/// the File Manager has one obvious place to look.
inline constexpr const char* kScriptDir = "/moonlive";

/// The largest script the loader will read into RAM at once. Not a language limit — the buffer is
/// sized to the FILE and freed the moment the compile ends — but a bound so a stray large file
/// cannot ask a 320 KB device for an allocation it will not survive.
inline constexpr long kScriptFileMax = 16384;

/// Longest script name accepted, and the bound on the path buffer below. The bindings size their
/// `script` control buffer from this (`kMaxScriptName + 1`), so a name this loader would accept can
/// always be held: a shorter control would truncate silently, and truncation can strip the `.mlv`
/// that makes a name valid at all.
inline constexpr size_t kMaxScriptName = 40;

/// Read `<kScriptDir>/<name>` and compile it. The source lives in a right-sized heap buffer for the
/// duration of the compile and is freed before returning, so a module holds a filename (~32 B) and
/// the emitted code — never the script text. That is the whole point: the fixed per-module arrays
/// this replaces cost ~2 KB EACH, resident whether or not a script was loaded.
///
/// Returns true when the script compiled. On any failure `err` names it, in the words a user needs:
/// which file, and what was wrong with it.
/// FNV-1a over the script text. A caller that must know "did this change" keeps 4 bytes rather than
/// a second copy of the source — which is the whole reason the text is not resident any more.
inline uint32_t scriptHash(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= static_cast<uint8_t>(s[i]); h *= 16777619u; }
    return h;
}

/// As compileScriptFile, and additionally reports the source's hash so a caller can tell a changed
/// script from an unchanged one without holding the text.
inline bool compileScriptFile(MoonLive& engine, const char* name,
                              const BuiltinTable& builtins, const SysVarTable& sysvars,
                              const char*& err, uint32_t* hashOut = nullptr) {
    // FIRST, before any validation can return: drop whatever is already compiled. Every check
    // below leaves through `return false`, and only engine.compile() releases the previous
    // program, so without this a rejected script (renamed, deleted, emptied) leaves the OLD one
    // executing while the module reports an error. The card says "script not found" and the
    // fixture keeps rendering the script that is gone.
    //
    // freeCode, not free: the control ARENA must survive, or a scripted control loses the live
    // value the user set whenever a compile fails.
    engine.freeCode();

    // The script directory must exist before anything can be SAVED into it, and on a fresh device
    // nothing has created it yet — the write endpoint does not make parent directories, so a first
    // save would fail with nowhere obvious to look. Creating it here (mkdir -p, a no-op when it is
    // already there) means naming a script is enough to make the folder appear.
    platform::fsMkdir(kScriptDir);

    if (!name || !name[0]) { err = "no script — set the script name"; return false; }

    // A BASENAME only. The fixed directory is the point — a module names a script, it does not
    // address the filesystem — so a separator or a `..` would let a control value reach outside
    // kScriptDir (`../.config/NetworkModule.json` reads the device's saved credentials). Rejected
    // rather than sanitised: a name that needs rewriting to be safe is a name a user mistyped.
    for (const char* c = name; *c; c++)
        if (*c == '/' || *c == '\\') { err = "script name is a file in the script folder, not a path"; return false; }
    if (std::strcmp(name, "..") == 0 || std::strncmp(name, "../", 3) == 0) {
        err = "script name is a file in the script folder, not a path"; return false;
    }
    // .mlv, so a stray name cannot pull in an unrelated file that happens to sit alongside. The
    // upper bound also lets the compiler see that the snprintf below cannot truncate.
    const size_t len = std::strlen(name);
    if (len < 5 || len > kMaxScriptName || std::strcmp(name + len - 4, ".mlv") != 0) {
        err = "script name must end in .mlv"; return false;
    }

    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kScriptDir, name);

    const long size = platform::fsSize(path);
    if (size < 0)               { err = "script not found"; return false; }
    if (size == 0)              { err = "script is empty";  return false; }
    if (size > kScriptFileMax)  { err = "script too large"; return false; }

    // +1 for the NUL the lexer reads as End. fsRead null-terminates on success, but the buffer has
    // to have room for it.
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) { err = "no memory for the script"; return false; }

    const int read = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    if (read <= 0) { platform::free(text); err = "script could not be read"; return false; }

    if (hashOut) *hashOut = scriptHash(text, static_cast<size_t>(read));
    const bool ok = engine.compile(text, builtins, sysvars);
    if (!ok) err = engine.error();
    // Freed on BOTH paths, before returning: the text has done its job either way, and a failed
    // compile is exactly when a device can least afford to leak.
    platform::free(text);
    return ok;
}

}  // namespace mm::moonlive
