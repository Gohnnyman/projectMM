#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

#include <cstring>   // std::strncmp (@control keyword match)

namespace mm::moonlive {

namespace {

// --- Lexer ---------------------------------------------------------------------------
// `ControlAnno` is a captured `// @control min..max` comment (a control's UI range). A plain
// `//` line comment is skipped like whitespace; only the @control form becomes a token, carrying
// its min/max in annoMin/annoMax. `Assign` is `=` (a control declaration's initializer).
enum class Tok { Ident, Number, Assign, LParen, RParen, LBrace, RBrace, Comma, Semicolon,
                 ControlAnno, Plus, Minus, Star, Less, End, Error };

struct Lexer {
    const char* p;
    Tok kind = Tok::Error;
    long number = 0;
    const char* identBeg = nullptr;
    size_t identLen = 0;
    long annoMin = 0, annoMax = 0;     // ControlAnno: the captured min..max
    const char* tokBeg = nullptr;
    const char* srcBeg;
    const char* err = "";

    explicit Lexer(const char* s) : p(s), srcBeg(s) { advance(); }

    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }
    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool isIdentCont(char c) { return isIdentStart(c) || isDigit(c); }
    uint16_t col() const { return static_cast<uint16_t>((tokBeg - srcBeg) + 1); }

    // Read a run of digits into v (capped); returns true if at least one digit was consumed.
    bool readNumber(long& v) {
        if (!isDigit(*p)) return false;
        v = 0;
        while (isDigit(*p)) { v = v * 10 + (*p - '0'); p++; if (v > 1000000) break; }
        return true;
    }

    void advance() {
        for (;;) {
            while (isSpace(*p)) p++;
            // Line comment: a plain `//…` is skipped; a `// @control min..max` is captured.
            if (p[0] == '/' && p[1] == '/') {
                const char* lineStart = p;
                p += 2;
                while (*p == ' ' || *p == '\t') p++;
                // Match `@control` only as a whole word — require a non-identifier
                // char after it, so a comment like `// @controlled …` is a plain
                // comment, not a malformed annotation.
                if (p[0] == '@' && std::strncmp(p, "@control", 8) == 0 && !isIdentCont(p[8])) {
                    tokBeg = lineStart;
                    p += 8;
                    while (*p == ' ' || *p == '\t') p++;
                    long lo = 0, hi = 0;
                    if (!readNumber(lo) || !(p[0] == '.' && p[1] == '.')) { kind = Tok::Error; err = "malformed @control (expected min..max)"; return; }
                    p += 2;
                    if (!readNumber(hi)) { kind = Tok::Error; err = "malformed @control (expected max)"; return; }
                    annoMin = lo; annoMax = hi; kind = Tok::ControlAnno; return;
                }
                // plain comment — skip to end of line and re-loop (treated as whitespace)
                while (*p && *p != '\n') p++;
                continue;
            }
            break;
        }
        tokBeg = p;
        char c = *p;
        if (c == 0) { kind = Tok::End; return; }
        if (c == '=') { p++; kind = Tok::Assign; return; }
        if (c == '(') { p++; kind = Tok::LParen; return; }
        if (c == ')') { p++; kind = Tok::RParen; return; }
        if (c == ',') { p++; kind = Tok::Comma; return; }
        if (c == ';') { p++; kind = Tok::Semicolon; return; }
        if (c == '+') { p++; kind = Tok::Plus;    return; }
        if (c == '-') { p++; kind = Tok::Minus;   return; }
        if (c == '*') { p++; kind = Tok::Star;    return; }
        if (c == '{') { p++; kind = Tok::LBrace;  return; }
        if (c == '}') { p++; kind = Tok::RBrace;  return; }
        if (c == '<') { p++; kind = Tok::Less;    return; }
        // '/' only reaches here when it is NOT the `//` a comment starts with (handled above).
        // '/' and '%' are deliberately NOT tokens yet. No ISA here has a cheap integer divide, so
        // both would lower to a host call — which the light domain already ships as `mod(a, b)` and
        // `turn(n)`, so the capability exists under a name instead of an operator. A script using
        // the character gets "unexpected character", which is the honest answer. Backlogged.
        if (isDigit(c)) {
            long v = 0; readNumber(v);
            number = v; kind = Tok::Number; return;
        }
        if (isIdentStart(c)) {
            identBeg = p;
            while (isIdentCont(*p)) p++;
            identLen = static_cast<size_t>(p - identBeg);
            kind = Tok::Ident; return;
        }
        kind = Tok::Error; err = "unexpected character";
    }
};

// --- Parser → IR ---------------------------------------------------------------------
// A recursive-descent parser that evaluates each expression into a virtual register and emits
// IR. It knows the GRAMMAR and resolves call names against the injected table; it owns no
// function names. Buffer writers (Kind::Inline) and helpers (Kind::Call) are dispatched
// generically.
struct Parser {
    Lexer&             lex;
    const BuiltinTable& table;
    const SysVarTable&  sysvars;
    IrProgram&         ir;
    VReg               nextTemp = kFirstTemp;   // high-water mark — also IrProgram.vregsUsed
    VReg               freeStack[kMaxVRegs] = {};   // recycled temps (LIFO), so a dead vreg is reused
    uint8_t            freeCount = 0;
    // Script-local variables — a `for` loop's counter and its limit. Distinct from a declared
    // control (a control is a UI value the script READS; a local is one the script WRITES) and from
    // a temp (a temp is write-once and recycled).
    //
    // A local lives in a FRAME SLOT, not a register. Holding it in a register for the whole of its
    // scope is what made a nested loop unfittable on the smallest target: Xtensa's windowed ABI
    // leaves ten registers, five carry the host arguments, and the reload temps take the rest — so a
    // loop counter that never yields its register left nothing to compute with and every looped
    // script was refused. In the frame, the number of live variables is bounded by memory rather
    // than by the register file, which is the whole point of the stack machine, and it is also what
    // makes function arguments and recursion fall out later rather than needing new machinery.
    struct Local { const char* name; size_t nameLen; uint8_t slot; };
    Local              locals[kMaxLocals] = {};
    uint8_t            localCount = 0;
    uint8_t            slotHighWater = 0;   // slots currently in scope
    uint8_t            slotsUsed = 0;       // PEAK slots — what the prologue reserves
    uint8_t            nextLabel = 0;          // IR label ids, handed out in source order

