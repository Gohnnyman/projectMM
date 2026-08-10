#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"
#include "moonlive_asm_xtensa.h"

#include <cstring>

// MoonLive Xtensa backend — lower a neutral IR program to Xtensa machine bytes by driving the
// Xtensa assembler. The device counterpart of moonlive_lower_host.cpp: same IR + the same
// StoreElem/FillElems inline ops the host registered; only the assembler/ABI differ. Built under
// __XTENSA__ (classic ESP32 / S3). The host args arrive as kArg0=buf, kArg1=nLights, kArg2=cpl,
// kArg3=t (the only LED-layout assumption, used to implement the inline ops).

#if defined(__XTENSA__)

namespace mm::moonlive {

namespace {
Reg reg(VReg v) { return static_cast<Reg>(v); }
}

size_t lowerToBytes(const IrProgram& ir, uint8_t* out, size_t cap) {
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

    XtensaAssembler a;
    a.prologue();

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

    for (uint8_t i = 0; i < ir.count; i++) {
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
            case IrOp::LoadCtrl: a.load8(reg(op.dst), reg(kArg4), op.imm); break;  // dst = ctrls[imm] (a6 = kArg4)
            case IrOp::Call:
                if (!op.callFn) return 0;
                a.call(reg(op.dst), reg(op.a), reg(op.b), reg(op.c), reinterpret_cast<const void*>(op.callFn));
                break;
            case IrOp::Inline:
                switch (op.inlineOp) {
                    case InlineOp::StoreElem: {
                        // setRGB(index=a, r=b, g=c, b=d): bounds-guard, then build the address in
                        // SCRATCH. It used to fold into the index vreg, on the assumption that the
                        // index is dead after the store — true for a throwaway temp, false for a
                        // `for` counter, which the loop's own step and test read again. `setRGB(i,…)`
                        // inside a loop therefore left the counter holding i*cpl+2 and the loop ran
                        // the wrong number of times.
                        Label skip = a.newLabel();
                        a.branchGeU(reg(op.a), reg(kArg1), skip);     // index >= nLights → skip
                        a.mulReg(sAddr, reg(op.a), reg(kArg2));        // addr = index * cpl
                        a.store8(reg(kArg0), sAddr, reg(op.b));        // store r
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.c));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.d));
                        a.bind(skip);
                        break;
                    }
                    case InlineOp::FillElems: {
                        // fill(r=a, g=b, b=c): for i in 0..nLights { addr=i*cpl; buf[addr+0..2]=r,g,b }.
                        // Two scratch: sCtr (i), sAddr (the per-light address).
                        Label done = a.newLabel(), top = a.newLabel();
                        a.movImm(sCtr, 0);
                        a.branchIfZero(reg(kArg1), done);
                        a.bind(top);
                        a.mulReg(sAddr, sCtr, reg(kArg2));            // addr = i * cpl
                        a.store8(reg(kArg0), sAddr, reg(op.a));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.b));
                        a.addImm(sAddr, sAddr, 1); a.store8(reg(kArg0), sAddr, reg(op.c));
                        a.addImm(sCtr, sCtr, 1);                       // i++
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
    if (a.overflowed() || a.size() > cap) return 0;
    std::memcpy(out, a.bytes(), a.size());
    return a.size();
}

}  // namespace mm::moonlive

#endif  // __XTENSA__
