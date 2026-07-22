#pragma once

// Include this one file to write a layout: it brings LayoutBase plus the maths helpers a layout commonly
// uses to place lights in space, so a layout is a single include:
//
//   #pragma once
//   #include "light/layouts/LayoutBase.h"
//   namespace mm {
//   class MyLayout : public LayoutBase { ... };
//   }
//
// A layout overrides lightCount() and forEachCoord() (reporting each light's (x,y,z)). The helper set
// below is the whole surface a layout commonly reaches for — the integer + float trig and the small
// standard helpers coordinate placement uses. Unused declarations cost zero firmware bytes; a layout
// needing something outside this surface adds that one extra include.

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType, Coord3D
#include "core/math8.h"        // sin8/cos8/atan2_8 — integer trig for circular/wheel layouts

#include <cmath>              // sinf/cosf/fmodf — float trig where a layout needs it
#include <cstdint>          // fixed-width ints
#include <limits>          // std::numeric_limits — the lightCount clamp GridLayout uses
#include <numbers>       // std::numbers::pi_v — portable pi (ring/circle layouts)

namespace mm {

/// Callback for layout coordinate iteration — a layout walks its positions and
/// invokes this per light with the physical index and (x,y,z). Owned by
/// LayoutBase: it's the signature of forEachCoord, which every layout overrides.
using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

/// Base for one layout child of the `Layouts` container. A concrete layout
/// (grid, sphere shell, …) implements `lightCount` and `forEachCoord` directly —
/// no wrapper. Every layout control changes the physical light count, so any
/// control change triggers the pipeline-wide rebuild.
class LayoutBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Layout; }
    virtual nrOfLightsType lightCount() const = 0;
    virtual void forEachCoord(CoordCallback cb, void* ctx) const = 0;

    /// Every layout control (grid width/height/depth, …) changes the physical light
    /// count and therefore needs the pipeline-wide rebuild. See MoonModule::onControlChanged.
    bool affectsPrepare(const char* /*controlName*/) const override { return true; }
};

} // namespace mm
