#pragma once

#include <cstdio>

#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveIr.h"   // kArg3 — the register `t` is passed in

#include <atomic>
#include <cstdint>

#include "core/math8.h"    // beatsin16 — the shared time vocabulary
#include "core/math16.h"   // beat16 / triwave16 — full-range waveforms
#include "light/draw.h"    // draw::line, the shared 3D Bresenham a script draws with

// MoonLive — the LIGHT-DOMAIN built-in registration. This is the only place the LED vocabulary
// lives: the function NAMES (`setRGB`, `fill`, `random16`), their arg counts, and the meaning
// of the inline opcodes (StoreElem = an RGB pixel write, FillElems = fill every light). The core
// compiler sees only the neutral BuiltinTable / InlineOp tags this file hands it. A different
// host (display, sensor) would write its own registration with its own names; the core is
// unchanged. (The ESPLiveScript / ARTI bound-function model, doc §3.4.)

namespace mm::moonlive {

// random16(n) → a pseudo-random value in [0, n). A simple LCG, deterministic enough that the
// runtime Bounds guard always sees an in-range index; the same implementation on every target
// so a script behaves identically. The one host helper exposed as a Call so far.
extern "C" inline uint32_t mm_light_random16(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t n = uint32_t(args[0]);
    // ATOMIC, because two threads run scripts at once: the render task walks a layout for the frame
    // while the HTTP task asks the same layout for its light count after a control edit (the reason
    // the addLight sink is a per-thread table below). A plain `static` here is a data race, and a
    // lost update would additionally let two draws return the SAME value, which for a "random"
    // helper is a correctness bug rather than a tolerable one. compare_exchange keeps the sequence
    // exactly the LCG's, just serialized.
    static std::atomic<uint32_t> seed{0x2545F491u};
    uint32_t prev = seed.load(std::memory_order_relaxed), next;
    do {
        next = prev * 1664525u + 1013904223u;
    } while (!seed.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return n ? (next >> 16) % n : 0u;
}

// mod(a, b) → a % b, the wrap a cyclic animation needs. `t` grows without bound, so every effect
// that repeats has to fold it back into a range: `mod(t * speed, width)` is a sweep that returns to
// the start instead of running off the end once and never coming back.
//
// A Call rather than an operator because no ISA here has a cheap integer divide — Xtensa has none at
// all, and emitting a division routine inline would cost more code than the whole script. One host
// function, called like any other builtin, keeps the emitted code small and the three backends
// identical. b == 0 returns 0 rather than trapping: a script must degrade, never fault.
extern "C" inline uint32_t mm_light_mod(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t a = uint32_t(args[0]), b = uint32_t(args[1]);
    return b ? a % b : 0u;
}

// beat(bpm) / beatsin(bpm, low, high) → the TIME vocabulary an animation is actually written in.
//
// An effect does not think in milliseconds, it thinks in beats: `beat` is a rising sawtooth at a
// given BPM, `beatsin` a sine oscillating between two bounds. Both wrap math8.h's beat8/beatsin16 —
// the same functions the compiled effects use (GEQ3D, FreqSaws, Lines) with the same FastLED
// semantics, so a script writes what an effect writer writes.
//
// SIXTEEN bit, not eight. A script's values are 32-bit, so an 8-bit beat would throw away range for
// nothing and cap a sweep at 255 — short of the 128x128 walls this drives, and short of what
// LinesEffect itself computes (a 16-bit beat scaled by the axis length). The full-scale range means
// `beat(30) * width` and a shift is the sweep position on ANY fixture size.
//
// `ms` is an explicit argument — a script writes `beat(30, t)`. Threading the clock implicitly was
// tried and is worse: a Call receives exactly the arguments the script names, so an implicit `ms`
// arrives as zero and the animation silently stands still. Explicit also matches the C++ signature
// (beat16(bpm, ms)), so a script and an effect read the same. The modulo and divide these need live
// in the host function, which is why they are Calls — no ISA here has a cheap integer divide.
extern "C" inline uint32_t mm_light_beat(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]);
    return beat16(static_cast<uint8_t>(bpm), ms);
}
extern "C" inline uint32_t mm_light_beatsin(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]), high = uint32_t(args[2]);
    // low is 0 and high is the caller's: a Call carries three arguments and bpm + ms take two, so
    // the common "oscillate from 0 up to N" form is the one exposed rather than a packed pair.
    return beatsin16(static_cast<uint8_t>(bpm), ms, 0, static_cast<uint16_t>(high));
}

