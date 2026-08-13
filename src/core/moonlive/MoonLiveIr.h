#pragma once

#include <cstdint>
#include <cstddef>
#include "core/moonlive/MoonLiveBuiltins.h"   // InlineOp (a neutral opcode tag)
#include "platform/platform.h"                // alloc/free — the op array is sized to the script

// MoonLive IR — the typed intermediate representation between the front-end and the per-ISA
// assembler (§3.2 of livescripts-analysis-top-down.md). The front-end lowers an AST to a flat
// list of three-address ops over virtual registers; each per-ISA backend lowers that same IR
// to machine bytes. The IR is the seam: it knows the *operations*, never the *ISA* and never a
// domain (no LEDs). Buffer writes are not a special IR op — they are `Inline` ops carrying a
// neutral opcode tag the HOST registered (see MoonLiveBuiltins.h); the core just threads the
// tag to the backend.
//
// It is a compile-time data structure — consumed during lowering, never present at run time.
// A fixed-capacity op list (no heap in the build path); a single statement is a handful of ops.
//
// Virtual registers are plain indices v0..v(kMaxVRegs-1). A backend maps them to machine
// registers. The named host arguments arrive in fixed vregs (kArg…) so the front-end refers to
// them without knowing the ABI. They are named neutrally — kArg0..kArg4 — and a host assigns
// meaning (for the light host: buf, nLights, cpl, t, and a controls-values pointer).

