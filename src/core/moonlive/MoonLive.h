#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveCompiler.h"   // CompileResult (carries the declared controls)
#include <cstring>   // std::strcmp — entry lookup by name

// MoonLive — the live-script engine core (domain-neutral, §3.1/§3.9 of
// livescripts-analysis-top-down.md). compile() turns a program into native code — either a
// source string (via MoonLiveCompiler) or a fixed routine direct from the emitter — places it
// in executable memory, and run() calls it over a host-supplied buffer. The path is
// emit → allocExec → call → write-the-buffer, running on Xtensa, RISC-V, and the desktop host.
//
// Neutral by construction: the engine includes only <cstdint>, the compiler/emitter seams, and
// the platform seam — never EffectBase, Buffer, or any projectMM type. The binding
// (src/light/moonlive/MoonLiveEffect.h) wraps it as a MoonModule.

namespace mm::moonlive {

class MoonLive {
public:
    MoonLive() = default;
    ~MoonLive() { free(); }

    // Owns a heap-backed exec block (freed in the destructor), so copying would duplicate
    // ownership and double-free. Non-copyable; each scripted module holds its own engine.
    MoonLive(const MoonLive&) = delete;
    MoonLive& operator=(const MoonLive&) = delete;

    // Compile a fixed-color program direct from the emitter: emit the routine, copy it into
    // an exec block, ready it to call. Returns ok(). A failure (no exec memory, emit too big)
    // leaves the engine !ok() with an error() — the caller degrades, never crashes.
    bool compile(uint8_t r, uint8_t g, uint8_t b);

    // Compile SOURCE TEXT, resolving function calls against `table` (the host's registered
    // built-ins — see MoonLiveBuiltins.h). The front-end parses an expression-call statement
    // and lowers it through the IR + per-ISA assembler. A parse/codegen error leaves the engine
    // !ok() with error() pointing at the diagnostic — the script editor's failure path.
    bool compile(const char* source, const BuiltinTable& table, const SysVarTable& sysvars);

    // Compile the animated routine (color derived from the per-frame `t`).
    bool compileAnimated();

    bool ok() const { return fn_ != nullptr || anim_ != nullptr || ctrl_ != nullptr; }

    /// The compiled function named `name`, or nullptr when the script did not define one.
    ///
    /// This is what makes a script's ROLE a question of what it defined rather than what type it is:
    /// a layout asks for `placeLights`, an effect for `tick`, and a script that defines both is
    /// served by both. The address is inside the ONE emitted block, at the offset the lowering
    /// recorded, which is why a script may define as many functions as it likes for the cost of one
    /// allocation.
    CtrlFn entry(const char* name) const {
        if (!code_ || !name) return nullptr;
        for (uint8_t i = 0; i < entryCount_; i++) {
            if (std::strcmp(entryNames_[i], name) != 0) continue;
            if (entries_[i].offset >= codeLen_) return nullptr;   // a corrupt map is not callable
            return reinterpret_cast<CtrlFn>(static_cast<uint8_t*>(code_) + entries_[i].offset);
        }
        return nullptr;
    }

    /// Names of the functions the script defined, for a binding that wants to report them and for
    /// the dispatch question "which entry points does this script have".
    const char* entryName(uint8_t i) const { return i < entryCount_ ? entryNames_[i] : nullptr; }
    uint8_t entryCount() const { return entryCount_; }
    const char* error() const { return error_; }

    // The hot path: run the compiled routine over the host's buffer. `t` is the host's
    // elapsed() ms; a static routine ignores it, an animated one derives its color from
    // it. No-op if !ok() (a failed compile renders nothing). The emitted routines write
    // channels +0/+1/+2 per light, so a buffer that can't hold RGB — null, zero lights, or
    // fewer than 3 channels per light — is left untouched rather than overrun (robust to any
    // grid size / layout, the hard rule).
    /// Run the script's ENTRY POINT called `name`, or the whole program when `name` is null.
    ///
    /// A binding names the function its role calls for: an effect wants `tick`, a layout
    /// `placeLights`. Passing a name a script did not define runs NOTHING, which is the honest
    /// answer: the module reports it rather than silently running some other function.
    void run(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t,
             const char* name = nullptr) const {
        if (!buf || nLights == 0 || cpl < 3) return;
        // The arena is the fifth argument, and a front-end-compiled program reads its controls and
        // system variables straight through it — a null there is dereferenced by the EMITTED code,
        // which faults as a LoadProhibited at a nonsense address with no C++ frame to blame. Checked
        // with the other preconditions rather than trusted: every other operand of the call is.
        if (ctrl_ && ctrlArena_) {
            // Start every frame at depth zero. The emitted code restores the counter as it unwinds,
            // so this is normally already 0. A script that HIT the limit stopped calling
            // rather than returning through the restore, and a leaked level would shrink the next
            // frame's budget, and the next, until a legal recursion no longer ran. One byte.
            ctrlArena_[kDepthSlot] = 0;
            // A named entry when asked for one; otherwise the block start, which is what the
            // hand-encoded programs and a single-function script both want.
            CtrlFn f = name ? entry(name) : ctrl_;
            if (f) f(buf, nLights, cpl, t, ctrlArena_);
        }
        else if (fn_) fn_(buf, nLights, cpl);                 // hand-encoded fixed fill
        else if (anim_) anim_(buf, nLights, cpl, t);          // hand-encoded animated fill
    }

