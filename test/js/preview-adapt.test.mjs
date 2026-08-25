// The preview's drops-driven resolution controller (src/ui/preview-adapt.js): two rules over
// 2-second windows, reading only the device's own drop reports. These tests are the functional
// documentation of how the preview trades detail for rate.
// Run: `node --test "test/js/**/*.test.mjs"`.
import test from "node:test";
import assert from "node:assert/strict";
import { nextPullState, initialPullState } from "../../src/ui/preview-adapt.js";

const step = (s, delivered, dropped) => nextPullState(s, delivered, dropped);

test("a clean link never loses detail: any run of drop-free windows holds full resolution", () => {
    let s = initialPullState();
    for (let i = 0; i < 50; i++) {
        s = step(s, 24, 0);
        assert.equal(s.stride, 1);
        assert.equal(s.request, false);
    }
});

test("one dirty window is a hiccup and changes nothing; a second in a row coarsens", () => {
    let s = initialPullState();
    s = step(s, 20, 8);                  // 40% dropped: dirty
    assert.equal(s.stride, 1);           // tolerated once
    s = step(s, 20, 8);
    assert.equal(s.stride, 2);           // persistent: halve the detail
    assert.ok(s.request);
});

test("trace drops are pacing jitter, not congestion: they hold refining but never coarsen", () => {
    let s = initialPullState();
    for (let i = 0; i < 20; i++) {
        s = step(s, 40, 2);              // 5% dropped, every window
        assert.equal(s.stride, 1);       // never coarsens on trace amounts
    }
    s = step(s, 10, 5); s = step(s, 10, 5);          // real congestion -> 2
    assert.equal(s.stride, 2);
    for (let i = 0; i < 20; i++) s = step(s, 40, 2); // trace forever: refine never fires either
    assert.equal(s.stride, 2);
});

test("heavy starvation coarsens immediately: more frames dropped than delivered", () => {
    let s = initialPullState();
    s = step(s, 3, 20);
    assert.equal(s.stride, 2);
    assert.ok(s.request);
});

test("a renderer-limited device keeps full detail: slow frames without drops are not congestion", () => {
    // A heavy effect renders 6 fps and every frame is delivered. Coarsening could not raise that
    // rate, and the controller never tries: no drops, no change.
    let s = initialPullState();
    for (let i = 0; i < 30; i++) {
        s = step(s, 12, 0);              // 6 fps = 12 frames per 2 s window
        assert.equal(s.stride, 1);
    }
});

test("a dead link holds its stride: nothing delivered and nothing dropped is not a verdict", () => {
    let s = initialPullState();
    s = step(s, 20, 8); s = step(s, 20, 8);          // congestion earned stride 2
    for (let i = 0; i < 20; i++) {
        s = step(s, 0, 0);                            // then total silence
        assert.equal(s.stride, 2);                    // no flapping on no information
    }
});

test("clean windows refine one rung at a time back to full detail", () => {
    let s = initialPullState();
    s = step(s, 10, 5); s = step(s, 10, 5);          // -> 2
    s = step(s, 10, 5); s = step(s, 10, 5);          // -> 4
    assert.equal(s.stride, 4);
    s = step(s, 20, 0); s = step(s, 20, 0);          // two clean windows -> refine
    assert.equal(s.stride, 2);
    assert.ok(s.request);
    // the refine held (4 clean windows forgive it), then the next refine may come
    let refined = false;
    for (let i = 0; i < 8 && !refined; i++) { s = step(s, 30, 0); refined = s.stride === 1; }
    assert.equal(s.stride, 1);
});

test("a refine that brings the drops back is taken back, and the next try waits twice as long", () => {
    let s = initialPullState();
    s = step(s, 10, 5); s = step(s, 10, 5);          // -> 2 (congested at full detail)
    s = step(s, 20, 0); s = step(s, 20, 0);          // clean at 2 -> refine to 1
    assert.equal(s.stride, 1);
    s = step(s, 10, 4);                               // drops immediately back: the refine failed
    assert.equal(s.stride, 2);                        // taken back at once, no second bad window
    assert.equal(s.refineWait, 4);                    // and the next attempt is more patient
    s = step(s, 20, 0); s = step(s, 20, 0); s = step(s, 20, 0);
    assert.equal(s.stride, 2);                        // 3 clean windows: still waiting
    s = step(s, 20, 0);
    assert.equal(s.stride, 1);                        // the 4th earns the retry
});

test("repeatedly failing refines back off exponentially and cap, so a borderline link cannot oscillate", () => {
    let s = initialPullState();
    s = step(s, 10, 5); s = step(s, 10, 5);          // -> 2
    let waits = [];
    for (let round = 0; round < 6; round++) {
        while (s.stride > 1) s = step(s, 20, 0);      // clean until the refine fires
        assert.equal(s.stride, 1);
        s = step(s, 10, 4);                           // and it always fails
        assert.equal(s.stride, 2);
        waits.push(s.refineWait);
    }
    assert.deepEqual(waits, [4, 8, 16, 32, 32, 32]);  // 2x each failure, capped at 32
});

test("the stride caps at 64 however bad it gets, and never goes below 1", () => {
    let s = initialPullState();
    for (let i = 0; i < 20; i++) s = step(s, 1, 50);
    assert.equal(s.stride, 64);
    for (let i = 0; i < 200; i++) s = step(s, 30, 0);
    assert.equal(s.stride, 1);
});

test("a held refine is forgiven: later congestion coarsens without extra backoff punishment", () => {
    let s = initialPullState();
    s = step(s, 10, 5); s = step(s, 10, 5);          // -> 2
    s = step(s, 20, 0); s = step(s, 20, 0);          // refine to 1
    for (let i = 0; i < 4; i++) s = step(s, 20, 0);  // the refine HOLDS for 4 clean windows
    assert.equal(s.refineWait, 2);                   // patience reset
    s = step(s, 10, 5); s = step(s, 10, 5);          // unrelated congestion much later
    assert.equal(s.stride, 2);
    assert.equal(s.refineWait, 2);                   // coarsened, but not punished as a failed refine
});