namespace mm::moonlive {

using VReg = uint8_t;

// kArg4 is a per-instance data pointer the host passes at run time (for the light host: the
// control-values arena). LoadCtrl reads a byte from it — a script control whose value the binding
// updates live, without a recompile (the kArg3/t pattern, one slot over).
enum : VReg { kArg0 = 0, kArg1 = 1, kArg2 = 2, kArg3 = 3, kArg4 = 4, kFirstTemp = 5 };

// How many values one program may NAME. Deliberately larger than any target's register file: the
// register allocator (MoonLiveSpill.h) parks whatever does not fit in frame slots, so this bounds
// the compiler's own tables — the interval array is one entry per vreg, on the stack of a 12 KB task
// — rather than the script. 32 keeps that array at a few hundred bytes.
static constexpr uint8_t kMaxVRegs = 32;
// An upper SANITY bound, not the working limit: the op array is sized to the script (see IrProgram),
// so a one-statement script pays for one statement. This exists only so a runaway source fails with
// a diagnostic instead of asking for an allocation that would exhaust a small device's heap.
static constexpr uint16_t kMaxIrOps = 4096;

// Ops a single source token can lower to, worst case. The compiler sizes its op array by counting
// tokens and multiplying — an over-estimate by construction, which is the safe direction: a few
// unused entries on a cold path, versus refusing a script that would have fit.
static constexpr uint16_t kIrOpsPerToken = 4;

// The op set — neutral. Three-address form: dst plus up to three source operands. (Counted
// Control flow arrived with the script-level `for`, which is what the note here anticipated: the
// StoreElem/FillElems inline ops still carry their own loop in the per-ISA lowering, but a loop a
// SCRIPT writes cannot live inside one opcode. Three ops carry it, and they are deliberately the
// smallest set that every backend already has instructions for — a label is a position, and the
// only branch every ISA here exposes is compare-and-branch-if-greater-or-equal (arm64 spells it
// cmp + b.hs, Xtensa and RISC-V have bgeu directly).
enum class IrOp : uint8_t {
    Const,     // dst = imm
    Add,       // dst = a + b
    AddImm,    // dst = a + imm
    Mul,       // dst = a * b
    Call,      // dst = (*callFn)(a, b, c) — call a host-registered function. Three operands
               // because a binding that hands the host a POSITION needs them at once; a
               // unary helper ignores b and c, and the compiler passes a zero vreg.
    Inline,    // a host-registered inline op (inlineOp tag); operands a/b/c/d (op-specific)
    LoadCtrl,  // dst = ((const uint8_t*)kArg4)[imm] — read a control value byte at offset imm
    Mov,       // dst = a — the assignment a loop variable needs (vregs are otherwise write-once)
    Label,     // a branch target; `imm` is the label id. Emits no instruction.
    BranchGe,  // if (a >= b) goto label `imm` — UNSIGNED. The loop's ENTRY guard: skip a loop
               // whose range is empty, which is also what makes `for (i = 0; i < 0; …)` correct.
    BranchNe,  // if (a != b) goto label `imm` — the BACKWARD edge that closes the loop.
    Spill,     // frame slot `imm` = a   — a value the register file could not hold, parked
    Reload,    // dst = frame slot `imm` — the same value brought back for one use
};

// Spill/Reload are the register allocator's output, never the parser's: MoonLiveSpill rewrites a
// program that names more live values than the target has registers into one that fits, parking the
// overflow in the CALL FRAME (the textbook answer — a value that outlives the register file lives in
// memory). `imm` is a slot INDEX; each backend turns it into an offset from its own frame pointer,
// so the core pass never knows a stack layout and the backends never know the algorithm.

// Why these two branches and no unconditional jump: a bottom-tested loop needs exactly an entry
// guard and a back edge, and every backend here already has both (bgeu / bne on Xtensa and RISC-V,
// cmp + b.hs / b.ne on arm64). It is the shape FillElems has always lowered by hand, so the
// instruction sequence is proven on all three ISAs before a script could ever emit one.

struct IrInst {
    IrOp     op;
    VReg     dst = 0;
    VReg     a = 0, b = 0, c = 0, d = 0;   // source vregs (op-dependent)
    int32_t  imm = 0;                      // immediate (Const) / addr offset
    HostCallFn callFn = nullptr;           // Call: the host C function pointer (typed alias)
    InlineOp inlineOp{};                   // Inline: the neutral opcode tag
};

// A control a script declared (`uint8_t speed = 50; // @control 0..99`). Neutral: the core
// knows {name, a neutral type, range, default, and the byte offset into the run-time controls
// arena it lives at}. The light-domain binding turns this into a real MoonModule control bound to
// the arena slot. `type` is a neutral kind — Uint8 only in Stage 1 — NOT a projectMM ControlType.
enum class CtrlType : uint8_t { Uint8 };

struct DeclaredControl {
    const char* name = nullptr;        // script-declared name (points into the source buffer)
    uint8_t     min = 0, max = 255, def = 0;   // uint8 range/default (Stage 1 is uint8 controls)
    uint8_t     nameLen = 0;           // length (the source is not NUL-terminated per token)
    CtrlType    type = CtrlType::Uint8;
    uint8_t     offset = 0;            // byte offset into the controls arena (declaration order)
};

/// Branch targets one IR program may use. Two per `for` (entry guard + back edge), and the counter
/// runs for the whole program rather than per scope — a label is never reused once a loop closes —
/// so this bounds the TOTAL number of loops in a script (8), not how deeply they nest. The
/// assemblers carry the same ceiling in their own label tables, and the compiler fails loudly
/// rather than silently miscompiling past it. Nesting depth is bounded separately, by `locals`.
static constexpr uint8_t kIrLabels = 16;

/// Script variables live in FRAME SLOTS, and this bounds how many one program may hold at once.
/// Sixteen matches what every backend's frame can address (`kMaxSpillSlots`), so a program that
/// parses is a program the assembler can encode. Raising it means widening the frame on all three
/// backends together — the slot index is an instruction field, not just a table size.
static constexpr uint8_t kMaxLocals = 16;


static constexpr uint8_t kMaxControlName = 24;   // max control-name length (incl. NUL); the compiler
                                                 // rejects longer names so the binding's name pool
                                                 // can't truncate distinct names into a collision

// A lowered program: the ops, sized to the script, plus the vreg high-water mark.
//
// The op array is HEAP-ALLOCATED rather than an `IrInst ops[kMaxIrOps]` member. As a member it cost
// the same ~2 KB of STACK for a one-statement script as for a full one, and this object is a local
// on the compile path of a 12 KB main task — so raising the ceiling by growing the array would have
// traded a compile limit for a stack overflow (this project has already lost a P4 to a large stack
// frame). Sizing to the script makes the small case cheaper AND the large case possible.
// Compilation is cold path, so the allocation costs nothing that matters.
//
// Ownership is RAII: one allocation, freed in the destructor, copying deleted. There is no manual
// free path to miss — the reverted 32026eb5 turned four tables into independently-nullable pointers
// and its own comment records the heap corruption that followed from missing one guard.
struct IrProgram {
    IrInst*  ops = nullptr;
    uint16_t cap = 0;                      // entries allocated
    uint16_t count = 0;
    VReg     vregsUsed = kFirstTemp;
    /// Frame slots the FRONT END allocated for script variables. Slot indices 0..localSlots-1 are
    /// already spoken for when the backend sizes its prologue, and the register allocator numbers
    /// any spill it still needs from here up — the two share one frame, so they cannot both start
    /// at zero without a variable and a spilled temp landing on the same bytes.
    uint8_t  localSlots = 0;