    DeclaredControl    controls[kMaxCtrls] = {};  // controls the script declared (decl lines)
    uint8_t            controlCount = 0;
    const char*        error = "";
    uint16_t           errorCol = 0;
    bool               failed = false;

    void fail(const char* msg) { if (!failed) { failed = true; error = msg; errorCol = lex.col(); } }

    // Find a declared control by name; returns its index or -1. Names point into the source buffer
    // (token spans, not NUL-terminated), so compare by length + bytes.
    int findControl(const char* name, size_t len) const {
        for (uint8_t i = 0; i < controlCount; i++)
            if (controls[i].nameLen == len && std::strncmp(controls[i].name, name, len) == 0)
                return i;
        return -1;
    }

    // A stack temp allocator: alloc() hands out a recycled vreg if one is free, else a fresh one;
    // free() returns a temp to the pool once its value has been consumed. This is what keeps a
    // multi-call statement (e.g. setRGB(random16(..), random16(..), random16(..), 0)) within the
    // small device register file — each call's argument temp dies and is reused for the next,
    // instead of the count growing without bound. The textbook tree-walk register stack.
    VReg alloc() {
        if (freeCount) return freeStack[--freeCount];
        if (nextTemp < kMaxVRegs) return nextTemp++;
        // Out of virtual registers — a statement deeper than the fixed file holds. Fail the
        // compile rather than aliasing the last vreg (which would silently produce wrong IR).
        fail("script too complex (out of registers)");
        return kFirstTemp;
    }
    void freeTemp(VReg v) {
        // No local-register guard is needed: a variable lives in a frame slot, and reading one
        // hands back an ordinary temp that the consumer owns. Every vreg reaching here is a temp.
        if (v >= kFirstTemp && freeCount < kMaxVRegs) freeStack[freeCount++] = v;
    }

