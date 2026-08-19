#pragma once

#include <cstdint>
#include <cstdio>   // snprintf, for describe()
#include <cstddef>
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveCompiler.h"   // CompileResult (carries the declared controls)
#include <cstring>   // std::strcmp: entry lookup by name

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

    /// Append a control the running `defineControls()` declared. The binding installs a sink that
    /// lands here, so the control list is built by the script CALLING addUint8, exactly as a
    /// compiled module's list is built by its defineControls() running.
    ///
    /// `name` must outlive the engine: it points into the string pool this engine owns, which is
    /// what the compiler interned it into.
    void addDeclaredControl(const char* name, uint8_t offset, uint8_t lo, uint8_t hi) {
        if (controlCount_ >= kMaxCtrls || !name || offset >= kArenaBytes) return;
        if (lo > hi) return;
        // Two controls on one member would give the UI two cards writing the same byte, each
        // overwriting the other, and two labels the same persistence key.
        for (uint8_t i = 0; i < controlCount_; i++)
            if (controls_[i].offset == offset) return;

        // The DEFAULT is whatever the member already holds: its initializer seeded the arena
        // before this ran, so the live byte is the declared value.
        //
        // CLAMPED IN THE ARENA, not just in the record. A script edit that narrows a range leaves
        // a live value outside it, and the native code reads the arena byte every tick, not the
        // record. Clamping only the record would leave the out-of-range value driving the effect
        // while the UI showed a slider that could not reach it. This is the one place that knows
        // both the range and the live byte at the same moment.
        uint8_t def = ctrlArena_ ? ctrlArena_[offset] : lo;
        if (def < lo) def = lo;
        else if (def > hi) def = hi;
        if (ctrlArena_) ctrlArena_[offset] = def;
        controls_[controlCount_] = {name, lo, hi, def, 0, CtrlType::Uint8, offset};
        // nameLen is what the binding reports; measured here rather than passed, so a caller
        // cannot disagree with the string it handed over.
        // The BOUND is tested first: `name[n] && n < limit` reads the byte before deciding whether
        // it may, so a name that fills the buffer without a terminator is read one past the end.
        uint8_t n = 0;
        while (n < kMaxControlName - 1 && name[n]) n++;
        controls_[controlCount_].nameLen = n;
        controlCount_++;
    }

    /// Forget the controls a previous defineControls() declared, so re-running it rebuilds rather
    /// than appends. A compiled module's defineControls() is re-runnable for the same reason.
    void clearDeclaredControls() { controlCount_ = 0; }

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

    /// How many bytes of machine code the current program IS, as opposed to what its allocation
    /// cost. `heapBytes` answers "what does this module take from the heap"; this answers "how big
    /// is my script", which is what a script author asks and what the card reports. 0 until
    /// compiled. Also the golden-bytes test's subject.
    size_t codeLen() const { return codeLen_; }
    // The allocated exec-block size (word-rounded codeLen) — the actual heap held, for memory
    // accounting. 0 until compiled / after free().
    size_t codeCap() const { return codeCap_; }

    /// Every HEAP byte this engine holds: the exec block plus the control arena.
    ///
    /// What a binding reports as its dynamicBytes. codeCap() alone missed the arena, which is small
    /// but is a real allocation with the module's lifetime, and "roughly right" is how a memory
    /// figure stops being worth reading.
    ///
    /// The string pool is NOT here, and that is not an omission: it is an inline member, so it is
    /// already counted in the module's own sizeof rather than its dynamic bytes. Adding it would
    /// report those bytes twice.
    size_t heapBytes() const { return codeCap_ + (ctrlArena_ ? kArenaBytes : 0); }

    /// Write "1700 B - controls 2/8" into `out`: how big the compiled program is, and the ONE
    /// budget it is closest to exhausting.
    ///
    /// Size first because it is what a script author asks and nothing could answer: the card's
    /// memory figure is the ALLOCATION, which is word-rounded and says nothing about the program.
    ///
    /// One budget, not five. A script can hit ten ceilings, but five are derived (IR ops, vregs,
    /// labels, fixups, locals all follow from code size or nesting) and reporting a number the
    /// author cannot act on is noise. Of the five that are actionable, showing the tightest is what
    /// answers "am I near a wall": the others by definition have more room. It appears only past
    /// half full, so an ordinary script reads its size and nothing else.
    void describe(char* out, size_t cap) const {
        if (!out || !cap) return;
        if (!codeLen_) { out[0] = '\0'; return; }
        // Each actionable budget as a percentage, so the tightest is comparable across units.
        struct Budget { const char* name; uint32_t used, max; };
        const Budget budgets[] = {
            {"controls", controlCount_, kMaxCtrls},
            {"strings",  stringLen_,    CompileResult::kStringPool},
            {"code",     static_cast<uint32_t>(codeLen_), kCodeCap},
            {"entries",  entryCount_,   kMaxEntryPoints},
        };
        const Budget* worst = &budgets[0];
        for (const Budget& b : budgets)
            if (b.used * uint64_t(worst->max) > worst->used * uint64_t(b.max)) worst = &b;
        if (worst->used * 2 > worst->max)
            std::snprintf(out, cap, "%u B, %s %u/%u", unsigned(codeLen_),
                          worst->name, unsigned(worst->used), unsigned(worst->max));
        else
            std::snprintf(out, cap, "%u B", unsigned(codeLen_));
    }

    // The controls the last compile() declared (empty if none / not a source compile). The binding
    // reads this to create real MoonModule controls bound to the arena slots.
    const DeclaredControl* declaredControls(uint8_t& count) const { count = controlCount_; return controls_; }
    // The backing byte for control `offset` in the live arena — the binding points a uint8 control
    // reference here. nullptr if offset is out of range. The arena is allocated once at full
    // capacity (ensureArena) and never moves, so a bound control pointer stays valid for the
    // engine's lifetime, across every recompile (the stable-slot contract).
    /// The live byte at an arena offset: a script-declared member (offset < kCtrlBytes) or a host
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
    // WHICH arena bytes hold a seeded member, one bit per offset, and the name each one was
    // seeded under. Identity is (offset, name), not declaration position: inserting a member at
    // the top of a class shifts every later declaration down one, and a position-keyed check would
    // then hand each member the value of the one that used to sit there. kArenaBytes is 17, so a
    // uint32_t covers the whole arena and the mask costs less than the count it replaces.
    //
    // The name is COPIED rather than pointed at. The compiler's record names a span of the source
    // buffer, which is freed the moment the compile returns, and the comparison happens on the
    // NEXT compile: a pointer would be dangling by then. Truncated to a prefix, which is enough to
    // tell two members apart in the only case that matters, and bounded so a long name cannot run
    // off the end of a record that carries its length instead of a terminator.
    // Indexed by MEMBER, not by arena byte. A class declares at most kMaxCtrls of them, so a row
    // per byte was 8x more rows than can ever exist: 768 bytes of a 1440-byte engine, held by value
    // inside every scripted module and constructed on the main task's stack by ModuleFactory's
    // probe. The offset is stored alongside, which is what the byte-indexed form was really using.
    static constexpr uint8_t kSeedNameLen = 12;
    struct SeededMember { uint8_t offset = 0; char name[kSeedNameLen] = {}; };
    // A uint64_t, and the table is sized to the SCRIPT's region rather than the whole arena. This
    // was a uint32_t when kCtrlBytes was 16; the byte budget then grew to 64 and the mask did not,
    // so `1u << off` for a member at offset 32 or beyond was undefined behaviour and in practice
    // aliased mod 32: that member was never recorded as seeded (snapping back to its initializer on
    // every recompile, losing the live value this exists to keep) while corrupting the bit of the
    // member it aliased. A static_assert now ties the two together so the next widening cannot
    // repeat it. The rows above kCtrlBytes were dead: a system variable is never seeded from a
    // declaration.
    static_assert(kCtrlBytes <= 64, "seeded_ is a 64-bit mask, one bit per script arena byte");
    uint64_t     seeded_ = 0;
    SeededMember seededName_[kMaxCtrls] = {};
    uint8_t      seededCount_ = 0;

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
    // Text a script wrote as a literal, copied out of the compile result so a pointer the emitted
    // code carries stays valid. In the ENGINE rather than the exec block: the block is IRAM on a
    // device, which takes 32-bit stores only and is instruction memory, so string bytes do not
    // belong in it. This is a plain member for the same reason ctrlNames_ is, and it lives as long
    // as the compiled program that points into it.
    char strings_[CompileResult::kStringPool] = {};
    // How much of the pool the current program uses, for the card's headroom readout.
    uint16_t stringLen_ = 0;
};

}  // namespace mm::moonlive