    IrProgram() = default;
    ~IrProgram() { platform::free(ops); }
    IrProgram(const IrProgram&) = delete;              // owns a buffer; a copy would double-free
    IrProgram& operator=(const IrProgram&) = delete;

    /// Size the op array to `n` entries. False when the allocation fails or `n` exceeds the sanity
    /// bound, so the caller reports a diagnostic instead of writing through a null pointer.
    bool reserve(uint16_t n) {
        if (n == 0 || n > kMaxIrOps) return false;
        platform::free(ops);
        ops = static_cast<IrInst*>(platform::alloc(sizeof(IrInst) * n));
        cap = ops ? n : 0;
        count = 0;
        return ops != nullptr;
    }

    bool push(const IrInst& i) {
        if (!ops || count >= cap) return false;
        // Reject any op that names a vreg outside the fixed register budget — an invalid program
        // is dropped at the seam rather than reaching a backend that would index past its map.
        if (i.dst >= kMaxVRegs || i.a >= kMaxVRegs || i.b >= kMaxVRegs ||
            i.c >= kMaxVRegs || i.d >= kMaxVRegs) return false;
        ops[count++] = i;
        if (i.dst + 1 > vregsUsed) vregsUsed = static_cast<VReg>(i.dst + 1);
        return true;
    }

    /// Exchange contents with `o`. The register allocator builds the rewritten program in a second
    /// IrProgram (inserting a Reload before a use and a Spill after a def cannot be done in place in
    /// a right-sized array) and swaps it in; the old buffer then dies with the local. Copy is deleted
    /// precisely because two owners would double-free, so a swap is how ownership moves here.
    void swap(IrProgram& o) {
        IrInst* p = ops; ops = o.ops; o.ops = p;
        uint16_t t = cap; cap = o.cap; o.cap = t;
        t = count; count = o.count; o.count = t;
        VReg v = vregsUsed; vregsUsed = o.vregsUsed; o.vregsUsed = v;
        uint8_t ls = localSlots; localSlots = o.localSlots; o.localSlots = ls;
    }

    /// Which inline ops this program contains, so a backend reserves scratch only for what is there.
    ///
    /// The backends reserved their maximum unconditionally, which cost a register no matter what the
    /// script did — and that one register is what made a nested loop refuse to compile on the
    /// smallest target, since a layout script neither fills nor stores elements. How MANY scratch
    /// registers each op costs is per-ISA (the host needs one for StoreElem, which Xtensa and RISC-V
    /// fold into the index vreg) and stays with each backend; WHICH ops are present is a property of
    /// the program, so it is answered once here.
    bool hasInline(InlineOp which) const {
        for (uint16_t i = 0; i < count; i++)   // uint16_t: `count` is, so a uint8_t never terminates
            if (ops[i].op == IrOp::Inline && ops[i].inlineOp == which) return true;
        return false;
    }
};

}  // namespace mm::moonlive