    // Append an IR op, failing the compile if the program is full or names an out-of-budget
    // vreg (IrProgram::push validates both). Centralises the check so no call site forgets it.
    void emit(const IrInst& i) { if (!ir.push(i)) fail("script too large"); }

    bool expect(Tok t, const char* msg) {
        if (lex.kind != t) { fail(msg); return false; }
        lex.advance();
        return true;
    }

    // expr    := term { ("+" | "-") term }
    // term    := primary { "*" primary }
    // primary := number | ident | call | "(" expr ")"
    //
    // Precedence climbing, the textbook shape: each level consumes the tighter-binding one below
    // it, so `2 + 3 * 4` is 14 rather than 20 without any special case. Every operator lowers to IR
    // the three backends ALREADY have (Const/Add/Mul) — a - b is emitted as a + (b * -1), because
    // no ISA here has a subtract and Xtensa's add-immediate encodes only 1..15, so negating the
    // immediate would silently produce a wrong constant.
    VReg parseExpr() {
        VReg lhs = parseTerm();
        while (!failed && (lex.kind == Tok::Plus || lex.kind == Tok::Minus)) {
            const bool negate = (lex.kind == Tok::Minus);
            lex.advance();
            VReg rhs = parseTerm();
            if (failed) return 0;
            if (negate) {
                VReg m = alloc();
                emit({IrOp::Const, m, 0,0,0,0, -1, nullptr, {}});
                VReg n = alloc();
                emit({IrOp::Mul, n, rhs, m, 0,0, 0, nullptr, {}});
                freeTemp(m); freeTemp(rhs);
                rhs = n;
            }
            VReg dst = alloc();
            emit({IrOp::Add, dst, lhs, rhs, 0,0, 0, nullptr, {}});
            freeTemp(lhs); freeTemp(rhs);
            lhs = dst;
        }
        return lhs;
    }

    VReg parseTerm() {
        VReg lhs = parsePrimary();
        while (!failed && lex.kind == Tok::Star) {
            lex.advance();
            VReg rhs = parsePrimary();
            if (failed) return 0;
            VReg dst = alloc();
            emit({IrOp::Mul, dst, lhs, rhs, 0,0, 0, nullptr, {}});
            freeTemp(lhs); freeTemp(rhs);
            lhs = dst;
        }
        return lhs;
    }

    // A bare ident that names a declared control reads its live value (a LoadCtrl of its arena
    // offset); an ident followed by `(` is a call.
    VReg parsePrimary() {
        if (failed) return 0;
        if (lex.kind == Tok::LParen) {                   // grouping
            lex.advance();
            VReg v = parseExpr();
            if (!expect(Tok::RParen, "expected ')'")) return 0;
            return v;
        }
        if (lex.kind == Tok::Minus) {                    // unary minus: 0 - v, as (v * -1)
            lex.advance();
            VReg v = parsePrimary();
            if (failed) return 0;
            VReg m = alloc();
            emit({IrOp::Const, m, 0,0,0,0, -1, nullptr, {}});
            VReg dst = alloc();
            emit({IrOp::Mul, dst, v, m, 0,0, 0, nullptr, {}});
            freeTemp(m); freeTemp(v);
            return dst;
        }
        if (lex.kind == Tok::Number) {
            if (lex.number < 0 || lex.number > 65535) { fail("number out of range (0..65535)"); return 0; }
            VReg v = alloc();
            emit({IrOp::Const, v, 0,0,0,0, static_cast<int32_t>(lex.number), nullptr, {}});
            lex.advance();
            return v;
        }
        if (lex.kind == Tok::Ident) {
            // A system variable the host defines: `t` (elapsed ms, an argument register) or a
            // per-frame value the binding writes (`width`/`height`/`depth`, arena slots). Resolved
            // BEFORE locals and controls so the name means one thing in every script; the
            // declaration paths reject the name, so nothing can shadow it.
            if (const SysVar* sv = sysvars.find(lex.identBeg, lex.identLen)) {
                lex.advance();
                if (sv->kind == SysVarKind::Arg) return sv->where;   // free: already in a register
                VReg v = alloc();
                emit({IrOp::LoadCtrl, v, 0,0,0,0, sv->where, nullptr, {}});
                return v;
            }
            for (uint8_t li = 0; li < localCount; li++) {
                if (locals[li].nameLen == lex.identLen &&
                    std::strncmp(locals[li].name, lex.identBeg, lex.identLen) == 0) {
                    // Load the variable from its frame slot into a fresh temp. The temp is ordinary:
                    // the caller consumes it and frees it like any other expression value, so a
                    // variable occupies a register only for the instruction that reads it rather
                    // than for the whole of its scope.
                    lex.advance();
                    VReg v = alloc();
                    emit({IrOp::Reload, v, 0,0,0,0, locals[li].slot, nullptr, {}});
                    return v;
                }
            }
            int ci = findControl(lex.identBeg, lex.identLen);
            if (ci >= 0) {                                // a declared control read
                VReg v = alloc();
                emit({IrOp::LoadCtrl, v, 0,0,0,0, controls[ci].offset, nullptr, {}});
                lex.advance();
                return v;
            }
            VReg out = 0;
            parseCall(&out);   // otherwise a call used as an expression must return a value
            return out;
        }
        fail("expected a number, a control name, or a function call");
        return 0;
    }

