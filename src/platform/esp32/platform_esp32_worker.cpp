// Pinned worker task + wake notification — the platform half of the multicore render↔encode split
// (Drivers owns the domain logic; this file owns the one seam so no FreeRTOS type escapes
// src/platform/, the platform-boundary rule). xTaskCreatePinnedToCore + a direct-to-task
// notification (xTaskNotifyGive / ulTaskNotifyTake) is the textbook lock-free single-producer/
// single-consumer wake FreeRTOS documents as the lightweight binary-semaphore replacement — same
// pairing Espressif's own examples use for a "wake this worker" handoff. The Drivers render-split is
// the first user; the async-ArtNet send wants the identical primitive, which is why it lives here.

#include "platform/platform.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"   // the render loop subscribes itself so a wedge self-heals (reboot) not hangs

#include <atomic>
#include <new>

namespace mm::platform {

namespace {
// stopPinnedTask's join deadline. A worker normally drains within a few ms of the stop-notify; this is
// the robustness floor for the pathological case (a same-core worker that can't be scheduled, a lost
// notify). ≫ any real per-frame job, ≪ a user-perceptible hang.
constexpr uint32_t kStopJoinTimeoutMs = 300;

// The opaque WorkerTask::impl. Holds the RTOS handle, the caller's fn/user, and a stop flag the
// spawned trampoline checks.
struct EspWorker {
    TaskHandle_t handle = nullptr;
    WorkerFn fn = nullptr;
    void* user = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    // Ownership handshake for the timeout path: normally stopPinnedTask waits for `finished` and frees
    // `w`. If it TIMES OUT (the worker couldn't be scheduled — a same-core teardown), it hands ownership
    // to the trampoline by setting `detached`, and whichever of the two runs last frees `w`. The
    // atomic-exchange below makes exactly one side win, so `w` is freed once and never used-after-free.
    std::atomic<bool> detached{false};
};

// Trampoline: run the caller's fn (which owns its own loop and returns when it observes the stop flag
// via a woken waitNotify), then self-delete the RTOS task. The worker isn't registered with the task
// watchdog — it blocks in waitNotify between frames (a natural yield), and each encode is one bounded
// frame, so it never trips the idle-task WDT the way a busy-spin would; taskWdtReset() is a no-op.
void workerTrampoline(void* arg) {
    auto* w = static_cast<EspWorker*>(arg);
    w->fn(w->user);                     // runs until stopPinnedTask flips w->stop and wakes it
    w->finished.store(true, std::memory_order_release);
    // If stopPinnedTask already timed out and detached, IT is gone and won't free `w` — we own it now.
    // The exchange makes exactly one side observe `detached==true` first: whoever sees it frees `w`.
    if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
    vTaskDelete(nullptr);
}
}  // namespace

bool spawnPinnedTask(WorkerTask& t, const char* name, WorkerFn fn, void* user,
                     size_t stackBytes, uint8_t priority, int core) {
    auto* w = new (std::nothrow) EspWorker();
    if (!w) return false;
    w->fn = fn;
    w->user = user;
    // t.impl BEFORE the create: a task pinned to the SPAWNER'S OWN core preempts inside
    // xTaskCreatePinnedToCore (equal/higher priority), and its fn may call waitNotify(t) immediately —
    // with impl still null that returns false without blocking, and a retry loop becomes a hot spin
    // that starves this caller from ever assigning impl (a live-lock; measured: board offline at boot).
    // Assigning first closes the window; on create-failure it is reset before anyone can be woken.
    t.impl = w;
    const BaseType_t coreId = (core < 0) ? tskNO_AFFINITY : static_cast<BaseType_t>(core);
    const BaseType_t ok = xTaskCreatePinnedToCore(
        &workerTrampoline, name, static_cast<uint32_t>(stackBytes), w,
        static_cast<UBaseType_t>(priority), &w->handle, coreId);
    if (ok != pdPASS) { t.impl = nullptr; delete w; return false; }   // caller runs inline (degrade)
    return true;
}

void notifyTask(WorkerTask& t) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (w && w->handle) xTaskNotifyGive(w->handle);
}

