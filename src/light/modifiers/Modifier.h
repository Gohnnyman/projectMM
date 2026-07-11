#pragma once

// Umbrella header for writing a modifier: include this one file and you have the
// base class plus the maths helpers a modifier commonly uses to fold coordinates.
// A new modifier is
//
//   #pragma once
//   #include "light/modifiers/Modifier.h"
//   namespace mm {
//   class MyModifier : public ModifierBase { ... };
//   }
//
// A modifier overrides one or more of modifyLogicalSize / modifyLogical /
// modifyLive to transform coordinates; ModifierBase brings the base + light_types.
// The set is the whole surface a modifier commonly reaches for — the integer trig,
// the float trig, and the small standard helpers coordinate folds use — so a
// modifier is one include. Unused declarations cost zero firmware bytes. A modifier
// needing something genuinely outside this surface adds that one extra include.

#include "light/modifiers/ModifierBase.h"   // ModifierBase + lengthType/nrOfLightsType/Dim (via light_types)
#include "core/math8.h"                      // sin8/cos8 — integer trig for a rotate/affine modifier

#include <cmath>                             // std::sqrt / sin / cos — float trig (circle/pinwheel folds)
#include <cstdint>                           // fixed-width ints
#include <cstdlib>                            // std::abs
#include <algorithm>                          // std::max / std::min / std::clamp
