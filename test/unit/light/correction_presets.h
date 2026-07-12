#pragma once

// Test-only convenience for building a Correction from a named color order (RGB/GRB/BGR/RGBW/GRBW).
// The PRODUCTION Correction has one rebuild — rebuild(brightness, ChannelRole* roles, nChannels) —
// because the real wirings come from the LightPresets library as role arrays. The curated-order enum
// + a rebuild(brightness, preset) overload used to live in Correction.h purely so tests could write
// `corr.rebuild(255, GRB)` tersely without hand-building a role array; that was a test convenience
// sitting in production code (zero production callers, and it duplicated "what does GRB mean", which
// the library's seedBuiltins now owns). It lives here instead: the tests keep the terse form, and
// Correction.h carries only the one role-array rebuild the device actually uses.

#include "light/drivers/Correction.h"
#include "light/ChannelRole.h"

namespace mm::test {

// The curated wire orders a strip/panel test drives. Values are arbitrary (test-local); the mapping
// to role arrays lives in rebuildFromPreset below — the one place "GRB means G,R,B" is stated for tests.
enum class PresetOrder : uint8_t { RGB, GRB, BGR, RGBW, GRBW };

// Fill a Correction from a named order at `brightness`, via the production role-array rebuild.
inline void rebuildFromPreset(mm::Correction& c, uint8_t brightness, PresetOrder order) {
    using R = mm::ChannelRole;
    switch (order) {
        case PresetOrder::RGB:  { R r[] = {R::Red, R::Green, R::Blue};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::GRB:  { R r[] = {R::Green, R::Red, R::Blue};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::BGR:  { R r[] = {R::Blue, R::Green, R::Red};            c.rebuild(brightness, r, 3); break; }
        case PresetOrder::RGBW: { R r[] = {R::Red, R::Green, R::Blue, R::White};  c.rebuild(brightness, r, 4); break; }
        case PresetOrder::GRBW: { R r[] = {R::Green, R::Red, R::Blue, R::White};  c.rebuild(brightness, r, 4); break; }
    }
}

}  // namespace mm::test
