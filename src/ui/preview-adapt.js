// The preview's adaptation controller, CLIENT-side, deliberately.
//
// The device used to guess link quality from how fast its socket drained, and cycled: the sender
// can only see its own buffer. The RECEIVER measures the true end-to-end rate (frames actually
// arriving, its own render cost included) and requests the stride it wants, the HLS/DASH shape,
// where the receiver picks the quality and the server serves what is asked.
//
// Pure module, no DOM: preview3d.js drives it with measurements; test/js pins it in node.

/// One controller evaluation, run per measurement window (~2 s).
///
/// `st` is the controller state and is not mutated: { stride, failedStride, goodWindows,
/// badWindows }. Returns the next state plus `request: true` when the new stride should be sent.
///
/// Bands (achieved fps vs the user's targetFps):
///   < 20%            → COARSER immediately, genuine starvation, waiting helps nobody
///   < 60% twice      → COARSER (double, cap 64), remembering the stride that failed. TWO windows,
///                      so a single hiccup (a GC pause, a timer stall) never costs a visible
///                      rebuild, it has to persist 4 s to count
///   ≥ 80% for 3 wins → FINER (halve), unless halving lands on the remembered failure, which is
///                      skipped ONCE and then cleared, so a temporary slowdown is forgotten while
///                      a persistent limit costs one retry per two clean runs instead of cycling.
///                      80, not higher: the sender's own fps-slot quantisation and ordinary jitter
///                      keep even a perfect link below ~95%, and a bar the link can never clear
///                      strands the preview coarse until a refresh (bench-observed on the desktop)
///   otherwise        → HOLD, resetting both runs, the dead band that makes settling stable
export function nextStrideState(st, achievedFps, targetFps) {
    const s = { stride: st.stride, failedStride: st.failedStride, goodWindows: st.goodWindows,
                badWindows: st.badWindows || 0, coarsenFrom: st.coarsenFrom || 0,
                coarsenFps: st.coarsenFps || 0, srcLimitFps: st.srcLimitFps || 0, request: false };
    if (!(targetFps > 0)) return s;

    // COARSENING MUST PAY. Halving the points should raise the rate; when it does not, the
    // bottleneck is upstream of the link, the device's own render loop (a heavy effect at 6 fps
    // sends 6 fps at ANY stride). Ratcheting coarser then destroys detail for nothing, all the way
    // to 1/64. So the window after a coarsen checks the receipt: no meaningful improvement →
    // revert, and hold at that detail until the rate genuinely changes.
    if (s.coarsenFrom) {
        const paid = achievedFps * 4 >= s.coarsenFps * 5;   // ≥ +25%
        if (!paid) {
            s.srcLimitFps = achievedFps;      // remember the source's ceiling
            s.stride = s.coarsenFrom;         // give the detail back, it cost nothing to keep
            s.failedStride = 0;
            s.request = true;
        }
        s.coarsenFrom = 0; s.coarsenFps = 0;
        s.goodWindows = 0; s.badWindows = 0;
        return s;
    }
    // While source-limited, a below-target rate is EXPECTED, only a real deterioration (well
    // under the source's own ceiling) or a recovery to target re-arms the bands.
    if (s.srcLimitFps) {
        if (achievedFps * 5 >= targetFps * 4) s.srcLimitFps = 0;        // source recovered
        else if (achievedFps * 2 >= s.srcLimitFps && achievedFps <= s.srcLimitFps * 2)
            return s;                                                    // steady at its ceiling: hold
        else s.srcLimitFps = 0;               // left the band either way: re-arm the bands
    }

    const coarsen = () => {
        if (s.stride < 64) {
            s.coarsenFrom = s.stride;         // next window audits whether this step paid
            s.coarsenFps = achievedFps;
            s.failedStride = s.stride;
            s.stride = Math.min(s.stride * 2, 64);
            s.request = true;
        }
        s.badWindows = 0;
    };
    if (achievedFps * 5 < targetFps * 3) {
        s.goodWindows = 0;
        s.badWindows++;
        if (achievedFps * 5 < targetFps || s.badWindows >= 2) coarsen();
    } else if (achievedFps * 5 >= targetFps * 4) {
        s.badWindows = 0;
        s.goodWindows++;
        if (s.goodWindows >= 3) {
            s.goodWindows = 0;
            if (s.stride > 1) {
                const finer = s.stride >> 1;
                if (finer === s.failedStride) s.failedStride = 0;   // skip once, then forget
                else { s.stride = finer; s.request = true; }
            }
        }
    } else {
        s.goodWindows = 0;
        s.badWindows = 0;
    }
    return s;
}

/// The fresh state a (re)connect starts from. Stride 1 = full detail: the first windows then
/// coarsen to what the link truly carries, rather than starting coarse on a link that never
/// needed it.
export function initialStrideState() {
    return { stride: 1, failedStride: 0, goodWindows: 0, badWindows: 0,
             coarsenFrom: 0, coarsenFps: 0, srcLimitFps: 0 };
}
