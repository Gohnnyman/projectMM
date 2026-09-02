// The preview's resolution controller: a pure function over 2-second windows, unit-tested in
// test/js/preview-adapt.test.mjs. It reads ONE signal: the device's own drops counter (each 0x02
// frame reports how many frames were discarded at the source since the last delivered one). Drops
// are ground truth for "the link cannot carry frames this big at this rate", so there is nothing
// to probe and nothing to guess:
//
//   COARSEN while drops persist. One window with drops is a hiccup and changes nothing; two in a
//   row (or a window where more frames were dropped than delivered) halves the detail.
//   REFINE after enough clean windows, one rung at a time. A refine that brings the drops back is
//   taken back, and the next attempt waits twice as long (exponential backoff, capped), the
//   abandon-fast retry-slowly rule ABR players use against oscillation.
//
// Everything else follows for free. A renderer-limited device (a heavy effect at 6 fps) delivers
// every frame it makes: zero drops, full detail, correctly held. A dead link delivers nothing and
// drops nothing: the stride simply stands. targetFps never enters this function; it is the rate
// half of the standing request, and rate and size meet only at the device's send slot.

export function initialPullState() {
    return {
        stride: 1,        // the detail this client requests (1 = full, halved per coarsen, cap 64)
        dropRun: 0,       // consecutive windows that saw drops
        cleanRun: 0,      // consecutive windows without drops
        refineWait: 2,    // clean windows required before the next refine try (backs off 2x)
        sinceRefine: -1,  // windows since the last refine (-1 = none pending judgment)
        request: false,   // this transition wants a new standing request announced
    };
}

export function nextPullState(st, delivered, dropped) {
    const s = { ...st, request: false };
    // A silent window (nothing delivered, nothing dropped) carries no evidence in either
    // direction: a dead link, a hibernating tab, a paused effect. Verdicts wait for data.
    if (delivered === 0 && dropped === 0) return s;
    // Grade the window by drop RATIO, not presence: a couple of skipped slots per window is the
    // sender's normal pacing jitter (a frame occasionally not drained by its slot), not
    // congestion, and coarsening on it made a healthy WiFi link wander (bench). HEAVY = more
    // dropped than delivered; DIRTY = a quarter or more; anything less is trace.
    const heavy = dropped >= Math.max(1, delivered);
    const dirty = dropped * 4 >= Math.max(1, delivered);
    if (dirty) {
        s.cleanRun = 0;
        s.dropRun++;
        // A refine done within the last two windows is what brought the drops back: take it back
        // and make the next attempt wait twice as long.
        const failedRefine = s.sinceRefine >= 0 && s.sinceRefine < 2;
        if (failedRefine || s.dropRun >= 2 || heavy) {
            if (s.stride < 64) { s.stride *= 2; s.request = true; }
            if (failedRefine) s.refineWait = Math.min(32, s.refineWait * 2);
            s.dropRun = 0;
            s.sinceRefine = -1;
        }
    } else if (dropped > 0) {
        // Trace drops: pressure exists but not enough to act on. Hold the refine clock (walking
        // finer INTO pressure would fail) without counting toward a coarsen.
        s.cleanRun = 0;
        s.dropRun = 0;
        if (s.sinceRefine >= 0) s.sinceRefine++;
    } else {
        s.dropRun = 0;
        s.cleanRun++;
        if (s.sinceRefine >= 0) s.sinceRefine++;
        if (s.sinceRefine >= 4) { s.sinceRefine = -1; s.refineWait = 2; }  // the refine held: forgiven
        if (s.stride > 1 && s.cleanRun >= s.refineWait && s.sinceRefine < 0) {
            s.stride >>= 1;
            s.cleanRun = 0;
            s.sinceRefine = 0;
            s.request = true;
        }
    }
    return s;
}
