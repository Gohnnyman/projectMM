#include "moonlive_asm_xtensa.h"

#include <cstring>

#if defined(__XTENSA__)   // the Xtensa assembler is only built for Xtensa targets

// MoonLive Xtensa assembler — named instructions encoded once, composed by the IR lowering.
// Encodings verified against xtensa-esp32s3-elf-as (see the plan / commit). Xtensa is
// little-endian with mixed 24-bit (wide) and 16-bit (narrow) instructions. The register
// convention: R0..R3 → a2..a5 (the windowed-ABI args buf/nLights/cpl/t), R4..R9 → a6..a11.
//
// Branch offset rule (verified): for the 8-bit-offset conditional branches we use, the offset
// byte = target - (branchInstrAddr + 4). All such branches put that byte at instrAddr+2, so one
// fixup kind covers them.

namespace mm::moonlive {

// R0..R3 → a2..a5 (the windowed-ABI args); R4..R11 → a6..a11, a14, a15. a12/a13 are internal
// scratch (store8 address, branchIfZero zero-reg, call result stash), so not in the pool.
static constexpr uint8_t kXtReg[kRegCount] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
// Map a vreg to its machine register. BOUNDS-CHECKED: the inline ops address their scratch as
// `vregsUsed + n`, which walks off the end of the map when a program uses the whole file — and an
// out-of-bounds read here returns whatever byte follows the array, so the emitted instruction names
// a register chosen by accident. That produced a12 (a call8 window register AND this assembler's own
// scratch) in a program that had no business touching it. Clamped to the last real entry so a
// mistake is a wrong-but-safe register rather than undefined behaviour; the static_assert below and
// the reservation in the lowerer are what keep it from happening at all.
static uint8_t ar(Reg r) { return kXtReg[r < kRegCount ? r : kRegCount - 1]; }

// The map, for the codegen test to assert against. Exposed rather than copied into the test, so the
// property being checked cannot drift away from the map it is about.
const uint8_t* xtRegMap(uint8_t& count) { count = kRegCount; return kXtReg; }

// A scratch register that is ALSO a vreg silently corrupts values — see the RISC-V backend, where
// kScratchFn aliased vreg R12 and every call returned a stale value. Checked here so the map can
// never grow over a scratch.
constexpr bool xtScratchOutsideMap() {
    // a12/a13 are call() scratch; a14/a15 carry the retw.n return linkage of our own `entry` frame.
    // Both classes are fatal as vregs, and the second one only faults once a script makes a CALL —
    // which is why it survived every effect and killed every layout.
    constexpr uint8_t scratch[] = {12, 13, 14, 15};
    for (uint8_t r : kXtReg) for (uint8_t s : scratch) if (r == s) return false;
    return true;
}
static_assert(xtScratchOutsideMap(), "a scratch or window register is also a vreg — calls corrupt it");


void XtensaAssembler::emit(const uint8_t* p, size_t n) {
    // !buf_ covers a failed allocation: the compile then fails cleanly at overflowed() instead
    // of writing through a null pointer.
    if (!buf_ || len_ + n > kCap) { overflow_ = true; return; }
    std::memcpy(buf_ + len_, p, n); len_ += n;
}
void XtensaAssembler::emit2(uint16_t w) {
    const uint8_t b[2] = {uint8_t(w), uint8_t(w >> 8)}; emit(b, 2);
}
void XtensaAssembler::emit3(uint32_t w) {
    const uint8_t b[3] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16)}; emit(b, 3);
}

// entry a1, N — a 48-byte frame leaves room for the call8 window rotation (a routine with no
// call would be fine with 32, but 48 is harmless and lets any program call a built-in). call() uses
// bytes 16..39 of it, so the register allocator's spill slots start at 48 and the frame simply grows
// to hold them: on this ISA the whole-routine frame already exists, so spilling costs a bigger
// immediate on ONE instruction and nothing else. a1 is the frame pointer, and the windowed ABI
// preserves it across callx8 — which is why a slot read after a host call still finds its value, and
// why this addressing carries over unchanged when script functions start nesting frames.
// One word inside call()'s own save area for the return value: offsets 16..28 hold the saved
// vregs, 32 is free, and a register cannot hold a result across a window rotation.
static constexpr uint8_t  kResultSlot = 8;    // byte offset 32
static constexpr uint16_t kFrameBase  = 48;   // first byte past the bytes call() reserves
static constexpr uint16_t kSlotStride = 4;