    // call := ident "(" [expr {"," expr}] ")".  If `resultOut` is non-null the call is used as
    // an expression and must return a value; the result vreg is written there. If null it is a
    // statement (void).
    void parseCall(VReg* resultOut) {
        if (lex.kind != Tok::Ident) { fail("expected a function name"); return; }
        const Builtin* fn = table.find(lex.identBeg, lex.identLen);
        if (!fn) { fail("unknown function"); return; }
        lex.advance();
        if (!expect(Tok::LParen, "expected '(' after the function name")) return;

        // Evaluate each argument, PARKING it in a frame slot as soon as it is finished.
        //
        // The op that consumes them reads every argument at once, so evaluating straight into vregs
        // keeps all four live simultaneously — and each argument's own sub-expression needs
        // registers on top of that. On Xtensa's ten that is what refused every looped effect: a
        // four-operand setRGB left nothing to compute the operands WITH. Staging through the frame
        // means only ONE argument occupies a register at a time; they come back together in the
        // reload just before the op, where the peak is exactly the operand count and nothing more.
        VReg args[4] = {0,0,0,0};
        uint8_t argSlot[4] = {0,0,0,0};
        const uint8_t argSlotBase = slotHighWater;
        uint8_t n = 0;
        if (lex.kind != Tok::RParen) {
            while (true) {
                if (n >= fn->argc || n >= 4) { fail("too many arguments"); return; }
                const VReg v = parseExpr();
                if (failed) return;
                if (slotHighWater >= kMaxLocals) { fail("expression too deeply nested"); return; }
                argSlot[n] = slotHighWater++;
                emit({IrOp::Spill, 0, v, 0,0,0, argSlot[n], nullptr, {}});
                freeTemp(v);                       // its register is free again immediately
                n++;
                if (lex.kind == Tok::Comma) { lex.advance(); continue; }
                break;
            }
        }
        // Bring the arguments back for the one instruction that reads them.
        for (uint8_t i = 0; i < n; i++) {
            args[i] = alloc();
            emit({IrOp::Reload, args[i], 0,0,0,0, argSlot[i], nullptr, {}});
        }
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        slotHighWater = argSlotBase;               // the staging slots are scratch, reused per call
        if (n != fn->argc) { fail("wrong number of arguments"); return; }
        if (!expect(Tok::RParen, "expected ')'")) return;

        // The IR Call op carries a single argument vreg, so a Call-kind builtin must be unary.
        // (Today random16 is the only one.) Reject a multi-arg Call up front rather than silently
        // dropping args[1..]; a future N-ary helper needs the IR Call contract widened first.
        if (fn->kind == BuiltinKind::Call && fn->argc > 3) { fail("a call takes at most three arguments"); return; }

        if (resultOut) {
            if (fn->kind != BuiltinKind::Call || !fn->returns) { fail("this function does not return a value"); return; }
            // The argument temps are consumed by the call; free them, then allocate the result
            // (which may reuse one of them) — this is what bounds the register count across a
            // chain of calls. The IR Call reads its arg before the result is written, so reuse is
            // safe even when result == arg.
            for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
            VReg r = alloc();
            emit({IrOp::Call, r, args[0], args[1], args[2], 0, 0, fn->fn, {}});
            *resultOut = r;
        } else {
            // A statement call. Call kinds with a result are also allowed as statements (result
            // discarded); Inline kinds emit the inline op with their operands.
            if (fn->kind == BuiltinKind::Call) {
                for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
                VReg r = alloc();
                emit({IrOp::Call, r, args[0], args[1], args[2], 0, 0, fn->fn, {}});
                freeTemp(r);
            } else {
                // Inline op: hand the operand vregs to the backend via an Inline IR op. The
                // operand mapping per inline op is the backend's contract (documented there).
                emit({IrOp::Inline, 0, args[0], args[1], args[2], args[3], 0, nullptr, fn->inlineOp});
                for (uint8_t i = 0; i < n; i++) freeTemp(args[i]);
            }
        }
    }

