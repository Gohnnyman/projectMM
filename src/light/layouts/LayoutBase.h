#pragma once

#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType

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
    /// count and therefore needs the pipeline-wide rebuild. See MoonModule::onUpdate.
    bool controlChangeTriggersBuildState(const char* /*controlName*/) const override { return true; }
};

} // namespace mm
