#pragma once

#include "core/MoonModule.h"

namespace mm {

/// The top-level container for the user-added Service modules, added and removed at runtime.
///
/// The core twin of the light domain's containers, holding children of one role.
/// So the generic add, replace, delete and persistence machinery applies unchanged.
/// Fixed infrastructure lives under System, so this is exactly the mutable half of that split.
/// A pure grouping node with no controls and no state, whose one job is to say what it accepts.
/// @card Services.png
class Services : public MoonModule {
public:
    /// Accept any service as a child, which is what makes this the mutable half.
    const char* acceptsChildRoles() const override { return "service"; }
};

} // namespace mm
