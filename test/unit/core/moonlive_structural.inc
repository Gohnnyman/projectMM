// The STRUCTURAL CHECKER over emitted machine code (Plan-20260813, verification item 2).
//
// A golden-bytes test says "the emission changed"; it never says "the emission is WRONG". These
// three checks say the second thing, and each one is a defect class that reached a bench:
//
//   1. Frame offsets stay inside the frame the prologue allocated, and clear of the region the ABI
//      reserves at its top. The parked host arguments landed inside Xtensa's 16-byte base-save
//      area — where the hardware spills the caller's a0..a3 on a window overflow — so a script's
//      own data and its RETURN ADDRESS shared memory. Any script at all reset an ESP32 classic and
//      an S3 with `IllegalInstruction` and a data address in A0. Found on a bench, not by a test.
//   2. Every branch and jump target lands on an instruction boundary inside the program. Xtensa's
//      conditional branches reach +/-127 bytes and were silently truncated past that, so a long
//      loop jumped into the middle of an instruction.
//   3. No instruction names a register outside the backend's map. a14/a15 carry the routine's
//      retw.n return linkage; using them as general registers produced IllegalInstruction the
//      moment a scripted layout ran.
//
// Each ISA's TU defines MM_ISA_DECODE — a decoder that walks the byte stream and reports, per
// instruction, its length plus any frame offset / branch target / register it names. The checks
// themselves are written once, here.
//
// Expects, from the including TU: MM_ISA_NAME, MM_ISA_LOWER, MM_ISA_RESERVED_TOP,
// MM_ISA_DECODE, and the compiler already included.

#include "moonlive_structural.h"


// Frame offsets must land inside the frame the prologue allocated, and BELOW the region the ABI
// reserves at the top of it. This is the check that would have caught the base-save-area bug on the
// host, in CI, instead of on two boards.
TEST_CASE("emitted " MM_ISA_NAME " code keeps every frame offset inside the frame") {
    struct Case { const char* name; const char* src; int binding; };
    const Case cases[] = {
        {"a bare write",      "setRGB(0, 255, 0, 0);\n", 1},
        {"a loop",            "for (i = 0; i < 3; i = i + 1) { }\n", 0},
        {"a call in a loop",  "for (i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }\n", 0},
        {"the shipped grid",  kGridLayout, 0},
    };
    for (const auto& c : cases) {
        bool ok = false;
        const auto& sysvars = c.binding == 0 ? mm::moonlive::layoutSysVars()
                                             : mm::moonlive::effectSysVars();
        const auto code = emitBytes(c.src, sysvars, ok);
        INFO("script: ", c.name, " on " MM_ISA_NAME);
        REQUIRE(ok);
        REQUIRE(code.size() > 0);

        // The frame the program ACTUALLY allocated, read from its own prologue.
        uint32_t frame = 0;
        {
            const mm_structural::Decoded p0 = MM_ISA_DECODE(code.data(), code.size(), 0);
            REQUIRE(p0.hasFrameAlloc);            // every emitted routine must open with a prologue
            frame = p0.frameAlloc;
        }
        const uint32_t usable = frame - (MM_ISA_RESERVED_TOP);

        size_t pc = 0;
        while (pc < code.size()) {
            const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), pc);
            INFO("at byte ", pc);
            REQUIRE(d.len > 0);                       // an undecodable byte is itself a defect
            if (d.hasFrameOff) {
                INFO("frame offset ", d.frameOff, " vs frame ", frame,
                     " (usable below ", usable, ")");
                CHECK(d.frameOff + 4 <= usable);      // the whole 4-byte word must fit
            }
            pc += d.len;
        }
        CHECK(pc == code.size());                     // the stream decoded exactly, no trailing byte
    }
}

// A branch that lands mid-instruction executes whatever the following bytes happen to encode.
TEST_CASE("emitted " MM_ISA_NAME " code branches only to instruction boundaries") {
    struct Case { const char* name; const char* src; int binding; };
    const Case cases[] = {
        {"one loop",     "for (i = 0; i < 3; i = i + 1) { }\n", 0},
        {"nested loops", kGridLayout, 0},
        // Long enough that a short-displacement branch would have to be relaxed to reach.
        {"a long loop body",
         "for (i = 0; i < 8; i = i + 1) {\n"
         "  addLight(i, 0, 0); addLight(i, 1, 0); addLight(i, 2, 0); addLight(i, 3, 0);\n"
         "  addLight(i, 4, 0); addLight(i, 5, 0); addLight(i, 6, 0); addLight(i, 7, 0);\n"
         "}\n", 0},
    };
    for (const auto& c : cases) {
        bool ok = false;
        const auto code = emitBytes(c.src, mm::moonlive::layoutSysVars(), ok);
        INFO("script: ", c.name, " on " MM_ISA_NAME);
        REQUIRE(ok);
        REQUIRE(code.size() > 0);

        // Pass 1: every byte offset at which an instruction starts.
        std::vector<bool> boundary(code.size() + 1, false);
        size_t pc = 0;
        while (pc < code.size()) {
            boundary[pc] = true;
            const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), pc);
            REQUIRE(d.len > 0);
            pc += d.len;
        }
        boundary[code.size()] = true;   // one-past-the-end: a jump to the epilogue's end is legal

        // Pass 2: every target must be one of them, and inside the program.
        pc = 0;
        while (pc < code.size()) {
            const mm_structural::Decoded d = MM_ISA_DECODE(code.data(), code.size(), pc);
            if (d.hasTarget) {
                INFO("branch at ", pc, " targets ", d.target, " of ", code.size(), " bytes");
                CHECK(d.target >= 0);
                CHECK(static_cast<size_t>(d.target) <= code.size());
                if (d.target >= 0 && static_cast<size_t>(d.target) <= code.size())
                    CHECK(boundary[static_cast<size_t>(d.target)]);
            }
            pc += d.len;
        }
    }
}
