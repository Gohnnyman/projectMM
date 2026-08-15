#pragma once

#include <cstdint>
#include <cstddef>

// MoonLive built-in table — the neutral seam by which a HOST registers the functions a script
// may call (the ESPLiveScript `arti_external_function` / ARTI / doc §3.4 model). The core
// compiler knows only *that a name maps to a descriptor*; it owns no function names and no
// domain semantics. The light domain (or any other host) populates the table with its own
// vocabulary — setRGB/fill/random16 for LEDs, something else for a display or a sensor.
//
// A descriptor says how a call lowers:
//   - Call   — a pure host helper: lower to a generic call to `fn` (a C function pointer),
//              one argument in, one result out. (random16, later sin/cos/hsvToRgb…)
//   - Inline — a routine the backend emits inline (no per-call overhead — the hot-path
//              writers): the descriptor carries an `inlineOp` TAG, a neutral opcode the
//              per-ISA lowering knows how to emit. The core never interprets the tag; it just
//              threads it through. The light domain decides which names map to which tags.
//
// This is what keeps the core domain-neutral while the hot path stays inline: the *name*
// "setRGB" and its RGB meaning live only in the host's registration; the core sees a tag.

namespace mm::moonlive {

// Neutral inline opcodes — "store shapes a backend can emit", not "LED operations". A host maps
// its function names onto these; a backend implements them. StoreElem = store N bytes (one
// element) at a computed index; FillElems = a counted loop writing one element per slot. The
// core treats them as opaque tags; the per-ISA backend and the host both know the element is 3
// bytes (RGB) for the light host, but that meaning lives outside the core.
enum class InlineOp : uint8_t {
    StoreElem,   // operands: bufVReg, indexVReg, v0, v1, v2  → store one element at index
    FillElems,   // operands: bufVReg, countVReg, strideVReg, v0, v1, v2  → loop store over count
};

enum class BuiltinKind : uint8_t { Call, Inline };

// A host callable. THREE unsigned args in, one unsigned result out.
//
// One argument covered a unary helper like random16, but a binding that hands the host a POSITION
// A host function receives a POINTER to its arguments, not the arguments themselves.
//
// The compiler already evaluates every argument into a CONSECUTIVE frame slot (the stack machine's
// argument staging), so the call only has to say where they start. Each backend materialises that
// address from its own frame pointer — the arithmetic spillStore/spillLoad already do — which means
// the number of arguments is bounded by frame slots rather than by how many the calling convention
// can carry. `draw::line` takes seven; a fixed three would have forced it to be split into bespoke
// halves, and every power function added after it would inherit the same distortion.
//
// Deliberately still THREE C parameters, so each assembler's call sequence is untouched. On Xtensa
// that sequence is a windowed `call8`, the most defect-prone code in this project; widening it would
// have spent the change there and still left a fixed maximum, just a larger one.
//
// `arena` is the control/system-variable block, as before.
// `args` points at `argc` frame slots. The element type is uintptr_t because a frame slot IS one
// machine word — 8 bytes on arm64, 4 on Xtensa and RISC-V — and the backends store a whole word per
// slot. Reading them as uint32_t made args[1] land on the upper half of slot 0 on a 64-bit host,
// which is a value of 0 rather than the argument: correct on both devices, wrong on the desktop.
using HostCallFn = uint32_t (*)(const uintptr_t* args, uint32_t argc, const uint8_t* arena);

struct Builtin {
    const char*  name = nullptr;      // the script-visible name (host-owned)
    uint8_t      argc = 0;            // number of arguments
    bool         returns = false;     // Call: produces a value (an expression) vs a void statement
    BuiltinKind  kind = BuiltinKind::Call;
    HostCallFn   fn = nullptr;        // Call: the host C function pointer
    InlineOp     inlineOp{};          // Inline: the neutral opcode tag
};

// A fixed-capacity table the host fills and the compiler reads. No heap; a host registers a
// handful of functions. Lookup is by name (linear — the table is tiny).
struct BuiltinTable {
    static constexpr uint8_t kMax = 16;
    Builtin items[kMax];
    uint8_t count = 0;

