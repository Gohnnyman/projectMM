#pragma once

#include <cmath> // std::pow — the gamma curve
#include <cstdint>

#include "light/ChannelRole.h"

namespace mm {

// A light's wire format — its channel order and whether it carries a white channel — is described by
// a ChannelRole array (roles[i] = what channel i emits), resolved from the LightPresets library into
// this Correction at cold-path rebuild time (see LightPresetsModule). Correction has one rebuild that
// takes that role array; there is no built-in preset enum here, because the curated wire orders live
// as seeded rows in the library, not as a second hard-coded list in core.

// White-derivation mode for RGBW lights. Effects write RGB only, so a driver feeding
// an RGBW fixture must SYNTHESIZE the white channel from RGB — and there is more than
// one accepted algorithm, so the method is a mode, not a fixed formula (the WLED
// "auto white" feature: None / Brighter / Accurate). None leaves white at 0 (the
// effect drives it, or the fixture's white is unused). Min takes the common white
// component min(R,G,B) — cheap, slightly desaturating. Accurate also subtracts that
// white back out of R/G/B so the total emitted color matches the RGB target rather
// than washing brighter. Applied only when the light carries a white channel
// (offWhite != kAbsent); ignored otherwise.
enum class WhiteMode : uint8_t { None, Min, Accurate };

inline constexpr const char* kWhiteModeOptions[] = {"None", "Min", "Accurate"};
inline constexpr uint8_t kWhiteModeCount = sizeof(kWhiteModeOptions) / sizeof(kWhiteModeOptions[0]);

// Output correction applied per-light by each physical driver as it reads the shared
// source buffer: brightness scale, gamma, per-channel white balance, channel reorder,
// and (for RGBW lights) white derivation. Each driver owns one Correction (DriverBase),
// rebuilds it on a brightness / gamma / balance / preset / role change (cheap, cold path),
// and apply() is the hot-path per-light transform. Today NetworkSendDriver and the WS2812
// LED drivers consume it.
//
// Channel model: a light is a run of `channelsPerLight` channels, each with a role
// (Red/Green/Blue/White/Pan/…). The canonical description is the driver's dynamic
// ChannelRole array — sized to the fixture, no fixed cap — which rebuild() reads to
// DERIVE the hot-path color offsets (offRed/offGreen/offBlue/offWhite): the byte
// position of each color role, or kAbsent if the light doesn't carry it. That derive
// is cold-path (once per config change), so apply() stays a branchless indexed store
// per channel — the same build-a-table-cold, read-it-hot shape as the brightness LUT.
// Non-color roles (pan/tilt/…) live in the role array for the fixture/preview to read;
// apply() only writes the color roles it derived offsets for.
//
// Brightness, gamma and white balance all bake into ONE per-channel table — `briLut[3][256]`,
// one row per SOURCE channel (0=R, 1=G, 2=B), filled as `gamma(v) × brightness × balance`.
struct Correction {
    static constexpr uint8_t kAbsent = 255;   // color role not carried by this light
    static constexpr uint8_t kGammaOff = 10; // gamma 1.0 — the identity curve, and the default

