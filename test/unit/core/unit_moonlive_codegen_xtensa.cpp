// @module MoonLive

// The Xtensa (ESP32 classic / S3) backend's emitted code, checked on the development machine.
// See moonlive_device_codegen.inc for why this exists.
//
// The backend is `#if defined(__XTENSA__)`, so on this host it normally compiles to nothing.
// Defining the macro and compiling the sources here runs the REAL emitter, and compileSource's
// `lower` seam points the shared front end at it — one function pointer rather than a second copy
// of the compiler. The firmware build never sees this file, so a device image still holds exactly
// one backend definition.

#include "doctest.h"
#include "moonlive_script_wrap.h"

// System and standard headers FIRST, at global scope. The backend below is wrapped in a namespace,
// and anything it includes for the first time would otherwise be declared INSIDE that namespace —
// which under GCC breaks both `std::memcpy` (not found where the backend calls it) and the system
// `ssize_t` typedef that <cstdio> drags in. Including them here means the namespace only ever wraps
// OUR code, which is all it is meant to wrap.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>


// The SHARED front end first, at real `mm::moonlive` scope: the backend below drags in the IR
// headers, and once their include guards are set the compiler header would otherwise resolve its
// types inside the wrapper namespace instead.
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveSpill.h"

namespace mm_xtensa_backend {
// The IR, the spill pass and the platform seam are SHARED — one definition each in the binary.
// Pull them into scope so the backend's unqualified references resolve to those, and only the
// ISA-specific classes below end up local to this namespace.
namespace mm { using namespace ::mm; using namespace ::mm::moonlive;
               namespace moonlive { using namespace ::mm::moonlive;
                                    using ::mm::moonlive::spillToBudget; } }
#define __XTENSA__ 1
#include "platform/esp32/moonlive_asm_xtensa.h"
#include "platform/esp32/moonlive_asm_xtensa.cpp"
#undef __XTENSA__
}  // namespace mm_xtensa_backend

#define MM_ISA_NAME "Xtensa"
// Golden values, recorded from this backend. See the .inc for what they are and are not.
#define MM_GOLD_GRID_LEN  227u
#define MM_GOLD_FX_LEN    105u
#define MM_GOLD_FILLLOOP_LEN 254u  // fits now: the host arguments left the register file
#define MM_GOLD_FXLOOP_LEN  190u
#define MM_GOLD_FXLOOP_HASH 307181036u
#define MM_GOLD_FX_HASH   2796457628u
#define MM_ISA_LOWER mm_xtensa_backend::mm::moonlive::lowerToBytes
// The assembler type itself, so the stack-budget check can measure the object the compile path
// puts on a 12 KB task rather than re-deriving its layout from the constants.
#define MM_ISA_ASM   mm_xtensa_backend::mm::moonlive::XtensaAssembler
#include "moonlive_device_codegen.inc"

// --- the structural checker's Xtensa decoder -------------------------------------------------
//
// Xtensa is variable-length: an instruction is TWO bytes when its op0 field (the low nibble of
// byte 0) is >= 8, otherwise THREE. That one rule is enough to walk the stream, and walking it is
// what makes "does this branch land on a boundary" answerable at all.
// The bytes the register-window spill hardware owns at the top of a frame, which no emitted
// slot may reach into. Stated independently of the emitter ON PURPOSE: kWindowSaveReserve in
// moonlive_asm_xtensa.cpp derives its value from the call it emits and carries the two bands
// and their `s32e` offsets, so a checker that imported it could never catch that derivation
// going wrong. This number is the expected answer, held separately.
#define MM_ISA_RESERVED_TOP 32u
// call8 rotates the window by 8, so a8..a15 are the callee's. call() saves and restores
// a8/a9/a10/a11 around it, so only a12..a15 are actually destroyed on the far side.
#define MM_ISA_CALL_CLOBBER 0xf000u   // a12..a15
#define MM_ISA_DECODE mm_xtensa_decode