    bool add(const Builtin& b) {
        if (count >= kMax || b.name == nullptr) return false;   // a null name would null-deref in find()
        items[count++] = b;
        return true;
    }
    const Builtin* find(const char* name, size_t len) const {
        for (uint8_t i = 0; i < count; i++) {
            const char* n = items[i].name;
            size_t j = 0;
            for (; j < len && n[j]; j++) if (n[j] != name[j]) break;
            if (j == len && n[j] == 0) return &items[i];
        }
        return nullptr;
    }
};

static constexpr uint8_t kMaxCtrls = 8;          // a script declares a handful of controls; fixed, no heap

// The controls arena holds two kinds of byte, in one allocation with a fixed split:
//   [0 .. kMaxCtrls)                  script-declared controls, offset == declaration index
//   [kMaxCtrls .. kArenaBytes)        host system variables (width/height/…), offset assigned by
//                                     the host and CONSTANT for the program's life
// System variables sit ABOVE the script's range so that adding or removing a control — which
// renumbers every control offset — cannot move them. The binding caches their slot pointers, so a
// moving offset would silently write the wrong byte.
// The emitted-code buffer is sized to THE SCRIPT (codeCapFor below), not to a constant, for the
// same reason the IR op array is: the backends differ by up to 1.9x on identical source — RISC-V is
// fixed-4-byte and saves the whole register pool around every call where Xtensa has 3-byte narrow
// forms — so any single number is either too small for the sparsest backend or wasteful for the
// densest. A fixed 2 KB let `plasma.mlv` run on an S3 and desktop and REFUSED it on an S31 by 96
// bytes, which is the second time one constant made a script's portability depend on its ISA.
//
// kCodeCap survives as the SANITY bound only: a runaway script fails with a diagnostic instead of
// exhausting the heap. It is not the working limit, so it is sized well above any real script.
static constexpr size_t  kCodeCap = 16384;

/// Bytes to reserve for a script of `tokens` tokens. Over-estimating costs one cold-path allocation
/// that is freed when the compile ends; under-estimating fails a script that would have fit, so the
/// direction of the error is deliberate — the same rule the IR's op estimate follows.
///
/// 64 bytes/token, measured across every shipped script on all three backends with `countTokens`
/// (which skips comments, so a long header does not inflate the count). The densest is
/// `random-pixel.mlv` at 39.3 — one statement, four nested `random16()` calls, and on RISC-V each
/// call saves and restores the whole register pool — so this is a ~1.6x margin over the worst real
/// case. A SHORT call-dense script sets the bound, not a long one: a call lowers to a save/restore
/// while declarations and operators lower to a few instructions each, so bytes-per-token FALLS as a
/// script grows. The floor covers a tiny script's fixed prologue and epilogue, which no per-token
/// figure expresses.
constexpr size_t codeCapFor(uint32_t tokens) {
    const size_t want = size_t(tokens) * 64 + 256;
    return want > kCodeCap ? kCodeCap : want;
}

static constexpr uint8_t kMaxSysVars  = 8;
static constexpr uint8_t kArenaBytes  = kMaxCtrls + kMaxSysVars;

/// A name the HOST defines and the script only reads: `width`, `height`, `depth`. Reserved — a
/// script cannot declare one, so the name means the same thing in every script (the `t` rule, one
/// construct wider). Distinct from a control: nobody sets it in the UI, and it never appears in
/// declaredControls(), so no binding has to hide it.
///
/// The common case is read from the controls arena like a control is, because the value changes per frame and
/// the emitted code must not bake it in. The difference is ownership: the BINDING owns the slot
/// and writes it (from the layer), and the compiler reserves the slot rather than the script
/// declaring it.
enum class SysVarKind : uint8_t {
    Arena,   // a byte in the controls arena the binding writes per frame (width/height/depth)
    Arg,     // an argument register the host passes on every run (t) — costs no instruction
};

struct SysVar {
    const char* name = nullptr;
    SysVarKind  kind = SysVarKind::Arena;
    uint8_t     where = 0;   // Arena: byte offset into the arena. Arg: the VReg (kArg0..kArg4).
};

/// The system variables one host domain defines. Same shape and lookup as BuiltinTable — a host
/// hands the compiler both, and the compiler resolves names against them without knowing the domain.
struct SysVarTable {
    // Bounded by the arena's system range, not chosen independently: a host that could register
    // more system variables than the arena reserves would hand out an offset controlSlot() rejects,
    // and the binding's per-frame write would be silently dropped.
    static constexpr uint8_t kMax = kMaxSysVars;
    SysVar  items[kMax];
    uint8_t count = 0;

    // Rejects an offset the arena cannot hold, rather than storing it and failing at run time:
    // controlSlot() would return nullptr for it and the binding's per-frame write would vanish
    // with no error anywhere. An Arena slot must sit in the system range (above the script's
    // controls, inside the arena); an Arg must name a real argument register.
    bool add(const SysVar& v) {
        if (count >= kMax || v.name == nullptr) return false;
        if (v.kind == SysVarKind::Arena && (v.where < kMaxCtrls || v.where >= kArenaBytes))
            return false;
        // kArg4 is the last argument register (MoonLiveIr.h owns the enum, and includes THIS
        // header, so the bound is spelled here rather than referenced).
        if (v.kind == SysVarKind::Arg && v.where > 4) return false;
        items[count++] = v;
        return true;
    }
    const SysVar* find(const char* name, size_t len) const {
        for (uint8_t i = 0; i < count; i++) {
            const char* n = items[i].name;
            size_t j = 0;
            for (; j < len && n[j]; j++) if (n[j] != name[j]) break;
            if (j == len && n[j] == 0) return &items[i];
        }
        return nullptr;
    }
};

}  // namespace mm::moonlive
