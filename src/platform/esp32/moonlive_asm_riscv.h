#pragma once

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
// top. x16/x17 (a6/a7) and x18..x23 (s2..s7) are free here: the emitted routine is a leaf that
// saves what it uses, so a callee-saved register is as usable as a caller-saved one.
enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11,
                     R12, R13, R14, R15, R16, R17, kRegCount };
using Label = uint8_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class RiscvAssembler {
public:
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
    static constexpr size_t kCap = 768;
    static constexpr uint8_t kMaxLabels = 16;
    static constexpr uint8_t kMaxFixups = 32;

    void emit32(uint32_t w);
    void addFixup(size_t at, Label label);   // enqueue a branch fixup (bounds-checked)

    uint8_t  buf_[kCap] = {};
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