// ENTRY is a BRI12-format instruction: op0=6, n=3, s=the base register, and the 12-bit immediate at
// bits 12..23 counts EIGHT-byte units. `entry a1, 48` is therefore 0x006136.
void XtensaAssembler::prologue(uint8_t slots) {
    if (slots > kMaxSpillSlots) { overflow_ = true; return; }
    // Rounded up to 8 because the immediate counts 8-byte units; the ABI additionally wants the
    // frame 16-byte aligned, and 48 + a multiple of 16 keeps that.
    const uint32_t bytes = (kFrameBase + uint32_t(slots) * kSlotStride + 15u) & ~15u;
    emit3(0x000136u | ((bytes / 8u) << 12));
}
void XtensaAssembler::epilogue() { emit2(0xf01du); }     // retw.n

// s32i/l32i aR, a1, #off — the offset field counts 4-byte words, so a slot index maps straight onto
// it. No teardown counterpart: `entry`'s frame is released by retw.n, so unlike the RISC-V and arm64
// backends there is nothing for an epilogue to undo.
void XtensaAssembler::spillStore(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint8_t off4 = static_cast<uint8_t>((kFrameBase + slot * kSlotStride) / 4);
    const uint8_t b[3] = {uint8_t((ar(r) << 4) | 0x2), 0x61, off4};
    emit(b, 3);
}
void XtensaAssembler::spillLoad(Reg r, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint8_t off4 = static_cast<uint8_t>((kFrameBase + slot * kSlotStride) / 4);
    const uint8_t b[3] = {uint8_t((ar(r) << 4) | 0x2), 0x21, off4};
    emit(b, 3);
}

Label XtensaAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit
    Label l = labelCount_++; labelPos_[l] = -1; return l;
}
void XtensaAssembler::bind(Label l) { if (l < kMaxLabels) labelPos_[l] = static_cast<int32_t>(len_); }

// Record a pending branch fixup, guarding the fixed table (overflow_ rather than an OOB write).
void XtensaAssembler::addFixup(size_t at, Label label) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label};
}

// movi aD, #imm. The narrow byte form carries only 0..255, so a wider constant (a uint16 like
// 65535) is built as hi8<<8 | lo8: movi aD,hi8 ; slli aD,aD,8 ; movi a13,lo8 ; add.n aD,aD,a13.
// a13 is the assembler's reserved scratch (also kZero in branchIfZero); it holds no live vreg.
// Single movi for the common 0..255 case. Without this, Const values >255 truncate to 8 bits.
// The ADDRESS of a frame slot, for a host call that reads its arguments from the frame. The slots
// already hold the arguments; the call passes where they start rather than the values, which is what
// makes the number of arguments a memory question instead of a register one.
void XtensaAssembler::slotAddr(Reg d, uint8_t slot) {
    if (slot >= kMaxSpillSlots) { overflow_ = true; return; }
    const uint32_t off = kFrameBase + uint32_t(slot) * kSlotStride;
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), uint8_t(0xc0 | 1), uint8_t(off)};
    emit(b, 3);                                            // addi aD, a1, #off
}

