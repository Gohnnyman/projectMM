#include "moonlive_asm_host.h"

#include <cstring>

// MoonLive host assembler — arm64 and x86-64 encodings, chosen at compile time (the same
// #if-by-arch shape moonlive_emit.cpp uses). Each named instruction is encoded ONCE here; the
// IR lowering composes them. Branch displacements are resolved by patchBranches() against
// bound labels, so no offset is ever hand-computed (the crash class the spike avoided by never
// composing now stays avoided by back-patching).

namespace mm::moonlive {

#if defined(__aarch64__) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

// arm64 register map: R0..R4 = the host-ABI arg registers x0..x4 (buf, nLights, cpl, t, ctrls —
// the control-values arena pointer, kArg4). R5..R13 = caller-saved scratch x9..x14 then x5..x7.
// Index math uses the 64-bit views (xN) for addresses, 32-bit (wN) for counters/colors — same
// register number, so one map suffices. x15 is the call() address/immediate scratch (not a vreg).
static constexpr uint8_t kArm64Reg[kRegCount] = {0, 1, 2, 3, 4, 9, 10, 11, 12, 13, 14, 5, 6, 7};
// BOUNDS-CHECKED. The inline ops address their scratch as `vregsUsed + n`, so an index one past the
// map is reachable whenever the reservation and the map disagree — and an out-of-bounds read returns
// whatever byte follows the array, making the emitted instruction name a register chosen by
// accident. Clamping turns that into a wrong-but-safe register instead of undefined behaviour; the
// static_assert below and the lowerer's reservation are what stop it happening at all.
static uint8_t mr(Reg r) { return kArm64Reg[r < kRegCount ? r : kRegCount - 1]; }

// A scratch register that is ALSO a vreg silently corrupts values — see the RISC-V backend, where
// kScratchFn aliased vreg R12 and every call returned a stale value. Checked here so the map can
// never grow over a scratch.
constexpr bool armScratchOutsideMap() {
    constexpr uint8_t scratch[] = {15, 16, 17};
    for (uint8_t r : kArm64Reg) for (uint8_t s : scratch) if (r == s) return false;
    return true;
}
static_assert(armScratchOutsideMap(), "a scratch register is also a vreg — calls will corrupt it");


Label HostAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit32
    Label l = labelCount_++;
    labelPos_[l] = -1;
    return l;
}
void HostAssembler::bind(Label l) { if (l < kMaxLabels) labelPos_[l] = static_cast<int32_t>(len_); }

// Record a pending branch fixup, guarding the fixed table — a script with too many branches sets
// overflow_ rather than writing past fixups_ (the same failure path as a full code buffer).
void HostAssembler::addFixup(size_t at, Label label, FixKind kind) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label, kind};
}