#include "moonlive_structural.h"
namespace {
mm_structural::Decoded mm_xtensa_decode(const uint8_t* p, size_t n, size_t pc);
}
#include "moonlive_structural.inc"
namespace {
mm_structural::Decoded mm_xtensa_decode(const uint8_t* p, size_t n, size_t pc) {
    mm_structural::Decoded d;
    if (pc >= n) return d;
    const uint8_t b0 = p[pc];
    const uint8_t op0 = b0 & 0x0f;
    d.len = (op0 >= 8) ? 2 : 3;
    if (pc + d.len > n) { d.len = 0; return d; }

    // s32i / l32i aR, a1, #off4 — byte1 is 0x61 (store) or 0x21 (load) when the base is a1, and
    // byte2 counts 4-byte words. These are the frame accesses the first check exists for.
    if (d.len == 3 && (p[pc + 1] == 0x61 || p[pc + 1] == 0x21)) {
        d.hasFrameOff = true;
        d.frameOff = static_cast<uint32_t>(p[pc + 2]) * 4u;
    }
    // addi aD, a1, #off — byte1 has the 0xc0 marker and the base register in its low nibble.
    if (d.len == 3 && (p[pc + 1] & 0xf0) == 0xc0 && (p[pc + 1] & 0x0f) == 1) {
        d.hasFrameOff = true;
        d.frameOff = p[pc + 2];
    }
    // entry a1, N — BRI12: byte0 op0 == 6 with the 0x30 marker in byte1's high nibble. The 12-bit
    // immediate at bits 12..23 counts EIGHT-byte units, so this is the frame the routine allocated.
    if (d.len == 3 && op0 == 0x6 && (b0 & 0xf0) == 0x30) {
        const uint32_t w = uint32_t(b0) | (uint32_t(p[pc + 1]) << 8) | (uint32_t(p[pc + 2]) << 16);
        d.hasFrameAlloc = true;
        d.frameAlloc = ((w >> 12) & 0xfff) * 8u;
    }
    // j — op0 == 6 with bits 4..5 of byte0 zero. The 18-bit signed displacement sits at bits 6..23
    // of the 24-bit word and is relative to the byte AFTER the instruction... on Xtensa, PC + 4.
    if (d.len == 3 && (b0 & 0x3f) == 0x06) {
        const uint32_t w = uint32_t(b0) | (uint32_t(p[pc + 1]) << 8) | (uint32_t(p[pc + 2]) << 16);
        int32_t off = static_cast<int32_t>(w >> 6) & 0x3ffff;
        if (off & 0x20000) off -= 0x40000;                    // sign-extend 18 bits
        d.hasTarget = true;
        d.target = static_cast<int32_t>(pc) + 4 + off;
    }
    // Conditional branches (BRI8): op0 == 7, byte2 is a signed 8-bit displacement, PC + 4 relative.
    if (d.len == 3 && op0 == 0x7) {
        d.hasTarget = true;
        d.target = static_cast<int32_t>(pc) + 4 + static_cast<int8_t>(p[pc + 2]);
        d.readsMask |= (1u << ((p[pc] >> 4) & 0xf)) | (1u << (p[pc + 1] & 0xf));   // s, t
    }
    // callx8 aN, emitted as the 24-bit word 0x0000e0 | (reg << 8), which is LITTLE-ENDIAN in the
    // stream: bytes e0, reg, 00. Testing p[0]==0x00 instead of p[0]==0xe0 matched nothing, so the
    // clobber check silently never saw a call, it passed for the same reason a broken analyser
    // reports zero findings.
    if (d.len == 3 && p[pc] == 0xe0 && p[pc + 2] == 0x00) {
        d.isCall = true;
        d.readsMask |= 1u << (p[pc + 1] & 0xf);
    }
    // s32i/l32i: byte0 high nibble is the value register (read on store, written on load).
    if (d.len == 3 && p[pc + 1] == 0x61) d.readsMask  |= 1u << ((p[pc] >> 4) & 0xf);   // store
    if (d.len == 3 && p[pc + 1] == 0x21) d.writesMask |= 1u << ((p[pc] >> 4) & 0xf);   // load
    // movi aD, #imm8, byte1 == 0xa0, byte0 high nibble is the destination.
    if (d.len == 3 && p[pc + 1] == 0xa0) d.writesMask |= 1u << ((p[pc] >> 4) & 0xf);
    // add.n / mov.n (narrow): (d<<12)|(a<<8)|(b<<4)|op.
    if (d.len == 2) {
        const uint16_t w = uint16_t(p[pc]) | (uint16_t(p[pc + 1]) << 8);
        const uint8_t op = w & 0xf;
        if (op == 0xa || op == 0xd) {                        // add.n, mov.n
            d.writesMask |= 1u << ((w >> 12) & 0xf);
            d.readsMask  |= 1u << ((w >> 8) & 0xf);
            if (op == 0xa) d.readsMask |= 1u << ((w >> 4) & 0xf);
        }
    }
    return d;
}
}  // namespace