void XtensaAssembler::movImm(Reg d, int32_t imm) {
    const uint8_t dr = ar(d);
    // The wide `movi` field is 12-bit SIGNED (-2048..2047), which is the only encoding here that can
    // hold a negative constant. The compiler emits Const(-1) to express subtraction — `a - b` is
    // `a + (b * -1)` — and building that through the zero-extended byte path below would materialise
    // 65535, making every subtraction correct only modulo 256: invisible in a stored colour byte,
    // silently fatal in a bounds-guarded index (the light is dropped) or a host-call argument.
    // A negative below the 12-bit field's reach has no encoding here, and falling through to the
    // unsigned path below would materialise a different number in silence — the failure mode that
    // cost this backend a long debugging session. Fail the compile instead.
    if (imm < -2048) { overflow_ = true; return; }
    if (imm < 0) {
        const uint32_t f = static_cast<uint32_t>(imm) & 0xfff;
        const uint8_t b[3] = {uint8_t((dr << 4) | 0x2),
                              uint8_t(0xa0 | ((f >> 8) & 0xf)),
                              uint8_t(f & 0xff)};
        emit(b, 3);                                                      // movi aD, #imm12
        return;
    }
    const uint32_t v = static_cast<uint32_t>(imm) & 0xffff;
    if (v <= 0xff) {
        const uint8_t b[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(v)};
        emit(b, 3);
        return;
    }
    static constexpr uint8_t kTmp = 13;                                  // a13 (reserved scratch)
    const uint8_t hi[3] = {uint8_t((dr << 4) | 0x2), 0xa0, uint8_t(v >> 8)};
    emit(hi, 3);                                                         // movi aD, hi8
    const uint8_t sl[3] = {0x80, uint8_t((dr << 4) | dr), 0x11};
    emit(sl, 3);                                                         // slli aD, aD, 8
    const uint8_t lo[3] = {uint8_t((kTmp << 4) | 0x2), 0xa0, uint8_t(v & 0xff)};
    emit(lo, 3);                                                         // movi a13, lo8
    emit2(uint16_t((dr << 12) | (dr << 8) | (kTmp << 4) | 0xa));         // add.n aD, aD, a13
}
// add.n aD, aA, aB : word (d<<12)|(a<<8)|(b<<4)|0xa
void XtensaAssembler::addReg(Reg d, Reg a, Reg b) {
    emit2(uint16_t((ar(d) << 12) | (ar(a) << 8) | (ar(b) << 4) | 0xa));
}
// mov.n aD, aA : bytes [ (d<<4)|0xd, a ]
void XtensaAssembler::movReg(Reg d, Reg a) {
    const uint8_t b[2] = {uint8_t((ar(d) << 4) | 0xd), ar(a)};
    emit(b, 2);
}
// addi.n aD, aA, #imm (1..15) : word (d<<12)|(a<<8)|(imm<<4)|0xb
void XtensaAssembler::addImm(Reg d, Reg a, int32_t imm) {
    emit2(uint16_t((ar(d) << 12) | (ar(a) << 8) | ((imm & 0xf) << 4) | 0xb));
}
// mull aD, aA, aB : 24-bit 0x820000 | (d<<12) | (a<<8) | (b<<4)
void XtensaAssembler::mulReg(Reg d, Reg a, Reg b) {
    emit3(0x820000u | (uint32_t(ar(d)) << 12) | (uint32_t(ar(a)) << 8) | (uint32_t(ar(b)) << 4));
}
// Xtensa s8i only offsets a base by an immediate (no register-offset store), so compute the
// address into a dedicated scratch a12 — OUTSIDE the R0..R9 → a2..a11 vreg map, so it never
// clobbers a live virtual register — then s8i aVal, a12, 0.
// add.n a12, aBase, aOff : (12<<12)|(base<<8)|(off<<4)|0xa  ;  s8i aVal, a12, 0 : [(val<<4)|2, 0x40|12, 0]
static constexpr uint8_t kAddrScratch = 12;   // a12
void XtensaAssembler::store8(Reg base, Reg off, Reg val) {
    emit2(uint16_t((kAddrScratch << 12) | (ar(base) << 8) | (ar(off) << 4) | 0xa));   // add.n a12, base, off
    const uint8_t b[3] = {uint8_t((ar(val) << 4) | 0x2), uint8_t(0x40 | kAddrScratch), 0x00};
    emit(b, 3);                                             // s8i aVal, a12, 0
}
// l8ui aDst, aBase, #imm (0..255) : bytes [ (dst<<4)|2, base, imm ] — zero-extended byte load.
void XtensaAssembler::load8(Reg d, Reg base, int32_t imm) {
    const uint8_t b[3] = {uint8_t((ar(d) << 4) | 0x2), ar(base), uint8_t(imm & 0xff)};
    emit(b, 3);
}

