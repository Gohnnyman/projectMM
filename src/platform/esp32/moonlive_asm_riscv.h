#pragma once

#include "platform/platform.h"   // alloc/free — the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap — one cap for the staging buffer and every backend

#include <cstdint>
#include <cstddef>

// MoonLive RISC-V assembler (ESP32-P4 backend) — the device counterpart of the host/Xtensa
// MacroAssemblers, same named-instruction interface. RV32: fixed 4-byte instructions, a
// standard (non-windowed) call ABI — simpler than Xtensa. Branch displacements are back-patched
// against bound labels.
//
// Register map: R0..R3 → a0..a3 (the host args buf/nLights/cpl/t); R4.. → caller-saved temps
// (t0..t6, a4..a7). All in the caller-saved set, so call() saves the live pool explicitly.

namespace mm::moonlive {

// Twelve was the count every backend started with; RISC-V has room for more, and a nested loop
// needs it — two loop levels hold four values live, and a three-argument call needs three temps on
// top. Fourteen is what the CALLER-SAVED registers alone provide, and that is the whole map.
//
// It briefly reached eighteen by also mapping x18..x21 (s2..s5) on the reasoning that "the emitted
// routine is a leaf that saves what it uses". It does not: prologue() is empty, so the routine has
// no entry/exit save at all and would have returned to its caller with four callee-saved registers
// clobbered. Giving the routine a prologue would cost every script a save/restore it almost never
// needs; dropping the four costs nothing, since fourteen still exceeds Xtensa's twelve and no
// script measured here uses more than eleven.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11,
                     R12, R13, kRegCount };
using Label = uint8_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class RiscvAssembler {
public:
    // Owns buf_ (see below). Freed here, copying deleted — an emitter that was copied
    // would double-free the buffer it emits into.
    ~RiscvAssembler() { platform::free(buf_); }
    RiscvAssembler() = default;
    RiscvAssembler(const RiscvAssembler&) = delete;
    RiscvAssembler& operator=(const RiscvAssembler&) = delete;

    void finalize() { patchBranches(); }
    const uint8_t* bytes() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

    void prologue() {}                   // RV needs no fixed prologue (sp managed in call())
    Label newLabel();
    void  bind(Label l);

    void movImm(Reg d, int32_t imm);     // li rd, imm  (addi rd, x0, imm)
    void movReg(Reg d, Reg a);           // mv rd, ra   (addi rd, ra, 0)
    void addImm(Reg d, Reg a, int32_t imm);   // addi rd, ra, imm
    void addReg(Reg d, Reg a, Reg b);    // add rd, ra, rb
    void mulReg(Reg d, Reg a, Reg b);    // mul rd, ra, rb
    void store8(Reg base, Reg off, Reg val);  // add tmp,base,off ; sb val,0(tmp)
    void load8(Reg d, Reg base, int32_t imm); // lbu rDst, imm(rBase) — a control read
    void branchIfZero(Reg a, Label l);   // beqz a, l  (bge x0, a... use bgeu against x0)
    void branchGeU(Reg a, Reg b, Label l);    // bgeu a, b, l
    void branchNe(Reg a, Reg b, Label l);     // bne a, b, l
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);  // standard call to a host built-in
    void epilogue() { ret(); }
    void ret();

private:
    // The emitted-code buffer, sized by the engine's shared cap (kCodeCap).
    static constexpr size_t kCap = kCodeCap;
    static constexpr uint8_t kMaxLabels = 16;
    static constexpr uint8_t kMaxFixups = 32;

    void emit32(uint32_t w);
    void addFixup(size_t at, Label label);   // enqueue a branch fixup (bounds-checked)

    // HEAP, not a member array: the assembler is a stack local in lowerToBytes, so a kCap-sized
    // member put 2 KB on the compile chain's stack — on top of the staging buffer and the parser
    // frames. On a classic ESP32 that overflowed the task and faulted inside _xt_context_save
    // (the plan named this: "buf_[kCap] inside the assembler, itself a stack local"). The buffer is
    // scratch that ends in a memcpy to the caller's output, so nothing outlives the object.
    uint8_t* buf_ = static_cast<uint8_t*>(platform::alloc(kCap));
    size_t   len_ = 0;
    bool     overflow_ = false;

    int32_t  labelPos_[kMaxLabels];
    uint8_t  labelCount_ = 0;
    struct Fixup { size_t at; Label label; };   // all B-type, 4 bytes at `at`
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();
};

}  // namespace mm::moonlive