void HostAssembler::emit32(uint32_t w) {
    if (!buf_ || len_ + 4 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(w); buf_[len_++] = uint8_t(w >> 8);
    buf_[len_++] = uint8_t(w >> 16); buf_[len_++] = uint8_t(w >> 24);
}
void HostAssembler::emitBytes(const uint8_t* p, size_t n) {
    if (!buf_ || len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}

// --- the call frame: the register allocator's overflow storage ---------------------------------
//
// x29 is the AAPCS frame pointer, callee-saved and outside both the vreg map and the {x15,x16,x17}
// scratch set, so nothing this backend emits can disturb it. Slots are addressed from x29 rather
// than sp precisely because call() moves sp by 128 bytes around every host call: sp-relative offsets
// would be wrong for the duration of the call, and a script whose spilled value is read after a
// random16() is the ordinary case, not an exotic one. It is also the layout a nested call needs —
// each activation gets its own x29 — which is why the frame pointer is here now rather than added
// later when script-defined functions arrive.
//
// Layout: [x29+0] = saved x29, [x29+8] = saved x30, slot n at [x29 + 16 + n*8].
static constexpr uint16_t kSlotBase = 16;

void HostAssembler::prologue(uint8_t slots) {
    if (slots == 0) return;                       // no spilling: no frame, no cost
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // 16-byte aligned, as the AAPCS requires of sp at every instruction boundary — an unaligned sp
    // faults on the first stp a callee executes, which would surface as a crash inside random16.
    const uint16_t bytes = static_cast<uint16_t>((kSlotBase + slots * 8 + 15) & ~15);
    frameBytes_ = bytes;
    emit32(0xa9800000u | ((uint32_t((-int32_t(bytes)) / 8) & 0x7f) << 15) | (30u << 10) | (31u << 5) | 29u);
    emit32(0x910003fdu);                          // mov x29, sp
}
void HostAssembler::epilogue() {
    if (frameBytes_ != 0) {
        emit32(0x910003bfu);                      // mov sp, x29   (drop anything call() left behind)
        emit32(0xa8c00000u | ((uint32_t(frameBytes_) / 8) << 15) | (30u << 10) | (31u << 5) | 29u);
    }
    ret();
}
// str/ldr with a 12-bit SCALED unsigned offset (imm12 counts 8-byte units for the 64-bit form).
// 64-bit, not 32: a vreg can hold a pointer — kArg0 is the buffer — and truncating one to 32 bits on
// the way to a slot would produce a wild store the moment a spilled pointer came back.
void HostAssembler::spillStore(Reg r, uint8_t slot) {
    // No frame means prologue() bailed; emitting would address the CALLER's stack.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(0xf9000000u | ((uint32_t(kSlotBase + slot * 8) / 8) << 10) | (29u << 5) | mr(r));
}
void HostAssembler::spillLoad(Reg r, uint8_t slot) {
    // No frame means prologue() bailed; emitting would address the CALLER's stack.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emit32(0xf9400000u | ((uint32_t(kSlotBase + slot * 8) / 8) << 10) | (29u << 5) | mr(r));
}

// The ADDRESS of a frame slot, for a host call that reads its arguments from the frame. The slots
// are already where the arguments live; a call passes where they start rather than the values, which
// is what makes the number of arguments a memory question instead of a register one.
void HostAssembler::slotAddr(Reg d, uint8_t slot) {
    // No frame means prologue() bailed; emitting would address the CALLER's stack.
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    const uint32_t off = kSlotBase + uint32_t(slot) * 8;
    emit32(0x91000000u | (off << 10) | (29u << 5) | mr(d));   // add xD, x29, #off
}

void HostAssembler::movImm(Reg d, int32_t imm) {
    // movz builds a ZERO-extended 16-bit constant, so a negative immediate would land as its
    // unsigned counterpart (-1 as 65535). The compiler emits Const(-1) to express subtraction —
    // `a - b` is `a + (b * -1)` — and a wrapped -1 makes every subtraction correct only modulo 256.
    // In a stored colour byte that is invisible; in a bounds-guarded index it silently drops the
    // light, and in a host-call argument it is nonsense. movn is the negative form: it writes
    // ~imm16, so movn #(~imm) materialises the true negative value.
    if (imm < 0) {
        // movn writes ~imm16, so it reaches -65536..-1 exactly. Below that the complement no longer
        // fits the 16-bit field and the constant would come out wrong in silence.
        if (imm < -65536) { overflow_ = true; return; }
        emit32(0x12800000u | ((uint32_t(~imm) & 0xffff) << 5) | mr(d));   // movn wD, #~imm16
        return;
    }
    emit32(0x52800000u | ((uint32_t(imm) & 0xffff) << 5) | mr(d));        // movz wD, #imm16
}
void HostAssembler::addImm(Reg d, Reg a, int32_t imm) {    // add xD, xA, #imm12 (64-bit)
    emit32(0x91000000u | ((uint32_t(imm) & 0xfff) << 10) | (mr(a) << 5) | mr(d));
}
void HostAssembler::addReg(Reg d, Reg a, Reg b) {          // add wD, wA, wB (32-bit)
    emit32(0x0b000000u | (mr(b) << 16) | (mr(a) << 5) | mr(d));
}
void HostAssembler::mulImm(Reg d, Reg a, int32_t imm) {    // d = a * imm via mov x15 + mul
    // Small-immediate multiply: load imm into x15 (not in the Reg map, so it clobbers no
    // vreg) then mul. x15 is caller-saved scratch on the host ABI.
    emit32(0x52800000u | ((uint32_t(imm) & 0xffff) << 5) | 15);          // movz w15, #imm
    emit32(0x1b007c00u | (15 << 16) | (mr(a) << 5) | mr(d));             // mul wD, wA, w15
}
void HostAssembler::mulReg(Reg d, Reg a, Reg b) {         // mul wD, wA, wB
    emit32(0x1b007c00u | (mr(b) << 16) | (mr(a) << 5) | mr(d));
}
void HostAssembler::store8(Reg base, Reg off, Reg val) {   // strb wVal, [xBase, xOff]
    emit32(0x38206800u | (mr(off) << 16) | (mr(base) << 5) | mr(val));
}
void HostAssembler::load8(Reg d, Reg base, int32_t imm) {  // ldrb wDst, [xBase, #imm12]
    emit32(0x39400000u | ((uint32_t(imm) & 0xfff) << 10) | (mr(base) << 5) | mr(d));
}
void HostAssembler::store16(Reg base, Reg off, Reg val) {  // strh wVal, [xBase, xOff]
    emit32(0x78206800u | (mr(off) << 16) | (mr(base) << 5) | mr(val));
}
// ldrh wDst, [xBase, #imm12]. The immediate is SCALED by the access size, so the field holds
// imm/2 and an odd offset cannot be encoded at all: a halfword member is placed on an even byte
// (see the arena cursor), which is what makes the scaled form usable rather than a constraint
// invented here.
void HostAssembler::load16(Reg d, Reg base, int32_t imm) {
    emit32(0x79400000u | (((uint32_t(imm) >> 1) & 0xfff) << 10) | (mr(base) << 5) | mr(d));
}
// ldrb wDst, [xBase, xOff] and ldrh wDst, [xBase, xOff]. The register-offset form takes the index
// UNSCALED for a byte; for a halfword the LSL amount would scale it, and it is left at 0 so the
// index the caller passes is a BYTE offset in both cases. That keeps one rule for the lowering:
// an element index is multiplied by the element width before it gets here, never after.
void HostAssembler::load8Idx(Reg d, Reg base, Reg off) {   // ldrb wDst, [xBase, xOff]
    emit32(0x38606800u | (mr(off) << 16) | (mr(base) << 5) | mr(d));
}
void HostAssembler::load16Idx(Reg d, Reg base, Reg off) {  // ldrh wDst, [xBase, xOff]
    emit32(0x78606800u | (mr(off) << 16) | (mr(base) << 5) | mr(d));
}
void HostAssembler::cmp(Reg a, Reg b) {                    // cmp wA, wB  (subs wzr, wA, wB)
    emit32(0x6b00001fu | (mr(b) << 16) | (mr(a) << 5));
}
void HostAssembler::branchIfZero(Reg a, Label l) {         // cbz wA, l  (offset patched)
    addFixup(len_, l, FixKind::Branch);
    emit32(0x34000000u | mr(a));
}
void HostAssembler::branchIf(Cond c, Label l) {            // b.cond l  (offset patched)
    // arm64 condition codes: NE=1, HS/CS=2, LO/CC=3.
    const uint8_t cond = (c == Cond::Lo) ? 0x3 : (c == Cond::Ne ? 0x1 : 0x2);
    addFixup(len_, l, FixKind::Branch);   // the condition is already in the instruction
    emit32(0x54000000u | cond);
}
// The fused forms the shared lowering calls. arm64 has no compare-and-branch pair, so each is
// cmp + b.cond here, and one instruction on RISC-V and Xtensa. Both spellings live behind the
// same name, which is what lets the IR walk be written once.
void HostAssembler::movReg(Reg d, Reg a) { addImm(d, a, 0); }    // mov wD, wA (add wD, wA, #0)
void HostAssembler::branchGeU(Reg a, Reg b, Label l) { cmp(a, b); branchIf(Cond::Hs, l); }
void HostAssembler::branchNe(Reg a, Reg b, Label l)  { cmp(a, b); branchIf(Cond::Ne, l); }

// movPtr: a full 64-bit address into a register, movz + three movk.
//
// The same four instructions call() emits for its target, parameterized on the destination. A
// pointer cannot ride an immediate (IrInst::imm is int32_t) and cannot be a PC-relative literal
// either, because the emitted block is copied to its final address after these bytes are built,
// so an absolute materialization is what stays correct across that move.
void HostAssembler::movPtr(Reg d, const void* p) {
    const uint64_t addr = reinterpret_cast<uint64_t>(p);
    const uint8_t r = mr(d);
    emit32(0xd2800000u | ((uint32_t(addr) & 0xffff) << 5) | r);                          // movz xD, #b0
    emit32(0xf2800000u | (1u << 21) | (((uint32_t(addr >> 16)) & 0xffff) << 5) | r);     // movk xD,#b1,lsl16
    emit32(0xf2800000u | (2u << 21) | (((uint32_t(addr >> 32)) & 0xffff) << 5) | r);     // movk xD,#b2,lsl32
    emit32(0xf2800000u | (3u << 21) | (((uint32_t(addr >> 48)) & 0xffff) << 5) | r);     // movk xD,#b3,lsl48
}

void HostAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // Preserve EVERY register that may hold a live value across the call: the host args
    // (x0/x1/x2/x3), the link register x30 (blr overwrites it; our function is a leaf), and the
    // whole vreg scratch pool (x4-x7, x9-x14) — because a value computed before the call (e.g.
    // a first random16's result) can be live across a SECOND call. Saving the full pool makes
    // the live-vreg-across-call contract hold for any expression; it's a cold path (once per
    // call, not per pixel). 128-byte frame (8 pairs) keeps sp 16-aligned.
    //
    // x3 is kArg3, the elapsed time — which scripts now read as the system variable `t`, so a
    // built-in clobbering it (legal for any callee under the AAPCS) would be a silent wrong-value
    // bug in any animated script that calls anything. Saved before it could become one. It pairs
    // with x8, which this backend never uses, because stp works on pairs.
    emit32(0xa9b807e0u);   // stp x0, x1,  [sp, #-128]!
    emit32(0xa9017be2u);   // stp x2, x30, [sp, #16]
    emit32(0xa90723e3u);   // stp x3, x8,  [sp, #112]
    emit32(0xa90217e4u);   // stp x4, x5,  [sp, #32]
    emit32(0xa9031fe6u);   // stp x6, x7,  [sp, #48]
    emit32(0xa9042be9u);   // stp x9, x10, [sp, #64]
    emit32(0xa90533ebu);   // stp x11,x12, [sp, #80]
    emit32(0xa9063bedu);   // stp x13,x14, [sp, #96]
    // args into x0/x1/x2 (the built-in's three parameters). Order matters: x0 is written first,
    // and a later source register could BE x0 — so read the sources before any of them is clobbered
    // by moving through a scratch that is outside the vreg pool.
    emit32(0xaa0003efu | (uint32_t(mr(a)) << 16));        // mov x15, x<a>
    emit32(0xaa0003f0u | (uint32_t(mr(b)) << 16));        // mov x16, x<b>
    emit32(0xaa0003f1u | (uint32_t(mr(c)) << 16));        // mov x17, x<c>
    emit32(0xaa0f03e0u);                                   // mov x0, x15
    emit32(0xaa1003e1u);                                   // mov x1, x16
    emit32(0xaa1103e2u);                                   // mov x2, x17
    // materialise the 64-bit absolute fn address into x15 (movz + 3×movk)
    uint64_t addr = reinterpret_cast<uint64_t>(fn);
    emit32(0xd2800000u | ((uint32_t(addr) & 0xffff) << 5) | 15);                 // movz x15, #b0
    emit32(0xf2800000u | (1u << 21) | (((uint32_t(addr >> 16)) & 0xffff) << 5) | 15);  // movk x15,#b1,lsl16
    emit32(0xf2800000u | (2u << 21) | (((uint32_t(addr >> 32)) & 0xffff) << 5) | 15);  // movk x15,#b2,lsl32
    emit32(0xf2800000u | (3u << 21) | (((uint32_t(addr >> 48)) & 0xffff) << 5) | 15);  // movk x15,#b3,lsl48
    emit32(0xd63f0000u | (15u << 5));                     // blr x15
    // Stash the result (x0) in x15 — a non-pool scratch — BEFORE restoring, since x0 and the
    // dst register are both in the saved set the restore overwrites.
    emit32(0xaa0003efu);   // mov x15, x0   (result → x15)
    // restore the full saved set (reverse order)
    emit32(0xa94723e3u);   // ldp x3, x8,  [sp, #112]
    emit32(0xa9463bedu);   // ldp x13,x14, [sp, #96]
    emit32(0xa94533ebu);   // ldp x11,x12, [sp, #80]
    emit32(0xa9442be9u);   // ldp x9, x10, [sp, #64]
    emit32(0xa9431fe6u);   // ldp x6, x7,  [sp, #48]
    emit32(0xa94217e4u);   // ldp x4, x5,  [sp, #32]
    emit32(0xa9417be2u);   // ldp x2, x30, [sp, #16]
    emit32(0xa8c807e0u);   // ldp x0, x1,  [sp], #128
    // now move the stashed result into dst (dst is restored/valid; x15 holds the result)
    emit32(0xaa0f03e0u | uint32_t(mr(d)));   // mov x<dst>, x15
}
void HostAssembler::ret() { emit32(0xd65f03c0u); }

// bl <label>: a call to a function in THIS block: the script-to-script call.
//
// `bl` links the return address into x30, which the callee's prologue saves into its own frame, so
// calls nest and therefore recurse.
//
// Pass the host arguments on (the contract is with IrOp::CallScript in core).
void HostAssembler::callLabel(Label l) {
    // Reloading them is a NO-OP on this backend as long as R0..R4 map onto the ABI argument
    // registers x0..x4 and `bl` leaves them alone, which is why removing these four instructions
    // does not fail a single test here while the same omission crashes an S3. Emitted anyway, so
    // the contract is expressed rather than depending on that mapping staying true.
    for (uint8_t v = 0; v < kHostArgSlots; v++) spillLoad(static_cast<Reg>(v), hostArgSlot(v));
    addFixup(len_, l, FixKind::Call);
    emit32(0x94000000u);   // bl #0: the 26-bit imm is patched below
}

void HostAssembler::patchBranches() {
    // Nothing was emitted if the buffer never allocated, so there is nothing to patch —
    // stated rather than left to the reader to derive from fixupCount_ being 0. And an
    // OVERFLOWED compile is refused by lowerWith after finalize(), so patching it is pointless —
    // and unsafe: a fixup recorded just before emit32 dropped its instruction points at the
    // buffer's end, and the memcpy below would write past buf_ (found as heap corruption on the
    // x86-64 backend; the pattern is identical here).
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        int32_t target = labelPos_[f.label];
        if (target < 0) continue;                                     // unbound label — leave the branch as-is (overflow_ already failed the compile)
        int32_t rel = (target - static_cast<int32_t>(f.at)) >> 2;     // PC-relative, /4
        uint32_t w; std::memcpy(&w, buf_ + f.at, 4);
        if (f.kind == FixKind::Call) {
            // bl: a 26-bit immediate at bits 0..25, not the 19-bit field the conditional branches
            // use. Sharing their arithmetic would silently retarget the call, which is why the
            // fixup carries a kind.
            if (rel < -33554432 || rel > 33554431) { overflow_ = true; return; }
            w |= uint32_t(rel) & 0x03ffffffu;
        } else {
            w |= (uint32_t(rel) & 0x7ffff) << 5;                      // imm19 field (cbz & b.cond)
        }
        std::memcpy(buf_ + f.at, &w, 4);
    }
}

