#include "moonlive_asm_riscv.h"

#include <cstring>

#if defined(__riscv)   // the RISC-V assembler is only built for RISC-V targets (ESP32-P4)

// MoonLive RISC-V assembler — RV32 named instructions, encodings verified against
// riscv32-esp-elf-as (see the plan / commit). Fixed 4-byte little-endian instructions; the
// standard call ABI (args a0.., result a0, ra = return). Branch offsets are back-patched.

namespace mm::moonlive {

// R0..R4 → a0..a4 (10..14, the host args: buf, nLights, cpl, t, ctrls — a4=kArg4 the controls
// arena pointer). R5..R11 → t0,t1,t2,t3,t4,t5,a5 (caller-saved temps). t6(31) is the internal
// scratch (store8 address, call address build + result stash), not a vreg.
static constexpr uint8_t kRvReg[kRegCount] = {10, 11, 12, 13, 14, 5, 6, 7, 28, 29, 30, 15,
                                              16, 17};
static uint8_t xr(Reg r) { return kRvReg[r]; }
// t6 is the ONLY caller-saved register outside kRvReg, so it is the only safe scratch: every other
// free register is callee-saved (s0/s1, s6..s11) and would have to be preserved. Both uses below are
// transient — store8 consumes it within two instructions, and call() finishes with it before any
// store8 can run — so one register serves both.
static constexpr uint8_t kScratchAddr = 31;   // t6 — store8 address temp
static constexpr uint8_t kScratchFn   = 31;   // t6 — call address build / result stash

// A scratch register that is ALSO a vreg silently corrupts values. kScratchFn was x16/a6, which is
// kRvReg[12] = vreg R12: call() stashed its result in a6, then the restore loop reloaded x16 from
// the frame and destroyed it, so every call returned R12's stale value. Latent only because it needs
// vregsUsed > 12. Checked here so the map can never grow over a scratch again.
constexpr bool rvScratchOutsideMap() {
    constexpr uint8_t scratch[] = {kScratchAddr, kScratchFn};
    for (uint8_t r : kRvReg) for (uint8_t s : scratch) if (r == s) return false;
    return true;
}
static_assert(rvScratchOutsideMap(), "a scratch register is also a vreg — calls will corrupt it");

void RiscvAssembler::emit32(uint32_t w) {
    if (!buf_ || len_ + 4 > kCap) { overflow_ = true; return; }
    buf_[len_++] = uint8_t(w); buf_[len_++] = uint8_t(w >> 8);
    buf_[len_++] = uint8_t(w >> 16); buf_[len_++] = uint8_t(w >> 24);
}

Label RiscvAssembler::newLabel() {
    if (labelCount_ == 0) for (auto& p : labelPos_) p = -1;
    if (labelCount_ >= kMaxLabels) { overflow_ = true; return 0; }   // same overflow signal as emit32
    Label l = labelCount_++; labelPos_[l] = -1; return l;
}
void RiscvAssembler::bind(Label l) { if (l < kMaxLabels) labelPos_[l] = static_cast<int32_t>(len_); }

// Record a pending branch fixup, guarding the fixed table (overflow_ rather than an OOB write).
void RiscvAssembler::addFixup(size_t at, Label label) {
    if (fixupCount_ >= kMaxFixups) { overflow_ = true; return; }
    fixups_[fixupCount_++] = {at, label};
}

// --- I/R/S-type encoders (rd/rs1/rs2 are real x-register numbers) ---
static uint32_t encAddi(uint8_t rd, uint8_t rs1, int32_t imm) {
    return ((uint32_t(imm) & 0xfff) << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x13;
}
static uint32_t encAdd(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return (rs2 << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x33;
}
static uint32_t encMul(uint8_t rd, uint8_t rs1, uint8_t rs2) {
    return (1u << 25) | (rs2 << 20) | (rs1 << 15) | (0 << 12) | (rd << 7) | 0x33;
}
static uint32_t encSb(uint8_t rs2, uint8_t rs1, int32_t imm) {   // sb rs2, imm(rs1)
    return (((uint32_t(imm) >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (0 << 12) |
           ((uint32_t(imm) & 0x1f) << 7) | 0x23;
}
static uint32_t encSw(uint8_t rs2, uint8_t rs1, int32_t imm) {   // sw rs2, imm(rs1)
    return (((uint32_t(imm) >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (2 << 12) |
           ((uint32_t(imm) & 0x1f) << 7) | 0x23;
}
static uint32_t encLw(uint8_t rd, uint8_t rs1, int32_t imm) {    // lw rd, imm(rs1)
    return ((uint32_t(imm) & 0xfff) << 20) | (rs1 << 15) | (2 << 12) | (rd << 7) | 0x03;
}
static uint32_t encLui(uint8_t rd, uint32_t imm20) {
    return (imm20 << 12) | (rd << 7) | 0x37;
}
static uint32_t encBranch(uint8_t rs1, uint8_t rs2, uint8_t f3, int32_t off) {  // B-type
    uint32_t o = uint32_t(off) & 0x1fff;
    return (((o >> 12) & 1) << 31) | (((o >> 5) & 0x3f) << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (((o >> 1) & 0xf) << 8) | (((o >> 11) & 1) << 7) | 0x63;
}

void RiscvAssembler::movImm(Reg d, int32_t imm) {
    // addi sign-extends a 12-bit immediate, so it alone covers only -2048..2047. For wider
    // constants (a uint16 like 65535) materialise the full value with lui (high 20 bits) + addi
    // (low 12), the hi/lo split — without this, larger Const values truncate. Single addi when
    // the value fits, to keep the common small-constant case one instruction.
    if (imm >= -2048 && imm <= 2047) {
        emit32(encAddi(xr(d), 0, imm));                    // li = addi rd, x0, imm
        return;
    }
    uint32_t v  = static_cast<uint32_t>(imm);
    uint32_t hi = (v + 0x800) >> 12;                       // round for the sign-extended addi
    int32_t  lo = static_cast<int32_t>(v) - static_cast<int32_t>(hi << 12);
    emit32(encLui(xr(d), hi & 0xfffff));                   // lui rd, hi
    emit32(encAddi(xr(d), xr(d), lo));                     // addi rd, rd, lo
}
void RiscvAssembler::movReg(Reg d, Reg a)       { emit32(encAddi(xr(d), xr(a), 0)); }   // mv = addi rd,ra,0
void RiscvAssembler::addImm(Reg d, Reg a, int32_t imm) { emit32(encAddi(xr(d), xr(a), imm)); }
void RiscvAssembler::addReg(Reg d, Reg a, Reg b) { emit32(encAdd(xr(d), xr(a), xr(b))); }
void RiscvAssembler::mulReg(Reg d, Reg a, Reg b) { emit32(encMul(xr(d), xr(a), xr(b))); }

void RiscvAssembler::store8(Reg base, Reg off, Reg val) {
    emit32(encAdd(kScratchAddr, xr(base), xr(off)));   // t6 = base + off
    emit32(encSb(xr(val), kScratchAddr, 0));           // sb val, 0(t6)
}
void RiscvAssembler::load8(Reg d, Reg base, int32_t imm) {   // lbu rDst, imm(rBase) — control read
    emit32(((uint32_t(imm) & 0xfff) << 20) | (xr(base) << 15) | (4 << 12) | (xr(d) << 7) | 0x03);
}
void RiscvAssembler::branchIfZero(Reg a, Label l) {    // a == 0  ⇔  bgeu x0, a (unsigned 0 >= a)
    addFixup(len_, l);
    emit32(encBranch(0, xr(a), 7, 0));                 // bgeu x0, a, l  (patched)
}
void RiscvAssembler::branchGeU(Reg a, Reg b, Label l) {
    addFixup(len_, l);
    emit32(encBranch(xr(a), xr(b), 7, 0));             // bgeu a, b, l
}
void RiscvAssembler::branchNe(Reg a, Reg b, Label l) {
    addFixup(len_, l);
    emit32(encBranch(xr(a), xr(b), 1, 0));             // bne a, b, l
}

// Standard call to a host built-in: d = fn(a). All vreg temps are caller-saved, so a value
// live across the call must be preserved — save the whole pool + ra + the host args around the
// call (mirrors the host backend). The fn address is built with lui+addi (the hi/lo split, +1
// to the upper when the low 12 bits' sign bit is set). 64-byte frame, 16-byte aligned.
void RiscvAssembler::call(Reg d, Reg a, Reg b, Reg c, const void* fn) {
    // 80-byte frame, 16-byte aligned: 14 saved registers (56 bytes), three argument staging slots
    // (56/60/64), and ra at 76. Every register the map hands out is saved here, or a value live
    // across a call is destroyed and the caller silently computes with rubbish — which is why the
    // list mirrors kRvReg exactly.
    emit32(encAddi(2, 2, -80));                        // addi sp, sp, -80
    emit32(encSw(1, 2, 76));                            // sw ra, 76(sp)
    static const uint8_t saved[] = {10, 11, 12, 13, 14, 5, 6, 7, 28, 29, 30, 15, 16, 17};
    int off = 0;
    for (uint8_t r : saved) { emit32(encSw(r, 2, off)); off += 4; }
    // The three args into a0/a1/a2 (the standard ABI registers a host built-in reads). Staged
    // through the frame first: a source may itself BE a0/a1/a2, so moving them in place could
    // overwrite a source a later move still needs. Slots 56/60/64 sit above the saved set (14
    // registers, offsets 0..52) and below ra at 76.
    emit32(encSw(xr(a), 2, 56));
    emit32(encSw(xr(b), 2, 60));
    emit32(encSw(xr(c), 2, 64));
    emit32(encLw(10, 2, 56));                          // a0 = arg0
    emit32(encLw(11, 2, 60));                          // a1 = arg1
    emit32(encLw(12, 2, 64));                          // a2 = arg2
    // t6 = fn address via lui + addi (hi/lo split). t6 is caller-saved, so the callee may clobber
    // it — harmless: it is dead across the call, written before jalr and rewritten after.
    uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
    uint32_t hi = (addr + 0x800) >> 12;                // round for the sign-extended addi
    int32_t  lo = static_cast<int32_t>(addr) - static_cast<int32_t>(hi << 12);
    emit32(encLui(kScratchFn, hi & 0xfffff));          // lui t6, hi
    emit32(encAddi(kScratchFn, kScratchFn, lo));       // addi t6, t6, lo
    emit32((kScratchFn << 15) | (1 << 7) | 0x67);      // jalr ra, t6, 0
    // stash the result (a0) in t6 before restoring (a0 is restored to the old buf). t6 is outside
    // the saved set, so the restore loop below cannot destroy it.
    emit32(encAddi(kScratchFn, 10, 0));                // mv t6, a0
    // restore
    off = 0;
    for (uint8_t r : saved) { emit32(encLw(r, 2, off)); off += 4; }
    emit32(encLw(1, 2, 76));                           // lw ra, 108(sp)
    emit32(encAddi(2, 2, 80));                         // addi sp, sp, 112
    emit32(encAddi(xr(d), kScratchFn, 0));             // mv dst, t6  (the result)
}

void RiscvAssembler::ret() { emit32(0x00008067u); }    // ret = jalr x0, ra, 0

void RiscvAssembler::patchBranches() {
    // Nothing was emitted if the buffer never allocated, so there is nothing to patch —
    // stated rather than left to the reader to derive from fixupCount_ being 0.
    if (!buf_) return;
    for (uint8_t i = 0; i < fixupCount_; i++) {
        const Fixup& f = fixups_[i];
        if (labelPos_[f.label] < 0) continue;                  // unbound label — leave as-is (overflow_ already failed the compile)
        int32_t off = labelPos_[f.label] - static_cast<int32_t>(f.at);
        uint32_t w; std::memcpy(&w, buf_ + f.at, 4);
        // re-scatter the offset into the B-type immediate fields, keeping the rest.
        w &= ~((1u<<31) | (0x3fu<<25) | (0xfu<<8) | (1u<<7));
        uint32_t o = uint32_t(off) & 0x1fff;
        w |= (((o>>12)&1)<<31) | (((o>>5)&0x3f)<<25) | (((o>>1)&0xf)<<8) | (((o>>11)&1)<<7);
        std::memcpy(buf_ + f.at, &w, 4);
    }
}

}  // namespace mm::moonlive

#endif  // __riscv