// branchIfZero(a, l): synthesised as `movi a13,0; bgeu a13, a, l`. Unsigned 0 >= a is true
// IFF a == 0, so this branches exactly when a is zero — using only the verified bgeu 8-bit
// branch (no separate beqz form / offset width). a13 is a scratch outside the vreg map.
void XtensaAssembler::branchIfZero(Reg a, Label l) {
    static constexpr uint8_t kZero = 13;   // a13
    const uint8_t mv[3] = {uint8_t((kZero << 4) | 0x2), 0xa0, 0x00};   // movi a13, 0
    emit(mv, 3);
    // Same relaxed form as the other branches: bltu a13, a, +3 (the inverse of bgeu) over a `j`.
    const uint8_t br[3] = {uint8_t((ar(a) << 4) | 0x7), uint8_t((0x3 << 4) | kZero), 0x02};
    emit(br, 3);
    addFixup(len_, l);
    const uint8_t j[3] = {0x06, 0x00, 0x00};
    emit(j, 3);
}
// A conditional branch to `l`, emitted as the INVERTED condition over an unconditional jump:
//
//     b<inv> aA, aB, +3      ; skip the jump when the branch is NOT taken
//     j      l               ; 18-bit displacement — reaches anywhere in a script
//
// Xtensa's conditional branches carry a single SIGNED BYTE of displacement (±127), which a loop body
// outgrows easily once spill traffic is in it — grid.mlv needed 177. Truncating that silently
// retargets the branch into the middle of the program, so the choice is relax or refuse. This is the
// textbook relaxation every compiler does (GCC and LLVM emit the short form and rewrite the ones that
// do not fit); the fixed six-byte form skips the iterate-to-convergence step, which is worth a few
// bytes per branch on a cold path in exchange for not having to reason about shifting offsets.
// The 3-byte `j` keeps its own fixup, and the inverted branch's +3 is already correct as emitted.
void XtensaAssembler::branchRelaxed(uint8_t condNibble, Reg a, Reg b, Label l) {
    // The inverted condition, skipping the 3-byte `j` that follows. Xtensa branch displacements are
    // relative to PC+4 (the same rule patchBranches uses), so clearing a 3-byte instruction is +2.
    // bne(0x9) <-> beq(0x1); bgeu(0xb) <-> bltu(0x3).
    const uint8_t inv = condNibble == 0x9 ? 0x1 : condNibble == 0x1 ? 0x9
                      : condNibble == 0xb ? 0x3 : 0xb;
    const uint8_t br[3] = {uint8_t((ar(b) << 4) | 0x7), uint8_t((inv << 4) | ar(a)), 0x02};
    emit(br, 3);
    addFixup(len_, l);
    const uint8_t j[3] = {0x06, 0x00, 0x00};      // j — the 18-bit offset is patched in
    emit(j, 3);
}
// bgeu aA, aB, l  (skip if a >= b, unsigned)
void XtensaAssembler::branchGeU(Reg a, Reg b, Label l) { branchRelaxed(0xb, a, b, l); }
// bne aA, aB, l
void XtensaAssembler::branchNe(Reg a, Reg b, Label l) { branchRelaxed(0x9, a, b, l); }