    // A control declaration: `uint8_t ident = number ;` optionally followed by `// @control min..max`.
    // The leading `uint8_t` keyword is already consumed by the caller. Records a DeclaredControl.
    void parseDecl() {
        if (lex.kind != Tok::Ident) { fail("expected a control name after the type"); return; }
        const char* name = lex.identBeg; size_t nameLen = lex.identLen;
        if (nameLen >= kMaxControlName) { fail("control name too long"); return; }   // no silent truncation downstream
        if (sysvars.find(name, nameLen)) { fail("name is a system variable"); return; }
        if (findControl(name, nameLen) >= 0) { fail("duplicate control name"); return; }
        // A control name must not shadow a builtin: a declared `random16` would make `random16(…)`
        // ambiguous (control read vs call). Reject it at the source so the resolution never collides.
        if (table.find(name, nameLen)) { fail("control name shadows a built-in function"); return; }
        if (controlCount >= kMaxCtrls) { fail("too many controls"); return; }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in a control declaration")) return;
        if (lex.kind != Tok::Number) { fail("expected a default value (a number)"); return; }
        if (lex.number < 0 || lex.number > 255) { fail("uint8_t default out of range (0..255)"); return; }
        long def = lex.number;
        lex.advance();
        if (!expect(Tok::Semicolon, "expected ';' after the control declaration")) return;
        // A malformed `// @control …` comment lexes to Tok::Error with a specific
        // message (e.g. "malformed @control (expected min..max)"). Surface it here
        // rather than letting it fall through to a generic later parse failure.
        if (lex.kind == Tok::Error) { fail(lex.err); return; }
        // Optional range annotation; default 0..255 if absent.
        long lo = 0, hi = 255;
        if (lex.kind == Tok::ControlAnno) {
            lo = lex.annoMin; hi = lex.annoMax;
            if (lo < 0 || hi > 255 || lo > hi) { fail("@control range out of order or out of 0..255"); return; }
            lex.advance();
        }
        // The default must lie within the (possibly annotated) range — a slider can't start outside
        // its own bounds.
        if (def < lo || def > hi) { fail("control default is outside its @control range"); return; }
        controls[controlCount] = {name, static_cast<uint8_t>(lo), static_cast<uint8_t>(hi),
                                  static_cast<uint8_t>(def), static_cast<uint8_t>(nameLen),
                                  CtrlType::Uint8, controlCount};
        controlCount++;
    }

    // Is the current Ident the `uint8_t` type keyword (the only declared type in Stage 1)?
    bool atTypeKeyword() const {
        return lex.kind == Tok::Ident && lex.identLen == 7 && std::strncmp(lex.identBeg, "uint8_t", 7) == 0;
    }