    /// Does the script define this entry point? A binding asks before reporting "no tick() to run".
    bool hasEntry(const char* name) const { return entry(name) != nullptr; }

    // Release the exec block + the control arena (the "destructor" role — release returns the
    // memory).
    void free();

    // Drop the compiled code, keeping the control arena. What a caller wants when a script STOPS
    // being usable (renamed, deleted, emptied) but the module lives on: ok() goes false so a tick
    // renders nothing, while a bound control pointer stays valid and a scripted control keeps the
    // live value the user set. free() would take the arena too, and the next compile would re-seed
    // every control from its declared default.
    void freeCode();

    // The emitted code length, for the golden-bytes test (0 until compiled).
    size_t codeLen() const { return codeLen_; }
    // The allocated exec-block size (word-rounded codeLen) — the actual heap held, for memory
    // accounting. 0 until compiled / after free().
    size_t codeCap() const { return codeCap_; }

    /// Every heap byte this engine holds — the exec block plus the control arena.
    ///
    /// What a binding reports as its dynamicBytes: the card is supposed to show the memory the
    /// module actually costs, and codeCap() alone missed the arena. The arena is small but it is a
    /// real allocation with the module's lifetime, and "roughly right" is how a memory figure stops
    /// being worth reading.
    size_t heapBytes() const { return codeCap_ + (ctrlArena_ ? kArenaBytes : 0); }

    // The controls the last compile() declared (empty if none / not a source compile). The binding
    // reads this to create real MoonModule controls bound to the arena slots.
    const DeclaredControl* declaredControls(uint8_t& count) const { count = controlCount_; return controls_; }
    // The backing byte for control `offset` in the live arena — the binding points a uint8 control
    // reference here. nullptr if offset is out of range. The arena is allocated once at full
    // capacity (ensureArena) and never moves, so a bound control pointer stays valid for the
    // engine's lifetime, across every recompile (the stable-slot contract).
    /// The live byte at an arena offset: a script-declared control (offset < kMaxCtrls) or a host
    /// system variable (above it). Bounded by the ARENA, not by controlCount_ — a system variable's
    /// slot exists whether or not the script declared any control, and the binding writes it every
    /// frame. Returns nullptr for an offset the arena does not hold.
    uint8_t* controlSlot(uint8_t offset) { return (ctrlArena_ && offset < kArenaBytes) ? &ctrlArena_[offset] : nullptr; }

private:
    // Shared post-emit step: copy `len` staged bytes into a fresh exec block. Returns the
    // block (already writeExec'd) or nullptr on failure (error_ set). The typed pointer is
    // cast by the caller.
    void* place(const uint8_t* staged, size_t len);

    // Ensure the control arena holds `count` bytes, seeding new slots from `decls[i].def`. Grows
    // capacity (never shrinks/moves) so a control pointer the binding holds stays valid across a
    // recompile; preserves an existing slot's live value when the script is edited but the control
    // persists. Returns false on alloc failure (the caller degrades).
    bool ensureArena(const DeclaredControl* decls, uint8_t count);

    void*   code_ = nullptr;     // allocExec block holding the emitted machine code
    size_t  codeCap_ = 0;        // its capacity (for freeExec)
    size_t  codeLen_ = 0;        // bytes actually emitted
    FillFn  fn_ = nullptr;       // static fill (3-arg), or nullptr
    AnimFn  anim_ = nullptr;     // animated fill (4-arg, reads t), or nullptr
    CtrlFn  ctrl_ = nullptr;     // front-end-compiled routine (5-arg, reads the controls arena)
    const char* error_ = "";

    // The functions the script defined, with their offsets into `code_`, and their names owned here
    // for the same reason the control names are: a CompileResult's `name` points into source text
    // the caller frees the moment compile() returns.
    EntryPoint entries_[kMaxEntryPoints] = {};
    char       entryNames_[kMaxEntryPoints][kMaxEntryName + 1] = {};
    uint8_t    entryCount_ = 0;

    uint8_t* ctrlArena_ = nullptr;   // live control + system-variable bytes (platform::alloc, kArenaBytes, fixed)
    uint8_t  controlCount_ = 0;      // controls the current program declared
    DeclaredControl controls_[kMaxCtrls] = {};   // the declared-control metadata for the binding
    // The declared NAMES, owned. A DeclaredControl's `name` points into the SOURCE TEXT, which the
    // caller is free to release the moment compile() returns — and does, now that a script is read
    // from a file into a transient buffer. Copying the bytes here is what lets the engine outlive
    // the text it was built from; without it a binding reads freed memory when it publishes its
    // controls, which showed up as a control literally named "\x05".
    char ctrlNames_[kMaxCtrls][kMaxControlName] = {};
};

}  // namespace mm::moonlive