// scale(value, n) → map a 0..65535 value onto 0..n-1. The other half of `beat`: a beat is full-scale
// by design so it is fixture-independent, and this is what lands it on an actual axis. `beat(30, t)`
// then `scale(…, width)` is the sweep position, which is exactly what LinesEffect computes
// (`beat * n / 65536`) — including the detail that it REACHES n-1, where the naive `/ 65535` form
// truncates one short and the last column never lights.
// sin(angle) / cos(angle) — the full-turn wave, angle 0..65535 for one revolution.
//
// math16's sin16/cos16 return SIGNED -32768..32767; a script's values are unsigned, so the result
// is biased into 0..65535 with the zero line at 32768. A script that wants a coordinate scales the
// result: `scale(sin(a), width)` sweeps the whole axis, which is the same `scale` a beat uses.
extern "C" inline uint32_t mm_light_sin(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t angle = uint32_t(args[0]);
    return static_cast<uint32_t>(sin16(static_cast<angle16>(angle)) + 32768);
}
extern "C" inline uint32_t mm_light_cos(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t angle = uint32_t(args[0]);
    return static_cast<uint32_t>(cos16(static_cast<angle16>(angle)) + 32768);
}

// turn(n) → the angle step that divides one full revolution into n parts. A full turn is 65536 —
// one past the largest number a script can write — so even with a divide operator the expression
// could not be spelled. A circle therefore needs this as a builtin rather than as arithmetic.
extern "C" inline uint32_t mm_light_turn(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t n = uint32_t(args[0]);
    return n ? 65536u / n : 0u;
}

extern "C" inline uint32_t mm_light_scale(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t value = uint32_t(args[0]), n = uint32_t(args[1]);
    return (value * n) >> 16;
}

// print(v) → write one value to the serial log, and return it so `print` can be dropped into an
// expression without changing what it computes (`setXYZ(0, print(x), y, z)` still stores x).
//
// This is the only way to see INSIDE a running script. A script that compiles cleanly and produces
// a black fixture gives no other clue: every part reports success and the result is simply wrong.
// That case cost a long debugging session before this existed.
//
// **Rate-limited, because the call sites are per-light.** A modifier's script runs once per light
// per mapping rebuild — 16,384 times on a 128x128 wall. Printing all of them would flood the serial
// line, stall the render (a UART write blocks) and bury the first values, which are the useful
// ones. So a burst is capped and the rest are counted, not printed: the tail of a flood tells you
// nothing the head did not.
/// The remaining print budget. A binding resets it when it compiles, so every edit of a script gets
/// a fresh window — without that, one burst silences the debugging tool for the life of the process,
/// which is exactly when a second look at a misbehaving script is most needed.
/// ATOMIC for the same reason random16's seed is: two threads run scripts concurrently. The decrement
/// below is the one that matters: read-modify-write on a plain uint32_t lets two threads both see 1,
/// both decrement, and the budget WRAP to ~4 billion, turning the bound that keeps `print` off the
/// render tick's critical path into no bound at all.
inline std::atomic<uint32_t>& printBudget() { static std::atomic<uint32_t> n{0}; return n; }

/// Grant a fresh burst. Call from the binding's prepare(), alongside the compile.
///
/// print() writes to serial, which blocks, and an effect script runs on the render tick — so the
/// burst is what bounds the cost: a handful of writes per compile, after which the call is a compare
/// and a return. Draining through a queue would take the last of it off the tick; backlogged.
inline void resetPrintBudget() { printBudget().store(32, std::memory_order_relaxed); }

