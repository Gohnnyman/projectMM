#pragma once

#include "core/module/MoonModule.h"

#include <cstddef>
#include <cstdint>

namespace mm {

/// Browse and manage the device filesystem: a folder tree with create, delete and edit.
///
/// The counterpart to FilesystemModule, which is the persistence engine rather than a browser.
/// This module owns only the view toggle and the usage gauges.
/// Its bodies live in the `.cpp`, so an edit recompiles only that file.
/// @card FileManagerModule.png
///
/// @moreinfo
///
/// ## Browsing lives in the UI
///
/// The tree is a client-side lazy tree, each folder loading its own children on first expand.
/// Expansion is UI state, so the module owns none of it and exposes only the operations.
/// That keeps the recursion and the paging out of core state.
///
/// A leading dot hides an entry unless the toggle is on, the standard convention.
/// The toggle is a per-session view preference, forced off at every boot.
///
/// ## Operations are endpoints, not controls
///
/// Creating a folder, deleting an entry and reading or writing a file are all HTTP calls.
/// The path rides the request, so an operation stores nothing on the device.
/// All share one path guard, and each fails cleanly rather than crashing.
///
/// Last-modified dates need a time source and filesystem support, both backlogged.
///
/// Prior art: the lazy folder tree is the standard file-explorer shape.
class FileManagerModule : public MoonModule {
public:
    /// Declare the view toggle and the usage gauges.
    void defineControls() override;
    /// Read the partition total, and force the hidden toggle off.
    void setup() override;
    /// Refresh the usage gauge, which walks the partition on a slow cadence.
    void tick1s() MM_NONBLOCKING override;

private:
    bool showHidden_ = false;         ///< reveal dot-prefixed entries
    uint32_t usedBytes_ = 0;          ///< bytes used, refreshed on the tick
    uint32_t totalBytes_ = 0;         ///< the partition total, read once
    uint8_t  secondsSinceScan_ = 0;   ///< paces the usage scan
};

} // namespace mm