    uint8_t briLut[3][256] = {}; // briLut[ch][v] = gamma(v) * brightness * balance[ch], ch: 0=R 1=G 2=B
    // Derived hot-path cache: the output-byte position of each color role. Source is
    // always RGB (src[0]=R, src[1]=G, src[2]=B); the offset says where in `out` that
    // role's byte lands. Recomputed from the role array by rebuild(); GRB by default.
    uint8_t offRed = 1;
    uint8_t offGreen = 0;
    uint8_t offBlue = 2;
    uint8_t offWhite = kAbsent;     // derived white at this offset (kAbsent = light has no white)
    // Extra emitters a fixture may carry beside cold white. The theory (why each is derived the way
    // it is; the honest limits) — a full fixture model with per-emitter spectral targets is the
    // proper home, see the light backlog:
    //   • WarmWhite is a BROADBAND ILLUMINATION emitter, like cold White — a low-CCT (~2700K)
    //     phosphor white. Its achromatic basis is real, so `min(R,G,B)` (the white component) is a
    //     sound approximation; warm vs cold differ only in phosphor CCT, which a byte value can't
    //     express, so from an RGB target both get the same min(R,G,B). Rides `whiteMode` with White.
    //   • Yellow/Amber is a SATURATED NARROW-BAND HUE (~590 nm real amber die, common in RGBA/RGBAW
    //     PARs), NOT illumination. It has no honest RGB pre-image: `min(R,G)` (the R+G overlap) is a
    //     crude stand-in that reads greener than a true amber AND fires on far too much (any red+green
    //     content — yellows, whites, skin tones — muddying the fixture). So it's a "light it up to
    //     eyeball the wiring" placeholder, not a correct render.
    //   • UV (~400 nm) is OUT OF the RGB gamut entirely (no pre-image at all). It reads to the eye as
    //     deep violet, so the blue excess `max(0, B - max(R,G))` (fires on blues/purples, dark on
    //     warm colors) is a deliberate eyeball hack, honest about being one.
    // apply() drives all three off the SAME `whiteMode` gate today (None zeroes them, else the
    // approximation above). That's expedient, not right: White/WarmWhite belong under whiteMode (real
    // achromatic extraction, subtraction-aware); Yellow/UV are targetable emitters an effect should
    // drive DIRECTLY via the fixture model, not synthesize from RGB. See backlog-light § fixture model.
    uint8_t offWarmWhite = kAbsent;
    uint8_t offYellow = kAbsent;
    uint8_t offUV = kAbsent;
    uint8_t outChannels = 3;        // bytes emitted per light (= channelsPerLight of the wiring)
    WhiteMode whiteMode = WhiteMode::Min;   // how white is synthesized from RGB (white lights only)

    // Gamma in TENTHS (22 = 2.2). An LED is near-linear in PWM duty while perception is a power
    // law, so an uncorrected ramp reads "bright fast then flat"; `(v/255)^gamma` restores an even
    // fade. Canon: Adafruit, "LED Tricks: Gamma Correction".
    uint8_t gamma10 = kGammaOff;
    // Per-channel white balance, 255 = untouched. Die efficiencies differ, so a white-looking RGB
    // triple rarely renders neutral — trim the stronger channels DOWN to match the weakest. Up is
    // not available: there is no headroom above 255, so raising clips instead of balancing.
    uint8_t balRed = 255, balGreen = 255, balBlue = 255;

    // Cold path: refresh the output tables and DERIVE the color-role offsets from the
    // light's channel-role array (`roles`, `nChannels` entries — the driver's dynamic
    // array, canonical). A role appearing at channel i sets that color's offset to i;
    // a color role not present stays kAbsent (apply() skips it). outChannels becomes the
    // channel count. Non-color roles (pan/tilt/…) are ignored here — they're written by
    // the fixture role writers, not by apply()'s RGB path.
    // Refill the three output tables from `brightness` plus the current gamma / balance fields.
    // Split out so a brightness-only change re-scales them without touching the channel offsets,
    // and so a driver can apply brightness even when the role source (the preset library) isn't
    // available yet. Every gamma or balance edit routes through here too — they are inputs to the
    // same fill, so there is one rebuild, not three.
    void rebuildBrightness(uint8_t brightness) {
        // Gamma FIRST, then the linear scales: scaling before the curve would re-shape it at every
        // brightness, so a colour would shift as the slider moved.
        uint8_t curve[256];
        const float exponent = gamma10 / 10.0f;
        for (int v = 0; v < 256; v++)
            curve[v] = static_cast<uint8_t>(std::pow(v / 255.0f, exponent) * 255.0f + 0.5f);

        const uint8_t balance[3] = {balRed, balGreen, balBlue};

        for (int ch = 0; ch < 3; ch++) {
            // Fold brightness and this channel's trim into one scale
            const uint32_t scale = (static_cast<uint32_t>(brightness) * balance[ch]) / 255;
            for (int v = 0; v < 256; v++) briLut[ch][v] = static_cast<uint8_t>((curve[v] * scale) / 255);
        }
    }