// A register outside the backend's map is a value written somewhere the program does not own. The
// map is a2..a11: a12/a13 are call scratch and the store8 address register, and a14/a15 carry this
// routine's own retw.n return linkage — using those two as general registers corrupted the return
// path and produced `Guru Meditation (IllegalInstruction)` the moment a scripted layout ran.
// Asserted against the map itself rather than a copy, so the check cannot drift from its subject.
// a8..a11 ARE in the map and that is deliberate: call8 rotates them out, so XtensaAssembler::call
// saves and restores exactly a8/a9/a10/a11 around every call. The map is legal because of that
// save-set, so the two have to agree — the count below is what fails if a register is added to the
// map without being added to the save-set.
TEST_CASE("the Xtensa vreg map names only registers the windowed ABI leaves free") {
    uint8_t n = 0;
    const uint8_t* map = mm_xtensa_backend::mm::moonlive::xtRegMap(n);
    REQUIRE(n == 10);                            // a2..a11 — widening this needs call()'s save-set widened too
    for (uint8_t i = 0; i < n; i++) {
        INFO("vreg R" << int(i) << " -> a" << int(map[i]));
        CHECK(map[i] >= 2);
        CHECK(map[i] <= 11);
    }
    // Two vregs sharing a machine register silently alias: one value overwrites the other and the
    // program computes with whichever was written last.
    for (uint8_t i = 0; i < n; i++)
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < n; j++) {
            INFO("R" << int(i) << " and R" << int(j) << " both name a" << int(map[i]));
            CHECK(map[i] != map[j]);
        }
}

// A change DETECTOR, not a correctness claim: emitted length is a cheap fingerprint that moves
// whenever codegen does. A failure here means "read the diff and decide", not "you broke it" —
// update the number and say why in the commit. It exists because the alternative way to notice an
// emission change was to flash a board and watch it reset.