#elif (defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

// x86-64 host assembler — Microsoft x64 (Windows) and System V (Linux / Intel-macOS) ABIs. The
// two diverge in the argument-register set (rcx/rdx/r8/r9 vs rdi/rsi/rdx/rcx/r8/r9), the
// callee-saved set (Win64 preserves rsi/rdi/xmm6-15; SysV does not), and the caller's obligation
// to reserve 32 bytes of shadow space below the call site (Win64 only). Encodings are identical
// between them — only the register-map and the call() argument shuffle differ, switched on
// _WIN32 in the register map below and inside call().
//
// Design choices worth stating:
// - Frame always emitted on x86-64, unlike arm64 which skips it when slots == 0. On Win64 we
//   need to save nonvolatile regs the vreg map uses (rbx, rdi, rsi, r12-r15) and reserve
//   32 bytes of shadow space for any host builtin call; without a frame there is nowhere to
//   put them. SysV is similar (rbx, r12-r15 nonvolatile). Cost: ~20 bytes of prologue and a
//   fixed stack window per script invocation, run once per frame — negligible against the
//   MoonLive tick budget.
// - rax lands as R13 (last vreg). It is volatile in both ABIs and doubles as the return-value
//   register, so call() must stash the return before restoring vregs and then move it into
//   the destination — matching the arm64 pattern that stashes into x15 for the same reason.
// - Conditional branches always use the 32-bit relative form (0F 8x rel32, 6 bytes). One
//   width, so patchBranches never has to choose — same rel32 story as bl uses on arm64.
//   Wastes 4 bytes on short branches, saves the "range fits?" retry loop and its correctness
//   burden.
// - movabs (REX.W B8+r imm64) is the movPtr / call-target instruction, the direct analog of
//   arm64's movz + 3×movk sequence. 10 bytes on x64 vs 16 on arm64.

// x86-64 machine register numbers, nested in a namespace so the RAX/RCX/... names cannot
// collide with the vreg enum's R0..R13 in the enclosing mm::moonlive scope.
namespace x64 {
enum : uint8_t {
    RAX = 0,  RCX = 1,  RDX = 2,  RBX = 3,
    RSP = 4,  RBP = 5,  RSI = 6,  RDI = 7,
    R8  = 8,  R9  = 9,  R10 = 10, R11 = 11,
    R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};
} // namespace x64
// The rest of this branch qualifies register names with `x64::` to avoid the collision with
// the vreg enum Reg::R8..R13 in the enclosing mm::moonlive scope. arm64 branch above never sees
// these constants; the #elif isolates them to the x86_64 impl.

