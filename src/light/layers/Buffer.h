#pragma once

#include "light/light_types.h"  // nrOfLightsType
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <span>

namespace mm {

/// Contiguous light-data buffer, shared between the layers that write it (effects)
/// and the driver groups that read it. When memory allows, layers and driver groups
/// each own a buffer (so they run in parallel); when memory is tight, one buffer is
/// shared.
///
/// **Storage:** a raw `uint8_t*` (not `RGB*`), so any channel layout fits — RGB,
/// RGBW, or multi-channel DMX fixtures — addressed by channel count + offset.
/// Allocated once via `platform::alloc` (PSRAM when available) outside the hot path
/// and reused every frame; a `std::span<uint8_t>` view is the zero-cost safe accessor.
///
/// **Locking:** a semaphore costs ~150 bytes on ESP32, so prefer lock-free patterns —
/// an atomic pointer swap for double-buffering, a single-slot SPSC handoff, or one
/// shared semaphore across layers rather than one per layer.
///
/// **Prior art:** MoonLight's `VirtualLayer.virtualChannels` — a raw `uint8_t*` sized
/// by `channelsPerLight * nrOfLights`, RGB/RGBW/DMX via LightsHeader offsets
/// (https://github.com/ewowi/MoonLight/blob/main/src/MoonLight/Effects/VirtualLayer.h).
class Buffer {
public:
    Buffer() = default;
    ~Buffer() { free(); }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept
        : data_(other.data_), count_(other.count_), channelsPerLight_(other.channelsPerLight_) {
        other.data_ = nullptr;
        other.count_ = 0;
        other.channelsPerLight_ = 0;
    }

    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            free();
            data_ = other.data_;
            count_ = other.count_;
            channelsPerLight_ = other.channelsPerLight_;
            other.data_ = nullptr;
            other.count_ = 0;
            other.channelsPerLight_ = 0;
        }
        return *this;
    }

    bool allocate(nrOfLightsType nrOfLights, uint8_t cpl) {
        free();
        size_t totalBytes = static_cast<size_t>(nrOfLights) * cpl;
        if (totalBytes == 0) return false;
        data_ = static_cast<uint8_t*>(platform::alloc(totalBytes));
        if (!data_) return false;
        count_ = nrOfLights;
        channelsPerLight_ = cpl;
        clear();
        return true;
    }

    void free() {
        if (data_) {
            platform::free(data_);
            data_ = nullptr;
        }
        count_ = 0;
        channelsPerLight_ = 0;
    }

    void clear() {
        if (data_) std::memset(data_, 0, bytes());
    }

    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }

    std::span<uint8_t> span() { return {data_, bytes()}; }
    std::span<const uint8_t> span() const { return {data_, bytes()}; }

    nrOfLightsType count() const { return count_; }
    uint8_t channelsPerLight() const { return channelsPerLight_; }
    size_t bytes() const { return static_cast<size_t>(count_) * channelsPerLight_; }

private:
    uint8_t* data_ = nullptr;
    nrOfLightsType count_ = 0;
    uint8_t channelsPerLight_ = 0;
};

} // namespace mm
