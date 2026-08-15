#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveSpill.h"   // the register allocator — core's, run before lowering
#include "moonlive_asm_host.h"

#include <cstring>

// MoonLive host backend — lower a typed IR program to machine bytes by driving the host
// assembler. The IR is neutral; this file knows how each IR op becomes assembler calls. The
// host arguments arrive in the lowest vregs (the light host assigns kArg0=buf, kArg1=nLights,
// kArg2=cpl, kArg3=t — the only place this backend assumes the LED layout, and only to
// implement the StoreElem/FillElems inline ops the host registered).
//
// Vregs map 1:1 onto the assembler's Reg handles. The inline ops use a couple of scratch
// registers ABOVE the program's high-water mark (the parser never allocates them), so they
// don't clobber a live vreg.

namespace mm::moonlive {

#if defined(__aarch64__)   // the host assembler is implemented for arm64 only (see moonlive_asm_host.cpp)

namespace {
Reg reg(VReg v) { return static_cast<Reg>(v); }
}

size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    // Reserve three scratch regs above the program's vregs for the inline ops' temps.
    // Reserve scratch only for the inline ops this program actually contains. Unlike Xtensa and
    // RISC-V, THIS backend's StoreElem needs one scratch too (sAddr — it does not fold the address
    // into the index vreg), so the two cases differ: FillElems needs three, StoreElem one, neither
    // needs any. Reserving the maximum unconditionally cost a register every script paid for.
    const uint8_t scratch = ir.hasInline(InlineOp::FillElems) ? 3
                          : ir.hasInline(InlineOp::StoreElem) ? 1 : 0;
    // A host CALL also needs two scratch registers — the address of its argument block and the
    // count — but it SHARES them with the inline ops rather than reserving its own. A Call and an
    // Inline are different IR instructions, so their scratch is never live at the same time, and
    // both die at the end of the one instruction that uses them. Reserving separately cost two
    // registers permanently, which on Xtensa's ten is the difference between compiling and not.
    // +1 for the host-argument reload. The host arguments live in frame slots now (core parks them
    // at entry), so an op that reads buf/nLights/cpl/ctrls brings one back for the instruction that
    // needs it. A call's two scratch registers still share with the inline ops' — different IR
    // instructions, never live at once.
    const uint8_t scratchTotal = static_cast<uint8_t>((scratch < 2 ? uint8_t(2) : scratch) + 1);
    if (!out || cap == 0) return 0;

    // Run the register allocator before lowering. It leaves a program that already fits untouched,
    // and rewrites one that does not into Spill/Reload against this backend's frame — replacing the
    // hand-rolled `vregsUsed + scratch > kRegCount` bail that used to REFUSE such a script outright.
    // False here means even the spilled form does not fit, which is a diagnostic, never a miscompile.
    uint8_t slots = 0;
    // `squeeze` overrides the REGISTER COUNT and slot count a test wants to constrain, but never
    // `reserved`: the scratch is what THIS lowerer is about to use for its inline ops and call
    // argument block, so a test-supplied value would let the allocator hand out a register the
    // lowerer then overwrites — miscompiling exactly the squeezed programs the seam exists to prove.
    const RegBudget budget = squeeze ? RegBudget{squeeze->regs, scratchTotal, squeeze->slots}
                                    : RegBudget{kRegCount, scratchTotal, HostAssembler::kMaxSpillSlots};
    if (!spillToBudget(ir, budget, slots)) return 0;
    // sAddr FIRST, because it is the one StoreElem also uses: a store-only program reserves a single
    // scratch, so the shared one has to be the lowest index or it would name a register outside the
    // reservation. sOff/sCtr are FillElems-only and sit above it.
    const Reg sAddr = static_cast<Reg>(ir.vregsUsed);        // per-channel address (off, off+1, off+2)
    const Reg sOff  = static_cast<Reg>(ir.vregsUsed + 1);    // base byte offset of the current light
    const Reg sCtr  = static_cast<Reg>(ir.vregsUsed + 2);    // loop counter

    // Size the assembler's buffer to the CALLER's — `cap` is what the staging buffer holds, so
    // the two can never disagree about how much a script may emit (they were separately
    // constant, and a script that fit one overflowed the other).
    HostAssembler a(cap);
    // Bring a parked host argument back for the one instruction that reads it.
    // The LAST reserved scratch index, derived from scratchTotal rather than hard-coded: the `+1`
    // in scratchTotal above IS this register, so the reservation and the use cannot drift apart.
    // A fixed offset sat OUTSIDE the reservation and only worked because the register maps happen
    // to have spare entries above the high-water mark.
    const Reg sHost = static_cast<Reg>(ir.vregsUsed + scratchTotal - 1);
    auto host = [&](VReg v) -> Reg { a.spillLoad(sHost, hostArgSlot(v)); return sHost; };
    // The frame must cover the parked HOST ARGUMENTS at the top as well as whatever the parser and
    // the allocator claimed at the bottom — they are stored before any script code runs, so a frame
    // sized only from `slots` would put them past its end.
    a.prologue(slots > kTotalSlots ? slots : kTotalSlots);