    void rebuild(uint8_t brightness, const ChannelRole* roles, uint8_t nChannels) {
        rebuildBrightness(brightness);
        offRed = offGreen = offBlue = offWhite = kAbsent;
        offWarmWhite = offYellow = offUV = kAbsent;
        for (uint8_t i = 0; i < nChannels; i++) {
            switch (roles[i]) {
                case ChannelRole::Red:       offRed = i;       break;
                case ChannelRole::Green:     offGreen = i;     break;
                case ChannelRole::Blue:      offBlue = i;      break;
                case ChannelRole::White:     offWhite = i;     break;
                case ChannelRole::WarmWhite: offWarmWhite = i; break;
                case ChannelRole::Yellow:    offYellow = i;    break;
                case ChannelRole::UV:        offUV = i;        break;
                default: break;   // None or a fixture motion role (pan/tilt/…) — apply() ignores it
            }
        }
        outChannels = nChannels;
    }

    // Hot path: transform one source light (3-channel RGB at `src`) into `out`
    // (`outChannels` bytes). Brightness via LUT, then place each present color role at
    // its derived offset, then synthesize white per whiteMode. No allocation, integer-only.
    // A color role the light doesn't carry (offset == kAbsent) is simply not written — so
    // a wiring that omits, say, red just doesn't emit it. Channels holding non-color roles
    // (pan/tilt) are left for their own writers; apply() never touches them.
    inline void apply(const uint8_t* src, uint8_t* out) const {
        uint8_t r = briLut[0][src[0]];
        uint8_t g = briLut[1][src[1]];
        uint8_t b = briLut[2][src[2]];
        // Every synthesized emitter (white + warm-white/yellow/UV) is gated by the ONE whiteMode:
        // None zeroes them (never a stale value — corrected_ is reused, not re-zeroed, frame to
        // frame), otherwise each is a best-effort approximation from RGB. Accurate additionally
        // subtracts the WHITE component back out of RGB (the standard RGBW auto-white behavior);
        // the other emitters are additive stand-ins only (no colorimetric model yet), so they don't
        // subtract. See the offWarmWhite/offYellow/offUV field comment for the approximation rationale.
        if (whiteMode == WhiteMode::None) {
            if (offWhite != kAbsent)     out[offWhite] = 0;
            if (offWarmWhite != kAbsent) out[offWarmWhite] = 0;
            if (offYellow != kAbsent)    out[offYellow] = 0;
            if (offUV != kAbsent)        out[offUV] = 0;
        } else {
            const uint8_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);  // min(r,g,b): the white component
            // The additive stand-ins (warm-white/yellow/UV) approximate from the CORRECTED RGB — the
            // values BEFORE Accurate pulls white out below. Compute them here, off the pre-subtraction
            // r/g/b, so Accurate's `r -= w` (which only rebalances the RGB emitters) can't corrupt them.
            // warm white ≈ the white component (same as cold white for a warm-white-only strip).
            if (offWarmWhite != kAbsent) out[offWarmWhite] = w;
            // yellow ≈ min(R,G) (the shared red+green component).
            if (offYellow != kAbsent)    out[offYellow] = r < g ? r : g;
            // UV is out of gamut (no RGB pre-image), but it reads to the eye as deep blue/violet, so
            // drive it from the BLUE component that has no red/green to pair with — the violet-ish
            // excess `max(0, B - max(R,G))`. So UV fires on blues/purples, stays dark on warm colors.
            if (offUV != kAbsent) {
                const uint8_t rg = r > g ? r : g;
                out[offUV] = b > rg ? static_cast<uint8_t>(b - rg) : 0;
            }
            // White last: it's the only emitter that (in Accurate) rebalances RGB, so it must run
            // after the stand-ins have read the pre-subtraction values.
            if (offWhite != kAbsent) {
                if (whiteMode == WhiteMode::Accurate) {
                    r -= w;
                    g -= w;
                    b -= w;
                } // pull white out of RGB
                out[offWhite] = w;
            }
        }
        if (offRed != kAbsent)   out[offRed] = r;
        if (offGreen != kAbsent) out[offGreen] = g;
        if (offBlue != kAbsent)  out[offBlue] = b;
    }
};

} // namespace mm
