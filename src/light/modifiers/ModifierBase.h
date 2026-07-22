#pragma once

// Include this one file to write a modifier: it brings ModifierBase plus the maths helpers a coordinate
// fold commonly reaches for, so a modifier is a single include:
//
//   #pragma once
//   #include "light/modifiers/ModifierBase.h"
//   namespace mm {
//   class MyModifier : public ModifierBase { ... };
//   }
//
// A modifier overrides one or more of modifyLogicalSize / modifyLogical / modifyLive to transform
// coordinates. The helper set below is the whole surface a modifier commonly uses — the integer trig, the
// float trig, and the small standard helpers coordinate folds use. Unused declarations cost zero firmware
// bytes; a modifier needing something outside this surface adds that one extra include.

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType, Dim
#include "core/math8.h"        // sin8/cos8 — integer trig for a rotate/affine modifier

#include <cmath>              // std::sqrt / sin / cos — float trig (circle/pinwheel folds)
#include <cstdint>           // fixed-width ints
#include <cstdlib>          // std::abs
#include <algorithm>       // std::max / std::min / std::clamp

namespace mm {

/// Light-domain MoonModule base for modifiers. A modifier is a **coordinate transform** that reshapes how a Layer's effect output maps onto the physical lights. Multiple modifiers on one Layer **compose**: they apply in child order, each reshaping the result of the one below (Region *then* Multiply-mirror *then* Rotate).
///
/// **The fold contract:** a Layer builds its mapping by walking the physical lights and folding each through every enabled modifier in order — the composition M₁∘M₂∘…∘Mₙ collapsed into one mapping, so the per-frame render stays a single lookup. Three hooks, each a no-op by default so a modifier implements only what it needs: `modifyLogicalSize` (static, once per rebuild, reshapes the running logical box), `modifyLogical` (static, per physical light, folds a coordinate into this stage's logical space, returns false to reject), and `modifyLive` (dynamic, per-frame, a backward map that remaps a coordinate without rebuilding). A beat-driven modifier sets a flag in `tick`; the Layer polls `consumeNeedsRebuild` and rebuilds the mapping once if any asks. `dimensions` advertises which axes the modifier can transform.
///
/// **Fan-out is free:** because the build walks physical lights, fan-out (one logical cell driving N physical lights — a Multiply kaleidoscope) emerges naturally: N physical lights fold onto the same logical cell. There is no build-time fan-out list and no product-of-multipliers ceiling — each physical light contributes at most one destination, so the mapping can never overflow.
///
/// **Affine modifiers:** most modifiers are non-affine (a mask is a predicate, a tile is modulo) and express their fold directly. The rotate modifier is the exception and the codebase's transform-matrix reference: rotation is the canonical affine transform, written as an explicit integer 2×2 rotation matrix in `modifyLive`. A future affine "Transform" modifier (translate+scale+rotate+shear in one) would compose its matrix the same way and apply it through the same hook — the fold interface hosts a matrix-backed modifier with no change.
///
/// **Prior art:** the two building blocks are the textbook image-warping pattern — bake a coordinate transform into a precomputed spatial LUT, and build that table by BACKWARD mapping (walk the destinations, find each one's source) so no output pixel is ever left unmapped (https://towardsdatascience.com/forward-and-backward-mapping-for-computer-vision-833436e2472/). What is specific here — and credited to MoonLight — is collapsing a whole *chain* of discrete pixel folds into ONE index table: a desktop node graph (TouchDesigner, shader graphs) gives each node its own frame buffer; an ESP32 can't spare a buffer per modifier, so the chain is folded into a single LUT and the hot path stays one gather. MoonLight's `modifySize` / `modifyPosition` / `modifyXYZ` map to our `modifyLogicalSize` / `modifyLogical` / `modifyLive`, written fresh against our `MappingLUT` (https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Nodes/Modifiers/M_MoonLight.h).
class ModifierBase : public MoonModule {
public:
    ModuleRole role() const override { return ModuleRole::Modifier; }

    /// A modifier control change alters the mapping, so the owning Layer must rebuild
    /// it — the pipeline-wide rebuild path. See MoonModule::onControlChanged.
    bool affectsPrepare(const char* /*controlName*/) const override { return true; }

    /// Which axes the modifier can transform. Defaults to D3 — a modifier that
    /// touches the mapping is assumed to work in 3D unless it declares otherwise.
    /// The UI uses this to render the 📏/🟦/🧊 chip so the user can see at a
    /// glance whether a modifier will do anything along z. Distinct from
    /// EffectBase::dimensions() (which controls Layer extrusion); here it is
    /// purely an advisory chip, never read in the render path.
    virtual Dim dimensions() const { return Dim::D3; }

    // --- Fold interface (composable modifiers) — see the class comment for the
    // composition contract and prior art -------------------------------------

    /// STATIC, build-time, once per rebuild in child order: fold the logical box.
    // Multiply divides it, Region crops it, a mask leaves it. `size` is the running
    /// logical box (starts at the physical box; each modifier reshapes it for the
    /// next). A modifier that needs the box in its per-light fold STASHES it here
    /// (the MoonLight `modifierSize` pattern) — so modifyLogical reads its own stage's
    /// box from itself, and the Layer needs no per-stage box array. Non-const: the
    /// stash mutates the modifier.
    virtual void modifyLogicalSize(Coord3D& /*size*/) {}

    /// STATIC, build-time, per physical light in child order: fold a coordinate into
    /// this stage's logical space (in place). The coord enters in the box this
    /// modifier saw at modifyLogicalSize time and leaves in the box it produced, so
    /// the next modifier in the chain continues from here. Return false to REJECT —
    /// the coordinate has no logical source (a mask drops it, a region light falls
    /// outside the crop). A bool, not a sentinel coord: a later modifier's `` `% size` ``
    /// can't alias a sentinel back into range. The modifier reads any box it needs
    /// from its own stash (see modifyLogicalSize).
    virtual bool modifyLogical(Coord3D& /*pos*/) const { return true; }

    /// DYNAMIC, per-frame at render time: remap a coordinate without rebuilding the
    /// mapping (smooth rotation/scroll). The Layer runs this pass ONLY when some
    /// enabled modifier overrides it (hasModifyLive()), so a static-only chain pays
    /// nothing per frame — the render path stays at full speed. `logical` is the box.
    virtual void modifyLive(Coord3D& /*pos*/, const Coord3D& /*logical*/) const {}

    /// True iff this modifier does per-frame work (overrides modifyLive). The Layer
    /// sums this across enabled modifiers at build time to gate the per-frame pass;
    /// a modifier that animates returns true so the seam runs only when needed.
    virtual bool hasModifyLive() const { return false; }

    /// A modifier whose mapping changes on a timer (RandomMap reshuffles on a beat)
    /// sets a flag in its tick(); the Layer polls this once per frame across all its
    /// enabled modifiers and rebuilds the mapping ONCE if any returns true — so
    /// several dynamic modifiers ticking together coalesce to a single rebuild rather
    /// than each re-entering prepare(). Returns true at most once per change,
    /// clearing the flag. Default false: a static modifier never asks for a rebuild.
    virtual bool consumeNeedsRebuild() { return false; }
};

} // namespace mm
