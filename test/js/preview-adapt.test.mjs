// The preview's client-side stride controller, the one place adaptation now lives. A device-side
// version of this logic cycled for a bench day precisely because it could not be tested like this:
// these cases pin the bands, the dead band, and the failed-stride skip-once-then-forget rule.
//
// Run: `node --test test/js/**/*.test.mjs`.

import { test } from "node:test";
import assert from "node:assert/strict";
import { nextStrideState, initialStrideState } from "../../src/ui/preview-adapt.js";

const step = (st, achieved, target = 24) => nextStrideState(st, achieved, target);

test("a single bad window is a hiccup and changes nothing; two coarsen", () => {
    let s = step({ stride: 1, failedStride: 0, goodWindows: 0, badWindows: 0 }, 7, 24);  // 29%
    assert.equal(s.stride, 1);          // one GC pause must not cost a visible rebuild
    assert.ok(!s.request);
    s = step(s, 7, 24);                 // ...but a persisting slowdown does
    assert.equal(s.stride, 2);
    assert.equal(s.failedStride, 1);
    assert.ok(s.request);
});

test("true starvation coarsens immediately, waiting helps nobody at ~0 fps", () => {
    const s = step({ stride: 2, failedStride: 0, goodWindows: 0, badWindows: 0 }, 0, 24);
    assert.equal(s.stride, 4);
    assert.ok(s.request);
});

test("ordinary jitter still counts as clean, the bar a real link can actually clear", () => {
    // 22/24 ≈ 92%: below the old 95% bar this window would RESET the clean run, stranding the
    // preview coarse until a refresh (bench-observed). At the 80% bar it refines normally.
    let s = { stride: 2, failedStride: 0, goodWindows: 0, badWindows: 0 };
    s = step(s, 22); s = step(s, 23); s = step(s, 22);
    assert.equal(s.stride, 1);
    assert.ok(s.request);
});

test("the dead band holds: 70% of target changes nothing, ever", () => {
    let s = { stride: 2, failedStride: 0, goodWindows: 2 };
    for (let i = 0; i < 50; i++) {
        s = step(s, 17, 24);            // ~70%
        assert.equal(s.stride, 2);
        assert.ok(!s.request);
        assert.equal(s.goodWindows, 0); // a mediocre window resets the clean run
        assert.equal(s.badWindows, 0);  // ...and the bad run: hold means hold
    }
});

test("meeting the target refines only after three consecutive clean windows", () => {
    let s = { stride: 4, failedStride: 0, goodWindows: 0 };
    s = step(s, 24); assert.equal(s.stride, 4);
    s = step(s, 24); assert.equal(s.stride, 4);
    s = step(s, 24);
    assert.equal(s.stride, 2);          // the third window earns the halving
    assert.ok(s.request);
});

test("a stride that failed is skipped once, then forgotten, one retry per two clean runs", () => {
    let s = { stride: 2, failedStride: 1, goodWindows: 0 };
    for (let i = 0; i < 3; i++) s = step(s, 24);
    assert.equal(s.stride, 2);          // first clean run: refinement into 1 is SKIPPED...
    assert.equal(s.failedStride, 0);    // ...and the verdict is cleared
    for (let i = 0; i < 3; i++) s = step(s, 24);
    assert.equal(s.stride, 1);          // second clean run: the retry happens
});

test("the coarse end is capped at 64 and the fine end at 1", () => {
    let s = { stride: 64, failedStride: 0, goodWindows: 0 };
    s = step(s, 0);
    assert.equal(s.stride, 64);
    assert.ok(!s.request);              // nothing coarser to ask for
    s = { stride: 1, failedStride: 0, goodWindows: 2 };
    s = step(s, 24);
    assert.equal(s.stride, 1);          // nothing finer than full detail
});

test("a fresh connection starts at full detail and a zero target does nothing", () => {
    const s0 = initialStrideState();
    assert.equal(s0.stride, 1);
    const s = nextStrideState(s0, 0, 0);
    assert.equal(s.stride, 1);
    assert.ok(!s.request);
});

test("a renderer-limited source keeps full detail: coarsening that does not pay is reverted", () => {
    // A heavy effect renders 6 fps; the device sends 6 fps at ANY stride. Target 24.
    let s = initialStrideState();
    s = step(s, 6);                      // bad window 1: hiccup tolerance
    s = step(s, 6);                      // bad window 2: coarsens to 2, expecting a payoff
    assert.equal(s.stride, 2);
    s = step(s, 6);                      // the audit window: still 6 fps, coarsening paid nothing
    assert.equal(s.stride, 1);           // detail restored
    assert.ok(s.request);
    for (let i = 0; i < 20; i++) {       // and it HOLDS there at the source's ceiling
        s = step(s, 6);
        assert.equal(s.stride, 1);
    }
});

test("a coarsen that pays is kept, and the chain may continue", () => {
    let s = initialStrideState();
    s = step(s, 5); s = step(s, 5);      // → stride 2
    assert.equal(s.stride, 2);
    s = step(s, 11);                     // audit: 5 → 11 fps, the step paid
    assert.equal(s.stride, 2);
    s = step(s, 11); s = step(s, 11);    // still <60% of 24 → next step
    assert.equal(s.stride, 4);
    s = step(s, 23);                     // audit passes again; near target now
    assert.equal(s.stride, 4);
});

test("source-limited hold re-arms on real deterioration, and audits the retry too", () => {
    let s = initialStrideState();
    s = step(s, 6); s = step(s, 6); s = step(s, 6);   // limited at 6 → held at stride 1
    assert.equal(s.stride, 1);
    s = step(s, 2);                      // far below the remembered ceiling: bands re-arm and,
    assert.equal(s.stride, 2);           // at hard starvation, a coarsen is attempted at once...
    s = step(s, 2);                      // ...audited, found unpaid (still 2 fps), and reverted:
    assert.equal(s.stride, 1);           // even a dying source never earns needless coarseness
    s = step(s, 13);                     // well above the old ceiling: re-arm (bad window 1)
    s = step(s, 13);                     // persistent shortfall coarsens again
    assert.equal(s.stride, 2);
    s = step(s, 30);                     // and this time it PAYS (13 → 30), so it is kept
    assert.equal(s.stride, 2);
});

test("a link delivering nothing never counts as a payoff, however often it is asked", () => {
    // 0 fps before and after a coarsen is not a 25% improvement, it is a dead link. Reading it as
    // "paid" would ratchet the stride to 64 while not one frame arrives.
    let s = initialStrideState();
    s = step(s, 0);                      // starvation coarsens at once
    assert.equal(s.stride, 2);
    for (let i = 0; i < 10; i++) {
        s = step(s, 0);
        assert.ok(s.stride <= 2, `stride ratcheted to ${s.stride} on a dead link`);
    }
    assert.equal(s.stride, 1);           // settles back at full detail: coarser bought nothing
});