    // An IR label id becomes an assembler label ON FIRST USE. Allocating the whole range up front
    // exhausts the assembler's fixed label table, and the inline ops (StoreElem's bounds guard,
    // FillElems' loop) then get nothing when they ask for their own — which broke every program
    // that contains no loop at all. Lazy allocation costs one lookup and leaves the table for the
    // labels a program actually has.
    Label labels[kIrLabels];
    bool  labelMade[kIrLabels] = {};
    auto  labelFor = [&](int32_t id) -> Label {
        if (!labelMade[id]) { labels[id] = a.newLabel(); labelMade[id] = true; }
        return labels[id];
    };

    // uint16_t, matching IrProgram::count: the op array is sized to the script now, so a
    // uint8_t counter wrapped at 256 ops and looped forever instead of emitting.
    for (uint16_t i = 0; i < ir.count; i++) {
        const IrInst& op = ir.ops[i];
        switch (op.op) {
            case IrOp::Const:  a.movImm(reg(op.dst), op.imm); break;
            case IrOp::Add:    a.addReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::AddImm: a.addImm(reg(op.dst), reg(op.a), op.imm); break;
            case IrOp::Mul:    a.mulReg(reg(op.dst), reg(op.a), reg(op.b)); break;
            case IrOp::Mov:    a.addImm(reg(op.dst), reg(op.a), 0); break;   // dst = a + 0
            case IrOp::Label:
                if (op.imm >= 0 && op.imm < kIrLabels) a.bind(labelFor(op.imm));
                break;
            case IrOp::BranchGe:
                if (op.imm >= 0 && op.imm < kIrLabels) {
                    a.cmp(reg(op.a), reg(op.b));
                    a.branchIf(Cond::Hs, labelFor(op.imm));     // unsigned >=
                }
                break;
            case IrOp::BranchNe:
                if (op.imm >= 0 && op.imm < kIrLabels) {
                    a.cmp(reg(op.a), reg(op.b));
                    a.branchIf(Cond::Ne, labelFor(op.imm));
                }
                break;
            case IrOp::LoadCtrl: a.load8(reg(op.dst), host(kArg4), op.imm); break;  // dst = ctrls[imm]
            // The allocator's two ops. `imm` is a slot INDEX; the assembler owns the frame layout.
            case IrOp::Spill:  a.spillStore(reg(op.a), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Reload: a.spillLoad(reg(op.dst), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Call: {
                // The arguments are in consecutive frame slots starting at `imm`; hand the host
                // their ADDRESS and their COUNT. Nothing is held in a register across the call, and
                // how many arguments a builtin takes stops being a property of this instruction.
                if (!op.callFn) return 0;
                const Reg argPtr = static_cast<Reg>(ir.vregsUsed);
                a.slotAddr(argPtr, static_cast<uint8_t>(op.imm));
                const Reg argN = static_cast<Reg>(ir.vregsUsed + 1);
                a.movImm(argN, static_cast<int32_t>(op.b));
                a.call(reg(op.dst), argPtr, argN,
                       host(kArg4), reinterpret_cast<const void*>(op.callFn));
                break;
            }
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        // setRGB(index=a, r=b, g=c, b=d): bounds-guard, addr = index*cpl, store 3.
                        Label skip = a.newLabel();
                        a.cmp(reg(op.a), host(kArg1));         // index vs nLights
                        a.branchIf(Cond::Hs, skip);           // index >= nLights → skip
                        a.mulReg(sAddr, reg(op.a), host(kArg2));   // addr = index * cpl
                        a.store8(host(kArg0), sAddr, reg(op.b));   // buf[addr+0] = r
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));  // +1 = g
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.d));  // +2 = b
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        // fill(r=a, g=b, b=c): for i in 0..nLights { buf[i*cpl+0..2] = r,g,b }.
                        // sOff = byte base of the current light; sAddr = sOff/+1/+2 per channel
                        // (a fresh copy each light so the +1/+2 never corrupt sOff); sOff += cpl.
                        Label done = a.newLabel(), top = a.newLabel();
                        a.movImm(sOff, 0);                    // off = 0
                        a.movImm(sCtr, 0);                    // i = 0
                        a.branchIfZero(host(kArg1), done);     // nLights==0 → skip
                        a.bind(top);
                        a.addImm(sAddr, sOff, 0); a.store8(host(kArg0), sAddr, reg(op.a));   // buf[off+0]=r
                        a.addImm(sAddr, sOff, 1); a.store8(host(kArg0), sAddr, reg(op.b));   // buf[off+1]=g
                        a.addImm(sAddr, sOff, 2); a.store8(host(kArg0), sAddr, reg(op.c));   // buf[off+2]=b
                        a.addReg(sOff, sOff, host(kArg2));     // off += cpl   (general stride)
                        a.addImm(sCtr, sCtr, 1);              // i++
                        a.cmp(sCtr, host(kArg1));
                        a.branchIf(Cond::Lo, top);
                        a.bind(done);
                        break;
                    }
                }
                break;
            default: break;   // Loop/LoopEnd/Bounds/BoundsEnd are not emitted by the parser now
        }
    }
    a.epilogue();   // tears the spill frame down (if any), then ret
    a.finalize();
    if (a.overflowed() || a.size() > cap) return 0;
    std::memcpy(out, a.bytes(), a.size());
    return a.size();
}

#else   // unsupported host ISA (e.g. Windows x64) — degrade: no codegen, compile fails cleanly.

size_t lowerToBytes(IrProgram&, uint8_t*, size_t, const RegBudget*) { return 0; }

#endif

}  // namespace mm::moonlive