bool waitNotify(WorkerTask& t, uint32_t timeoutMs) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (!w) return false;
    // ulTaskNotifyTake(pdTRUE, …) clears the notification count on return (the binary-semaphore
    // form). Non-zero return = a notify (or stop-wake) landed; 0 = timed out.
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeoutMs)) != 0;
}

void stopPinnedTask(WorkerTask& t) {
    auto* w = static_cast<EspWorker*>(t.impl);
    if (!w) return;
    w->stop.store(true, std::memory_order_release);   // the fn re-checks this after its next wake
    if (w->handle) xTaskNotifyGive(w->handle);         // wake it so it observes the stop
    // Wait for the trampoline to run its fn out and self-delete. Normally bounded by ONE wake (the fn
    // returns after it sees `stop` via a woken waitNotify). BOUNDED, never infinite: if this stop runs
    // on the SAME core the worker is pinned to and the worker is mid-job (not parked in waitNotify), the
    // worker can't be scheduled until this caller yields — and an unbounded spin here would deadlock
    // (measured: a config-change teardown of the core-0 helper hanging the whole device). vTaskDelay
    // yields, so the worker normally drains within a few ms; the deadline is the robustness floor for
    // the pathological case (a lost notify, a wedged job). On timeout we DETACH — leak the worker rather
    // than free-while-running (a use-after-free is worse than a bounded leak) — and its stop flag stays
    // set, so if it ever wakes it self-deletes cleanly against the still-live EspWorker.
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kStopJoinTimeoutMs);
    while (!w->finished.load(std::memory_order_acquire)) {
        if (xTaskGetTickCount() > deadline) {
            // DETACH — hand `w` to the trampoline. The exchange makes exactly one side free it: if the
            // trampoline already ran (raced us to `detached`), WE free `w` here; otherwise the trampoline
            // frees it when it finally exits. Either way `w` is freed once, never used-after-free, and
            // this caller returns instead of deadlocking (see the timeout rationale above).
            if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
            t.impl = nullptr;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    // NORMAL path frees through the SAME exchange as the timeout path — not a bare delete. The trampoline
    // touches `w->detached` AFTER storing `finished` (see workerTrampoline), so once we observe finished
    // the trampoline may still be mid-exchange on `w`; a bare delete here would free it under that access.
    // The exchange makes exactly one side win the free, whichever runs the store last.
    if (w->detached.exchange(true, std::memory_order_acq_rel)) delete w;
    t.impl = nullptr;
}

// Whether taskWdtSubscribe succeeded — so taskWdtReset only feeds a real subscription. Written once at
// render-loop start, read each tick on the same (render) task, so no synchronization is needed.
static bool s_renderWdtSubscribed = false;

// Subscribe the CURRENT (render-loop) task to the task WDT. The sdkconfig runs the TWDT with idle-task
// checking OFF (a saturated core is healthy), so nothing is watched unless a task subscribes — this is
// that one subscription: if the render loop stops feeding the WDT (a genuine wedge, not a busy frame),
// it panics and reboots (the self-heal) instead of hanging silently, and leaves a backtrace. Idempotent
// enough for one caller; a failure (WDT not inited) just leaves s_renderWdtSubscribed false and reset a
// no-op, degrading to today's unwatched behavior rather than crashing.
void taskWdtSubscribe() {
    if (s_renderWdtSubscribed) return;
    if (esp_task_wdt_add(nullptr) == ESP_OK) s_renderWdtSubscribed = true;
}

// Feed the render task's WDT subscription (esp_task_wdt_reset), called each render tick. No-op until/unless
// taskWdtSubscribe ran, so a build/config without the WDT (or a worker that never subscribed) is unaffected.
void taskWdtReset() {
    if (s_renderWdtSubscribed) esp_task_wdt_reset();
}

}  // namespace mm::platform
