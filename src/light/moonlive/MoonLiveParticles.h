#pragma once

#include "core/MoonModule.h"
#include "core/ScratchBuffer.h"
#include "light/particles.h"

namespace mm::moonlive {

/// One scripted module's particle pool: the buffers the particles live in, the `particles::Pool`
/// view over them, and the frame clock that makes the physics run at the same speed everywhere.
///
/// Held BY VALUE in the binding, the shape MoonLiveScript already established: a concern the three
/// scripted bindings would each grow their own copy of gets one home rather than a shared base.
///
/// **Why the pool cannot live in the script's own arena.** A Pool is eight parallel arrays. The
/// script arena is 64 bytes across 8 members, so a script could hold about five particles against
/// the hundreds a particle look needs. Widening the arena is the wrong answer: `sizeof(MoonLive)`
/// is held by value in every scripted module and probed on the main task's stack by registerType,
/// which boot-looped the P4 at 1440 bytes. So the particles live OUTSIDE the arena, in
/// ScratchBuffers the binding owns, and the script only ever names whole-pool operations.
///
/// Six buffers, not eight: `acc` and `size` are documented optional in particles.h (`valid()` does
/// not require them) and neither feeds a builtin. FireworksEffect sizes exactly these six.
class MoonLiveParticles {
public:
    explicit MoonLiveParticles(MoonModule& owner)
        : x_(owner), y_(owner), vx_(owner), vy_(owner), ttl_(owner), hue_(owner) {}

    /// Size the pool to `count` particles, or free it at 0. Returns the count actually available,
    /// which is 0 when the allocation failed: that is what a script sees, so a device with less
    /// PSRAM than the author assumed reports the truth rather than rendering nothing in silence.
    ///
    /// A failed resize must leave `valid()` false rather than a stale pool pointing at freed
    /// memory, which is the trap ParticlesEffect documents at its own prepare().
    uint16_t resize(uint16_t count) {
        if (count == 0) { release(); return 0; }
        const bool ok = x_.resize(count) && y_.resize(count) && vx_.resize(count) &&
                        vy_.resize(count) && ttl_.resize(count) && hue_.resize(count);
        if (!ok) { release(); return 0; }
        pool_ = particles::Pool{};
        pool_.x = x_.data(); pool_.y = y_.data();
        pool_.vx = vx_.data(); pool_.vy = vy_.data();
        pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
        pool_.count = count;
        pool_.clear();
        time_.reset();
        return count;
    }

    /// Free every buffer and leave the pool invalid. Called from the binding's release(), before it
    /// chains to the base: MoonModule::release() frees the buffers on its own free-list walk, but
    /// the Pool's pointers would still name that freed memory.
    void release() {
        x_.resize(0); y_.resize(0); vx_.resize(0); vy_.resize(0); ttl_.resize(0); hue_.resize(0);
        pool_ = particles::Pool{};
        time_.reset();
    }

    particles::Pool& pool() MM_NONBLOCKING { return pool_; }
    uint16_t count() const MM_NONBLOCKING { return pool_.count; }

    /// How much of a reference frame this frame covered, in 8.8 fixed point. Every per-frame
    /// builtin passes this to the kernel, so framerate independence is a property of the system
    /// rather than something a script author remembers to type.
    uint32_t advance(uint32_t nowMs) MM_NONBLOCKING { return time_.advance(nowMs); }

private:
    ScratchBuffer<draw::pos_t> x_, y_, vx_, vy_;
    ScratchBuffer<uint16_t>    ttl_;
    ScratchBuffer<uint8_t>     hue_;
    particles::Pool            pool_;
    particles::FrameTime       time_{60};
};

}  // namespace mm::moonlive