extern "C" inline uint32_t mm_light_print(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t v = uint32_t(args[0]);
    // Claim one unit before printing, and only if there is one: a plain `if (left > 0) --left`
    // can underflow past zero when two threads pass the test together.
    auto& left = printBudget();
    uint32_t have = left.load(std::memory_order_relaxed);
    while (have > 0 && !left.compare_exchange_weak(have, have - 1, std::memory_order_relaxed)) {}
    if (have > 0) {
        std::printf("[script] %u\n", static_cast<unsigned>(v));
        if (have == 1) std::printf("[script] (burst spent; edit the script for a fresh one)\n");
    }
    return v;
}

// addLight(x, y, z) → place one light at a position. The call a scripted LAYOUT is built on.
//
// A layout cannot write into a buffer the way an effect does: it does not know how many lights it
// will place until it has placed them, and on a classic ESP32 a 16k-light fixture would need 48 KB
// of coordinate staging — memory that board does not have. So the script CALLS OUT instead, once
// per light, and the host decides what to do with each: count it on the sizing pass, emit it into
// the consumer's sink on the walk. Nothing is stored.
//
// The active sink is set by the binding around each run. Outside a run it is null and a call is
// ignored — a script that reaches addLight from an effect places nothing rather than corrupting
// something.
using AddLightFn = void (*)(void* ctx, uint16_t x, uint16_t y, uint16_t z);

/// PER-THREAD, not one global: the sink belongs to whichever thread is running a script, and more
/// than one does. A layout is asked for its light count and its coordinates from the HTTP task when a
/// control is edited, while the render task walks the same layout for the frame — as one global, one
/// thread cleared the sink while the other was mid-run and the built-in called through a live
/// function pointer with a null context. That is a null dereference on the render core, seen as an
/// intermittent crash while resizing a scripted layout.
///
/// Keyed on platform::currentThreadId() rather than C++ `thread_local`, which is UNUSABLE on the
/// ESP32: the compiler reaches TLS through the THREADPTR special register, and a FreeRTOS task
/// created without TLS has THREADPTR = 0 — so the access dereferences a small offset from null and
/// dies inside the exception handler. Measured: EXCVADDR 0xfffffff0, `Double exception` in
/// _xt_context_save, on every scripted LAYOUT (the only binding whose script calls a host function).
/// Reading the task handle costs one load and needs no per-task setup.
///
/// The function and the context are ONE struct so they cannot be observed half-updated. Two slots:
/// the render task and whichever task edits a control are the two that ever run a script at once,
/// and a third would mean a genuinely new concurrency story rather than a bigger table.
struct AddLightSink { AddLightFn fn = nullptr; void* ctx = nullptr; };

