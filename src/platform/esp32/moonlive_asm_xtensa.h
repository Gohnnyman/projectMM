#pragma once

#include "platform/platform.h"   // alloc/free — the branch tables are sized to the script

#include <cstdint>
#include <cstddef>

// MoonLive Xtensa assembler (ESP32 classic/S3 backend) — the device counterpart of the host
// MacroAssembler. Same named-instruction interface (the IR lowering is written once against
// it); the encodings and the windowed ABI are Xtensa-specific. Branch displacements are
// back-patched against bound labels, so no offset is hand-computed (the crash class the
// verbatim-blob spike avoided by never composing stays avoided by back-patching).
//
// Windowed ABI: the emitted routine opens with `entry` and returns with `retw.n`. The host
// args arrive in a2..a5 (buf, nLights, cpl, t); R0..R3 map to those, R4..R9 to a6..a11.

namespace mm::moonlive {

enum Reg : uint8_t { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, kRegCount };
// uint16_t, not uint8_t: every bounds-guarded store burns a label, so a 256-label
// ceiling is ~256 statements — a limit a real script reaches.
using Label = uint16_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class XtensaAssembler {
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
    XtensaAssembler(uint8_t* out, size_t cap, uint16_t branches) : buf_(out), cap_(cap) {
        labelCap_ = branches ? branches : 8;
        fixupCap_ = labelCap_;
        labelPos_ = static_cast<int32_t*>(platform::alloc(size_t(labelCap_) * sizeof(int32_t)));
        fixups_   = static_cast<Fixup*>(platform::alloc(size_t(fixupCap_) * sizeof(Fixup)));
        if (!labelPos_ || !fixups_) overflow_ = true;   // degrade: the compile fails cleanly
    }
    ~XtensaAssembler() { platform::free(labelPos_); platform::free(fixups_); }
    XtensaAssembler(const XtensaAssembler&) = delete;
    XtensaAssembler& operator=(const XtensaAssembler&) = delete;
    void finalize() { patchBranches(); }
    const uint8_t* bytes() const { return buf_; }
    size_t size() const { return len_; }
    bool overflowed() const { return overflow_; }

    void prologue();                     // entry a1, 48  (must be the first instruction)
    Label newLabel();
    void  bind(Label l);

    void movImm(Reg d, int32_t imm);     // movi aD, #imm (0..255)
    void movReg(Reg d, Reg a);           // mov.n aD, aA
    void addImm(Reg d, Reg a, int32_t imm);   // addi.n aD, aA, #imm (1..15)
    void addReg(Reg d, Reg a, Reg b);    // add.n aD, aA, aB
    void mulReg(Reg d, Reg a, Reg b);    // mull aD, aA, aB
    void store8(Reg base, Reg off, Reg val);  // s8i via computed address (add then s8i,0)
    void load8(Reg d, Reg base, int32_t imm); // l8ui aDst, aBase, #imm — a control read
    void branchIfZero(Reg a, Label l);   // beqz aA, l  (nLights==0 guard)
    void branchGeU(Reg a, Reg b, Label l);    // bgeu aA, aB, l  (Bounds: skip if a>=b)
    void branchNe(Reg a, Reg b, Label l);     // bne aA, aB, l   (loop test)
    void call(Reg d, Reg a, Reg b, Reg c, const void* fn);  // windowed call8 to a host built-in
    void epilogue();                     // retw.n

private:

    void emit(const uint8_t* p, size_t n);
    void emit2(uint16_t w);              // narrow (16-bit) instruction
    void emit3(uint32_t w);              // wide (24-bit) instruction
    void addFixup(size_t at, Label label);   // enqueue a branch fixup (bounds-checked)

    uint8_t* buf_ = nullptr;   // the caller's buffer — see the constructor
    size_t   cap_ = 0;
    size_t   len_ = 0;
    bool     overflow_ = false;

    int32_t* labelPos_ = nullptr;   // sized to the script; see the constructor
    uint16_t labelCap_ = 0;
    uint16_t labelCount_ = 0;
    struct Fixup { size_t at; Label label; };   // all our branches use the 8-bit offset at byte+2
    Fixup*   fixups_ = nullptr;
    uint16_t fixupCap_ = 0;
    uint16_t fixupCount_ = 0;

    void patchBranches();
};

}  // namespace mm::moonlive
