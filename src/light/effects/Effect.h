#pragma once

// Umbrella header for writing an effect: include this one file and you have
// everything a typical effect needs — the base class, the render context
// accessors, and the common drawing / colour / maths helpers. A new effect is
//
//   #pragma once
//   #include "light/effects/Effect.h"
//   namespace mm {
//   class MyEffect : public EffectBase { ... };
//   }
//
// (Effect.h, not EffectBase.h: EffectBase forward-declares Layer and its
// accessors are defined in Layer.h, so an effect must pull in Layer.h anyway —
// this umbrella does that plus the helpers, so the author writes one line.)
// An effect that needs a helper NOT bundled here (audio input, a specific
// modifier util) just adds that one extra include; nothing forces the set.

#include "light/layers/Layer.h"   // EffectBase + its accessors (layer/buffer/width/height/…)
#include "light/draw.h"           // draw::pixel / fill / line / fade / blur — write pixels by coordinate
#include "light/Palette.h"        // colorFromPalette, Palettes::active — the palette system
#include "core/math8.h"           // beat8 / beatsin8 / sin8 / random8 — the integer animation helpers
#include "core/color.h"           // RGB