    // program := { decl } { stmt }.  Declarations (control vars) come first, then one-or-more
    // call statements. (Multi-statement now: a script has decl lines AND a statement line.)
    /// stmt := call ";" | forStmt
    /// forStmt := "for" "(" ident "=" expr ";" ident "<" expr ";" ident "=" expr ")" "{" {stmt} "}"
    ///
    /// C-style deliberately: it is the form a script author already knows, and the third clause is
    /// what a serpentine layout needs (`i = i + 2`, or counting down) without inventing more syntax.
    ///
    /// Lowered as a BOTTOM-TESTED loop, which is the shape FillElems has always emitted by hand and
    /// so is proven on all three ISAs:
    ///
    ///     i = init
    ///     BranchGe i, limit -> done      ; an empty range runs the body zero times
    ///   top:
    ///     body
    ///     i = step
    ///     BranchNe i, limit -> top       ; back edge
    ///   done:
    ///
    /// The back edge tests NOT-EQUAL rather than less-than because no ISA here has branch-if-less;
    /// that is exact for the `i = i + 1` case and terminates for any step that eventually hits the
    /// limit. A step that overshoots (`i = i + 3` over a limit it skips past) would not — so the
    /// limit is re-tested with BranchGe at the top of each iteration instead. See below.
    bool parseFor() {
        lex.advance();                                     // consume `for`
        if (!expect(Tok::LParen, "expected '(' after for")) return false;

        // --- init: ident = expr ---
        if (lex.kind != Tok::Ident) { fail("expected a loop variable"); return false; }
        // Two slots per loop: the counter and the limit. Both must outlive the body, and both live
        // in the frame — nesting depth is now bounded by frame slots, not by the register file.
        if (localCount + 2 > kMaxLocals) { fail("too many nested loops"); return false; }
        const char* varName = lex.identBeg;
        const size_t varLen = lex.identLen;
        if (sysvars.find(varName, varLen)) { fail("name is a system variable"); return false; }
        // A nested loop reusing the enclosing loop's name would bind a SECOND register to that name:
        // the inner step then writes the register the outer back edge tests, and the emitted program
        // never terminates — a hang on the render task from a script a user can type. Refused for the
        // same reason a duplicate control name is.
        for (uint8_t li = 0; li < localCount; li++)
            if (locals[li].nameLen == varLen &&
                std::strncmp(locals[li].name, varName, varLen) == 0) {
                fail("loop variable already in use"); return false;
            }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in the for's first clause")) return false;
        VReg init = parseExpr();
        if (failed) return false;
        // The counter starts life in its slot; the temp that computed it is released immediately.
        const uint8_t counterSlot = slotHighWater++;
        emit({IrOp::Spill, 0, init, 0,0,0, counterSlot, nullptr, {}});
        freeTemp(init);
        const uint8_t myLocal = localCount;
        locals[localCount++] = {varName, varLen, counterSlot};
        if (!expect(Tok::Semicolon, "expected ';' after the for's first clause")) return false;

        // --- condition: ident < expr  (the only comparison the language has) ---
        // The name must be the loop variable: the emitted code tests `counter` whatever is written
        // here, so a different name compiles clean and runs as if it said the right one. That is a
        // wrong fixture with no diagnostic anywhere — the failure mode hardest to trace back to a
        // typo. (`for (y…) { for (x = 0; y < cols; x = x + 1) … }` is the realistic version.)
        if (lex.kind != Tok::Ident) { fail("expected the loop variable in the condition"); return false; }
        if (lex.identLen != varLen || std::strncmp(lex.identBeg, varName, varLen) != 0) {
            fail("the condition must test the loop variable"); return false;
        }
        lex.advance();
        if (!expect(Tok::Less, "expected '<' — it is the only comparison a for condition takes")) return false;
        // The bound goes to its own slot: it is read at the entry guard and again at the back edge,
        // so it has to survive the body — and a body containing a call would otherwise have to keep
        // it in a register across that call.
        VReg limitTmp = parseExpr();
        if (failed) return false;
        const uint8_t limitSlot = slotHighWater++;
        emit({IrOp::Spill, 0, limitTmp, 0,0,0, limitSlot, nullptr, {}});
        freeTemp(limitTmp);
        if (!expect(Tok::Semicolon, "expected ';' after the for's condition")) return false;

        // --- step: ident = expr (parsed now, emitted after the body) ---
        if (lex.kind != Tok::Ident) { fail("expected the loop variable in the step"); return false; }
        if (lex.identLen != varLen || std::strncmp(lex.identBeg, varName, varLen) != 0) {
            fail("the step must advance the loop variable"); return false;   // it advances `counter` regardless
        }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in the for's third clause")) return false;
        const char* stepSrc = lex.tokBeg;                  // re-lexed after the body
        // Skip the step expression without emitting: scan to the closing ')'.
        int depth = 0;
        while (!failed && lex.kind != Tok::End) {
            // A lexer error stops the scan. Tok::Error is not Tok::End and advance() does not move
            // past the offending character, so without this the loop spins forever on a script with
            // a stray character in the step expression — a hang, not a diagnostic.
            if (lex.kind == Tok::Error) { fail(lex.err); return false; }
            if (lex.kind == Tok::LParen) depth++;
            else if (lex.kind == Tok::RParen) { if (depth == 0) break; depth--; }
            lex.advance();
        }
        if (!expect(Tok::RParen, "expected ')' to close the for")) return false;
        if (!expect(Tok::LBrace, "expected '{' — a for's body is braced")) return false;

        if (nextLabel + 2 > kIrLabels) { fail("too many loops in one script"); return false; }
        const uint8_t lDone = nextLabel++;
        const uint8_t lTop  = nextLabel++;

        // Each test reloads both operands: they live in the frame, so a comparison is
        // load-load-branch. The reload temps die immediately, which is what keeps the body's
        // register demand independent of how deeply loops nest.
        {
            VReg c = alloc(), l = alloc();
            emit({IrOp::Reload, c, 0,0,0,0, counterSlot, nullptr, {}});
            emit({IrOp::Reload, l, 0,0,0,0, limitSlot,   nullptr, {}});
            emit({IrOp::BranchGe, 0, c, l, 0,0, lDone, nullptr, {}});   // empty range
            freeTemp(l); freeTemp(c);
        }
        emit({IrOp::Label,    0, 0,0,0,0,             lTop,  nullptr, {}});

        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End) {
            if (!parseStatement()) return false;
        }
        if (!expect(Tok::RBrace, "expected '}' to close the for's body")) return false;

