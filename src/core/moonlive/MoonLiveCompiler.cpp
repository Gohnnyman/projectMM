#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

#include <cstring>   // std::strncmp (keyword matching)

namespace mm::moonlive {

namespace {

// --- Lexer ---------------------------------------------------------------------------
// A `//` line comment is whitespace. `Assign` is `=` (a member declaration's initializer).
enum class Tok { Ident, Number, String, Assign, LParen, RParen, LBrace, RBrace, Comma, Semicolon,
                 Plus, Minus, Star, Less, End, Error };

struct Lexer {
    const char* p;
    Tok kind = Tok::Error;
    long number = 0;
    const char* identBeg = nullptr;
    size_t identLen = 0;
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
            // A line comment is whitespace, with no exception.
            if (p[0] == '/' && p[1] == '/') {
                p += 2;
                // Every line comment is whitespace. A comment that CHANGED BEHAVIOR lived here:
                // `// @control 1..120` declared a control's range, which is not C and does not
                // resemble the compiled module a script stands in for. `defineControls()` calling
                // `addUint8("bpm", bpm, 1, 120)` replaced it, so the token, its capture and the
                // `lineStart` this needed are all gone.
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
        // A quoted string: a control's UI label. The span goes in identBeg/identLen, the same
        // fields an identifier uses, because both are a run of source bytes the parser reads
        // without copying. No escapes: a control name with a quote or a newline in it is not a
        // thing anyone needs, and the absence is one less rule to document.
        if (c == '"') {
            p++;
            identBeg = p;
            while (*p && *p != '"' && *p != '\n') p++;
            if (*p != '"') { kind = Tok::Error; err = "unterminated string"; return; }
            identLen = static_cast<size_t>(p - identBeg);
            p++;                                    // the closing quote
            kind = Tok::String; return;
        }
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
    char*              classNameOut = nullptr;   // the caller's buffer; the parser fills it
    // Each function the class defined, with the IR index its body starts at. An IR index, not a byte
    // offset: the parser runs before lowering, so the byte an entry lands on is not known yet. The
    // emitter converts one to the other, which is the same seam a linker crosses.
    struct FnMark { const char* name; uint8_t nameLen; uint16_t irStart; };
    FnMark             fns[kMaxEntryPoints] = {};
    uint8_t            fnCount = 0;
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

    // Every class-scope `uint8_t x = 0;` is a MEMBER: the class model, where a declaration inside
    // the class is a member of it. Whether the UI shows one is a separate question the script
    // answers by calling addUint8 in defineControls, so `controls` below is a VIEW of these rather
    // than a second storage: a control's offset IS its member's arena byte.
    //
    // `DeclaredControl` carries both because they are the same record. A member that no control
    // names simply never appears in the control list, and the binding creates no card for it.
    char*              strings = nullptr;      // the caller's pool; see compileSource
    uint16_t           stringCap = 0;
    uint16_t           stringLen = 0;
    DeclaredControl    members[kMaxCtrls] = {};
    uint8_t            memberCount = 0;

    const char*        error = "";
    uint16_t           errorCol = 0;
    bool               failed = false;

    void fail(const char* msg) { if (!failed) { failed = true; error = msg; errorCol = lex.col(); } }


    /// Copy a token's text into the program's string pool and return a pointer to it.
    ///
    /// The source buffer is freed the moment the compile returns, so a pointer into it would
    /// dangle before the emitted code ran. The pool travels with the compiled program instead,
    /// which is the same lifetime answer the engine already gives control and entry-point names.
    /// Null when the pool is full, which the caller turns into a diagnostic rather than a silent
    /// truncation.
    const char* internString(const char* text, size_t len) {
        if (!strings || stringLen + len + 1 > stringCap) return nullptr;
        char* at = strings + stringLen;
        for (size_t i = 0; i < len; i++) at[i] = text[i];
        at[len] = '\0';
        stringLen = static_cast<uint16_t>(stringLen + len + 1);
        return at;
    }


    /// Find a declared MEMBER by name; its index, or -1. Names are token spans into the source
    /// rather than NUL-terminated strings, so compare by length and bytes.
    int findMember(const char* name, size_t len) const {
        for (uint8_t i = 0; i < memberCount; i++)
            if (members[i].nameLen == len && std::strncmp(members[i].name, name, len) == 0)
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
                if (sv->kind == SysVarKind::Arg) {
                    VReg v = alloc();   // parked at entry — bring it back for this one read
                    emit({IrOp::Reload, v, 0,0,0,0, hostArgSlot(sv->where), nullptr, {}});
                    return v;
                }
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
            // A MEMBER read. A control is a member the UI shows, so this one lookup answers both:
            // the arena byte is the same byte either way.
            const int mi = findMember(lex.identBeg, lex.identLen);
            if (mi >= 0) {
                VReg v = alloc();
                emit({IrOp::LoadCtrl, v, 0,0,0,0, members[mi].offset, nullptr, {}});
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
        if (!fn) {
            // Not a built-in: the script's own function, if it declared one by this name. Resolved
            // against the class's function list rather than the builtin table, which is what makes
            // a helper callable and, when the name is the running function's own, what makes
            // recursion work: nothing here treats the two cases differently.
            //
            // Only functions ALREADY PARSED are visible. A forward call (to a helper declared
            // further down) is refused rather than half-supported, because resolving it needs a
            // second pass over the class body. The cost is that "unknown function" is what a
            // forward call reports too, with the column but not the name, so a helper has to be
            // declared above its caller. Recursion is unaffected: a function is added to the list
            // before its body is parsed, so it can see itself.
            for (uint8_t i = 0; i < fnCount; i++) {
                if (fns[i].nameLen != lex.identLen) continue;
                if (std::strncmp(fns[i].name, lex.identBeg, lex.identLen) != 0) continue;
                lex.advance();
                if (!expect(Tok::LParen, "expected '(' after the function name")) return;
                if (!expect(Tok::RParen, "a script function takes no arguments yet")) return;
                if (resultOut) { fail("a script function returns nothing yet"); return; }
                // `imm` is the callee's FUNCTION NUMBER, not its position in the op array. An IR
                // index would be the more obvious choice and was the first one, but the spill pass
                // rewrites the array and every index past its first insertion shifts: the call then
                // named a position that no longer started a function, and the lowering opened the
                // next frame mid-statement. A function number survives any rewrite of the ops.
                emit({IrOp::CallScript, 0, 0,0,0,0, static_cast<int32_t>(i), nullptr, {}});
                return;
            }
            fail("unknown function"); return;
        }
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
        // Stage every argument into a CONSECUTIVE frame slot as it is evaluated. The call then
        // carries the slot BASE and the COUNT rather than the values, so how many arguments a
        // builtin takes is bounded by frame slots — `draw::line` wants seven — and only one
        // argument occupies a register at a time.
        const uint8_t argBase = slotHighWater;
        uint8_t n = 0;
        if (lex.kind != Tok::RParen) {
            while (true) {
                if (n >= fn->argc) { fail("too many arguments"); return; }
                // Two argument forms an ordinary expression cannot carry, both needed so a control
                // is declared by the same call a compiled module makes:
                //
                //   a STRING, for the UI label. A frame slot is a machine word, so the pointer
                //   into the source fits; the host reads it as a `const char*`.
                //
                //   a MEMBER BY NAME, meaning its ADDRESS rather than its value. `addUint8("bpm",
                //   bpm, 1, 120)` reads as the reference a compiled module passes, and the compiler
                //   supplies the arena offset the host binds to. Only where the builtin asks for it
                //   (byRef), so `setRGB(bpm, …)` still reads bpm's value as it always did.
                VReg v = 0;
                const bool wantStr = (fn->byStr >> n) & 1u;
                if (wantStr && lex.kind != Tok::String) {
                    fail("this argument must be a name in quotes"); return;
                }
                // And a string ONLY where one is wanted: `setRGB("red", 0, 0, 0)` would otherwise
                // pass the low bits of a pointer as a color index.
                if (!wantStr && lex.kind == Tok::String) {
                    fail("this argument is a number, not a name in quotes"); return;
                }
                if (lex.kind == Tok::String) {
                    // The label is recorded HERE, at compile time, rather than travelling through
                    // a frame slot: the source buffer is freed after the compile, so a pointer the
                    // emitted code carried would dangle by the time the host read it. The engine
                    // already copies control names into its own pool, which is the same lifetime
                    // problem solved once. The slot still gets a value so the argument count is
                    // honest; the host reads the name from the control record, not from the slot.
                    // INTERNED, so the pointer outlives the source. The text is freed the moment
                    // the compile returns, and this pointer travels in the emitted code to a host
                    // that reads it later, so it cannot point into the source. The pool is part of
                    // the compiled program, exactly as the entry-point and control names already
                    // are: the same lifetime problem, solved the same way.
                    // Interned into the caller's pool, which outlives the compile, so the address
                    // is final the moment it is made and the emitted code carries it directly.
                    const char* interned = internString(lex.identBeg, lex.identLen);
                    if (!interned) { fail("no room for this script's strings"); return; }
                    v = alloc();
                    emit({IrOp::ConstPtr, v, 0,0,0,0, 0, nullptr, interned, {}});
                    lex.advance();
                } else if (fn->byRef && (fn->byRef >> n) & 1u) {
                    if (lex.kind != Tok::Ident) { fail("expected the member this control is bound to"); return; }
                    const int mi = findMember(lex.identBeg, lex.identLen);
                    if (mi < 0) { fail("no member of that name is declared in this class"); return; }
                    v = alloc();
                    emit({IrOp::Const, v, 0,0,0,0, members[mi].offset, nullptr, {}});
                    lex.advance();
                } else {
                    v = parseExpr();
                }
                if (failed) return;
                if (slotHighWater >= kMaxLocals) { fail("too many arguments to hold"); return; }
                emit({IrOp::Spill, 0, v, 0,0,0, slotHighWater++, nullptr, {}});
                freeTemp(v);                       // its register is free again immediately
                n++;
                if (lex.kind == Tok::Comma) { lex.advance(); continue; }
                break;
            }
        }
        if (slotHighWater > slotsUsed) slotsUsed = slotHighWater;
        if (n != fn->argc) { fail("wrong number of arguments"); return; }

        if (!expect(Tok::RParen, "expected ')'")) return;

        // The IR Call op carries a single argument vreg, so a Call-kind builtin must be unary.
        // (Today random16 is the only one.) Reject a multi-arg Call up front rather than silently
        // dropping args[1..]; a future N-ary helper needs the IR Call contract widened first.
        if (resultOut) {
            if (fn->kind != BuiltinKind::Call || !fn->returns) { fail("this function does not return a value"); return; }
            // The arguments live in the frame, so nothing is held in a register across the call:
            // `imm` carries the slot they start at and `b` how many there are.
            VReg r = alloc();
            emit({IrOp::Call, r, 0, n, 0, 0, argBase, fn->fn, {}});
            *resultOut = r;
        } else {
            // A statement call. Call kinds with a result are also allowed as statements (result
            // discarded); Inline kinds emit the inline op with their operands.
            if (fn->kind == BuiltinKind::Call) {
                VReg r = alloc();
                emit({IrOp::Call, r, 0, n, 0, 0, argBase, fn->fn, {}});
                freeTemp(r);
            } else {
                // An inline op reads its operands from REGISTERS (it is emitted as instructions, not
                // a call), so reload the staged arguments for the one op that consumes them.
                //
                // FOUR is the IrInst operand ceiling for an inline op. A builtin declaring more used
                // to be truncated here — the extra arguments evaluated, then silently dropped — which
                // emits a working-looking op that computes the wrong thing. Refuse instead: only a
                // CALL is unbounded (its arguments go through the frame), so a wider inline builtin
                // needs the IR widened first, not its arguments quietly discarded.
                if (n > 4) { fail("this function takes too many arguments to inline"); return; }
                VReg a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                VReg* slot[4] = {&a0, &a1, &a2, &a3};
                for (uint8_t i = 0; i < n && i < 4; i++) {
                    *slot[i] = alloc();
                    emit({IrOp::Reload, *slot[i], 0,0,0,0, static_cast<int32_t>(argBase + i), nullptr, {}});
                }
                emit({IrOp::Inline, 0, a0, a1, a2, a3, 0, nullptr, nullptr, fn->inlineOp});
                for (uint8_t i = 0; i < n && i < 4; i++) freeTemp(*slot[i]);
            }
        }
        // Give the staging slots back — but only down to where THIS call started, and only once its
        // result has been produced. A nested call (`setRGB(1, mod(t, 200), …)`) stages inside its
        // parent's argument block, so resetting to a fixed base would hand the inner call the slots
        // the outer one is still filling: measured as mod()'s arguments landing on setRGB's, and its
        // result then overwriting them. `slotsUsed` keeps the peak, which is what the prologue
        // reserves, so releasing here costs nothing and lets sequential calls reuse the space.
        slotHighWater = argBase;
    }

    // A MEMBER declaration: `uint8_t ident = number ;`. Whether the UI shows it is a separate
    // question the script answers by naming it in defineControls().
    // The leading `uint8_t` keyword is already consumed by the caller. Records a DeclaredControl.
    void parseDecl() {
        if (lex.kind != Tok::Ident) { fail("expected a member name after the type"); return; }
        const char* name = lex.identBeg; size_t nameLen = lex.identLen;
        if (nameLen >= kMaxControlName) { fail("member name too long"); return; }   // no silent truncation downstream
        if (sysvars.find(name, nameLen)) { fail("name is a system variable"); return; }
        // Against MEMBERS, which is where a declaration now lands. Checked against the controls
        // before, which stopped catching anything the moment a declaration became a member: two
        // members of one name would both exist, and every read would resolve to the first while
        // the second silently owned an arena byte nobody could reach.
        if (findMember(name, nameLen) >= 0) { fail("duplicate member name"); return; }
        // A control name must not shadow a builtin: a declared `random16` would make `random16(…)`
        // ambiguous (control read vs call). Reject it at the source so the resolution never collides.
        if (table.find(name, nameLen)) { fail("member name shadows a built-in function"); return; }
        lex.advance();
        if (!expect(Tok::Assign, "expected '=' in a member declaration")) return;
        if (lex.kind != Tok::Number) { fail("expected a default value (a number)"); return; }
        if (lex.number < 0 || lex.number > 255) { fail("uint8_t default out of range (0..255)"); return; }
        long def = lex.number;
        lex.advance();
        if (!expect(Tok::Semicolon, "expected ';' after the member declaration")) return;
        // A lexer error carries a specific message; surface it rather than letting it fall through
        // to a generic later parse failure.
        if (lex.kind == Tok::Error) { fail(lex.err); return; }
        // A MEMBER, and only that. Whether the UI shows it is a separate question the script
        // answers by naming it in `defineControls()`, so a declaration no longer carries a range:
        // the range belongs to the control, and a member that no control surfaces has none.
        if (memberCount >= kMaxCtrls) { fail("too many members"); return; }
        members[memberCount] = {name, 0, 255, static_cast<uint8_t>(def),
                                static_cast<uint8_t>(nameLen), CtrlType::Uint8, memberCount};
        memberCount++;
    }

    // Is the current Ident this exact keyword? Keywords are matched by text rather than lexed as
    // their own token kind: the set is tiny, and a script may still use `class` or `for` as part of
    // a longer identifier, which a length-checked compare gets right for free.
    bool atKeyword(const char* kw, size_t len) const {
        return lex.kind == Tok::Ident && lex.identLen == len && std::strncmp(lex.identBeg, kw, len) == 0;
    }
    // Is the current Ident the `uint8_t` type keyword (the only declared type in Stage 1)?
    bool atTypeKeyword() const { return atKeyword("uint8_t", 7); }

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
        // Bounded on slotHighWater, not just localCount: a call RELEASES its argument staging slots
        // (slotHighWater = argBase) without ever having counted them as locals, so the two can
        // diverge and the localCount check above is not sufficient on its own.
        if (slotHighWater >= kMaxLocals) { fail("too many loop variables"); return false; }
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
        if (slotHighWater >= kMaxLocals) { fail("too many loop variables"); return false; }
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
        if (atKeyword("for", 3)) return parseFor();
        if (lex.kind != Tok::Ident) { fail("expected a function call"); return false; }
        parseCall(nullptr);
        if (failed) return false;
        return expect(Tok::Semicolon, "expected ';'");
    }

    /// The body of one named function: `name() { statements }`, with the name already consumed.
    /// Stage 1 emits it INLINE at the point the class body reaches it, which is what makes `tick()`
    /// the whole program while it is the only entry point. Real per-function frames arrive with the
    /// call support in this same step; this is the parse shape they will attach to.
    bool parseFunctionBody() {
        if (!expect(Tok::LParen,  "expected '(' after the function name")) return false;
        if (!expect(Tok::RParen,  "expected ')': parameters arrive with typed members")) return false;
        if (!expect(Tok::LBrace,  "expected '{' to open the function body")) return false;
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End)
            if (!parseStatement()) return false;
        if (failed) return false;
        return expect(Tok::RBrace, "expected '}' to close the function body");
    }

