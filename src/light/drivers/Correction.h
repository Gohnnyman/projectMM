#pragma once

#include <cstdint>

#include "light/ChannelRole.h"

namespace mm {

// A light's wire format — its channel order and whether it carries a white channel — is described by
// a ChannelRole array (roles[i] = what channel i emits), resolved from the LightPresets library into
// this Correction at cold-path rebuild time (see LightPresetsModule). Correction has one rebuild that
// takes that role array; there is no built-in preset enum here, because the curated wire orders live
// as seeded rows in the library, not as a second hard-coded list in core.

// White-derivation mode for RGBW lights. Effects write RGB only, so a driver feeding
// an RGBW fixture must SYNTHESISE the white channel from RGB — and there is more than
// one accepted algorithm, so the method is a mode, not a fixed formula (the WLED
// "auto white" feature: None / Brighter / Accurate). None leaves white at 0 (the
// effect drives it, or the fixture's white is unused). Min takes the common white
// component min(R,G,B) — cheap, slightly desaturating. Accurate also subtracts that
// white back out of R/G/B so the total emitted colour matches the RGB target rather
// than washing brighter. Applied only when the light carries a white channel
// (offWhite != kAbsent); ignored otherwise.
enum class WhiteMode : uint8_t { None, Min, Accurate };

inline constexpr const char* kWhiteModeOptions[] = {"None", "Min", "Accurate"};
inline constexpr uint8_t kWhiteModeCount =
    sizeof(kWhiteModeOptions) / sizeof(kWhiteModeOptions[0]);


// Output correction applied per-light by each physical driver as it reads the shared
// source buffer: brightness scale, channel reorder, and (for RGBW lights) white
// derivation. Each driver owns one Correction (DriverBase), rebuilds it on a
// brightness / preset / role change (cheap, cold path), and apply() is the hot-path
// per-light transform. Today NetworkSendDriver and the WS2812 LED drivers consume it.
//
// Channel model: a light is a run of `channelsPerLight` channels, each with a role
// (Red/Green/Blue/White/Pan/…). The canonical description is the driver's dynamic
// ChannelRole array — sized to the fixture, no fixed cap — which rebuild() reads to
// DERIVE the hot-path colour offsets (offRed/offGreen/offBlue/offWhite): the byte
// position of each colour role, or kAbsent if the light doesn't carry it. That derive
// is cold-path (once per config change), so apply() stays a branchless indexed store
// per channel — the same build-a-table-cold, read-it-hot shape as the brightness LUT.
// Non-colour roles (pan/tilt/…) live in the role array for the fixture/preview to read;
// apply() only writes the colour roles it derived offsets for.
//
// Brightness uses a single 256-entry LUT applied to every channel. Gamma /
// white-balance (which need a per-channel R/G/B split) are deliberately not here
// yet — when they land, briLut becomes three tables. The name stays brightness-
// neutral (`briLut`) so the gamma addition is a fill-logic change, not a rename.
struct Correction {
    static constexpr uint8_t kAbsent = 255;   // colour role not carried by this light

    uint8_t briLut[256] = {};       // briLut[v] = (v * brightness) / 255 (scale8)
    // Derived hot-path cache: the output-byte position of each colour role. Source is
    // always RGB (src[0]=R, src[1]=G, src[2]=B); the offset says where in `out` that
    // role's byte lands. Recomputed from the role array by rebuild(); GRB by default.
    uint8_t offRed = 1;
    uint8_t offGreen = 0;
    uint8_t offBlue = 2;
    uint8_t offWhite = kAbsent;     // derived white at this offset (kAbsent = light has no white)
    // Extra synthesised-from-RGB emitters a fixture may carry beside cold white. There is no
    // effect-side source for these yet (no CCT target, no UV channel in RGB), so apply() drives
    // them off the SAME whiteMode as White with a best-effort approximation — the goal is "every
    // channel lights up so a fixture can be eyeballed", not colorimetric accuracy (a real fixture
    // model with per-emitter targets is the proper home; see the light backlog). warm white ≈ the
    // white component (min RGB, same as cold white for a warm-white-only strip); yellow ≈ min(R,G)
    // (the R+G component); UV reads to the eye as deep blue/violet, so it's driven from the blue
    // excess `max(0, B - max(R,G))` (fires on blues/purples). All zeroed when whiteMode==None, so
    // none can hold a stale value.
    uint8_t offWarmWhite = kAbsent;
    uint8_t offYellow = kAbsent;
    uint8_t offUV = kAbsent;
    uint8_t outChannels = 3;        // bytes emitted per light (= channelsPerLight of the wiring)
    WhiteMode whiteMode = WhiteMode::Min;   // how white is synthesised from RGB (white lights only)