        // The step, re-lexed from the source it was skipped over.
        {
            Lexer stepLex(stepSrc);
            Lexer save = lex;
            lex = stepLex;
            VReg s = parseExpr();
            if (failed) return false;
            // parseExpr stops at the first token it cannot consume, so without this the step
            // silently ignores whatever follows it — `i = i + 1 garbage` compiled clean. The
            // skip-scan above already found the real ')', so anything else here is a typo.
            if (lex.kind != Tok::RParen) {
                lex = save; fail("unexpected token in the for's step"); return false;
            }
            emit({IrOp::Spill, 0, s, 0,0,0, counterSlot, nullptr, {}});
            freeTemp(s);
            lex = save;
        }
        // Re-test the limit at the top rather than relying on equality alone: a step that jumps
        // PAST the limit would never make counter == limit, and the loop would run away.
        {
            VReg c = alloc(), l = alloc();
            emit({IrOp::Reload, c, 0,0,0,0, counterSlot, nullptr, {}});
            emit({IrOp::Reload, l, 0,0,0,0, limitSlot,   nullptr, {}});
            emit({IrOp::BranchGe, 0, c, l, 0,0, lDone, nullptr, {}});
            emit({IrOp::BranchNe, 0, c, l, 0,0, lTop,  nullptr, {}});
            freeTemp(l); freeTemp(c);
        }
        emit({IrOp::Label,    0, 0,0,0,0,             lDone, nullptr, {}});

