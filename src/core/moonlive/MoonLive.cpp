#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "platform/platform.h"

namespace mm::moonlive {


// Drop the prior compilation's CODE (the exec block + the typed fn pointers), but NOT the control
// arena — the arena's address must survive a recompile so a control pointer the binding bound to
// a slot stays valid (the arena only ever grows; see ensureArena). free() additionally releases
// the arena (full release).
void MoonLive::freeCode() {
    if (code_) platform::freeExec(code_, codeCap_);
    code_ = nullptr;
    codeCap_ = 0;
    codeLen_ = 0;
    fn_ = nullptr;
    anim_ = nullptr;
    ctrl_ = nullptr;
}

// Copy `len` already-emitted bytes into a fresh exec block. writeExec hides the ISA quirks
// (IRAM's 32-bit-store-only rule, the I-cache sync), so the engine stays target-agnostic.
// Returns the block (ready to call) or nullptr on failure (error_ set, prior state freed).
void* MoonLive::place(const uint8_t* staged, size_t len) {
    freeCode();   // drop any prior compilation's code — (re)compile is a clean re-emit (arena kept)
    if (len == 0) { error_ = "emit failed"; return nullptr; }
    // Allocate only what was emitted, word-rounded (writeExec stores 32-bit words on IRAM), not
    // the worst-case kCodeCap — a fill is ~50 bytes, a four-call setRGB ~600. The staging buffer
    // is sized for the worst case; the live exec block is sized for THIS program.
    size_t cap = (len + 3) & ~size_t(3);
    void* block = platform::allocExec(cap);
    if (!block) { error_ = "no executable memory"; return nullptr; }
    platform::writeExec(block, staged, len);
    code_ = block;
    codeCap_ = cap;
    codeLen_ = len;
    error_ = "";
    return block;
}

// The emitted-code staging buffer, on the HEAP.
//
// It was `uint8_t staging[kCodeCap]` — 2 KB of stack, in a call chain that also holds the
// assembler's own 2 KB buffer and its tables: ~4.7 KB in one go. On a classic ESP32 that overflowed
// the task the compile runs on, and the fault surfaced as `Double exception` inside
// _xt_context_save (the handler faulting while saving context) with LBEG pointing back into
// MoonLive::compile — a crash on the HTTP task from naming a script. Compilation is cold path, so
// the allocation costs nothing that matters, and this is the same reasoning that moved IrProgram's
// op array off the stack.
// Sized per compile rather than to kCodeCap: that constant is now the SANITY bound (16 KB), and
// allocating it for every script would trade one wall for a heap cost on the smallest script.
namespace {
struct Staging {
    explicit Staging(size_t bytes) : p(static_cast<uint8_t*>(platform::alloc(bytes))), n(bytes) {}
    ~Staging() { platform::free(p); }
    Staging(const Staging&) = delete;              // owns a buffer; a copy would double-free
    Staging& operator=(const Staging&) = delete;
    explicit operator bool() const { return p != nullptr; }
    uint8_t* p;
    size_t   n;
};
}  // namespace

bool MoonLive::compile(uint8_t r, uint8_t g, uint8_t b) {
    // A fixed blob with no source to measure, so the sanity bound IS the size — emitFill emits a
    // few dozen bytes and the exec block is allocated to the real length.
    Staging staging(codeCapFor(0));
    if (!staging) { error_ = "no memory to compile"; return false; }
    size_t len = emitFill(staging.p, staging.n, r, g, b);
    void* block = place(staging.p, len);
    if (!block) return false;
    fn_ = reinterpret_cast<FillFn>(block);
    return true;
}

bool MoonLive::compile(const char* source, const BuiltinTable& table, const SysVarTable& sysvars) {
    Staging staging(codeCapFor(countTokens(source)));
    if (!staging) { freeCode(); error_ = "no memory to compile"; return false; }
    CompileResult cr = compileSource(source, table, sysvars, staging.p, staging.n);
    if (!cr.ok) { freeCode(); error_ = cr.error; return false; }   // surface the parse diagnostic
    // Allocate the control arena (fixed address) and seed new slots, BEFORE publishing the control
    // set — ensureArena reads the previous controlCount_ to know which slots are new.
    if (!ensureArena(cr.controls, cr.controlCount)) { freeCode(); error_ = "no control memory"; return false; }
    // Place the code. Only after it succeeds do we publish the new control set — a failed place()
    // must not leave declaredControls() advertising controls for code that isn't running.
    void* block = place(staging.p, cr.len);
    if (!block) return false;                                      // controlCount_/controls_ unchanged
    // Clamp any kept slot whose range shrank (e.g. @control 0..99 edited to 0..10) so a stale live
    // value can't fall outside the new bounds before the native code reads it.
    for (uint8_t i = 0; i < cr.controlCount && i < controlCount_; i++) {
        uint8_t lo = static_cast<uint8_t>(cr.controls[i].min), hi = static_cast<uint8_t>(cr.controls[i].max);
        if (ctrlArena_[i] < lo) ctrlArena_[i] = lo;
        else if (ctrlArena_[i] > hi) ctrlArena_[i] = hi;
    }
    controlCount_ = cr.controlCount;
    for (uint8_t i = 0; i < cr.controlCount; i++) {
        controls_[i] = cr.controls[i];
        // Re-point `name` at our own copy: the parser's pointer is into the source text, which the
        // caller may free as soon as this returns.
        const uint8_t len = cr.controls[i].nameLen < kMaxControlName - 1
                          ? cr.controls[i].nameLen : static_cast<uint8_t>(kMaxControlName - 1);
        for (uint8_t j = 0; j < len; j++) ctrlNames_[i][j] = cr.controls[i].name[j];
        ctrlNames_[i][len] = '\0';
        controls_[i].name = ctrlNames_[i];
    }
    ctrl_ = reinterpret_cast<CtrlFn>(block);
    return true;
}

// Ensure the control arena exists and seed newly-declared slots. The arena is allocated ONCE at
// full kMaxCtrls capacity and never reallocated, so its address — and every control pointer the
// binding bound to a slot — is fixed for the engine's lifetime (the stable-slot contract
// controlSlot() promises; a recompile that adds a control must not move a pointer the previous
// defineControls already published). kMaxCtrls bytes is a handful; the up-front allocation is
// cheaper than the move-and-rebind it avoids. A NEW slot (beyond the previous count) is seeded
// from its declared default; an EXISTING slot keeps its live value (a source edit that keeps the
// control preserves the slider position). Returns false on alloc failure.
bool MoonLive::ensureArena(const DeclaredControl* decls, uint8_t count) {
    if (!ctrlArena_) {
        ctrlArena_ = static_cast<uint8_t*>(platform::alloc(kArenaBytes));
        if (!ctrlArena_) return false;
        for (uint8_t i = 0; i < kArenaBytes; i++) ctrlArena_[i] = 0;
    }
    for (uint8_t i = controlCount_; i < count; i++) ctrlArena_[i] = static_cast<uint8_t>(decls[i].def);
    return true;
}

bool MoonLive::compileAnimated() {
    Staging staging(codeCapFor(0));   // a fixed blob, like compile(r,g,b) — no source to measure
    if (!staging) { error_ = "no memory to compile"; return false; }
    size_t len = emitAnimatedFill(staging.p, staging.n);
    void* block = place(staging.p, len);
    if (!block) return false;
    anim_ = reinterpret_cast<AnimFn>(block);
    return true;
}

void MoonLive::free() {
    freeCode();                       // exec block + fn pointers
    if (ctrlArena_) platform::free(ctrlArena_);   // full release also releases the control arena
    ctrlArena_ = nullptr;
    controlCount_ = 0;
}

}  // namespace mm::moonlive
