#pragma once

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType, Dim

#include <cstdint>

namespace mm {

class Layer; // forward declaration

// Dim enum lives in light/light_types.h so both EffectBase and ModifierBase can
// refer to it without including each other. Used by Layer::extrude to fill unused
// axes (D1 column → x and z; D2 slice → z; D3 native) and by the UI to derive the
// 📏/🟦/🧊 dimensional emoji (so it isn't repeated in each module's tags()).
// ModuleFactory::registerType<T> captures dim from a probe via if-constexpr —
// no per-domain registration wrapper is needed.

/// Light-domain MoonModule subclass for effects — adds the rendering context.
///
/// **Design.** A zero-state convenience layer: it holds no data of its own, just
/// accessors (`buffer()`, `width()`, `height()`, `depth()`, `elapsed()`, …) that
/// forward to the parent Layer. An effect reads its rendering context through these
/// instead of caching a `Layer*` and the dimensions itself. `DriverBase` plays the
/// same role for drivers against the Drivers container.
///
/// **Animation guidelines.** Effects use BPM (beats per minute) for speed controls,
/// not abstract 0–255 ranges, giving users a musical reference (60 BPM = one beat per
/// second, 120 BPM = two; default 60). Animation must be resolution-independent:
/// multiply the time offset by the panel dimension (width or height) so the perceived
/// visual speed is the same regardless of display size — otherwise large displays look
/// sluggish and small ones frantic at the same BPM. Animation is driven by elapsed
/// millis (via `elapsed()`), not frame count, so speed stays consistent regardless of
/// FPS; the speed slider controls the animation dynamics, never the framerate, which
/// should always be maximal for smooth motion.
///
/// **Prior art:** MoonLight's Node + VirtualLayer — effects access
/// `layer->width()/height()/depth()` directly via the VirtualLayer pointer (no separate
/// EffectBase), buffer access via `layer->virtualChannels`, time via `timeMicros()`
/// (https://github.com/ewowi/MoonLight/blob/main/src/MoonBase/Nodes.h,
/// https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Layers/VirtualLayer.h).
class EffectBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Effect; }

    /// Which axes the effect *iterates* — a claim, not a guarantee about the layer.
    ///
    /// Default D3 means "I iterate every axis the layer gives me" — the framework
    /// doesn't extrude on your behalf. Override to D2 if you write only the z=0
    /// slice (Layer::extrude duplicates it across z); to D1 if you write only the
    /// x=0,z=0 column (1D runs along Y — extrude fills x and z). Declaring D2/D1 is an opt-in promise:
    /// the framework treats slices you don't write as authoritative copies of the
    /// ones you did. On a 2D layer (depth=1) the D3-vs-D2 distinction is free —
    /// extrude's z-fill loop is guarded by `depth_ > 1` and does nothing. The `dim`
    /// int (1/2/3) is emitted in `/api/types`; the UI derives the 📏/🟦/🧊 chip from
    /// it, so it isn't repeated in each module's `tags()`.
    ///
    /// **Contract — tick() must honour layer dimensions.** `dimensions()` is a
    /// claim about which axes the effect *iterates*, not a guarantee that the
    /// layer has that many axes. A D3 effect may run on a D1 or D2 layer (the
    /// layer just has depth=1 and/or height=1). Your loop must read `width()`,
    /// `height()`, and `depth()` at frame time and iterate exactly those bounds —
    /// never hardcode a maximum, never assume your declared D matches the
    /// layer's. Concretely:
    ///   - A D3 effect on a 1D layer (h=d=1) iterates only x; y and z stay 0.
    ///   - A D2 effect on a 1D layer (h=d=1) iterates only x; y stays 0.
    ///   - A D1 effect on a 3D layer writes its (x=0) column and extrude fills the rest.
    /// Hardcoding a fixed `z < SOMETHING` is a buffer-overrun bug — the buffer
    /// is sized to width × height × depth × channels, no more. Tests in
    /// test_extrude.cpp pin the D3-on-2D and D3-on-1D paths for the shipped
    /// effects; add similar pinning for new D3 effects with z-aware math.
    virtual Dim dimensions() const { return Dim::D3; }

    /// Parent is always a Layer (defined in Layer.h after Layer is complete).
    Layer* layer() const;

    // Convenience accessors — delegate to parent Layer.
    // Defined after Layer is complete (in Layer.h).
    uint8_t* buffer();
    lengthType width() const;
    lengthType height() const;
    lengthType depth() const;
    uint8_t channelsPerLight() const;
    nrOfLightsType nrOfLights() const;
    uint32_t elapsed() const;         ///< Milliseconds since render start — drive animation off this, not frame count.
};

} // namespace mm
