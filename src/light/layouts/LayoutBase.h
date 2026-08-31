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
// A layout overrides lightCount() and placeLights() (reporting each light's (x,y,z)). The helper set
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

/// Callback for layout coordinate iteration — invoked per light with the physical
/// index and (x,y,z). The underlying function-pointer form a `CoordSink` carries.
using CoordCallback = void(*)(void* ctx, nrOfLightsType idx, lengthType x, lengthType y, lengthType z);

/// The sink a layout emits its positions into — a builder with one method per KIND
/// of pixel, not a coordinate plus a boolean flag. `pixel()` is a normal light that
/// maps to a logical cell; `blackPixel()` is a GAP: a physical wire slot the driver
/// still clocks (so data flows through it on a continuous strand) that stays black,
/// mapping to no logical light. A layout with no dark regions only ever calls
/// `pixel()` and stays unaware `blackPixel()` exists; the black/lit choice is made
/// once, at the emit site, where the layout already knows the position.
///
/// This is the builder/visitor-sink shape — named emit methods per variant, as in
/// Skia's `SkPath` (`moveTo`/`lineTo`/`close`) or a SAX/serde visitor — chosen over
/// a `bool black` parameter (the "boolean-trap" a named method avoids). The two
/// function pointers are the consumer's wiring: `cb` receives lit pixels; `blackCb`
/// receives gaps, and is null for a consumer that doesn't distinguish them (a plain
/// bounding-box or count walk), in which case a gap falls back to a normal pixel.
struct CoordSink {
    CoordCallback cb;             // lit pixel handler (required)
    CoordCallback blackCb;        // gap handler; null → gaps fall back to `cb`
    void* ctx;

    /// A normal light at physical index `idx`, position (x,y,z). What every layout calls.
    void pixel(nrOfLightsType idx, lengthType x, lengthType y, lengthType z) const {
        cb(ctx, idx, x, y, z);
    }
    /// A GAP light: a physical slot that stays black. Only a layout with dark regions
    /// calls this. Falls back to `pixel` when the consumer left `blackCb` null.
    void blackPixel(nrOfLightsType idx, lengthType x, lengthType y, lengthType z) const {
        (blackCb ? blackCb : cb)(ctx, idx, x, y, z);
    }
};

/// Base for one layout child of the `Layouts` container. A concrete layout
/// (grid, sphere shell, …) implements `lightCount` and `placeLights` directly ,
/// no wrapper. Every layout control changes the physical light count, so any
/// control change triggers the pipeline-wide rebuild.
class LayoutBase : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Layout; }
    virtual nrOfLightsType lightCount() const = 0;
    virtual void placeLights(const CoordSink& sink) const = 0;

    /// The shape this layout LAYS OUT: a line, a picture, a volume.
    ///
    /// **Every layout states it, including the ones that match this default.** A module that says
    /// nothing is indistinguishable from one nobody checked, and the default is expected to MOVE to
    /// D3 as the library becomes 3D-native: everything stated survives that change untouched, where
    /// everything implicit would silently follow it.
    ///
    /// It describes THIS layout, not the rig: two 1D strands added to a tree compose into a 2D
    /// grid, and the composed extent is what the Layouts card reports (`256 lights, 16x16x1`).
    /// So this answers "what is this one capable of", which is what a user picking from the
    /// catalog needs, and nothing consumes it: unlike an EFFECT's dimensions, which drives
    /// Layer::extrude, a layout's is the advisory 📏/🟦/🧊 chip alone.
    virtual Dim dimensions() const { return Dim::D2; }

    /// Whether this layout emits any GAP (black) pixels — physical wire slots held dark. Default
    /// false: a layout with no dark regions never overrides this and stays unaware gaps exist. Gates
    /// the Layer's dense-identity fast path (which would light a gap) off when true. See CoordSink.
    virtual bool hasBlackPixels() const { return false; }

    /// Every layout control (grid width/height/depth, …) changes the physical light
    /// count and therefore needs the pipeline-wide rebuild. See MoonModule::onControlChanged.
    bool affectsPrepare(const char* /*controlName*/) const override { return true; }
};

} // namespace mm
