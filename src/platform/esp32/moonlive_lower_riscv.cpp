#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveSpill.h"   // the register allocator — core's, run before lowering
#include "moonlive_asm_riscv.h"

#include <cstring>

// MoonLive RISC-V backend (ESP32-P4) — lower a neutral IR program to RV32 bytes by driving the
// RISC-V assembler. The device counterpart of the Xtensa/host lowerings: same IR + the same
// StoreElem/FillElems inline ops; only the assembler differs. Host args: kArg0=buf, kArg1=nLights,
// kArg2=cpl, kArg3=t.

#if defined(__riscv)

namespace mm::moonlive {

namespace {
Reg reg(VReg v) { return static_cast<Reg>(v); }
}

size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    // StoreElem folds the address into the index vreg (no scratch); FillElems needs two.
    // Reserve scratch only for the inline ops this program actually contains: FillElems needs two
    // (loop counter + per-channel address), StoreElem one (the address — it must NOT be folded into
    // the caller's index vreg, which destroys a `for` counter). Reserving the maximum unconditionally
    // cost a register every script paid for, and that register is what a nested loop was short of on
    // the smallest file.
    const uint8_t scratch = ir.hasInline(InlineOp::FillElems) ? 2
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
                                    : RegBudget{kRegCount, scratchTotal, RiscvAssembler::kMaxSpillSlots};
    if (!spillToBudget(ir, budget, slots)) return 0;
    // sAddr FIRST: it is the one StoreElem also uses, and a store-only program reserves a single
    // scratch — so the shared one has to be the lowest index or it would name an unreserved register.
    const Reg sAddr = static_cast<Reg>(ir.vregsUsed);       // per-channel address (both ops)
    const Reg sCtr  = static_cast<Reg>(ir.vregsUsed + 1);   // FillElems loop counter

    // Size the assembler's buffer to the CALLER's — `cap` is what the staging buffer holds, so
    // the two can never disagree about how much a script may emit (they were separately
    // constant, and a script that fit one overflowed the other).
    RiscvAssembler a(cap);
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
            // A real register move, NOT add-immediate-zero: Xtensa's addi.n cannot encode 0 —
            // the ISA reuses that slot for -1 — so `dst = a + 0` silently computed a - 1. A loop
            // counter initialised through Mov therefore started at -1, the unsigned loop guard saw
            // 0xffffffff >= limit, and the body never ran. It compiled, reported no error, and
            // placed no lights.
            case IrOp::Mov:    a.movReg(reg(op.dst), reg(op.a)); break;
            case IrOp::Label:
                if (op.imm >= 0 && op.imm < kIrLabels) a.bind(labelFor(op.imm));
                break;
            case IrOp::BranchGe:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchGeU(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::BranchNe:
                if (op.imm >= 0 && op.imm < kIrLabels)
                    a.branchNe(reg(op.a), reg(op.b), labelFor(op.imm));
                break;
            case IrOp::LoadCtrl: a.load8(reg(op.dst), host(kArg4), op.imm); break;  // dst = ctrls[imm] (a4 = kArg4)
            // The allocator's two ops. `imm` is a slot INDEX; the assembler owns the frame layout.
            case IrOp::Spill:  a.spillStore(reg(op.a), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Reload: a.spillLoad(reg(op.dst), static_cast<uint8_t>(op.imm)); break;
            case IrOp::Call: {
                // The IR carries the host's function pointer (valid in the single flashed image) and
                // the FRAME SLOT its arguments start at. Hand the host their address and their
                // count: nothing is held in a register across the call, and how many arguments a
                // builtin takes stops being a property of this instruction.
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
                        Label skip = a.newLabel();
                        // The address goes in SCRATCH, not the index vreg: folding it in destroyed
                        // a `for` counter, which the loop's step and test read again after the store.
                        a.branchGeU(reg(op.a), host(kArg1), skip);
                        a.mulReg(sAddr, reg(op.a), host(kArg2));        // addr = index * cpl
                        a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.d));
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        Label done = a.newLabel(), top = a.newLabel();
                        a.movImm(sCtr, 0);
                        a.branchIfZero(host(kArg1), done);
                        a.bind(top);
                        a.mulReg(sAddr, sCtr, host(kArg2));
                        a.store8(host(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(host(kArg0), sAddr, reg(op.c));
                        a.addImm(sCtr, sCtr, 1);
                        a.branchNe(sCtr, host(kArg1), top);
                        a.bind(done);
                        break;
                    }
                }
                break;
            default: break;
        }
    }
    a.epilogue();
    a.finalize();
    if (a.overflowed() || a.size() > cap) return 0;
    std::memcpy(out, a.bytes(), a.size());
    return a.size();
}

}  // namespace mm::moonlive

#endif  // __riscv
