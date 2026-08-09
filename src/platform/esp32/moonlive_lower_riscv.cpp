#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
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

size_t lowerToBytes(const IrProgram& ir, uint8_t* out, size_t cap) {
    // StoreElem folds the address into the index vreg (no scratch); FillElems needs two.
    // Reserve scratch only for the inline ops this program actually contains: FillElems needs two
    // (loop counter + per-channel address), StoreElem one (the address — it must NOT be folded into
    // the caller's index vreg, which destroys a `for` counter). Reserving the maximum unconditionally
    // cost a register every script paid for, and that register is what a nested loop was short of on
    // the smallest file.
    const uint8_t scratch = ir.hasInline(InlineOp::FillElems) ? 2
                          : ir.hasInline(InlineOp::StoreElem) ? 1 : 0;
    if (!out || cap == 0 || ir.vregsUsed + scratch > kRegCount) return 0;
    // sAddr FIRST: it is the one StoreElem also uses, and a store-only program reserves a single
    // scratch — so the shared one has to be the lowest index or it would name an unreserved register.
    const Reg sAddr = static_cast<Reg>(ir.vregsUsed);       // per-channel address (both ops)
    const Reg sCtr  = static_cast<Reg>(ir.vregsUsed + 1);   // FillElems loop counter

    // Two branch slots per IR op is an upper bound: only StoreElem, FillElems and the branch
    // ops emit one, and each is a single op. Plus the inline ops' own labels.
    RiscvAssembler a(out, cap, static_cast<uint16_t>(ir.count * 2 + 8));

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
            case IrOp::LoadCtrl: a.load8(reg(op.dst), reg(kArg4), op.imm); break;  // dst = ctrls[imm] (a4 = kArg4)
            case IrOp::Call:
                // The IR carries the host's function pointer (the light TU's random16), valid in
                // the single flashed image — call it directly, same as the other backends.
                if (!op.callFn) return 0;
                a.call(reg(op.dst), reg(op.a), reg(op.b), reg(op.c), reinterpret_cast<const void*>(op.callFn));
                break;
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        Label skip = a.newLabel();
                        // The address goes in SCRATCH, not the index vreg: folding it in destroyed
                        // a `for` counter, which the loop's step and test read again after the store.
                        a.branchGeU(reg(op.a), reg(kArg1), skip);
                        a.mulReg(sAddr, reg(op.a), reg(kArg2));        // addr = index * cpl
                        a.store8(reg(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.c));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.d));
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        Label done = a.newLabel(), top = a.newLabel();
                        a.movImm(sCtr, 0);
                        a.branchIfZero(reg(kArg1), done);
                        a.bind(top);
                        a.mulReg(sAddr, sCtr, reg(kArg2));
                        a.store8(reg(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.c));
                        a.addImm(sCtr, sCtr, 1);
                        a.branchNe(sCtr, reg(kArg1), top);
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
    if (a.overflowed()) return 0;
    return a.size();
}

}  // namespace mm::moonlive

#endif  // __riscv
