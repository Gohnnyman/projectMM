#pragma once

#include <cstdint>

#include "light/ChannelRole.h"

namespace mm {

// Light preset = the physical wire format a driver emits: channel order plus
// whether the light has a white channel. The order in this enum is index-aligned
// with kLightPresetOptions below (the Select control's option list), so the
// control's uint8 value casts straight to LightPreset.
//
// This is a CURATED list of the wire orders real hardware actually ships — the
// three-channel orders WS2812/APA-family strips and network sinks use (GRB is the
// WS2812 default; RGB and BGR cover the rest), plus their RGBW counterparts. It is
// deliberately NOT the full 24 white-position permutations: a preset is just a named
// default channel-role layout, and any wiring the curated list doesn't name is reached
// through the `Custom` entry (per-channel roles set in the UI), so enumerating
// permutations nobody wires would add dropdown surface without adding capability.
// `Custom` is the last entry: selecting it reveals the per-channel role pickers and
// leaves the roles as the user set them. (Curated-list-plus-custom is the WLED
// colour-order model.)
enum class LightPreset : uint8_t { RGB, GRB, BGR, RGBW, GRBW, Custom };

inline constexpr const char* kLightPresetOptions[] =
    {"RGB", "GRB", "BGR", "RGBW", "GRBW", "Custom"};
inline constexpr uint8_t kLightPresetCount =
    sizeof(kLightPresetOptions) / sizeof(kLightPresetOptions[0]);

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

// Fill `roles` (a caller-owned array of at least `cap` entries) with the channel-role
// layout for a curated preset, and report the channel count in `nCh`. This is the one
// place a preset name maps to a wiring: GRB → {Green, Red, Blue}. Custom is not handled
// here — its roles are user-set and left as the caller has them. Returns silently doing
// nothing if the preset is Custom or out of range (the caller keeps its current roles),
// or if `cap` is too small for the preset (defensive; every curated preset needs ≤ 4).
inline void fillRolesFromPreset(LightPreset preset, ChannelRole* roles, uint8_t cap, uint8_t& nCh) {
    using R = ChannelRole;
    switch (preset) {
        case LightPreset::RGB:  if (cap >= 3) { roles[0]=R::Red; roles[1]=R::Green; roles[2]=R::Blue; nCh=3; } break;
        case LightPreset::GRB:  if (cap >= 3) { roles[0]=R::Green; roles[1]=R::Red; roles[2]=R::Blue; nCh=3; } break;
        case LightPreset::BGR:  if (cap >= 3) { roles[0]=R::Blue; roles[1]=R::Green; roles[2]=R::Red; nCh=3; } break;
        case LightPreset::RGBW: if (cap >= 4) { roles[0]=R::Red; roles[1]=R::Green; roles[2]=R::Blue; roles[3]=R::White; nCh=4; } break;
        case LightPreset::GRBW: if (cap >= 4) { roles[0]=R::Green; roles[1]=R::Red; roles[2]=R::Blue; roles[3]=R::White; nCh=4; } break;
        case LightPreset::Custom: break;   // user-set roles, left as the caller has them
    }
}

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
        for (uint8_t i = 0; i < nChannels; i++) {
            switch (roles[i]) {
                case ChannelRole::Red:   offRed = i;   break;
                case ChannelRole::Green: offGreen = i; break;
                case ChannelRole::Blue:  offBlue = i;  break;
                case ChannelRole::White: offWhite = i; break;
                default: break;   // None or a fixture role — not part of the RGB(W) apply path
            }
        }
        outChannels = nChannels;
    }

    // Cold-path convenience: configure from a curated preset name. Fills a tiny stack role
    // array from the preset, then derives the offsets — the same result as seeding a role array
    // and calling rebuild(brightness, roles, n), for the curated (≤ 4-channel) case. The Custom
    // and fixture paths use the roles overload with the driver's dynamic array; this overload is
    // for the curated presets a strip/network sink uses and for tests.
    void rebuild(uint8_t brightness, LightPreset preset) {
        if (static_cast<uint8_t>(preset) >= kLightPresetCount || preset == LightPreset::Custom) {
            preset = LightPreset::RGB;   // no roles to seed for Custom here — default to RGB
        }
        ChannelRole roles[4];
        uint8_t nCh = 3;
        fillRolesFromPreset(preset, roles, 4, nCh);
        rebuild(brightness, roles, nCh);
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
        if (offWhite != kAbsent && whiteMode != WhiteMode::None) {
            const uint8_t w = r < g ? (r < b ? r : b) : (g < b ? g : b);  // min(r,g,b)
            if (whiteMode == WhiteMode::Accurate) {
                // Pull the shared white component out of RGB so the white LED carries
                // it instead of the colour channels — total emitted colour matches
                // the RGB target rather than washing brighter.
                r -= w; g -= w; b -= w;
            }
            out[offWhite] = w;
        }
        if (offRed != kAbsent)   out[offRed] = r;
        if (offGreen != kAbsent) out[offGreen] = g;
        if (offBlue != kAbsent)  out[offBlue] = b;
    }
};

} // namespace mm
