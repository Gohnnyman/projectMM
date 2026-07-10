#pragma once

// Umbrella header for writing an effect: include this one file and you have the
// effect author's standard library — the base class, the render context accessors,
// the common drawing / colour / maths helpers, scratch memory, and the audio source.
// A new effect is
//
//   #pragma once
//   #include "light/effects/Effect.h"
//   namespace mm {
//   class MyEffect : public EffectBase { ... };
//   }
//
// (Effect.h, not EffectBase.h: EffectBase forward-declares Layer and its accessors
// are defined in Layer.h, so an effect must pull in Layer.h anyway — this umbrella
// does that plus the helpers, so the author writes one line.)
//
// The set is deliberately the WHOLE surface available to an effect, not the minimal
// set: the render context, the draw/palette/math/noise primitives, scratch memory
// (ScratchBuffer), the audio spectrum (AudioService/AudioFrame), the crc fingerprint,
// and the <cstring>/<cmath> the bodies use. This is also the surface a scripted
// MoonLive effect gets uniformly, and it costs zero firmware bytes (unused declarations
// emit no code). An effect that needs a helper genuinely OUTSIDE this surface — a font
// table, the module factory, a network packet format, a platform primitive — adds that
// one extra include; nothing else should appear at the top of an effect header.

#include "light/layers/Layer.h"   // EffectBase + its accessors (layer/buffer/width/height/…)
#include "light/draw.h"           // draw::pixel / fill / line / fade / blur — write pixels by coordinate
#include "light/Palette.h"        // colorFromPalette, Palettes::active — the palette system
#include "core/math8.h"           // beat8 / beatsin8 / sin8 / random8 — the integer animation helpers
#include "core/noise.h"           // inoise8 — the shared value-noise field
#include "core/color.h"           // RGB
#include "core/crc.h"             // crc16 — grid/state fingerprints (stasis detection)
#include "core/ScratchBuffer.h"   // ScratchBuffer<T> — self-sizing scratch memory for stateful effects
#include "core/AudioService.h"    // AudioService::latestFrame() — the shared audio source
#include "core/AudioFrame.h"      // AudioFrame — level + 16-band spectrum an audio-reactive effect reads

#include <cstring>                // memset / memcpy / strcmp — buffer + control-name handling
#include <cmath>                  // sqrtf / sinf / log10f — per-frame float maths (never per-light)
