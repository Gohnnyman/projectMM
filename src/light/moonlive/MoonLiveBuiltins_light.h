#pragma once

#include <cstdio>

#include "core/moonlive/MoonLiveBuiltins.h"

#include <cstdint>

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
    // random16(n)             → a value in [0,n). A Call to the host helper (typed fn pointer).
    t.add({"random16", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_random16, {}});
    // print(v)                → log v and return it. The script-level debugger.
    t.add({"print", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_print, {}});
    // addLight(x, y, z)      → place a light. A scripted layout's whole vocabulary.
    t.add({"addLight", 3, /*returns*/ false, BuiltinKind::Call, &mm_light_addLight, {}});
    return t;
}

}  // namespace mm::moonlive
