#pragma once

#include <cstdio>

#include "core/moonlive/MoonLiveBuiltins.h"

#include <cstdint>

#include "core/math8.h"    // beatsin16 — the shared time vocabulary
#include "core/math16.h"   // beat16 / triwave16 — full-range waveforms

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
extern "C" inline uint32_t mm_light_random16(uint32_t n, uint32_t, uint32_t) {
    static uint32_t s = 0x2545F491u;
    s = s * 1664525u + 1013904223u;
    return n ? (s >> 16) % n : 0u;
}

// mod(a, b) → a % b, the wrap a cyclic animation needs. `t` grows without bound, so every effect
// that repeats has to fold it back into a range: `mod(t * speed, width)` is a sweep that returns to
// the start instead of running off the end once and never coming back.
//
// A Call rather than an operator because no ISA here has a cheap integer divide — Xtensa has none at
// all, and emitting a division routine inline would cost more code than the whole script. One host
// function, called like any other builtin, keeps the emitted code small and the three backends
// identical. b == 0 returns 0 rather than trapping: a script must degrade, never fault.
extern "C" inline uint32_t mm_light_mod(uint32_t a, uint32_t b, uint32_t) {
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
extern "C" inline uint32_t mm_light_beat(uint32_t bpm, uint32_t ms, uint32_t) {
    return beat16(static_cast<uint8_t>(bpm), ms);
}
extern "C" inline uint32_t mm_light_beatsin(uint32_t bpm, uint32_t ms, uint32_t high) {
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
extern "C" inline uint32_t mm_light_sin(uint32_t angle, uint32_t, uint32_t) {
    return static_cast<uint32_t>(sin16(static_cast<angle16>(angle)) + 32768);
}
extern "C" inline uint32_t mm_light_cos(uint32_t angle, uint32_t, uint32_t) {
    return static_cast<uint32_t>(cos16(static_cast<angle16>(angle)) + 32768);
}

// turn(n) → the angle step that divides one full revolution into n parts. A full turn is 65536 —
// one past the largest number a script can write — so even with a divide operator the expression
// could not be spelled. A circle therefore needs this as a builtin rather than as arithmetic.
extern "C" inline uint32_t mm_light_turn(uint32_t n, uint32_t, uint32_t) {
    return n ? 65536u / n : 0u;
}

extern "C" inline uint32_t mm_light_scale(uint32_t value, uint32_t n, uint32_t) {
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
inline uint32_t& printBudget() { static uint32_t n = 0; return n; }

/// Grant a fresh burst. Call from the binding's prepare(), alongside the compile.
///
/// print() writes to serial, which blocks, and an effect script runs on the render tick — so the
/// burst is what bounds the cost: a handful of writes per compile, after which the call is a compare
/// and a return. Draining through a queue would take the last of it off the tick; backlogged.
inline void resetPrintBudget() { printBudget() = 32; }

extern "C" inline uint32_t mm_light_print(uint32_t v, uint32_t, uint32_t) {
    uint32_t& left = printBudget();
    if (left > 0) {
        std::printf("[script] %u\n", static_cast<unsigned>(v));
        if (--left == 0) std::printf("[script] (burst spent; edit the script for a fresh one)\n");
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

/// THREAD_LOCAL, not one global: the sink belongs to whichever thread is running a script, and more
/// than one does. A layout is asked for its light count and its coordinates from the HTTP task when a
/// control is edited, while the render task walks the same layout for the frame — as one global, one
/// thread cleared the sink while the other was mid-run and the built-in called through a live
/// function pointer with a null context. That is a null dereference on the render core, seen as an
/// intermittent crash while resizing a scripted layout.
///
/// The function and the context are ONE struct so they cannot be observed half-updated. Same shape
/// as the WDT subscription flag in the ESP32 worker, which had the same bug for the same reason.
struct AddLightSink { AddLightFn fn = nullptr; void* ctx = nullptr; };
inline AddLightSink& addLightSink() { static thread_local AddLightSink s; return s; }

/// Point addLight at a consumer for the duration of one run; pass nullptr to detach.
inline void setAddLightSink(AddLightFn fn, void* ctx) { addLightSink() = {fn, ctx}; }

extern "C" inline uint32_t mm_light_addLight(uint32_t x, uint32_t y, uint32_t z) {
    // Both halves checked: a sink is only ever installed as a pair, but a context of null with a live
    // function is exactly what the crash was, so the guard states the whole precondition.
    const AddLightSink s = addLightSink();
    if (s.fn && s.ctx)
        s.fn(s.ctx, static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(z));
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
    return t;
}

}  // namespace mm::moonlive
