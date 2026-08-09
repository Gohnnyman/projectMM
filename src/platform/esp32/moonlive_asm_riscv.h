#pragma once

#include "platform/platform.h"   // alloc/free — the branch tables are sized to the script

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
// uint16_t, not uint8_t: every bounds-guarded store burns a label, so a 256-label
// ceiling is ~256 statements — a limit a real script reaches.
using Label = uint16_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class RiscvAssembler {
public:
    /// Emit into `out` (capacity `cap`). The buffer belongs to the caller and is already
    /// sized to the script, so there is no fixed code ceiling and no second copy: this was
    /// a `uint8_t buf_[768]` member, which capped every script at 768 bytes AND made the
    /// assembler a ~1.3 KB stack local on a 12 KB task.
    /// Emit into `out` (capacity `cap`), with branch tables sized for `branches` of them.
    ///
    /// The caller's buffer is already sized to the script, so there is no fixed code ceiling
    /// and no second copy: this was a `uint8_t buf_[768]` member, which capped every script
    /// at 768 bytes AND made the assembler a ~1.3 KB stack local on a 12 KB task.
    ///
    /// `branches` is an upper bound on labels and fixups. It comes from the IR op count
    /// rather than the code size: only a handful of ops emit a branch at all, so sizing from
    /// bytes over-allocated by orders of magnitude (a 6 KB script asked for ~1 MB).
    RiscvAssembler(uint8_t* out, size_t cap, uint16_t branches) : buf_(out), cap_(cap) {
        labelCap_ = branches ? branches : 8;
        fixupCap_ = labelCap_;
        labelPos_ = static_cast<int32_t*>(platform::alloc(size_t(labelCap_) * sizeof(int32_t)));
        fixups_   = static_cast<Fixup*>(platform::alloc(size_t(fixupCap_) * sizeof(Fixup)));
        if (!labelPos_ || !fixups_) overflow_ = true;   // degrade: the compile fails cleanly
    }
    ~RiscvAssembler() { platform::free(labelPos_); platform::free(fixups_); }
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

    void emit32(uint32_t w);
    void addFixup(size_t at, Label label);   // enqueue a branch fixup (bounds-checked)

    uint8_t* buf_ = nullptr;   // the caller's buffer — see the constructor
    size_t   cap_ = 0;
    size_t   len_ = 0;
    bool     overflow_ = false;

    int32_t* labelPos_ = nullptr;   // sized to the script; see the constructor
    uint16_t labelCap_ = 0;
    uint16_t labelCount_ = 0;
    struct Fixup { size_t at; Label label; };   // all B-type, 4 bytes at `at`
    Fixup*   fixups_ = nullptr;
    uint16_t fixupCap_ = 0;
    uint16_t fixupCount_ = 0;

    void patchBranches();
};

}  // namespace mm::moonlive
