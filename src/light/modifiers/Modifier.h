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
// This adds the trig helpers rotate-style modifiers use; add any other include
// you need.

#include "light/modifiers/ModifierBase.h"   // ModifierBase + lengthType/nrOfLightsType/Dim (via light_types)
#include "core/math8.h"                      // sin8/cos8 — integer trig for a rotate/affine modifier
