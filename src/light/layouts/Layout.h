#pragma once

// Umbrella header for writing a layout: include this one file and you have the
// base class plus the maths helpers a layout commonly needs to place lights in
// space. A new layout is
//
//   #pragma once
//   #include "light/layouts/Layout.h"
//   namespace mm {
//   class MyLayout : public LayoutBase { ... };
//   }
//
// A layout overrides lightCount() and forEachCoord() (reporting each light's
// (x,y,z)); LayoutBase brings the base + light_types. The set is the whole surface
// a layout commonly reaches for — the integer + float trig and the small standard
// helpers coordinate placement uses — so a layout is one include. Unused
// declarations cost zero firmware bytes. A layout needing something genuinely
// outside this surface adds that one extra include.

#include "light/layouts/LayoutBase.h"   // LayoutBase + lengthType/nrOfLightsType/Dim (via light_types)
#include "light/light_types.h"          // lengthType, nrOfLightsType, Coord3D (also via LayoutBase)
#include "core/math8.h"                 // sin8/cos8/atan2_8 — integer trig for circular/wheel layouts

#include <cmath>                        // sinf/cosf/fmodf — float trig where a layout needs it
#include <cstdint>                      // fixed-width ints
#include <limits>                       // std::numeric_limits — the lightCount clamp GridLayout uses
#include <numbers>                      // std::numbers::pi_v — portable pi (ring/circle layouts)