#if defined(_WIN32)
// Microsoft x64 (Windows). Args in rcx/rdx/r8/r9; arg 5 arrives on the stack at [rbp+48] and
// is loaded into rdi (R4 = kArg4) once in prologue. Nonvolatiles rbx, rdi, rsi, r12-r15 are
// saved by prologue. rsp and rbp are reserved (rbp = frame pointer).
static constexpr uint8_t kX64Reg[kRegCount] = {
    x64::RCX, x64::RDX, x64::R8,  x64::R9,  x64::RDI,   // R0..R4  = kArg0..kArg4
    x64::R10, x64::R11,                                 // R5..R6  = volatile scratch
    x64::RBX, x64::R12, x64::R13, x64::R14, x64::R15,   // R7..R11 = nonvolatile
    x64::RSI,                                           // R12     = nonvolatile
    x64::RAX,                                           // R13     = volatile, holds every return
};
static constexpr uint8_t  kNonvolSaveBytes = 56;   // rbx+rdi+rsi+r12..r15 (7×8)
static constexpr int32_t  kArg5Offset      = 48;   // ret-addr 8 + saved-rbp 8 + shadow 32
static constexpr uint16_t kShadowSpace     = 32;   // Win64 shadow for OUR callees
#else
// System V (Linux, Intel macOS). Args in rdi/rsi/rdx/rcx/r8/r9 — all five kArg registers arrive
// in registers. Nonvolatiles rbx, r12-r15 (5) are saved by prologue.
static constexpr uint8_t kX64Reg[kRegCount] = {
    x64::RDI, x64::RSI, x64::RDX, x64::RCX, x64::R8,    // R0..R4  = kArg0..kArg4 (all in registers)
    x64::R9,  x64::R10, x64::R11,                       // R5..R7  = volatile scratch
    x64::RBX, x64::R12, x64::R13, x64::R14, x64::R15,   // R8..R12 = nonvolatile
    x64::RAX,                                           // R13     = volatile, holds every return
};
static constexpr uint8_t  kNonvolSaveBytes = 40;   // rbx+r12..r15 (5×8)
static constexpr uint16_t kShadowSpace     = 0;    // SysV has no shadow-space obligation
#endif

// Bounds-checked vreg→machine lookup. Same defense the arm64 backend uses: a mapping/reservation
// disagreement is a bug, but reading past the map returns undefined memory, which would name a
// wrong-and-unrelated register in the emitted byte. Clamp to the last entry so the failure is
// wrong-but-safe.
static uint8_t xr(Reg r) { return kX64Reg[r < kRegCount ? r : kRegCount - 1]; }

// rax must not sit inside the vreg pool AND be the call-target scratch at once, or a `movabs
// rax, fn` in call() would clobber whatever vreg R13 held before the pool-save happened. Rax is
// R13 by design; call() saves it before it becomes scratch. Assert it explicitly, matching the
// arm64 scratch check.
static_assert(kX64Reg[kRegCount - 1] == x64::RAX,
              "rax must be the last vreg — call() relies on saving it before using it as the fn-target scratch");

Label HostAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }
    Label l = labelCount_++;
    labelPos_[l] = -1;
    return l;
}
void HostAssembler::bind(Label l) { if (l < kMaxLabels) labelPos_[l] = static_cast<int32_t>(len_); }

void HostAssembler::addFixup(size_t at, Label label, FixKind kind) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label, kind};
}