namespace detail {
// `owner` is ATOMIC and claimed with compare_exchange: the claim used to be a load then a store,
// so two threads could both see the same slot free and both take it — leaving them sharing one
// sink, which is the very aliasing this table exists to prevent.
//
// The slot is the ONE per-thread home for everything a running script's built-ins reach: the
// addLight sink (a layout run installs it) and the draw canvas (an effect run installs it). A
// second table would repeat the claim/release machinery for the same lifetime.
struct SinkSlot { std::atomic<uintptr_t> owner{0}; AddLightSink sink; draw::Canvas canvas; };
/// Two slots: the render task and whichever task edits a control are the two that ever run a script
/// at once. A third concurrent runner gets the overflow slot, which holds no sink — so its addLight
/// calls no-op instead of writing through someone else's context.
// constinit at namespace scope, not a function-local static: a local static carries a thread-safe
// initialisation guard, which is a lock, and this is read from the render tick. Constant
// initialisation happens before main, so the accessor is a plain address.
inline constinit SinkSlot gSinkSlots[2]{};
inline SinkSlot* sinkSlots() MM_NONBLOCKING { return gSinkSlots; }
/// PERMANENTLY EMPTY. A third concurrent runner reads this and finds no sink, so its addLight calls
/// no-op — setAddLightSink deliberately never installs here, because a shared sink would let two
/// overflow threads write through each other's context.
inline const AddLightSink& sinkOverflow() { static const AddLightSink s; return s; }
/// The canvas twin of sinkOverflow: data stays null, so a third runner's draw calls no-op.
inline const draw::Canvas& canvasOverflow() { static const draw::Canvas c{}; return c; }

/// The slot this thread owns; with `claim`, take a free one when none is owned yet. Null when
/// unowned and not claiming, or when both slots belong to other threads (the overflow case).
inline SinkSlot* ownedSlot(bool claim) MM_NONBLOCKING {
    const uintptr_t me = platform::currentThreadId();
    SinkSlot* slots = sinkSlots();
    for (uint8_t i = 0; i < 2; i++)
        if (slots[i].owner.load(std::memory_order_acquire) == me) return &slots[i];
    if (!claim) return nullptr;
    for (uint8_t i = 0; i < 2; i++) {
        uintptr_t free = 0;
        if (slots[i].owner.compare_exchange_strong(free, me, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
            return &slots[i];
    }
    return nullptr;
}
/// Release only a fully empty slot: the sink and the canvas detach independently, and a release
/// while the other half is live would hand this thread's context to the next claimer.
inline void releaseIfEmpty(SinkSlot* s) MM_NONBLOCKING {
    if (s && !s->sink.fn && !s->sink.ctx && !s->canvas.data)
        s->owner.store(0, std::memory_order_release);
}
}  // namespace detail

/// This thread's sink. A READ never claims: a freshly claimed slot is empty by construction, so
/// claiming here could only ever return the empty sink it just made, while pinning a slot that
/// nothing will release (only the detach paths release, and a thread that installed nothing never
/// takes one). A script calling addLight from a binding that installs no sink (a modifier) would
/// hold a slot for the life of its task, and two such tasks would exhaust the table and silently
/// stop every later install. Installing claims; reading does not.
inline const AddLightSink& addLightSink() {
    detail::SinkSlot* s = detail::ownedSlot(false);
    return s ? s->sink : detail::sinkOverflow();
}

/// Point addLight at a consumer for the duration of one run; pass nullptr to detach.
///
/// Detaching RELEASES this thread's slot (unless the canvas half is still live), so two slots are
/// not exhausted by tasks that come and go: an HTTP request lands on whichever worker is free.
inline void setAddLightSink(AddLightFn fn, void* ctx) {
    if (!fn && !ctx) {
        // Clear FIRST, release last: the slot must not look free until the sink is cleared.
        detail::SinkSlot* s = detail::ownedSlot(false);
        if (s) { s->sink = {}; detail::releaseIfEmpty(s); }
        return;   // unowned: the overflow holds no sink to clear (see below)
    }
    // Install ONLY into an owned slot. Writing through addLightSink() would install into the shared
    // overflow sink when both slots are taken, and a second overflow thread would then run through
    // the first one's context — the exact aliasing the two-slot table exists to prevent. A third
    // concurrent runner instead gets no sink at all, so its addLight calls no-op: visibly nothing
    // placed, rather than lights written through another thread's layout.
    detail::SinkSlot* s = detail::ownedSlot(true);
    if (s) s->sink = {fn, ctx};
}

extern "C" inline uint32_t mm_light_addLight(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]), z = uint32_t(args[2]);
    // Both halves checked: a sink is only ever installed as a pair, but a context of null with a live
    // function is exactly what the crash was, so the guard states the whole precondition.
    const AddLightSink s = addLightSink();
    if (s.fn && s.ctx)
        s.fn(s.ctx, static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(z));
    return 0;
}

