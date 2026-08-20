#pragma once

#include <cstdio>

#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLive.h"   // runDefineControls drives the engine
#include "core/moonlive/MoonLiveIr.h"   // kArg3 — the register `t` is passed in

#include <atomic>
#include <cstdint>

#include "core/math8.h"    // beatsin16 — the shared time vocabulary
#include "core/math16.h"   // beat16 / triwave16 — full-range waveforms
#include "core/noise.h"    // inoise8 — the shared value-noise field
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
// The palette, as THREE builtins — `paletteR(i, bri)`, `paletteG(i, bri)`, `paletteB(i, bri)`.
// `bri` is the brightness colorFromPalette already takes, and it is what gives a shape a
// radial falloff instead of a flat fill. A builtin returns
// one uint32_t, so a packed 0xRRGGBB would need the script to unpack it, and the language has no
// division or shift to do that with. Three calls is the shape that works today, and
// `setRGB(idx, paletteR(i), paletteG(i), paletteB(i))` reads clearly.
//
// The ACTIVE palette, so a script follows the device's palette control exactly as a compiled
// effect does — which is the whole point: before this, a script could only hard-code colour.
//
// Deliberately NO hsv() alongside it. A hue wheel is how an effect picks colour while IGNORING
// the user's palette, which is the habit the compiled effects were moved off (47 of 52 read the
// palette; the exceptions are effects where colour carries meaning, like the axis-identifying
// red/green/blue in LinesEffect). Giving scripts hsv() would reintroduce it as the easy default.
extern "C" inline uint32_t mm_light_paletteR(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(args[0]),
                            static_cast<uint8_t>(args[1])).r;
}
extern "C" inline uint32_t mm_light_paletteG(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(args[0]),
                            static_cast<uint8_t>(args[1])).g;
}
extern "C" inline uint32_t mm_light_paletteB(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), static_cast<uint8_t>(args[0]),
                            static_cast<uint8_t>(args[1])).b;
}

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

// noise(x, y, z) → the 0..255 value-noise field at that point, the primitive behind fire, clouds,
// plasma and lava. Coordinates are 16.0 fixed point: the HIGH byte picks the noise cell and the low
// byte interpolates within it, so `x * 256 / scale` zooms and feeding `t` into an axis makes the
// field flow. Three arguments is exactly a Call's budget, and 2D is the same call with z held at a
// constant — one builtin rather than an arity family.
extern "C" inline uint32_t mm_light_noise(const uintptr_t* args, uint32_t, const uint8_t*) {
    return inoise8(uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]));
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
// polarA(dx, dy) / polarR(dx, dy) — the POLAR pair (Angle, Radius), for an effect written around distance and
// bearing from a centre rather than around x/y. Both take offsets that a script computes as
// `x - cx`, which is unsigned and therefore wraps for a point left of centre: the builtins
// re-centre it themselves (see below), so a script does not have to reason about the wrap.
//
// polarA() returns an angle16 (65536 = one turn), so it feeds straight into sin()/cos(). polarR()
// returns the true distance, not the octagonal approximation, because a visibly non-circular
// "circle" is exactly what an effect using this would be trying to draw.
extern "C" inline uint32_t mm_light_polarA(const uintptr_t* args, uint32_t, const uint8_t*) {
    // A script's arithmetic is unsigned, so `x - cx` for x < cx arrives as a huge value rather
    // than a negative one. Anything above half the range is that wrap, and subtracting the range
    // recovers the signed offset the maths needs.
    int32_t dx = static_cast<int32_t>(uint32_t(args[0]));
    int32_t dy = static_cast<int32_t>(uint32_t(args[1]));
    if (dx > 32767) dx -= 65536;
    if (dy > 32767) dy -= 65536;
    return static_cast<uint32_t>(atan16(dy, dx));
}
extern "C" inline uint32_t mm_light_polarR(const uintptr_t* args, uint32_t, const uint8_t*) {
    int32_t dx = static_cast<int32_t>(uint32_t(args[0]));
    int32_t dy = static_cast<int32_t>(uint32_t(args[1]));
    if (dx > 32767) dx -= 65536;
    if (dy > 32767) dy -= 65536;
    return dist16(dx, dy);
}

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