// Emit N bytes little-endian into the code buffer via the existing emitBytes path (which owns
// bounds checking + the overflow_ flag). x86 opcodes are 1-15 bytes long; a stack buffer of 16
// is enough for any single instruction the backend emits here.
// No emit32 here: x86-64 instructions are 1-15 bytes, so every encoder below marshals into a
// local buffer and calls emitBytes. The header still declares emit32 for the arm64 branch, where
// a fixed 4-byte word is the natural unit; leaving it undefined on this ISA is what says it has
// no meaning here, and nothing links against it.
void HostAssembler::emitBytes(const uint8_t* p, size_t n) {
    if (!buf_ || len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}

// x86-64 encoding primitives — all inline in this file since they're one-liners used only here.
// REX prefix: W=1 for 64-bit operand size; R/X/B are the high-register bits for the reg / index /
// base fields. Emit only when at least one bit is needed OR when 64-bit operand size is required.
static inline uint8_t rex_(bool w, bool r_high, bool x_high, bool b_high) {
    return 0x40 | (w ? 0x08 : 0) | (r_high ? 0x04 : 0) | (x_high ? 0x02 : 0) | (b_high ? 0x01 : 0);
}
static inline uint8_t modrm_(uint8_t mod, uint8_t reg, uint8_t rm) {
    return uint8_t((mod & 3) << 6) | uint8_t((reg & 7) << 3) | uint8_t(rm & 7);
}
static inline uint8_t sib_(uint8_t scale, uint8_t index, uint8_t base) {
    return uint8_t((scale & 3) << 6) | uint8_t((index & 7) << 3) | uint8_t(base & 7);
}

// Mov r64, r64  (REX.W 89 /r) — the workhorse register-copy instruction.
static void emitMovRegReg(HostAssembler* A, uint8_t dst, uint8_t src) {
    uint8_t b[3] = {
        rex_(true, src >= 8, false, dst >= 8),
        0x89,
        modrm_(0b11, src & 7, dst & 7),
    };
    A->emitBytes(b, 3);
}

// The shared body of the three [base + disp] memory forms below: REX.W + opcode + ModR/M with a
// disp8 (mod=01) when the displacement fits a signed byte, disp32 (mod=10) otherwise. The disp8
// form saves 3 bytes per access, and the frame's hot offsets — the parked host arguments and the
// spill region's upper half — all fit it. Density matters: spill traffic is most of what a
// script emits, and a fill script that fits an arm64-sized 256-byte buffer must keep fitting
// here (the "compiled fill is BEHAVIORALLY identical" test's buffer pins exactly that).
// If base is rsp/r12 (low 3 bits = 100), a SIB byte is required.
static void emitMemDispOp(HostAssembler* A, uint8_t opcode, uint8_t reg, uint8_t base,
                          int32_t disp) {
    const bool needsSIB = ((base & 7) == x64::RSP);
    const bool disp8 = (disp >= -128 && disp <= 127);
    uint8_t b[8]; size_t n = 0;
    b[n++] = rex_(true, reg >= 8, false, base >= 8);
    b[n++] = opcode;
    b[n++] = modrm_(disp8 ? 0b01 : 0b10, reg & 7, needsSIB ? 0b100 : (base & 7));
    if (needsSIB) b[n++] = sib_(0, 0b100, base & 7);        // scale=0, index=none (0b100)
    b[n++] = uint8_t(disp);
    if (!disp8) {
        b[n++] = uint8_t(disp >> 8); b[n++] = uint8_t(disp >> 16); b[n++] = uint8_t(disp >> 24);
    }
    A->emitBytes(b, n);
}

// Mov r64, [base + disp]  (REX.W 8B /r) — the workhorse load-from-frame instruction.
static void emitMovRegMemDisp(HostAssembler* A, uint8_t dst, uint8_t base, int32_t disp) {
    emitMemDispOp(A, 0x8B, dst, base, disp);
}

// Mov [base + disp], r64  (REX.W 89 /r)
static void emitMovMemDispReg(HostAssembler* A, uint8_t base, int32_t disp, uint8_t src) {
    emitMemDispOp(A, 0x89, src, base, disp);
}

// LEA r64, [base + disp]  (REX.W 8D /r) — the address-arithmetic instruction, used by slotAddr
// to hand a callee the address of a frame slot without materializing it via a mov.
static void emitLeaRegMemDisp(HostAssembler* A, uint8_t dst, uint8_t base, int32_t disp) {
    emitMemDispOp(A, 0x8D, dst, base, disp);
}

// Push r64  (50+r; REX.B for r8-r15). One byte per non-high register — the compact prologue path.
static void emitPushReg(HostAssembler* A, uint8_t reg) {
    uint8_t b[2]; size_t n = 0;
    if (reg >= 8) b[n++] = rex_(false, false, false, true);
    b[n++] = uint8_t(0x50 + (reg & 7));
    A->emitBytes(b, n);
}
// Pop r64  (58+r; REX.B for r8-r15).
static void emitPopReg(HostAssembler* A, uint8_t reg) {
    uint8_t b[2]; size_t n = 0;
    if (reg >= 8) b[n++] = rex_(false, false, false, true);
    b[n++] = uint8_t(0x58 + (reg & 7));
    A->emitBytes(b, n);
}

// Sub rsp, imm  (REX.W 83 /5 ib for a byte-sized amount — call()'s 32-byte shadow — 81 /5 id
// for the frame reservation, which exceeds 127).
static void emitSubRspImm32(HostAssembler* A, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        uint8_t b[4] = {
            rex_(true, false, false, false), 0x83, modrm_(0b11, 5, x64::RSP), uint8_t(imm),
        };
        A->emitBytes(b, 4);
        return;
    }
    uint8_t b[7] = {
        rex_(true, false, false, false),
        0x81,
        modrm_(0b11, 5, x64::RSP),                       // /5 = SUB, r/m = rsp
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    A->emitBytes(b, 7);
}

// (No emitAddRspImm32 helper: the epilogue uses `lea rsp, [rbp - kNonvolSaveBytes]` to unwind
// the frame in one instruction, which also drops anything call() left on the stack.)

// --- the call frame -----------------------------------------------------------------------------
//
// Frame layout, address ORDER (low → high):
//   [rsp+0] .. [rsp+kShadowSpace-1]              shadow space our callees are owed (Win64; SysV 0)
//   [rsp+kShadowSpace]                           callLabel's outgoing arg 5 (Win64)
//   ... alignment padding ...
//   [rbp - kNonvolSaveBytes - 8*kTotalSlots]     slot 0            (deepest)
//   [rbp - kNonvolSaveBytes - 8]                 slot kTotalSlots-1
//   [rbp - kNonvolSaveBytes]                     first saved nonvolatile (pushed)
//   [rbp - 8]                                    last saved nonvolatile (pushed)
//   [rbp]                                        saved rbp (from `push rbp`)
//   [rbp + 8]                                    return address
//   [rbp + 16 .. rbp+kArg5Offset-1]              (Win64) caller's shadow space
//   [rbp + kArg5Offset]                          (Win64) caller's arg 5 = the ctrls pointer
//
// Slots ASCEND with the index (slotOffsetFromRbp), because IrOp::Call hands a builtin the address
// of its first argument slot and the callee reads upward from there. Addressed from rbp rather
// than rsp so they survive call() moving rsp, the same decision arm64 makes with x29.
//
// call() saves the vreg pool BELOW this frame with pushes; nothing in the frame is reserved for
// it beyond the shadow space above.

// call() saves the vreg pool with PUSHES (1-2 bytes each), not rsp-relative movs (8 bytes each)
// — the pool is 14 registers and a script calls builtins per-pixel-loop, so the encoding size of
// call() dominates a call-dense script's emitted bytes. The frame reserves only the OUTGOING
// area at the bottom of rsp: the Win64 shadow space our callees are owed plus one stack slot for
// callLabel's arg 5 (the SysV build needs neither, but carrying the 8 bytes uniformly beats a
// second frame formula). Spill slots are rbp-relative, so the rsp movement inside call() cannot
// disturb them.
// Slot n's offset from rbp (negative — the slots live BELOW the saved nonvols). ASCENDING with
// the slot index, exactly like arm64's x29 + 16 + n*8: IrOp::Call passes slotAddr(firstSlot) and
// the host builtin reads its arguments as an ARRAY walking upward from that address
// (moonlive_lower.h), so consecutive slot indices MUST be consecutive ascending memory. Reverse
// this and every non-inlined builtin (random16, addLight, addUint8, line, palette reads)
// receives its arguments backwards, while everything inlined keeps working.
//
// The region is sized at kTotalSlots whatever the script uses: a fixed 168 bytes of desktop stack
// against a formula that would need the slot count at every call site.
static inline int32_t slotOffsetFromRbp(uint8_t slot) {
    return -int32_t(kNonvolSaveBytes) - 8 * int32_t(kTotalSlots - slot);
}

void HostAssembler::prologue(uint8_t slots) {
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // Unlike arm64, x86-64 ALWAYS emits a frame — the reasons are stated at the top of this
    // branch (nonvols need saving, shadow space required by Win64).
    emitPushReg(this, x64::RBP);                              // push rbp
    emitMovRegReg(this, x64::RBP, x64::RSP);                       // mov rbp, rsp

    // Save nonvolatiles this ABI's vreg map uses.
#if defined(_WIN32)
    // Order: rbx, rdi, rsi, r12, r13, r14, r15 — matches the offsets used at epilogue's pop.
    emitPushReg(this, x64::RBX);
    emitPushReg(this, x64::RDI);
    emitPushReg(this, x64::RSI);
    emitPushReg(this, x64::R12);
    emitPushReg(this, x64::R13);
    emitPushReg(this, x64::R14);
    emitPushReg(this, x64::R15);
#else
    emitPushReg(this, x64::RBX);
    emitPushReg(this, x64::R12);
    emitPushReg(this, x64::R13);
    emitPushReg(this, x64::R14);
    emitPushReg(this, x64::R15);
#endif

    // Reserve stack for: the outgoing area (callee shadow space + callLabel's stack arg 5) plus
    // the spill slots. Align the whole thing to 16 so rsp is 16-aligned before any call.
    //
    // At entry: rsp was (X - 8) for return addr. After push rbp: (X - 16), 16-aligned. After N
    // pushes of 8 bytes each: 16-aligned iff N is even. Win64 pushes 7 nonvols → odd → rsp is
    // 8 mod 16. SysV pushes 5 → odd → same. To land 16-aligned we need frameBytes % 16 == 8.
    uint32_t needed = uint32_t(kShadowSpace) + 8   // outgoing: callee shadow + arg-5 slot
                    + uint32_t(kTotalSlots) * 8;   // full slot region — slotOffsetFromRbp is fixed
    // Bring rsp back to 16-alignment: needed + (rsp offset mod 16) must be 0 mod 16.
    // After the 7 (or 5) pushes above, rsp % 16 == 8, so we need frame % 16 == 8.
    if ((needed % 16) != 8) needed += (8 - (needed % 16) + 16) % 16;
    frameBytes_ = uint16_t(needed);
    emitSubRspImm32(this, int32_t(needed));

#if defined(_WIN32)
    // Load kArg4 (arg 5 = the ctrls arena pointer) from the caller's stack into R4 (= rdi).
    // arg 5 lives at [rbp + kArg5Offset]. Under Win64 it's on the caller's stack because only
    // the first four args ride registers; SysV skips this because r8 IS the arg-5 register and
    // is already in place as R4.
    emitMovRegMemDisp(this, x64::RDI, x64::RBP, kArg5Offset);
#endif

    // Zero-extend the narrow arguments. REQUIRED ON BOTH ABIs, which is why it is not inside
    // the switch above. CtrlFn is
    //   void (uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t, const uint8_t* ctrls)
    // and x86-64 leaves the UPPER bits of a register holding a narrower argument UNDEFINED: a
    // caller sets edx / r8b / r9d (Win64) or esi / dl / ecx (SysV) and may leave anything above.
    // The IR then spills each host argument to a frame slot as a full 64-bit store, and the loop
    // math multiplies a 64-bit view of that slot, so a dirty `cpl` reaches the pixel write as
    // ~0x7fffffff_ffffff03 and the address lands outside the buffer. arm64's ABI extends narrow
    // arguments for us, which is why only this backend needs it.
    //
    // The pointers (buf, ctrls) are full-width by definition and need nothing.
#if defined(_WIN32)
    { uint8_t b[2] = {0x89, 0xD2};             emitBytes(b, 2); }  // mov   edx, edx   (nLights)
    { uint8_t b[4] = {0x4D, 0x0F, 0xB6, 0xC0}; emitBytes(b, 4); }  // movzx r8,  r8b   (cpl)
    { uint8_t b[3] = {0x45, 0x89, 0xC9};       emitBytes(b, 3); }  // mov   r9d, r9d   (t)
#else
    { uint8_t b[2] = {0x89, 0xF6};             emitBytes(b, 2); }  // mov   esi, esi   (nLights)
    { uint8_t b[4] = {0x48, 0x0F, 0xB6, 0xD2}; emitBytes(b, 4); }  // movzx rdx, dl    (cpl)
    { uint8_t b[2] = {0x89, 0xC9};             emitBytes(b, 2); }  // mov   ecx, ecx   (t)
#endif
}

void HostAssembler::epilogue() {
    // Restore rsp to point AT the last-pushed nonvol.
    // lea rsp, [rbp - kNonvolSaveBytes]  — one instruction, drops any adjustment call() made.
    emitLeaRegMemDisp(this, x64::RSP, x64::RBP, -int32_t(kNonvolSaveBytes));
    // Pops in reverse push order.
#if defined(_WIN32)
    emitPopReg(this, x64::R15);
    emitPopReg(this, x64::R14);
    emitPopReg(this, x64::R13);
    emitPopReg(this, x64::R12);
    emitPopReg(this, x64::RSI);
    emitPopReg(this, x64::RDI);
    emitPopReg(this, x64::RBX);
#else
    emitPopReg(this, x64::R15);
    emitPopReg(this, x64::R14);
    emitPopReg(this, x64::R13);
    emitPopReg(this, x64::R12);
    emitPopReg(this, x64::RBX);
#endif
    emitPopReg(this, x64::RBP);
    ret();
}

void HostAssembler::spillStore(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emitMovMemDispReg(this, x64::RBP, slotOffsetFromRbp(slot), xr(r));
}
void HostAssembler::spillLoad(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emitMovRegMemDisp(this, xr(r), x64::RBP, slotOffsetFromRbp(slot));
}
void HostAssembler::slotAddr(Reg d, uint8_t slot) {
    if (slot >= kMaxSpillSlots || frameBytes_ == 0) { overflow_ = true; return; }
    emitLeaRegMemDisp(this, xr(d), x64::RBP, slotOffsetFromRbp(slot));
}

// --- movs ---------------------------------------------------------------------------------------

// mov r64, imm64  (movabs, REX.W B8+r imm64) — 10 bytes. The direct analog of arm64's
// movz + 3×movk sequence for materializing a full-width pointer.
void HostAssembler::movPtr(Reg d, const void* p) {
    const uint64_t addr = reinterpret_cast<uint64_t>(p);
    const uint8_t dst = xr(d);
    uint8_t b[10];
    b[0] = rex_(true, false, false, dst >= 8);
    b[1] = uint8_t(0xB8 + (dst & 7));
    for (int i = 0; i < 8; i++) b[2 + i] = uint8_t(addr >> (i * 8));
    emitBytes(b, 10);
}

// mov r64, imm32 sign-extended  (REX.W C7 /0 id) — the 32-bit-immediate form. Handles both
// positive and negative int32_t immediates uniformly (sign-extended to 64 bits by the CPU).
// Wider than the imm16-based arm64 movz/movn but simpler: one instruction covers the whole
// int32 range, so there is no "does it fit in 16 bits?" check.
void HostAssembler::movImm(Reg d, int32_t imm) {
    const uint8_t dst = xr(d);
    uint8_t b[7] = {
        rex_(true, false, false, dst >= 8),
        0xC7,
        modrm_(0b11, 0, dst & 7),                        // /0 = MOV r/m64, imm32
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    emitBytes(b, 7);
}

// mov r64, r64  — the direct copy. Same encoding as spillStore/Load's core, exposed for the
// lowering (movReg is emitted anywhere the IR wants to alias one vreg to another).
void HostAssembler::movReg(Reg d, Reg a) {
    if (d == a) return;                                  // no-op copy, elide
    emitMovRegReg(this, xr(d), xr(a));
}

// --- arithmetic ---------------------------------------------------------------------------------

// add r64, imm  (REX.W 83 /0 ib when the immediate fits a signed byte — the common case: loop
// steps and channel offsets are ±1 or ±2 — REX.W 81 /0 id otherwise). Sign-extended by the CPU.
void HostAssembler::addImm(Reg d, Reg a, int32_t imm) {
    // d = a + imm  →  if d != a, first mov d, a. Then add d, imm.
    if (d != a) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    if (imm >= -128 && imm <= 127) {
        uint8_t b[4] = {
            rex_(true, false, false, dst >= 8),
            0x83,
            modrm_(0b11, 0, dst & 7),                    // /0 = ADD
            uint8_t(imm),
        };
        emitBytes(b, 4);
        return;
    }
    uint8_t b[7] = {
        rex_(true, false, false, dst >= 8),
        0x81,
        modrm_(0b11, 0, dst & 7),                        // /0 = ADD
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    emitBytes(b, 7);
}

// add r64, r64  (REX.W 01 /r).
void HostAssembler::addReg(Reg d, Reg a, Reg b) {
    // d = a + b  →  if d == a: add d, b.  Elif d == b: add d, a.  Else: mov d, a; add d, b.
    if (d != a && d != b) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    const uint8_t src = (d == a || (d != a && d != b)) ? xr(b) : xr(a);
    uint8_t bytes[3] = {
        rex_(true, src >= 8, false, dst >= 8),
        0x01,
        modrm_(0b11, src & 7, dst & 7),
    };
    emitBytes(bytes, 3);
}

// imul r64, r64, imm32  (REX.W 69 /r id) — the three-operand form: d = a * imm.
void HostAssembler::mulImm(Reg d, Reg a, int32_t imm) {
    const uint8_t dst = xr(d), src = xr(a);
    uint8_t b[7] = {
        rex_(true, dst >= 8, false, src >= 8),
        0x69,
        modrm_(0b11, dst & 7, src & 7),
        uint8_t(imm), uint8_t(imm >> 8), uint8_t(imm >> 16), uint8_t(imm >> 24),
    };
    emitBytes(b, 7);
}

// imul r64, r64  (REX.W 0F AF /r) — two-operand: d = d * a.
void HostAssembler::mulReg(Reg d, Reg a, Reg b) {
    // d = a * b → if d == a: imul d, b.  Elif d == b: imul d, a.  Else: mov d, a; imul d, b.
    if (d != a && d != b) emitMovRegReg(this, xr(d), xr(a));
    const uint8_t dst = xr(d);
    const uint8_t src = (d == a || (d != a && d != b)) ? xr(b) : xr(a);
    uint8_t bytes[4] = {
        rex_(true, dst >= 8, false, src >= 8),
        0x0F, 0xAF,
        modrm_(0b11, dst & 7, src & 7),
    };
    emitBytes(bytes, 4);
}

// --- memory ops ---------------------------------------------------------------------------------

// The shared body of the four INDEXED memory ops: `[base + index]`, unscaled, which is how this
// backend addresses a pixel (the lowering multiplies an element index by the element width before
// it gets here, so the index is always a byte offset).
//
// Two encoding rules live here once instead of four times:
//
// - A base whose low three bits are 101 (rbp, r13) cannot use mod=00: the SDM gives that slot to
//   "disp32, no base", so the emitted instruction would address an absolute constant instead of
//   the register. mod=01 with an explicit zero disp8 means the same thing and encodes correctly.
//   Unreachable today (the lowering only ever bases these on kArg0/kArg4), but the cost is one
//   comparison, and the previous version of this code carried a comment claiming it was guarded
//   when it was not, which is the kind of note that stops the next reader from checking.
// - REX is emitted UNCONDITIONALLY for the byte store. Without it, an 8-bit operand naming
//   register 4-7 means ah/ch/dh/bh, not sil/dil/bpl/spl, and this backend's vreg maps do put
//   values in rsi/rdi/rbx, so the wrong half-register would be stored with no diagnostic.
void HostAssembler::emitIndexed(const uint8_t* opcode, size_t opLen, bool prefix66,
                                bool forceRex, uint8_t reg, uint8_t base, uint8_t index) {
    const bool baseNeedsDisp = ((base & 7) == 0b101);    // rbp / r13
    uint8_t b[8]; size_t n = 0;
    if (prefix66) b[n++] = 0x66;                         // operand-size override → 16-bit
    if (forceRex || reg >= 8 || index >= 8 || base >= 8)
        b[n++] = rex_(false, reg >= 8, index >= 8, base >= 8);
    for (size_t i = 0; i < opLen; i++) b[n++] = opcode[i];
    b[n++] = modrm_(baseNeedsDisp ? 0b01 : 0b00, reg & 7, 0b100);   // r/m=100 → SIB follows
    b[n++] = sib_(0, index & 7, base & 7);                          // scale=0: unscaled index
    if (baseNeedsDisp) b[n++] = 0x00;                               // the explicit zero disp8
    emitBytes(b, n);
}

// mov byte ptr [base + off], val_l  (88 /r with SIB): indexed 1-byte store, the pixel write.
void HostAssembler::store8(Reg base, Reg off, Reg val) {
    const uint8_t op = 0x88;
    emitIndexed(&op, 1, /*prefix66=*/false, /*forceRex=*/true, xr(val), xr(base), xr(off));
}
// mov r64_low16, [base + off]  — index-in-reg. Used for control byte reads.
// x86 zero-extends 8-bit loads to 32 bits automatically (movzx). The 64-bit destination is
// implicitly zero-extended above bit 31, matching the arm64 ldrb behavior.
void HostAssembler::load8(Reg d, Reg base, int32_t imm) {
    const uint8_t dst = xr(d), b_reg = xr(base);
    // movzx r32, byte ptr [base + disp32]  (0F B6 /r) — zero-extend to 32; the r32 write clears
    // the upper 32 bits of the r64.
    const bool needsSIB = ((b_reg & 7) == x64::RSP);
    uint8_t b[9]; size_t n = 0;
    b[n++] = rex_(false, dst >= 8, false, b_reg >= 8);   // no REX.W needed (r32 dest zero-extends)
    b[n++] = 0x0F; b[n++] = 0xB6;
    b[n++] = modrm_(0b10, dst & 7, needsSIB ? 0b100 : (b_reg & 7));
    if (needsSIB) b[n++] = sib_(0, 0b100, b_reg & 7);
    b[n++] = uint8_t(imm); b[n++] = uint8_t(imm >> 8);
    b[n++] = uint8_t(imm >> 16); b[n++] = uint8_t(imm >> 24);
    emitBytes(b, n);
}
// mov word ptr [base + off], val_l16  (66 89 /r SIB) — indexed 2-byte store.
// The 66 prefix switches operand size to 16 bits for a 32-bit-mode instruction.
void HostAssembler::store16(Reg base, Reg off, Reg val) {
    const uint8_t op = 0x89;
    emitIndexed(&op, 1, /*prefix66=*/true, /*forceRex=*/false, xr(val), xr(base), xr(off));
}
// movzx r32, word ptr [base + disp32]  (0F B7 /r) — 16-bit zero-extending load. Immediate offset.
void HostAssembler::load16(Reg d, Reg base, int32_t imm) {
    const uint8_t dst = xr(d), b_reg = xr(base);
    const bool needsSIB = ((b_reg & 7) == x64::RSP);
    uint8_t b[9]; size_t n = 0;
    b[n++] = rex_(false, dst >= 8, false, b_reg >= 8);
    b[n++] = 0x0F; b[n++] = 0xB7;
    b[n++] = modrm_(0b10, dst & 7, needsSIB ? 0b100 : (b_reg & 7));
    if (needsSIB) b[n++] = sib_(0, 0b100, b_reg & 7);
    b[n++] = uint8_t(imm); b[n++] = uint8_t(imm >> 8);
    b[n++] = uint8_t(imm >> 16); b[n++] = uint8_t(imm >> 24);
    emitBytes(b, n);
}
// movzx r32, byte ptr [base + off]  (0F B6 /r SIB) — indexed 8-bit zero-extending load.
void HostAssembler::load8Idx(Reg d, Reg base, Reg off) {
    const uint8_t op[2] = {0x0F, 0xB6};
    emitIndexed(op, 2, /*prefix66=*/false, /*forceRex=*/false, xr(d), xr(base), xr(off));
}
// movzx r32, word ptr [base + off]  (0F B7 /r SIB) — indexed 16-bit zero-extending load.
void HostAssembler::load16Idx(Reg d, Reg base, Reg off) {
    const uint8_t op[2] = {0x0F, 0xB7};
    emitIndexed(op, 2, /*prefix66=*/false, /*forceRex=*/false, xr(d), xr(base), xr(off));
}

// --- compare and branch -------------------------------------------------------------------------

// cmp r64, r64  (REX.W 39 /r) — sets flags = a - b.
void HostAssembler::cmp(Reg a, Reg b) {
    const uint8_t left = xr(a), right = xr(b);
    uint8_t bytes[3] = {
        rex_(true, right >= 8, false, left >= 8),
        0x39,
        modrm_(0b11, right & 7, left & 7),
    };
    emitBytes(bytes, 3);
}

// Conditional branch, near-32-bit-relative  (0F 8x rel32). Always the rel32 form — one width,
// so the fixup table is uniform. Condition tt values:  NE=0x5, HS/AE=0x3, LO/B=0x2.
void HostAssembler::branchIf(Cond c, Label l) {
    uint8_t tt;
    switch (c) {
        case Cond::Ne: tt = 0x05; break;
        case Cond::Hs: tt = 0x03; break;                // aka AE: unsigned >=
        case Cond::Lo: tt = 0x02; break;                // aka B:  unsigned <
        default:       tt = 0x05; break;
    }
    // Fixup site is the START of the branch instruction; patchBranches computes rel32 relative
    // to the byte AFTER the instruction (PC-relative on x86). The fixup helper stores the site;
    // patchBranches knows the instruction is 6 bytes long, so it applies rel32 at site+2.
    addFixup(len_, l, FixKind::Branch);
    uint8_t b[6] = { 0x0F, uint8_t(0x80 | tt), 0, 0, 0, 0 };
    emitBytes(b, 6);
}

// cbz-equivalent: test r64, r64  (REX.W 85 /r reg,reg self) then je rel32.
void HostAssembler::branchIfZero(Reg a, Label l) {
    const uint8_t reg = xr(a);
    // test reg, reg — flags reflect zero-ness of reg.
    uint8_t t[3] = {
        rex_(true, reg >= 8, false, reg >= 8),
        0x85,
        modrm_(0b11, reg & 7, reg & 7),
    };
    emitBytes(t, 3);
    // je rel32 — fixup site is where the je starts.
    addFixup(len_, l, FixKind::Branch);
    uint8_t j[6] = { 0x0F, 0x84, 0, 0, 0, 0 };
    emitBytes(j, 6);
}

// The fused compare-and-branch pair.
void HostAssembler::branchGeU(Reg a, Reg b, Label l) { cmp(a, b); branchIf(Cond::Hs, l); }
void HostAssembler::branchNe(Reg a, Reg b, Label l)  { cmp(a, b); branchIf(Cond::Ne, l); }

// --- ret ----------------------------------------------------------------------------------------
void HostAssembler::ret() {
    uint8_t b = 0xC3;
    emitBytes(&b, 1);
}

// --- call ---------------------------------------------------------------------------------------
//
// Host builtin call: d = fn(a, b, c). The textbook push/pop shape: push the whole vreg pool
// (the live-vreg-across-call contract), load the arguments from their pushed copies (memory
// sources, so an argument register overlapping a source is harmless — the "one is x0" problem
// arm64 solves with x15/x16/x17 scratch), call through rax, write the zero-extended return over
// dst's pushed slot, pop everything back.
//
// Pushes rather than rsp-relative movs because call() dominates a call-dense script's emitted
// size: 1-2 bytes against 8, which is ~100 bytes per call instead of ~270. That is the difference
// between fitting the buffers arm64 fits and overflowing them. The density canary in
// unit_moonlive_codegen_x86_64.cpp is what holds that.
//
// Alignment: prologue leaves rsp 16-aligned; kRegCount (14) pushes move it by 112 ≡ 0 (mod 16),
// so it is still 16-aligned at the call — with the Win64 shadow sub folded in around it.

// A pushed vreg's offset from rsp while the pool is on the stack (before the shadow sub): vreg 0
// was pushed first so it sits highest.
static constexpr int32_t pushedSlot(uint8_t v) {
    return int32_t(kRegCount - 1 - v) * 8;
}

void HostAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // 1) Push the whole vreg pool, v0 first (v0 ends up at the highest address).
    for (uint8_t v = 0; v < kRegCount; v++) emitPushReg(this, kX64Reg[v]);
#if defined(_WIN32)
    // 2) Win64: reserve the callee's 32-byte shadow space. (SysV owes none.)
    emitSubRspImm32(this, int32_t(kShadowSpace));
#endif
    // 3) Load args from the pushed copies into the ABI argument registers.
    const int32_t base = int32_t(kShadowSpace);   // pushed slots sit above the shadow sub
#if defined(_WIN32)
    emitMovRegMemDisp(this, x64::RCX, x64::RSP, base + pushedSlot(a));
    emitMovRegMemDisp(this, x64::RDX, x64::RSP, base + pushedSlot(b));
    emitMovRegMemDisp(this, x64::R8,  x64::RSP, base + pushedSlot(c));
#else
    emitMovRegMemDisp(this, x64::RDI, x64::RSP, base + pushedSlot(a));
    emitMovRegMemDisp(this, x64::RSI, x64::RSP, base + pushedSlot(b));
    emitMovRegMemDisp(this, x64::RDX, x64::RSP, base + pushedSlot(c));
#endif
    // 4) movabs rax, fn — rax's vreg value (R13) is safe in its pushed slot.
    {
        const uint64_t addr = reinterpret_cast<uint64_t>(fn);
        uint8_t bytes[10];
        bytes[0] = rex_(true, false, false, false);
        bytes[1] = 0xB8;                                 // MOV rax, imm64
        for (int i = 0; i < 8; i++) bytes[2 + i] = uint8_t(addr >> (i * 8));
        emitBytes(bytes, 10);
    }
    // 5) call rax  (FF /2).
    {
        uint8_t bytes[2] = { 0xFF, modrm_(0b11, 2, x64::RAX) };
        emitBytes(bytes, 2);
    }
#if defined(_WIN32)
    // 6) Drop the shadow space so the pushed slots are back at [rsp + pushedSlot(v)].
    //    add rsp, 32 in the imm8 form (REX.W 83 /0 ib).
    {
        uint8_t bytes[4] = {
            rex_(true, false, false, false), 0x83, modrm_(0b11, 0, x64::RSP),
            uint8_t(kShadowSpace),
        };
        emitBytes(bytes, 4);
    }
#endif
    // 7) Zero-extend the return — the ABI leaves rax's upper bits undefined for a narrower-than-
    //    64-bit return (random16 is uint16_t), and this backend's index math is 64-bit-wide, so
    //    dirty upper bits would ride into addresses. `mov eax, eax` (89 C0) zeroes them.
    {
        uint8_t bytes[2] = {0x89, 0xC0};
        emitBytes(bytes, 2);
    }
    // 8) Write the return over DST's pushed slot — the pop below then restores every OTHER vreg
    //    and loads dst with the return, in one uniform restore. (If dst is R13 = rax, its slot is
    //    overwritten and the pop hands rax its own return — still correct.)
    emitMovMemDispReg(this, x64::RSP, pushedSlot(d), x64::RAX);
    // 9) Pop the pool in reverse (v13 first).
    for (uint8_t v = kRegCount; v-- > 0;) emitPopReg(this, kX64Reg[v]);
}

// Script-to-script call: bl-equivalent on x64 is `call rel32` (E8 xx xx xx xx). The callee's
// prologue saves rbp and nonvolatiles, exactly like our own prologue, so calls nest.
void HostAssembler::callLabel(Label l) {
    // Reload parked host arguments from their spill slots — the contract with IrOp::CallScript
    // in core, mirroring arm64. On x64 this matters: the arg registers ARE vregs the callee may
    // have consumed, so their live-across-a-called-function meaning has to be re-established.
    for (uint8_t v = 0; v < kHostArgSlots; v++) spillLoad(static_cast<Reg>(v), hostArgSlot(v));
#if defined(_WIN32)
    // Win64 passes arg 5 (= kArg4, the ctrls-arena pointer) on the CALLER's stack at [rsp+32],
    // above the 32-byte shadow space. The entry-point function's prologue reads kArg4 from
    // [rbp+48] and it works because the C++ caller followed Win64 exactly. A script-to-script
    // call must do the same: without this store, the callee's `mov rdi, [rbp+48]` reads whatever
    // happened to be in that stack slot (an old vreg-save byte, a return-address byte from a
    // previous call), rdi becomes garbage, and the first host(kArg4) dereferences it and faults.
    // [rsp+kShadowSpace] is the outgoing arg-5 slot prologue reserves for exactly this (see the
    // frame layout above); call() saves vregs with pushes below rsp and reserves nothing here.
    // SysV passes arg 5 in r8, which IS R4 in the SysV map; the spillLoad above already put it
    // in the right register, so no stack store is needed.
    emitMovMemDispReg(this, x64::RSP, kShadowSpace, xr(R4));
#endif
    addFixup(len_, l, FixKind::Call);
    uint8_t b[5] = { 0xE8, 0, 0, 0, 0 };
    emitBytes(b, 5);
}

// Resolve all pending fixups: overwrite the rel32 field in each branch/call with (target - site).
void HostAssembler::patchBranches() {
    // An OVERFLOWED compile is refused by lowerWith after finalize(), so patching it is pointless —
    // and unsafe: a fixup recorded just before emitBytes dropped its instruction points at the
    // buffer's end, and the memcpy below would write up to 5 bytes past buf_. "an unencodable
    // script is refused, not truncated" is the test that reaches this path.
    if (!buf_ || overflow_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        int32_t target = labelPos_[f.label];
        if (target < 0) continue;                        // unbound label — leave alone
        if (f.kind == FixKind::Call) {
            // `call rel32` at [f.at]: opcode E8 at f.at, imm32 at f.at+1. rel is relative to the
            // byte AFTER the imm32 field, so subtract (f.at + 5).
            int32_t rel = target - (int32_t(f.at) + 5);
            uint8_t bytes[4] = {
                uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24)
            };
            std::memcpy(buf_ + f.at + 1, bytes, 4);
        } else {
            // `0F 8x rel32` at [f.at]: 2-byte opcode then imm32. rel is relative to (f.at + 6).
            int32_t rel = target - (int32_t(f.at) + 6);
            uint8_t bytes[4] = {
                uint8_t(rel), uint8_t(rel >> 8), uint8_t(rel >> 16), uint8_t(rel >> 24)
            };
            std::memcpy(buf_ + f.at + 2, bytes, 4);
        }
    }
}

#else

// No host assembler on this ISA. The matching `lowerToBytes` stub in moonlive_lower_host.cpp
// returns 0, so MoonLive::compile fails cleanly and a scripted module runs dark — the desktop
// binary still builds and runs everything else.

#endif

}  // namespace mm::moonlive