/// The canvas the DRAW builtins render into, valid for the duration of one run(). The binding
/// installs it just before engine.run and detaches (a default Canvas) right after. It lives in
/// the same per-thread slot as the addLight sink, the one home for what a running script's
/// built-ins may reach, because C++ `thread_local` is unusable on the ESP32 (see the sink's
/// comment: THREADPTR is 0 on a task without TLS). A binding that installs nothing (a layout, a
/// modifier) leaves the data pointer null and every draw call no-ops: visibly nothing drawn,
/// never a write through another binding's buffer.
inline const draw::Canvas& drawCanvas() {
    detail::SinkSlot* s = detail::ownedSlot(false);   // a read never claims, see addLightSink
    return s ? s->canvas : detail::canvasOverflow();
}
inline void setDrawCanvas(const draw::Canvas& cv) MM_NONBLOCKING {
    if (!cv.data) {
        // Clear FIRST, release last: the same order the sink detach keeps.
        detail::SinkSlot* s = detail::ownedSlot(false);
        if (s) { s->canvas = {}; detail::releaseIfEmpty(s); }
        return;
    }
    detail::SinkSlot* s = detail::ownedSlot(true);
    if (s) s->canvas = cv;
}

/// line(x1, y1, x2, y2, r, g, b) → a straight segment on the effect's canvas, z = 0.
///
/// The first seven-argument builtin, riding the args-array call ABI (every Call builtin receives
/// its script arguments as a memory array, so arity is a data question, not a register one).
/// Endpoints are CLAMPED to the canvas extents before drawing: script arithmetic is unsigned and
/// wraps, so a "negative" coordinate arrives as a huge value, and an unclamped pair would send the
/// Bresenham walker on a billions-of-steps march. The clamp turns that into a segment pinned to
/// the edge, visible and instant.
extern "C" inline uint32_t mm_light_line(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;
    const auto clampAxis = [](uintptr_t v, lengthType n) -> lengthType {
        return v >= uintptr_t(n) ? lengthType(n > 0 ? n - 1 : 0) : lengthType(v);
    };
    const Coord3D a{clampAxis(args[0], cv.dims.x), clampAxis(args[1], cv.dims.y), 0};
    const Coord3D b{clampAxis(args[2], cv.dims.x), clampAxis(args[3], cv.dims.y), 0};
    draw::line(cv, a, b, RGB{uint8_t(args[4]), uint8_t(args[5]), uint8_t(args[6])});
    return 0;
}

// The light-domain SYSTEM VARIABLES: names the host defines and a script may only read. Reserved,
// so a script cannot declare one and a name means the same thing in every script.
//
// `t` is an argument register (free to read); the rest are arena slots the BINDING writes each
// frame from the layer it renders into. Their offsets are fixed constants above the script's
// control range (see kMaxCtrls) — a binding caches these slot pointers, so they must never move.
//
// Adding one is a single line here plus the binding writing its slot.
enum : uint8_t {
    kSysWidth  = kMaxCtrls + 0,
    kSysHeight = kMaxCtrls + 1,
    kSysDepth  = kMaxCtrls + 2,
    kSysX      = kMaxCtrls + 3,
    kSysY      = kMaxCtrls + 4,
    kSysZ      = kMaxCtrls + 5,
};

/// The system variables a light script can read. Each binding registers the names it actually
/// WRITES, so an unwritten name stays unknown rather than reading a silent 0 — a script that asks
/// for something its host never supplies gets a compile error naming it, which is the honest answer.
///
/// Registering is also what RESERVES the name: a script cannot declare a control or a loop variable
/// that shadows one. Keeping the lists tight is therefore what leaves `x` and `y` usable as ordinary
/// loop counters in the two bindings that have no coordinate to hand out.
///
/// Adding one is a single line here plus the binding writing its slot.

/// `t` alone — every script animates, so every list starts here.
inline void addClock(SysVarTable& t) {
    // Elapsed milliseconds, passed in kArg3 on every run. An argument register, so it costs no
    // instruction and no arena byte.
    t.add({"t", SysVarKind::Arg, kArg3});
}

