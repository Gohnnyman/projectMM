#pragma once

#include <atomic>

namespace mm {

/// A non-blocking mutual-exclusion latch guarding a resource two threads reach — in this codebase,
/// the WebSocket sender, which the render core drains on tick20ms while the offloaded PreviewDriver
/// arms and streams into it from the encode core.
///
/// **ONLY try-acquire is offered, never a blocking lock.** The hot-path rule forbids a render/encode
/// thread blocking on a peer (CLAUDE.md § Hot path — "no blocking … use try_lock"), so a caller that
/// loses the race SKIPS its slot rather than waiting. That single constraint is what lets this be an
/// `std::atomic_flag` test-and-set — the textbook lock-free try-lock — instead of an OS mutex: with
/// no waiting there is nothing to sleep on, nothing to wake, and no priority to inherit. So it costs
/// ONE atomic read-modify-write (tens of ns) with no RTOS call, and needs no init/destroy lifecycle.
/// `std::atomic_flag` is the one type the standard *guarantees* lock-free.
///
/// It lives in **core, not platform**: it is pure portable C++ with no hardware backing, so it fails
/// the platform layer's charter (the one place hardware APIs are allowed). Domain-neutral and
/// reusable — any module coordinating two threads over one resource can take it.
///
/// **NOT recursive:** a thread that already holds it must not re-acquire (test_and_set would refuse).
class TryLock {
public:
    bool tryAcquire() { return !flag_.test_and_set(std::memory_order_acquire); }
    void release() { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

/// RAII scope guard: `if (LockGuard g{lk}; g) { …exclusive… }` — releases on scope exit, no-ops when
/// the latch was busy. The standard `scoped_lock` / `unique_lock(try_to_lock)` shape.
class LockGuard {
public:
    explicit LockGuard(TryLock& l) : lock_(l), held_(l.tryAcquire()) {}
    ~LockGuard() { if (held_) lock_.release(); }
    explicit operator bool() const { return held_; }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    TryLock& lock_;
    bool held_;
};

}  // namespace mm