// Windowed call to a host built-in: d = fn(a). CALL8 rotates the window by 8, so the arg goes
// in a10 and the result returns in a10. The caller's a2..a7 are preserved by the window for
// free; only a8..a11 rotate out — and those hold vreg values that may be live across the call
// (a script with two random16 calls keeps the first result live across the second). So this
// SAVES a8/a9/a11 to the entry frame around the call (a10 carries the arg, then the result),
// mirroring the host backend's full-register-save. Cold path (once per call). The 48-byte
// frame from prologue() has room at offsets 16/20/28. The 32-bit fn address is built in a8
// byte-by-byte (movi/slli/add) — no l32r literal pool.
void XtensaAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // Save the rotate-out scratch a8, a9, a11 (a10 will carry arg→result).
    auto s32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x61,off4}; emit(enc,3); };
    auto l32i = [&](uint8_t r, uint8_t off4){ const uint8_t enc[3]={uint8_t((r<<4)|2),0x21,off4}; emit(enc,3); };
    s32i(8, 4); s32i(9, 5); s32i(11, 7);                  // [a1+16]=a8, [a1+20]=a9, [a1+28]=a11
    // a14/a15 are deliberately NOT saved here, because they are no longer vregs (kXtReg): they carry
    // this routine's own return linkage for retw.n, so writing saved copies back into them after the
    // call is what broke every scripted layout. See the Reg enum for the failure that produced.
    s32i(10, 6);                                          // [a1+24]=a10 — a vreg (R8) call8 rotates out

    // The three args into a10/a11/a12 — call8 shifts the window by 8, so the callee reads them as
    // its a2/a3/a4. Moved HIGH-first (a12, then a11, then a10) so an earlier write cannot clobber a
    // source a later one still needs.
    //
    // argA goes through a13 first. a11 is vreg R9, so argA can BE a11 — and writing argB into a11
    // would then destroy argA before the a10 move reads it. High-first ordering alone does not cover
    // that case; a13 is outside the vreg map, so staging there does.
    emit2(uint16_t((uint32_t(ar(a)) << 8) | (13 << 4) | 0xd));   // mov a13, argA  (a13 is scratch)
    emit2(uint16_t((uint32_t(ar(c)) << 8) | (12 << 4) | 0xd));   // mov a12, argC
    emit2(uint16_t((uint32_t(ar(b)) << 8) | (11 << 4) | 0xd));   // mov a11, argB
    emit2(uint16_t((13u << 8) | (10 << 4) | 0xd));               // mov a10, a13

    uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    auto moviA8 = [&](uint8_t v){ const uint8_t enc[3]={0x82,0xa0,v}; emit(enc,3); };
    auto moviA9 = [&](uint8_t v){ const uint8_t enc[3]={0x92,0xa0,v}; emit(enc,3); };
    auto slliA8 = [&]{ const uint8_t enc[3]={0x80,0x88,0x11}; emit(enc,3); };
    auto addA8A9 = [&]{ emit2(0x889au); };
    moviA8(uint8_t(addr >> 24));
    slliA8(); moviA9(uint8_t(addr >> 16)); addA8A9();
    slliA8(); moviA9(uint8_t(addr >> 8));  addA8A9();
    slliA8(); moviA9(uint8_t(addr));       addA8A9();
    emit3(0x0000e0u | (8u << 8));                          // callx8 a8  → result in a10
    // Park the result in the FRAME, not in a register.
    //
    // It used to be stashed in a12 — but call8 rotates the window by eight, so the callee's a4 IS
    // our a12: the callee overwrites the stash with its own second argument while it runs, and the
    // "result" moved to the destination afterwards is whatever the callee happened to leave there.
    // a12/a13 are safe as scratch only BEFORE the call, never across it. Measured: A0 = 0x100 in the
    // crash dump — nLights, a script value that reached the return-address register this way.
    // Slot kResultSlot sits in the bytes call() already owns, so this costs no extra frame.
    s32i(10, kResultSlot);                                 // [a1+kResultSlot*4] = result
    l32i(8, 4); l32i(9, 5); l32i(10, 6); l32i(11, 7);
    l32i(ar(d), kResultSlot);                              // dst = the parked result
}

void XtensaAssembler::patchBranches() {
    // Nothing was emitted if the buffer never allocated, so there is nothing to patch —
    // stated rather than left to the reader to derive from fixupCount_ being 0.
    if (!buf_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] < 0) continue;                                  // unbound label — leave as-is (overflow_ already failed the compile)
        // Every fixup now points at a `j`, whose displacement is relative to the byte AFTER the
        // instruction and occupies bits 6..23 — eighteen signed bits, so it reaches any script the
        // code buffer can hold. Still range-checked: refusing beats silently retargeting a jump.
        const int32_t off = labelPos_[f.label] - (static_cast<int32_t>(f.at) + 4);
        if (off < -131072 || off > 131071) { overflow_ = true; return; }
        const uint32_t enc = 0x06u | ((static_cast<uint32_t>(off) & 0x3ffffu) << 6);
        buf_[f.at + 0] = static_cast<uint8_t>(enc);
        buf_[f.at + 1] = static_cast<uint8_t>(enc >> 8);
        buf_[f.at + 2] = static_cast<uint8_t>(enc >> 16);
    }
}

}  // namespace mm::moonlive

#endif  // __XTENSA__
