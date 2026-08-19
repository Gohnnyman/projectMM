#pragma once

#include "core/moonlive/MoonLive.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm::moonlive {

/// Where a scripted module's script file lives. One fixed directory, the way `/.config` holds
/// persisted state: a module stores a NAME, not a path, so it cannot reach outside this folder and
/// the File Manager has one obvious place to look.
inline constexpr const char* kScriptDir = "/moonlive";

/// A script's ROLE, in its file name. One language, three extensions: an effect is `.mle`, a
/// layout `.mll`, a modifier `.mlm`.
///
/// Stated by the author rather than derived from the script's contents. Deriving it is tempting
/// (the entry point a class defines already tells the engine which moment to call), but that
/// couples a UI filter to a language feature: the day a modifier wants a per-frame `tick()`, every
/// modifier would start appearing in effect pickers with no code changed anywhere. A name is
/// explicit, visible in any file listing without opening the file, and cannot drift.
///
/// The engine itself is role-BLIND and stays that way: it runs whichever moment the binding asks
/// for, so a class defining several is still legal. The extension decides which picker offers a
/// file, not what the engine will do with it.
inline constexpr const char* kEffectExt   = ".mle";
inline constexpr const char* kLayoutExt   = ".mll";
inline constexpr const char* kModifierExt = ".mlm";

/// What a NEW script starts out as, per role. A created file is a WORKING example rather than an
/// empty one: an empty file fails to parse the moment it is made, so the first thing a new script
/// would say is an error message. Each role gets the moment it is actually asked about (`tick` for
/// an effect, `placeLights` for a layout, `modifyLogical` for a modifier) and one control, so the
/// shape of a script is visible before a line is typed.
inline constexpr const char* kEffectTemplate =
    "class NewEffect {\n"
    "  uint8_t bpm = 60;\n"
    "\n"
    "  defineControls() {\n"
    "    addUint8(\"bpm\", bpm, 1, 255);\n"
    "  }\n"
    "\n"
    "  tick() {\n"
    "    fill(scale(beat(bpm, t), 256), 0, 100);\n"
    "  }\n"
    "}\n";

inline constexpr const char* kLayoutTemplate =
    "class NewLayout {\n"
    "  uint8_t cols = 16;\n"
    "  uint8_t rows = 16;\n"
    "\n"
    "  defineControls() {\n"
    "    addUint8(\"cols\", cols, 1, 64);\n"
    "    addUint8(\"rows\", rows, 1, 64);\n"
    "  }\n"
    "\n"
    "  placeLights() {\n"
    "    for (y = 0; y < rows; y = y + 1) {\n"
    "      for (x = 0; x < cols; x = x + 1) {\n"
    "        addLight(x, y, 0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n";

inline constexpr const char* kModifierTemplate =
    "class NewModifier {\n"
    "  modifyLogical() {\n"
    "    setXYZ(width - 1 - xPos, yPos, zPos);\n"
    "  }\n"
    "}\n";

/// What a `script` control tells the UI: where the files are, which of them to offer, and what a
/// new one starts as. Borrowed by the control descriptor (addFilePath), so these live here next to
/// the directory they name rather than being repeated in each binding.
inline constexpr const char* kEffectPick[3]   = {kScriptDir, kEffectExt,   kEffectTemplate};
inline constexpr const char* kLayoutPick[3]   = {kScriptDir, kLayoutExt,   kLayoutTemplate};
inline constexpr const char* kModifierPick[3] = {kScriptDir, kModifierExt, kModifierTemplate};

/// The largest script the loader will read into RAM at once. Not a language limit — the buffer is
/// sized to the FILE and freed the moment the compile ends — but a bound so a stray large file
/// cannot ask a 320 KB device for an allocation it will not survive.
inline constexpr long kScriptFileMax = 16384;

/// Longest script name accepted, and the bound on the path buffer below. The bindings size their
/// `script` control buffer from this (`kMaxScriptName + 1`), so a name this loader would accept can
/// always be held: a shorter control would truncate silently, and truncation can strip the
/// extension that makes a name valid at all.
inline constexpr size_t kMaxScriptName = 40;

/// Read `<kScriptDir>/<name>` and compile it. The source lives in a right-sized heap buffer for the
/// duration of the compile and is freed before returning, so a module holds a filename (~32 B) and
/// the emitted code — never the script text. That is the whole point: the fixed per-module arrays
/// this replaces cost ~2 KB EACH, resident whether or not a script was loaded.
///
/// Returns true when the script compiled. On any failure `err` names it, in the words a user needs:
/// which file, and what was wrong with it.
/// FNV-1a over the script text. A caller that must know "did this change" keeps 4 bytes rather than
/// a second copy of the source, which is the whole reason the text is not resident any more.
inline uint32_t scriptHash(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= static_cast<uint8_t>(s[i]); h *= 16777619u; }
    return h;
}

/// The hash of `<kScriptDir>/<name>`'s CURRENT text, without compiling it.
///
/// Answers "has the file changed since I compiled it" for the cost of ONE read, which is what a
/// binding asks on every prepare sweep. It costs the same whole-file read compileScriptFile makes
/// and skips everything after: the parse, the codegen, and the exec-block allocation.
///
/// False when the file is missing, unreadable or outside the accepted bounds, which the caller
/// treats as "not the thing I compiled" and lets compileScriptFile report properly. Reporting the
/// diagnostic here too would put the same message in two places.
inline bool scriptFileHash(const char* name, uint32_t& out) {
    if (!name || !name[0]) return false;
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", kScriptDir, name);
    const long size = platform::fsSize(path);
    if (size <= 0 || size > kScriptFileMax) return false;

    // ONE read of the WHOLE file, not a chunked walk. fsReadAt opens and closes the file on every
    // call, so hashing a 2 KB script through a small stack window cost 16 open/close cycles per
    // module per prepare sweep. On a P4 that boot-looped with `Cache error`: LittleFS sits behind
    // the flash cache and the sweep runs three scripted modules at once.
    //
    // Hashing only a WINDOW was the other tempting fix and is worse: four shipped scripts are over
    // 1 KB, so an edit past the window would go undetected and the module would keep running the
    // previous program. A change-detector that misses changes is not one.
    //
    // The heap allocation is the same one compileScriptFile makes, on the same cold path, and it is
    // freed before returning. It buys the whole file with one open, and this runs only when a
    // prepare sweep asks, not per frame.
    char* text = static_cast<char*>(platform::alloc(static_cast<size_t>(size) + 1));
    if (!text) return false;                          // no memory is "cannot answer", not "unchanged"
    const int got = platform::fsRead(path, text, static_cast<size_t>(size) + 1);
    const bool ok = got > 0;
    if (ok) out = scriptHash(text, static_cast<size_t>(got));
    platform::free(text);
    return ok;
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
    // One of the three script extensions, so a stray name cannot pull in an unrelated file that
    // happens to sit alongside. ANY of them: the loader is role-blind, exactly as the engine is,
    // and which picker offered the file is the binding's business. The upper bound also lets the
    // compiler see that the snprintf below cannot truncate.
    const size_t len = std::strlen(name);
    const char* tail = len >= 4 ? name + len - 4 : "";
    const bool known = std::strcmp(tail, kEffectExt) == 0 ||
                       std::strcmp(tail, kLayoutExt) == 0 ||
                       std::strcmp(tail, kModifierExt) == 0;
    if (len < 5 || len > kMaxScriptName || !known) {
        err = "script name must end in .mle, .mll or .mlm"; return false;
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
