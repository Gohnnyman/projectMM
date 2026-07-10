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
// (x,y,z)); LayoutBase brings the base + light_types. This adds the trig /
// clamp helpers the shipped layouts use; add any other include you need.

#include "light/layouts/LayoutBase.h"   // LayoutBase + lengthType/nrOfLightsType/Dim (via light_types)
#include "core/math8.h"                 // sin8/cos8/atan2_8 — integer trig for circular/wheel layouts
#include <cmath>                        // sinf/cosf/fmodf — float trig where a layout needs it
#include <limits>                       // std::numeric_limits — the lightCount clamp GridLayout uses
