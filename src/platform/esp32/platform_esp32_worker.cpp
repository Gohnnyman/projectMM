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

#include <atomic>
#include <new>

namespace mm::platform {

namespace {
// The opaque WorkerTask::impl. Holds the RTOS handle, the caller's fn/user, and a stop flag the
// spawned trampoline checks.
struct EspWorker {
    TaskHandle_t handle = nullptr;
    WorkerFn fn = nullptr;
    void* user = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
};

// Trampoline: run the caller's fn (which owns its own loop and returns when it observes the stop flag
// via a woken waitNotify), then self-delete the RTOS task. The worker isn't registered with the task
// watchdog — it blocks in waitNotify between frames (a natural yield), and each encode is one bounded
// frame, so it never trips the idle-task WDT the way a busy-spin would; taskWdtReset() is a no-op.
void workerTrampoline(void* arg) {
    auto* w = static_cast<EspWorker*>(arg);
    w->fn(w->user);                     // runs until stopPinnedTask flips w->stop and wakes it
    w->finished.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}
}  // namespace

bool spawnPinnedTask(WorkerTask& t, const char* name, WorkerFn fn, void* user,
                     size_t stackBytes, uint8_t priority, int core) {
    auto* w = new (std::nothrow) EspWorker();
    if (!w) return false;
    w->fn = fn;
    w->user = user;
    const BaseType_t coreId = (core < 0) ? tskNO_AFFINITY : static_cast<BaseType_t>(core);
    const BaseType_t ok = xTaskCreatePinnedToCore(
        &workerTrampoline, name, static_cast<uint32_t>(stackBytes), w,
        static_cast<UBaseType_t>(priority), &w->handle, coreId);
    if (ok != pdPASS) { delete w; return false; }   // caller runs inline (degrade)
    t.impl = w;
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
    // Wait for the trampoline to run its fn out and self-delete. The fn returns only after it sees
    // stop via a woken waitNotify, so this is bounded by one encode; poll off the hot path.
    while (!w->finished.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(1));
    delete w;
    t.impl = nullptr;
}

// No-op: the worker isn't registered with the task WDT (it yields via waitNotify between bounded
// per-frame encodes). Kept as a seam so the domain code can call it uniformly across platforms.
void taskWdtReset() {}

}  // namespace mm::platform
