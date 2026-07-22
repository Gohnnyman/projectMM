#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// Three planes sweep in sync at a given BPM:
//   Red   — YZ plane sweeps left→right (x oscillates)
//   Green — XZ plane sweeps top→bottom (y oscillates)
//   Blue  — XY plane sweeps front→back (z oscillates)
// Useful for verifying preview axis orientation: each colour names its axis.
// Port of MoonLight's Lines effect via projectMM-v1/LinesEffect.h.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
/// Test effect: axis-aligned planes sweeping in sync (RGB = XYZ).
/// @card LinesEffect.png
class LinesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }

    uint8_t speed = 30;   // BPM
    uint8_t axis  = 0;    // 0=all 1=x(red) 2=y(green) 3=z(blue)
    uint8_t mode  = 0;    // 0=lines (the sweeping planes), 1=panel dots (a static mapping test)
    uint8_t panelW = 16;  // panel block width  in lights (panel-dots mapping)
    uint8_t panelH = 16;  // panel block height in lights — SEPARATE from width (panels aren't square:
                          //   a 16×6 module has W≠H, and one square size skips whole panel rows)

    void defineControls() override {
        static constexpr const char* kAxisOptions[] = {"all", "x (red)", "y (green)", "z (blue)"};
        static constexpr const char* kModeOptions[] = {"lines", "panel dots"};
        controls_.addSelect("mode", mode, kModeOptions, 2);
        // The two modes have disjoint controls, so show only the active mode's. defineControls re-runs
        // on every control change (MoonModule), so toggling `mode` re-hides these automatically — the
        // conditional-control shape the driver uses for latchPin/loopback pins.
        const bool dots = (mode == 1);
        controls_.addUint8("speed", speed, 1, 240);
        controls_.setHidden(controls_.count() - 1, dots);          // lines only
        controls_.addSelect("axis", axis, kAxisOptions, 4);
        controls_.setHidden(controls_.count() - 1, dots);          // lines only
        controls_.addUint8("panelW", panelW, 1, 64);
        controls_.setHidden(controls_.count() - 1, !dots);         // panel dots only
        controls_.addUint8("panelH", panelH, 1, 64);
        controls_.setHidden(controls_.count() - 1, !dots);         // panel dots only
    }

    void tick() override {
        uint8_t* buf = buffer();
        const lengthType w   = width();
        const lengthType h   = height();
        const lengthType d   = depth();
        const uint8_t    cpl = channelsPerLight();

        // Nothing to draw on an empty volume — a zero in ANY dimension (or channel count) makes the
        // buffer zero-length, and buf itself may be null. Guard before the memset (and before either
        // mode's loops) so no null / zero-length allocation is ever touched. Covers 0×0×0.
        if (!buf || w == 0 || h == 0 || d == 0 || cpl == 0) return;

        memset(buf, 0, static_cast<size_t>(w) * h * d * cpl);

        // PANEL DOTS — a static mapping aid, NOT an animation. Each panelW×panelH block lights
        // (panelIndex + 1) LEDs along its top row: panel 0 shows 1 dot, panel 1 shows 2, and so on, so
        // you can read straight off the wall which physical panel carries which grid block (and thus
        // which strand). Panel index is row-major in GRID space (panelsPerRow across, then down); the
        // physical wiring order is the layout's job, so any mismatch between the count you see and the
        // position is exactly the mapping fact this reveals. W and H are SEPARATE so non-square panels
        // (e.g. 16×6) get one dot-group per panel — a single square step skips whole rows of panels.
        if (mode == 1) {
            const lengthType pw = panelW ? panelW : 1;
            const lengthType ph = panelH ? panelH : 1;
            const lengthType panelsPerRow = (w + pw - 1) / pw;
            for (lengthType py = 0; py < h; py += ph) {
                for (lengthType px = 0; px < w; px += pw) {
                    const lengthType panelIdx = (py / ph) * panelsPerRow + (px / pw);
                    const lengthType dots = panelIdx + 1;   // panel 0 → 1 dot, panel 1 → 2, …
                    for (lengthType i = 0; i < dots && (px + i) < w && i < pw; i++) {
                        const size_t off = (static_cast<size_t>(py) * w + (px + i)) * cpl;
                        if (cpl >= 1) buf[off + 0] = 255;   // white dots, brightest for a clear read
                        if (cpl >= 2) buf[off + 1] = 255;
                        if (cpl >= 3) buf[off + 2] = 255;
                    }
                }
            }
            return;
        }

        // Sawtooth 0–65535 at speed BPM. Use 64-bit to avoid overflow.
        const uint32_t period = 60000u / static_cast<uint32_t>(speed ? speed : 1);
        const uint16_t beat   = static_cast<uint16_t>(
            (static_cast<uint64_t>(elapsed() % period) * 65535u) / period
        );

        auto setRGB = [&](lengthType x, lengthType y, lengthType z,
                          uint8_t r, uint8_t g, uint8_t b) {
            size_t off = (static_cast<size_t>(z) * h * w
                         + static_cast<size_t>(y) * w + x) * cpl;
            if (cpl >= 1) buf[off + 0] = r;
            if (cpl >= 2) buf[off + 1] = g;
            if (cpl >= 3) buf[off + 2] = b;
        };

        // Sweep position: map the 0–65535 beat into N equal buckets, beat * N / 65536, giving each
        // index 0..N-1 an equal 1/N slice of the cycle. Crucially this REACHES N-1: `beat` tops out
        // below 65535 (it is (elapsed % period) * 65535 / period, and elapsed % period maxes at
        // period-1), so the textbook `beat * (N-1) / 65535` truncates to N-2 at the top and the
        // sweep never lights the last row/column — the off-by-one this fixes. `beat * N / 65536`
        // maps that same sub-full-scale top to N-1 (e.g. 65502*8/65536 = 7 for N=8).
        auto sweepIndex = [&](lengthType n) {
            return static_cast<lengthType>(static_cast<uint32_t>(beat) * n / 65536u);
        };

        // Red — YZ plane at x = beat position, sweeps left→right
        if (w > 1 && (axis == 0 || axis == 1)) {
            const lengthType x = sweepIndex(w);
            for (lengthType z = 0; z < d; z++)
                for (lengthType y = 0; y < h; y++)
                    setRGB(x, y, z, 255, 0, 0);
        }

        // Green — XZ plane at y = beat position, sweeps top→bottom
        if (h > 1 && (axis == 0 || axis == 2)) {
            const lengthType y = sweepIndex(h);
            for (lengthType z = 0; z < d; z++)
                for (lengthType x = 0; x < w; x++)
                    setRGB(x, y, z, 0, 255, 0);
        }

        // Blue — XY plane at z = beat position, sweeps front→back (3D only)
        if (d > 1 && (axis == 0 || axis == 3)) {
            const lengthType z = sweepIndex(d);
            for (lengthType y = 0; y < h; y++)
                for (lengthType x = 0; x < w; x++)
                    setRGB(x, y, z, 0, 0, 255);
        }
    }
};

} // namespace mm