    /// program := "class" NAME "{" { decl | function } "}"
    ///
    /// ONE top-level form. A bare statement list is no longer accepted: keeping it would mean two
    /// parse paths, two sets of rules to document and two things to test, permanently, so that the
    /// shortest scripts could stay one line shorter. The class declaration is what makes a script
    /// read as the module it stands in for, which is the whole point of the shape.
    bool parseProgram() {
        if (!atKeyword("class", 5)) { fail("a script is a class: expected `class <Name> { … }`"); return false; }
        lex.advance();
        if (lex.kind != Tok::Ident) { fail("expected a name after `class`"); return false; }
        // Copied, not borrowed: the source buffer is freed as soon as the compile returns, and the
        // name outlives it in the status line.
        const size_t n = lex.identLen < kMaxClassName ? lex.identLen : kMaxClassName;
        std::memcpy(classNameOut, lex.identBeg, n);
        classNameOut[n] = '\0';
        lex.advance();
        if (!expect(Tok::LBrace, "expected '{' to open the class body")) return false;

        // Declarations first (the controls), then the functions. Both live inside the braces now.
        while (!failed && atTypeKeyword()) { lex.advance(); parseDecl(); }
        if (failed) return false;

        bool any = false;
        while (!failed && lex.kind != Tok::RBrace && lex.kind != Tok::End) {
            if (lex.kind != Tok::Ident) { fail("expected a function, or '}' to close the class"); return false; }
            if (fnCount >= kMaxEntryPoints) { fail("too many functions in one class"); return false; }
            // The engine copies entry names into a fixed buffer, so a longer one would be
            // TRUNCATED there. Two functions sharing a 23-character prefix would then land under
            // the same name and `entry()` would return whichever came first: a call dispatched to
            // the wrong function, silently. Refused here, where a control name already is, so the
            // script author is told rather than the engine guessing.
            if (lex.identLen > kMaxEntryName) { fail("function name too long"); return false; }
            fns[fnCount] = {lex.identBeg, static_cast<uint8_t>(lex.identLen),
                            static_cast<uint16_t>(ir.count)};
            // The IR carries the start INDEX; the lowering turns it into a byte offset.
            ir.fnIrStart[fnCount] = static_cast<uint16_t>(ir.count);
            ir.fnCount = static_cast<uint8_t>(fnCount + 1);
            fnCount++;
            lex.advance();                       // the function name
            // Park the host arguments in this FUNCTION's frame. Read-only, so one store each, and
            // every later read is a Reload, which frees five registers for the body. Per function
            // rather than per program because each function owns its own frame now: a spill emitted
            // before the first prologue would write to a frame that does not exist yet.
            for (VReg v = 0; v < kFirstTemp; v++)
                emit({IrOp::Spill, 0, v, 0,0,0, hostArgSlot(v), nullptr, {}});
            if (!parseFunctionBody()) return false;
            any = true;
        }
        if (failed) return false;
        if (!any) { fail("a class with no function does nothing"); return false; }
        return expect(Tok::RBrace, "expected '}' to close the class");
    }
};

}  // namespace

uint32_t countTokens(const char* source) {
    if (!source) return 0;
    uint32_t tokens = 0;
    for (Lexer scan(source); scan.kind != Tok::End && scan.kind != Tok::Error; scan.advance()) {
        if (++tokens > kMaxIrOps) break;   // runaway source — reserve() rejects past the bound
    }
    return tokens;
}

CompileResult compileSource(const char* source, const BuiltinTable& table,
                            const SysVarTable& sysvars, uint8_t* out, size_t cap,
                            const RegBudget* squeeze, LowerFn lower,
                            char* strings, uint16_t stringCap) {
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
    const uint32_t tokens = countTokens(source);
    IrProgram ir;
    // +8 covers a program's fixed overhead (the prologue/epilogue ops a tiny script still needs)
    // so a one-token source cannot round down to nothing.
    if (!ir.reserve(static_cast<uint16_t>(tokens * kIrOpsPerToken + 8 > kMaxIrOps
                                          ? kMaxIrOps : tokens * kIrOpsPerToken + 8))) {
        r.error = "script too large";
        return r;
    }
    Lexer lex(source);
    Parser parser{lex, table, sysvars, ir, r.className};
    parser.strings = strings;      // where string literals are interned; see compileSource
    parser.stringCap = stringCap;
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
    r.memberCount = parser.memberCount;
    for (uint8_t i = 0; i < parser.memberCount; i++) r.members[i] = parser.members[i];
    // The functions the class defined, each with the byte its code starts at. The parser recorded
    // an IR index and the lowering converted it while emitting, so this is a real symbol table: a
    // binding asks for an entry by name and gets an address inside the one emitted block.
    r.entryCount = parser.fnCount;
    for (uint8_t i = 0; i < parser.fnCount; i++)
        r.entries[i] = {parser.fns[i].name, parser.fns[i].nameLen, ir.fnOffset[i]};
    return r;
}

}  // namespace mm::moonlive
