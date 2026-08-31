#pragma once

#include "core/math16.h"            // map32 — the shared, fencepost-safe range map
#include "light/effects/EffectBase.h"

namespace mm {

// GEQ: the classic flat 2D graphic equaliser. The 16 audio bands are spread across the columns of a
// 2D panel; each column rises from the bottom to a bar height set by its band's loudness, and a peak
// "dot" sits at the highest the bar has recently reached and falls back down slowly — the recognisable
// WLED "GEQ" look (distinct from the 3D-perspective GEQ3D effect in this folder).
//
// Per frame the whole buffer fades a little (fadeOut → motion trail), then for each column x its band
// is read, optionally smoothed against its neighbours (smoothBars), mapped to a bar height, and the
// column is filled from the floor up. The bar color is either per-column (colorBars) or per-row (the
// gradient runs up the bar). A per-column peak tracker remembers the tallest the bar reached; when the
// live bar is shorter, the remembered peak is drawn as a single dot and decays downward at a rate set
// by `ripple` (0 = the peak dot is disabled; otherwise it falls one row every `ripple` frames).
//
// Prior art: WLED's "GEQ" / 2D GEQ (mode_2DGEQ, Aircoookie / Andrew Tuline lineage), carried into
// MoonLight as the GEQ effect. The band→column mapping, the 7·band + 3·prev + 3·next smoothing weights,
// the bottom-up bar fill, the colorBars / smoothBars toggles, and the falling-peak dot are reproduced
// here, written fresh on projectMM's EffectBase + the shared draw / palette primitives. Reads
// AudioService::latestFrame(); silence → bars flat → peaks fall away → dark, safe on any target and grid
// size. The per-column peak-fall state lives on the heap (sized to width()), allocated in prepare
// and freed in release — never a large inline member.
// Author: Andrew Tuline (WLED-SR) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
/// Audio-reactive graphic-equaliser effect: 16 bands as vertical bars.
class GEQEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🐙🎶"; }  // MoonLight origin · 2D · audio
    Dim dimensions() const override { return Dim::D2; }      // writes only the z=0 slice; extrude fills z

    // Defaults match the WLED/MoonLight GEQ.
    uint8_t fadeOut    = 248;   // per-frame fade-to-black amount (motion trail). WLED maps its 0..255
                                // "fade" slider straight onto fadeToBlackBy; the GEQ default is a fast
                                // fade so bars snap rather than smear.
    uint8_t ripple     = 4;     // peak-dot fall rate: the dot drops one row every `ripple` frames
                                // (0 = no peak dot). WLED's "ripple" slider gates the falling peak.
    bool    colorBars  = false; // color each bar by its column (true) instead of by row height (false)
    bool    smoothBars = false; // blend each band with its neighbours for a smoother profile

    void defineControls() override {
        controls_.addControl("fadeOut", fadeOut, 0, 255);
        controls_.addControl("ripple", ripple, 0, 255);
        controls_.addControl("colorBars", colorBars);
        controls_.addControl("smoothBars", smoothBars);
    }

    // One peak tracker per column: previousBarHeight[width]. WLED stores this in the segment's data
    // block, sized to the column count and zero-initialised; matched here with a heap allocation that
    // re-sizes only when the column count changes, zeroed on (re)build so a grid/control change starts
    // every peak at the floor. Entries are lengthType (the row-count type) so a panel taller than 255
    // rows doesn't truncate the remembered peak height.
    void prepare() override {
        // One peak entry per column. resize() reallocs only when the column count changes (frees on
        // 0, keeps dynamicBytes current). Zero every peak on EVERY (re)build so a grid/control change
        // starts each peak at the floor (matching WLED's zero-init) — resize() only zero-fills on a
        // size change, so a same-width rebuild (e.g. a height-only edit) needs the explicit clear.
        peaks_.resize(static_cast<size_t>(width() > 0 ? width() : 0));
        if (peaks_) std::memset(peaks_.data(), 0, peaks_.bytes());
        rippleCounter_ = 0;
    }