// `addi.n aD, aA, #imm` encodes its immediate in a 4-bit field whose value 0 means MINUS ONE: the
// narrow form covers 1..15 and cannot express "add zero" at all. Every caller passed a literal 1
// until an array based at arena offset 0 asked for `+0`, which emitted `addi.n aX, aX, -1` and
// shifted every element access down a byte. It compiled, emitted a plausible length, and passed
// every host test, because only this backend has the narrow form. The fixture stayed dark.
//
// Asserted on the ENCODER rather than on a script's bytes: a difference-based test cannot see it
// (the wrong bytes still differ from other wrong bytes), which a control run confirmed.
TEST_CASE("Xtensa addImm never encodes an add of zero as the narrow form") {
    using Asm = mm_xtensa_backend::mm::moonlive::XtensaAssembler;
    using mm_xtensa_backend::mm::moonlive::R0;
    using mm_xtensa_backend::mm::moonlive::R1;
    // add 0 into the SAME register is a no-op and must emit nothing at all.
    {
        Asm a(64);
        a.addImm(R0, R0, 0);
        CHECK(a.size() == 0);
    }
    // Into a DIFFERENT register it is still a move, so it must emit something that is not the
    // narrow add: the low nibble of a narrow addi.n is 0xb.
    {
        Asm a(64);
        a.addImm(R1, R0, 0);
        REQUIRE(a.size() > 0);
        CHECK((a.bytes()[0] & 0x0f) != 0x0b);
    }
    // 1..15 keep the narrow form, and the immediate field must hold the value itself.
    for (int imm = 1; imm <= 15; imm++) {
        Asm a(64);
        a.addImm(R0, R0, imm);
        REQUIRE(a.size() == 2);
        const uint16_t w = uint16_t(a.bytes()[0]) | uint16_t(uint16_t(a.bytes()[1]) << 8);
        INFO("imm " << imm);
        CHECK((w & 0x0f) == 0x0b);              // still addi.n
        CHECK(((w >> 4) & 0x0f) == imm);        // and it carries the right immediate
    }
    // Past 15 the narrow field cannot hold it, so the wide RRI8 form has to take over rather than
    // silently truncating: `addi.n` with imm 16 would wrap to 0, which is the -1 bug again.
    {
        Asm a(64);
        a.addImm(R0, R0, 40);
        REQUIRE(a.size() == 3);
        CHECK((a.bytes()[1] & 0xf0) == 0xc0);   // addi (RRI8)
        CHECK(a.bytes()[2] == 40);
    }
}

// The signed 16-bit load differs from the unsigned one ONLY in the r field (the second byte's
// high nibble: l16ui r=1, l16si r=9). Asserted on the encoder because this exact encoding
// shipped WRONG once: the 0x9 was first placed in the first byte's low nibble, the disassembler
// read garbage, and every int16_t member load was an illegal instruction.
TEST_CASE("Xtensa load16S emits l16si, one r-nibble away from l16ui") {
    using Asm = mm_xtensa_backend::mm::moonlive::XtensaAssembler;
    using mm_xtensa_backend::mm::moonlive::R0;
    using mm_xtensa_backend::mm::moonlive::R1;
    Asm u(64); u.load16(R0, R1, 4);
    Asm s(64); s.load16S(R0, R1, 4);
    REQUIRE(u.size() == 3);
    REQUIRE(s.size() == 3);
    CHECK((s.bytes()[0] & 0x0f) == 0x02);          // LSAI opcode, same as l16ui
    CHECK((u.bytes()[1] >> 4) == 0x1);             // l16ui: r = 1
    CHECK((s.bytes()[1] >> 4) == 0x9);             // l16si: r = 9
    CHECK(s.bytes()[2] == 2);                      // the RRI8 immediate is scaled by 2
}

// The relaxed branch emits the INVERTED condition over a jump, so signed bge appears as blt
// (0x2) where unsigned bgeu appears as bltu (0x3). This is the nibble the old inversion table's
// fallthrough would have gotten wrong, emitting the OPPOSITE condition.
TEST_CASE("Xtensa branchGeS inverts to blt where branchGeU inverts to bltu") {
    using Asm = mm_xtensa_backend::mm::moonlive::XtensaAssembler;
    using mm_xtensa_backend::mm::moonlive::R0;
    using mm_xtensa_backend::mm::moonlive::R1;
    Asm u(64); { auto l = u.newLabel(); u.branchGeU(R0, R1, l); u.bind(l); u.finalize(); }
    Asm s(64); { auto l = s.newLabel(); s.branchGeS(R0, R1, l); s.bind(l); s.finalize(); }
    REQUIRE(u.size() == 6);                        // inverted branch (3) + j (3)
    REQUIRE(s.size() == 6);
    CHECK((u.bytes()[1] >> 4) == 0x3);             // bltu
    CHECK((s.bytes()[1] >> 4) == 0x2);             // blt: the SIGNED inversion
}