/// Where a running `defineControls()` sends each `addUint8` / `addUint16`. Same shape and same
/// reason as the addLight sink: a builtin has no receiver, so the binding installs one for the
/// duration of the run and the call reaches the engine through it.
///
/// `type` is the width the SCRIPT declared, which is what decides the UI control and how many
/// arena bytes a write touches. It is checked against the member's own type by the compiler, so
/// by the time a call arrives here the two already agree.
using AddControlFn = void (*)(void* ctx, const char* name, uint8_t offset,
                              uint16_t lo, uint16_t hi, CtrlType type);
struct AddControlSink { AddControlFn fn = nullptr; void* ctx = nullptr; };

namespace detail {
// `owner` is ATOMIC and claimed with compare_exchange: the claim used to be a load then a store,
// so two threads could both see the same slot free and both take it — leaving them sharing one
// sink, which is the very aliasing this table exists to prevent.
//
// The slot is the ONE per-thread home for everything a running script's built-ins reach: the
// addLight sink (a layout run installs it) and the draw canvas (an effect run installs it). A
// second table would repeat the claim/release machinery for the same lifetime.
struct SinkSlot { std::atomic<uintptr_t> owner{0}; AddLightSink sink; draw::Canvas canvas;
                  AddControlSink controls; };
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
/// Release only a fully empty slot: the three halves (addLight sink, draw canvas, control sink)
/// detach independently, and a release while any of them is live would hand this thread's context
/// to the next claimer, whose script would then reach a dead engine through it.
inline void releaseIfEmpty(SinkSlot* s) MM_NONBLOCKING {
    if (s && !s->sink.fn && !s->sink.ctx && !s->canvas.data && !s->controls.fn)
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

/// The control sink for this thread, or an empty one. Reading does not claim a slot, for the same
/// reason addLightSink() does not: a binding that installs nothing must not hold a slot for life.
inline const AddControlSink& addControlSink() {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit AddControlSink none{};
    return s ? s->controls : none;
}

/// Point addUint8 at a consumer for the duration of one defineControls() run; nullptr to detach.
/// False when the two-slot table is full, which the caller must not treat as an installed sink:
/// every addUint8 would then be a silent no-op and the script would publish no controls at all.
inline bool setAddControlSink(AddControlFn fn, void* ctx) {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return false;
    s->controls = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
    return true;
}

/// Point addLight at a consumer for the duration of one run; pass nullptr to detach.
///
/// Detaching RELEASES this thread's slot (unless another half is still live), so two slots are
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

// addUint8(name, memberOffset, min, max): the run-time half of declaring a control.
//
// The CONTROL RECORD is built by the compiler, which knows the name span and the member's offset,
// so nothing has to travel through a frame slot into a source buffer that is freed by the time
// this runs. What is left is the call itself, which exists so that a script declares a control the
// way a compiled module does: `defineControls()` is an ordinary function the binding calls after a
// successful compile, and this is an ordinary builtin it calls.
// Shared by addUint8 and addUint16: identical but for the width they declare, so the bound check
// and the sink call live once rather than in two copies that could drift.
inline uint32_t addControlDecl(const uintptr_t* args, CtrlType type) {
    // args: (name, memberOffset, min, max). The name is a pointer into the compiled program's
    // string pool, which outlives the run; the offset is the member's arena byte, which the
    // compiler passed by reference.
    const char* name = reinterpret_cast<const char*>(args[0]);
    const AddControlSink s = addControlSink();
    if (!name || !s.fn || !s.ctx) return 0;      // no binding listening: the call is a no-op
    // The range is an ARBITRARY EXPRESSION, so `addUint8("n", n, 0, x * 64)` can compute past what
    // the declared width holds. Truncating would publish a slider whose top silently wraps to a
    // small number; refusing the declaration leaves the control absent, which the user can see.
    const uintptr_t limit = (type == CtrlType::Uint16) ? 65535u : 255u;
    if (args[2] > limit || args[3] > limit) return 0;
    // Same stance for an INVERTED range: with min > max the write path's `v < min || v > max` is
    // true for every value, so the slider would appear and then refuse everything the user does to
    // it. Refusing the declaration leaves it absent, which is visible.
    if (args[2] > args[3]) return 0;
    s.fn(s.ctx, name, static_cast<uint8_t>(args[1]),
         static_cast<uint16_t>(args[2]), static_cast<uint16_t>(args[3]), type);
    return 0;
}

extern "C" inline uint32_t mm_light_addUint8(const uintptr_t* args, uint32_t, const uint8_t*) {
    return addControlDecl(args, CtrlType::Uint8);
}

extern "C" inline uint32_t mm_light_addUint16(const uintptr_t* args, uint32_t, const uint8_t*) {
    return addControlDecl(args, CtrlType::Uint16);
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

/// setPaletteColor(x, y, index, brightness) → one pixel, coloured from the ACTIVE palette.
///
/// One call where a script used to write three: `paletteR/G/B` each returned a single channel, so
/// a palette pixel cost three host calls AND three evaluations of whatever expression produced the
/// brightness — the compiler evaluates each argument independently. Measured on an S3, that was
/// 1451 us flat vs 1940 us with a per-pixel falloff; folding it into one call removes two of the
/// three calls and two of the three brightness computations.
///
/// Takes x/y rather than a flat index so the buffer layout stops leaking into every script: a
/// script was writing `mod(bx + dx, width) + mod(by + dy, height) * width` at every call site.
/// Out-of-range coordinates are dropped, not wrapped — a "negative" coordinate arrives as a huge
/// unsigned value, and wrapping it would paint the wrong edge rather than nothing.
extern "C" inline uint32_t mm_light_setPaletteColor(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;                       // no canvas installed (a layout, a modifier)
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]);
    if (x >= uint32_t(cv.dims.x) || y >= uint32_t(cv.dims.y)) return 0;
    draw::pixel(cv, Coord3D{lengthType(x), lengthType(y), 0},
                colorFromPalette(*Palettes::active(),
                                 static_cast<uint8_t>(args[2]),
                                 static_cast<uint8_t>(args[3])));
    return 0;
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
// control range (see kCtrlBytes): a binding caches these slot pointers, so they must never move.
//
// Adding one is a single line here plus the binding writing its slot.
enum : uint8_t {
    kSysWidth  = kCtrlBytes + 0,
    kSysHeight = kCtrlBytes + 1,
    kSysDepth  = kCtrlBytes + 2,
    kSysX      = kCtrlBytes + 3,
    kSysY      = kCtrlBytes + 4,
    kSysZ      = kCtrlBytes + 5,
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

/// The entry points the light domain calls, by role. A script defines the ones its role needs; the
/// engine looks each up by name in the one emitted block.
///
/// A name is a MOMENT, not a role. The host owns the moments and calls whatever the script defined
/// for each one: `tick` when a frame is rendered, `placeLights` when lights are being placed,
/// `modifyLogical` when one coordinate is folded. An entry a script did not define is simply not
/// called, which is why nothing validates which names belong to which module.
///
/// This is what makes the bindings differ by which moments they OWN rather than by kind, and it is
/// what lets one class serve more than one: an effect that also defines `modifyLogical` gets both,
/// with no feature to add. It also leaves `tick` free to mean something in a layout later without a
/// grammar change. Guarding any of it would be code spent forbidding what a script author is
/// entitled to do, and the cost of a name nothing calls is a function that does not run, which is
/// visible immediately rather than silent.
inline constexpr const char* kEntryTick        = "tick";           // an effect, per frame
// The declaration moment, run once after a successful compile rather than per tick: the same
// place a compiled module's defineControls() sits in its lifecycle.
inline constexpr const char* kEntryDefineControls = "defineControls";
inline constexpr const char* kEntryPlaceLights = "placeLights";  // a layout, placing lights
inline constexpr const char* kEntryModify       = "modifyLogical"; // a modifier, folding one light

/// The system variables EVERY light script can read. One vocabulary for all three roles, rather
/// than a table per role.
///
/// The split that preceded this bought less than it cost. It prevented no mistake (a layout reading
/// `width` got a compile error, which is the same outcome as reading a variable that is always
/// zero) and it created a trap: the tables were different vocabularies rather than nested ones, so
/// a name meant one thing in one role and was RESERVED in another. `disasm.py` compiled against the
/// widest table and therefore refused `grid.mll`, the shipped default layout, with "name is a
/// system variable" -- the tool was blind to the one script most worth inspecting.
///
/// `width`/`height`/`depth` mean the same thing everywhere: the dimensions of the grid. A layout
/// DEFINES them by the coordinates it places; an effect and a modifier READ them. What a binding
/// still decides is which slots it WRITES each frame; reading is uniform.
///
/// The per-light coordinate is `xPos`/`yPos`/`zPos`, not `x`/`y`/`z`. Those are the names an author
/// reaches for as loop counters (`grid.mll` uses both), so reserving them globally would break the
/// most ordinary code there is. Only a modifier is handed a coordinate; elsewhere the slots read 0.
inline SysVarTable lightSysVars() {
    SysVarTable t;
    // Elapsed milliseconds, passed in kArg3 on every run. An argument register, so it costs no
    // instruction and no arena byte.
    t.add({"t", SysVarKind::Arg, kArg3});
    t.add({"width",  SysVarKind::Arena, kSysWidth});
    t.add({"height", SysVarKind::Arena, kSysHeight});
    t.add({"depth",  SysVarKind::Arena, kSysDepth});
    t.add({"xPos",   SysVarKind::Arena, kSysX});
    t.add({"yPos",   SysVarKind::Arena, kSysY});
    t.add({"zPos",   SysVarKind::Arena, kSysZ});
    return t;
}

/// The three roles keep their names as aliases of the one table: a binding says which role it is
/// playing, and every call site reads the same way it always did.
inline SysVarTable layoutSysVars()   { return lightSysVars(); }
inline SysVarTable effectSysVars()   { return lightSysVars(); }
inline SysVarTable modifierSysVars() { return lightSysVars(); }

// The light-domain built-in table the binding injects into the compiler. setRGB and fill are
// Inline (they lower to stores — the hot-path writers, no per-call cost); random16 is a Call.
inline BuiltinTable lightBuiltins() {
    BuiltinTable t;
    // setRGB(index, r, g, b)  → write one pixel (bounds-guarded). Inline op StoreElem.
    t.add({"setRGB", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreElem});
    // setXYZ(x, y, z)         → write one POSITION (bounds-guarded). The same StoreElem as setRGB:
    // three values at index * stride, and what differs is the destination the binding hands run()
    // (a colour buffer for an effect, a coordinate for a modifier), so one op serves both and the
    // engine stays free of any notion of what the three bytes mean.
    //
    // A different OP from setRGB, not the same one with an argument hidden: StoreFirst writes
    // element 0, which is the whole of what a modifier can do. The two are asked different
    // questions. An effect picks a pixel out of a whole buffer, so its index is the point; a
    // modifier is handed ONE coordinate per call, so there is no index to give.
    t.add({"setXYZ", 3, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreFirst});
    // fill(r, g, b)           → write every light. Inline op FillElems.
    t.add({"fill", 3, false, BuiltinKind::Inline, nullptr, InlineOp::FillElems});
    // mod(value, limit)      → value % limit. The wrap every cyclic animation needs; see above.
    t.add({"mod", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_mod, {}});
    // beat(bpm, t)           → 0..65535 sawtooth at bpm. The clock an animation is written against.
    t.add({"beat", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_beat, {}});
    // beatsin(bpm, t, high)  → a sine 0..high at bpm. The same shape an effect reaches for.
    t.add({"beatsin", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_beatsin, {}});
    // noise(x, y, z)         → 0..255 value noise. The one primitive fire/clouds/plasma all start from.
    t.add({"noise", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_noise, {}});
    // scale(value, n)        → a 0..65535 value onto 0..n-1. Lands a beat on an axis.
    t.add({"scale", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_scale, {}});
    // turn(n)                → one revolution split n ways, for stepping a circle.
    t.add({"turn", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_turn, {}});
    // random16(n)             → a value in [0,n). A Call to the host helper (typed fn pointer).
    // sin(angle) / cos(angle) → the circle. One turn is 0..65535, so a loop over N points steps
    // by 65536/N; the result is biased unsigned (see above).
    t.add({"sin", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_sin, {}});
    // polarA(dx, dy) / polarR(dx, dy) → polar from a centre. atan16 and dist16 already exist in
    // math16.h. NOT named `angle`/`radius`: a script wants those for its own controls
    // (ring.mll and balls.mle both declare `radius`), and a builtin would shadow them. Exposing
    // them here is what lets a radial effect drop its precomputed lookup table.
    t.add({"polarA", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_polarA, {}});
    t.add({"polarR", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_polarR, {}});
    t.add({"cos", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_cos, {}});
    t.add({"random16", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_random16, {}});
    // print(v)                → log v and return it. The script-level debugger.
    t.add({"print", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_print, {}});
    // addLight(x, y, z)      → place a light. A scripted layout's whole vocabulary.
    t.add({"addLight", 3, /*returns*/ false, BuiltinKind::Call, &mm_light_addLight, {}});
    // line(x1, y1, x2, y2, r, g, b) → a segment on the canvas, via the shared draw::line.
    t.add({"line", 7, /*returns*/ false, BuiltinKind::Call, &mm_light_line, {}});
    // addUint8(name, member, min, max) → declare a control on a member, the same call a compiled
    // module makes (`controls_.addUint8("speed", speed, 1, 255)`). Bit 1 of byRef marks the second
    // argument as the MEMBER, so the compiler passes its arena offset rather than its value, which
    // is what makes the script read as the reference a compiled module passes.
    t.add({"addUint8", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_addUint8, {},
           /*byRef*/ 0x2, /*byStr*/ 0x1, /*refType*/ CtrlType::Uint8});
    // addUint16(name, member, min, max) → the same call against a uint16_t member, so a script can
    // expose a value a byte cannot hold (a dwell time, a 0..1000 scale) instead of packing it into
    // two byte controls. Same by-ref/by-str marking: only the declared width differs.
    t.add({"addUint16", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_addUint16, {},
           /*byRef*/ 0x2, /*byStr*/ 0x1, /*refType*/ CtrlType::Uint16});
    // setPaletteColor(x, y, i, bri) → one palette-coloured pixel. The form a script should reach
    // for: one call, one brightness evaluation, and no buffer-layout arithmetic at the call site.
    t.add({"setPaletteColor", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_setPaletteColor, {}});
    // paletteR/G/B(i, bri)    → one channel each, for a script that needs the components. Kept
    // because setPaletteColor writes a pixel and cannot serve a script that wants the value.
    t.add({"paletteR", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteR, {}});
    t.add({"paletteG", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteG, {}});
    t.add({"paletteB", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteB, {}});
    // The table is built once at startup; a dropped registration would surface much later as
    // "unknown function" in a script, so it is caught HERE.
    MM_ASSERT_NO_BUILTIN_OVERFLOW(t);
    return t;
}

/// Run a script's `defineControls()`, so the controls it declares exist.
///
/// A compiled module's controls exist because `defineControls()` RAN: the Scheduler calls it on
/// every module at setup, and again whenever a Select reshapes the visible set. A scripted one
/// works the same way. This calls the entry point, each `addUint8` inside it reaches the engine
/// through the control sink, and the binding's `rebuildControls()` then finds a populated list.
///
/// Re-runnable, like its compiled counterpart: the list is cleared first, so calling it twice
/// rebuilds rather than appends. A script that defines no `defineControls()` declares no controls,
/// which is the honest answer for a script that wants no UI.
inline void runDefineControls(MoonLive& engine) {
    // A script with no defineControls() declares no controls, which is the honest answer for one
    // that wants no UI: there is nothing to clear and nothing to run.
    if (!engine.hasEntry(kEntryDefineControls)) return;
    // Install BEFORE clearing. With the two-slot table full the sink cannot be installed, and a
    // clear-then-run would drop every control and rebuild none of them: the script would appear to
    // declare nothing. Keeping the previous set is the honest degrade, and the run is skipped
    // rather than executed into a dead sink.
    if (!setAddControlSink([](void* ctx, const char* n, uint8_t off,
                              uint16_t lo, uint16_t hi, CtrlType type) {
            static_cast<MoonLive*>(ctx)->addDeclaredControl(n, off, lo, hi, type);
        }, &engine)) return;
    engine.clearDeclaredControls();      // re-runnable: rebuild rather than append
    // A one-light scratch buffer: this entry point writes no pixels, but `run` refuses a null or
    // undersized one, and honoring that contract costs less than carving out an exception.
    uint8_t scratch[3] = {};
    engine.run(scratch, 1, 3, 0, kEntryDefineControls);
    setAddControlSink(nullptr, nullptr);
}

}  // namespace mm::moonlive
