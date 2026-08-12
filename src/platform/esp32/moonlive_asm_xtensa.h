#pragma once

#include "platform/platform.h"   // alloc/free — the emit buffer is heap, not stack

#include "core/moonlive/MoonLiveIr.h"   // kCodeCap — one cap for the staging buffer and every backend

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
using Label = uint8_t;
enum class Cond : uint8_t { Lo /* unsigned < */, Hs /* unsigned >= */ };

class XtensaAssembler {
public:
    // Owns buf_ (see below). Freed here, copying deleted — an emitter that was copied
    // would double-free the buffer it emits into.
    ~XtensaAssembler() { platform::free(buf_); }
    XtensaAssembler() = default;
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
    // The emitted-code buffer, sized by the engine's shared cap (kCodeCap).
    static constexpr size_t kCap = kCodeCap;
    static constexpr uint8_t kMaxLabels = 16;
    static constexpr uint8_t kMaxFixups = 32;

    void emit(const uint8_t* p, size_t n);
    void emit2(uint16_t w);              // narrow (16-bit) instruction
    void emit3(uint32_t w);              // wide (24-bit) instruction
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
    struct Fixup { size_t at; Label label; };   // all our branches use the 8-bit offset at byte+2
    Fixup    fixups_[kMaxFixups];
    uint8_t  fixupCount_ = 0;

    void patchBranches();
};

}  // namespace mm::moonlive
