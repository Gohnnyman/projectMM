#pragma once

#include "light/layouts/LayoutBase.h"  // LayoutBase + CoordCallback — the Layouts container casts its children to it
#include "core/MoonModule.h"
#include "light/light_types.h" // lengthType, nrOfLightsType

#include <cstdio> // std::snprintf for the status line

namespace mm {


/// Top-level container for one or more `LayoutBase` children — it defines the physical light topology of the installation and is shared by every Layer in the `Layers` container (one Layouts describing the physical setup, multiple Layers render into it).
///
/// **Coordinate iteration is owned by the container, not the layer:** `forEachCoord` walks every enabled child layout's coordinates in registration order, offsetting physical indices so multiple layouts (for example 16 strips making one panel) stitch into a single flat physical address space without overlap. A Layer *uses* those coordinates to build its LUT. `totalLightCount` (the sum across enabled children) sizes both the layer buffer and the driver output buffer.
///
/// **Disabling a layout:** disabling a layout child (the `enabled` toggle) removes its lights from the LUT entirely, and the indices of any layouts after it shift down to close the gap — with two grids of 4 and 2 lights, disabling the first leaves the second at indices 0–1 and `totalLightCount` drops from 6 to 2. A `Scheduler::buildState` fires so the LUT, layer buffer, and driver output buffer reallocate. Side effect: ArtNet universe assignments shift with the indices — to keep driver-to-fixture mapping stable across enable changes, disable the driver instead of the layout. Disabling the container itself reports zero lights and an empty iteration, the same effect as disabling every child.
///
/// **Reordering:** layout children reorder by drag-and-drop (`POST /api/modules/<name>/move` with `{"to": <index>}`), with insert (not swap) semantics — the standard reorderable-list behaviour (Finder, Trello, SortableJS). Order sets the physical index range each layout occupies, which drives ArtNet universe assignment. The same `move` op applies to every container.
///
/// **Status:** the status slot shows the physical setup it describes — `` `<N> lights · <W>×<H>×<D>` `` — the total light count summed across enabled children (the driver output buffer size) and the physical bounding box (the extent of all light coordinates, the dense render buffer size). For a dense grid the count equals the box volume; for a sparse layout (a sphere shell) the count is smaller than the box, and that gap is the at-a-glance signal that the layout is sparse. An empty setup reports Warning severity. Recomputed on every rebuild, not per tick.
/// @card Layouts.png
class Layouts : public MoonModule {
public:
    const char* acceptsChildRoles() const override { return "layout"; }

    /// Sum of `lightCount` across enabled children — sizes the layer buffer and the
    /// driver output buffer. Disabled children are skipped, the same gate
    /// Layer/Layers/Drivers apply to their children. Indices of subsequent enabled
    /// layouts shift down to close the gap — disable Layout A and Layout B's lights
    /// move to indices 0..N. Users who need a stable index-to-fixture mapping disable
    /// the driver, not the layout.
    ///
    /// Disabling the container itself reports zero lights and an empty iteration —
    /// same effect as disabling every child, so the universal-gate intent ("enabled
    /// on every module means: exclude my contribution") holds for the container too.
    /// The Scheduler can't enforce this for us because Layouts has no loop() — the
    /// work happens in these cold-path methods called from Layer::onBuildState
    /// and Drivers::onBuildState.
    nrOfLightsType totalLightCount() const {
        if (!enabled()) return 0;
        nrOfLightsType total = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
            total += static_cast<LayoutBase*>(child(i))->lightCount();
        }
        return total;
    }

    void forEachCoord(CoordCallback cb, void* ctx) const {
        if (!enabled()) return;
        nrOfLightsType offset = 0;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (!child(i)->enabled()) continue;
            auto* layout = static_cast<LayoutBase*>(child(i));
            // Wrap callback to add physical index offset
            struct WrapCtx {
                CoordCallback cb;
                void* ctx;
                nrOfLightsType offset;
            };
            WrapCtx wctx{cb, ctx, offset};
            layout->forEachCoord([](void* wc, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* w = static_cast<WrapCtx*>(wc);
                w->cb(w->ctx, idx + w->offset, x, y, z);
            }, &wctx);
            offset += layout->lightCount();
        }
    }

    /// Status line: total physical lights + the physical bounding box (the extent
    /// of all light coordinates). Both are derived facts the container owns — the
    /// count is the driver buffer size, the box is the dense render extent. Shown
    /// via the status slot (not controls) so it costs no spec-check entry and
    /// renders generically. Recomputed only on a rebuild (cold path). A degenerate
    /// setup (no lights / zero box) flags Warning so the UI shows it's empty.
    void onBuildState() override {
        const nrOfLightsType lights = totalLightCount();
        // One forEachCoord pass for the bounding box: max coordinate + 1 per axis.
        struct Extent { lengthType x, y, z; bool any; } e{0, 0, 0, false};
        forEachCoord([](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            auto* ex = static_cast<Extent*>(ctx);
            if (x > ex->x) ex->x = x;
            if (y > ex->y) ex->y = y;
            if (z > ex->z) ex->z = z;
            ex->any = true;
        }, &e);
        const lengthType w = e.any ? e.x + 1 : 0;
        const lengthType h = e.any ? e.y + 1 : 0;
        const lengthType d = e.any ? e.z + 1 : 0;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u lights · %u×%u×%u",
                      static_cast<unsigned>(lights),
                      static_cast<unsigned>(w), static_cast<unsigned>(h), static_cast<unsigned>(d));
        setStatus(statusBuf_, lights == 0 ? Severity::Warning : Severity::Status);
        MoonModule::onBuildState();
    }

private:
    char statusBuf_[40] = {};  // "65535 lights · 999×999×999" fits; owned (setStatus borrows)
};

} // namespace mm