/// A LAYOUT: the clock, and nothing else. It is upstream of the logical grid — it contributes the
/// physical coordinates that several layouts together bound (architecture.md § Layouts) — so there
/// is no size to hand it, and it names its own controls (`cols`, `radius`).
inline SysVarTable layoutSysVars() {
    SysVarTable t;
    addClock(t);
    return t;
}

/// An EFFECT: the logical grid it renders into. The Layer derives width/height/depth from the
/// layouts and its modifier chain and writes them each tick; an effect is TOLD its canvas rather
/// than declaring it, because a size restated as a control is a second answer that can disagree.
inline SysVarTable effectSysVars() {
    SysVarTable t;
    addClock(t);
    t.add({"width",  SysVarKind::Arena, kSysWidth});
    t.add({"height", SysVarKind::Arena, kSysHeight});
    t.add({"depth",  SysVarKind::Arena, kSysDepth});
    return t;
}

/// A MODIFIER: the grid, plus the coordinate of the light being folded, which the binding writes
/// per call. This is the only binding that supplies x/y/z.
inline SysVarTable modifierSysVars() {
    SysVarTable t = effectSysVars();
    t.add({"x", SysVarKind::Arena, kSysX});
    t.add({"y", SysVarKind::Arena, kSysY});
    t.add({"z", SysVarKind::Arena, kSysZ});
    return t;
}

// The light-domain built-in table the binding injects into the compiler. setRGB and fill are
// Inline (they lower to stores — the hot-path writers, no per-call cost); random16 is a Call.
inline BuiltinTable lightBuiltins() {
    BuiltinTable t;
    // setRGB(index, r, g, b)  → write one pixel (bounds-guarded). Inline op StoreElem.
    t.add({"setRGB", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreElem});
    // setXYZ(index, x, y, z)  → write one POSITION (bounds-guarded). The same StoreElem as
    // setRGB: three values at index * stride. What differs is the destination the binding hands
    // run() — a colour buffer for an effect, a coordinate for a modifier — so one op serves both
    // and the engine stays free of any notion of what the three bytes mean.
    t.add({"setXYZ", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreElem});
    // fill(r, g, b)           → write every light. Inline op FillElems.
    t.add({"fill", 3, false, BuiltinKind::Inline, nullptr, InlineOp::FillElems});
    // mod(value, limit)      → value % limit. The wrap every cyclic animation needs; see above.
    t.add({"mod", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_mod, {}});
    // beat(bpm, t)           → 0..65535 sawtooth at bpm. The clock an animation is written against.
    t.add({"beat", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_beat, {}});
    // beatsin(bpm, t, high)  → a sine 0..high at bpm. The same shape an effect reaches for.
    t.add({"beatsin", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_beatsin, {}});
    // scale(value, n)        → a 0..65535 value onto 0..n-1. Lands a beat on an axis.
    t.add({"scale", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_scale, {}});
    // turn(n)                → one revolution split n ways, for stepping a circle.
    t.add({"turn", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_turn, {}});
    // random16(n)             → a value in [0,n). A Call to the host helper (typed fn pointer).
    // sin(angle) / cos(angle) → the circle. One turn is 0..65535, so a loop over N points steps
    // by 65536/N; the result is biased unsigned (see above).
    t.add({"sin", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_sin, {}});
    t.add({"cos", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_cos, {}});
    t.add({"random16", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_random16, {}});
    // print(v)                → log v and return it. The script-level debugger.
    t.add({"print", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_print, {}});
    // addLight(x, y, z)      → place a light. A scripted layout's whole vocabulary.
    t.add({"addLight", 3, /*returns*/ false, BuiltinKind::Call, &mm_light_addLight, {}});
    // line(x1, y1, x2, y2, r, g, b) → a segment on the canvas, via the shared draw::line.
    t.add({"line", 7, /*returns*/ false, BuiltinKind::Call, &mm_light_line, {}});
    return t;
}

}  // namespace mm::moonlive