        localCount = myLocal;                              // the loop variable leaves scope
        // ...and its two SLOTS are returned, so sequential loops reuse the same frame space instead
        // of each costing its own for the rest of the program. Nesting still stacks, which is what
        // makes a script bounded by frame size rather than by register count. `slotsUsed` keeps the
        // PEAK, because that is what the prologue has to reserve.
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        slotHighWater = counterSlot;
        return true;
    }

    /// One statement: a call, or a for.
    bool parseStatement() {
        if (lex.kind == Tok::Ident && lex.identLen == 3 &&
            std::strncmp(lex.identBeg, "for", 3) == 0) {
            return parseFor();
        }
        if (lex.kind != Tok::Ident) { fail("expected a function call"); return false; }
        parseCall(nullptr);
        if (failed) return false;
        return expect(Tok::Semicolon, "expected ';'");
    }

    bool parseProgram() {
        while (!failed && atTypeKeyword()) { lex.advance(); parseDecl(); }
        if (failed) return false;
        if (lex.kind == Tok::End) { fail("empty program (no statement)"); return false; }
        bool any = false;
        while (!failed && lex.kind != Tok::End) {
            if (!parseStatement()) return false;
            any = true;
        }
        if (!any) { fail("expected a statement"); return false; }
        return true;
    }
};

}  // namespace

CompileResult compileSource(const char* source, const BuiltinTable& table,
                            const SysVarTable& sysvars, uint8_t* out, size_t cap,
                            const RegBudget* squeeze, LowerFn lower) {
    CompileResult r;
    if (!source) { r.error = "no source"; return r; }
    if (!out || cap == 0) { r.error = "no code buffer"; return r; }

    // Size the op array to THIS script before parsing. The bound is per-TOKEN rather than
    // per-construct: no token the lexer can produce lowers to more than a handful of ops (the
    // densest is a call argument — evaluate, then the Call itself), so counting tokens and
    // multiplying is an over-estimate that cannot undershoot. Over-estimating costs a few unused
    // entries on a cold path; undershooting would fail a script that fits, so the direction of the
    // error is the whole point. push() still refuses past `cap`, so a wrong estimate degrades with
    // a diagnostic rather than corrupting memory.
    uint32_t tokens = 0;
    for (Lexer scan(source); scan.kind != Tok::End && scan.kind != Tok::Error; scan.advance()) {
        if (++tokens > kMaxIrOps) break;   // runaway source — reserve() rejects past the bound
    }
    IrProgram ir;
    // +8 covers a program's fixed overhead (the prologue/epilogue ops a tiny script still needs)
    // so a one-token source cannot round down to nothing.
    if (!ir.reserve(static_cast<uint16_t>(tokens * kIrOpsPerToken + 8 > kMaxIrOps
                                          ? kMaxIrOps : tokens * kIrOpsPerToken + 8))) {
        r.error = "script too large";
        return r;
    }
    Lexer lex(source);
    Parser parser{lex, table, sysvars, ir};
    if (!parser.parseProgram()) { r.error = parser.error; r.errorCol = parser.errorCol; return r; }
    // Hand the backend the frame the script's variables need. The register allocator numbers any
    // further slot from here up, so the two never overlap in the one frame they share.
    ir.localSlots = parser.slotsUsed;

    // `lower` is normally this build's own backend; a test passes a DIFFERENT ISA's lowerer to read
    // what a device would execute without flashing one. The front end is identical either way, which
    // is the point — the seam is one function pointer, not a second copy of the compiler.
    size_t len = lower ? lower(ir, out, cap, squeeze) : lowerToBytes(ir, out, cap, squeeze);
    if (len == 0) { r.error = kCodegenFailed; return r; }
    r.ok = true;
    r.len = len;
    // Surface the declared controls so the binding can create real MoonModule controls.
    r.controlCount = parser.controlCount;
    for (uint8_t i = 0; i < parser.controlCount; i++) r.controls[i] = parser.controls[i];
    return r;
}

}  // namespace mm::moonlive