    void tick() MM_NONBLOCKING override {
        const int cols = width();
        const int rows = height();
        if (!peaks_) return;   // build hasn't allocated yet (e.g. disabled) — nothing to draw

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // null-safe (latestFrame returns silence, never null, but guard regardless)

        const draw::Canvas cv = canvas();

        // Motion trail: dim the whole buffer each frame (WLED: fadeToBlackBy(fadeOut)).
        layer()->fadeToBlackBy(fadeOut);

        // Advance the peak-fall clock once per frame. The remembered peaks drop one row whenever the
        // counter wraps `ripple`; ripple == 0 disables the dot entirely (handled at draw time).
        bool fallThisFrame = false;
        if (ripple > 0) {
            if (++rippleCounter_ >= ripple) { rippleCounter_ = 0; fallThisFrame = true; }
        }

        for (int x = 0; x < cols; x++) {
            // Map this column onto one of the 16 GEQ bands (band = map(x, 0, cols-1, 0, 15)). The
            // 0..cols-1 / 0..15 form (vs the spec's literal map(x,0,size.x,0,16)) is the real WLED
            // mode_2DGEQ shape and keeps the last column on band 15 rather than an out-of-range 16.
            int band = map32(x, 0, cols - 1, 0, NUM_GEQ_CHANNELS - 1);
            if (band < 0) band = 0;
            if (band > NUM_GEQ_CHANNELS - 1) band = NUM_GEQ_CHANNELS - 1;

            int bandHeight = f->bands[band];

            // smoothBars: weighted blend with the neighbouring bands so the bar profile is less spiky.
            // WLED weights: (7·band + 3·prev + 3·next) / 12, only for interior bands. RECONSTRUCTED from
            // WLED's mode_2DGEQ smoothing (the fetched source did not include the exact body; the 7/3/3
            // weights and /12 divisor are the WLED constants).
            if (smoothBars && band > 0 && band < NUM_GEQ_CHANNELS - 1) {
                const int lastBandHeight = f->bands[band - 1];
                const int nextBandHeight = f->bands[band + 1];
                bandHeight = (7 * bandHeight + 3 * lastBandHeight + 3 * nextBandHeight) / 12;
                if (bandHeight < 0)   bandHeight = 0;
                if (bandHeight > 255) bandHeight = 255;
            }

            // Bar height in rows: map the 0..255 band magnitude onto 0..rows.
            int barHeight = map32(bandHeight, 0, 255, 0, rows);
            if (barHeight < 0)    barHeight = 0;
            if (barHeight > rows) barHeight = rows;

            // Per-column peak: rise instantly to a new high, otherwise fall slowly. peaks_[x] is the
            // row count (0..rows) the dot currently sits at, measured from the floor.
            if (barHeight > peaks_[x]) {
                peaks_[x] = static_cast<lengthType>(barHeight);
            } else if (fallThisFrame && peaks_[x] > 0) {
                peaks_[x] = static_cast<lengthType>(peaks_[x] - 1);   // RECONSTRUCTED: WLED's peak decays one row per ripple tick
            }

            // Fill the bar from the floor (row rows-1) upward. colorBars: one hue per column. Else
            // the gradient runs up the bar, so the color is a function of the height along it —
            // which is the index draw::bar hands the callback.
            const uint8_t columnIndex = static_cast<uint8_t>(map32(x, 0, cols - 1, 0, 255));
            draw::bar(cv, static_cast<lengthType>(x), static_cast<lengthType>(rows - 1),
                      static_cast<lengthType>(barHeight), draw::Grow::Up, [&](lengthType h) {
                          const uint8_t colorIndex = colorBars
                              ? columnIndex
                              : static_cast<uint8_t>(map32(h, 0, rows - 1, 0, 255));
                          return colorFromPalette(*Palettes::active(), colorIndex);
                      });

            // Falling peak dot, drawn at the remembered peak row if it stands above the live bar.
            // RECONSTRUCTED: WLED draws a single peak pixel (white-ish / palette top) at previousBarHeight.
            if (ripple > 0 && peaks_[x] > 0 && peaks_[x] > barHeight) {
                const int y = rows - peaks_[x];      // peaks_[x] rows up from the floor
                if (y >= 0 && y < rows) {
                    // Peak color: top of the palette (index 255) so the dot reads as the crest.
                    const RGB peakCol = colorFromPalette(*Palettes::active(), 255);
                    draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(y), 0}, peakCol);
                }
            }
        }
    }

private:
    static constexpr int NUM_GEQ_CHANNELS = 16;

    // previousBarHeight[width]: per-column peak-dot row (0..rows from floor). The buffer sizes
    // itself in prepare(), frees itself on disable/teardown, and reports its own bytes.
    ScratchBuffer<lengthType> peaks_{*this};
    uint8_t  rippleCounter_ = 0;   // counts frames toward the next peak-fall step (gated by `ripple`)
};

} // namespace mm