    // Cold path: refresh the brightness LUT and DERIVE the colour-role offsets from the
    // light's channel-role array (`roles`, `nChannels` entries — the driver's dynamic
    // array, canonical). A role appearing at channel i sets that colour's offset to i;
    // a colour role not present stays kAbsent (apply() skips it). outChannels becomes the
    // channel count. Non-colour roles (pan/tilt/…) are ignored here — they're written by
    // the fixture role writers, not by apply()'s RGB path.
    // Refresh just the brightness LUT (briLut[v] = v * brightness / 255). Split out so a brightness-
    // only change re-scales the LUT without touching the channel offsets, and so a driver can apply
    // brightness even when the role source (the preset library) isn't available yet.
    void rebuildBrightness(uint8_t brightness) {
        for (int v = 0; v < 256; v++) briLut[v] = static_cast<uint8_t>((v * brightness) / 255);
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
    // (`outChannels` bytes). Brightness via LUT, then place each present colour role at
    // its derived offset, then synthesise white per whiteMode. No allocation, integer-only.
    // A colour role the light doesn't carry (offset == kAbsent) is simply not written — so
    // a wiring that omits, say, red just doesn't emit it. Channels holding non-colour roles
    // (pan/tilt) are left for their own writers; apply() never touches them.
    inline void apply(const uint8_t* src, uint8_t* out) const {
        uint8_t r = briLut[src[0]];
        uint8_t g = briLut[src[1]];
        uint8_t b = briLut[src[2]];
        // Every synthesised emitter (white + warm-white/yellow/UV) is gated by the ONE whiteMode:
        // None zeroes them (never a stale value — corrected_ is reused, not re-zeroed, frame to
        // frame), otherwise each is a best-effort approximation from RGB. Accurate additionally
        // subtracts the WHITE component back out of RGB (the standard RGBW auto-white behaviour);
        // the other emitters are additive stand-ins only (no colorimetric model yet), so they don't
        // subtract. See the offWarmWhite/offYellow/offUV field comment for the approximation rationale.
        if (whiteMode == WhiteMode::None) {
            if (offWhite != kAbsent)     out[offWhite] = 0;
            if (offWarmWhite != kAbsent) out[offWarmWhite] = 0;
            if (offYellow != kAbsent)    out[offYellow] = 0;
            if (offUV != kAbsent)        out[offUV] = 0;
        } else {
            const uint8_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);  // min(r,g,b): the white component
            if (offWhite != kAbsent) {
                if (whiteMode == WhiteMode::Accurate) { r -= w; g -= w; b -= w; }  // pull white out of RGB
                out[offWhite] = w;
            }
            // warm white ≈ the white component (same as cold white for a warm-white-only strip).
            if (offWarmWhite != kAbsent) out[offWarmWhite] = w;
            // yellow ≈ min(R,G) (the shared red+green component).
            if (offYellow != kAbsent)    out[offYellow] = r < g ? r : g;
            // UV is out of gamut (no RGB pre-image), but it reads to the eye as deep blue/violet, so
            // drive it from the BLUE component that has no red/green to pair with — the violet-ish
            // excess `max(0, B - max(R,G))`. So UV fires on blues/purples, stays dark on warm colours.
            if (offUV != kAbsent) {
                const uint8_t rg = r > g ? r : g;
                out[offUV] = b > rg ? static_cast<uint8_t>(b - rg) : 0;
            }
        }
        if (offRed != kAbsent)   out[offRed] = r;
        if (offGreen != kAbsent) out[offGreen] = g;
        if (offBlue != kAbsent)  out[offBlue] = b;
    }
};

} // namespace mm
