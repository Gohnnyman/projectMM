// RTOS task introspection — the platform half of TasksModule (src/core/TasksModule.h). The module
// does the domain work (rows, nesting under the render task); this file owns the one seam: reading
// the FreeRTOS task table so no FreeRTOS type escapes src/platform/ (the platform-boundary rule).
//
// uxTaskGetSystemState is the textbook RTOS-introspection call (Espressif examples, FreeRTOS+CLI,
// MoonLight all use it). It needs CONFIG_FREERTOS_USE_TRACE_FACILITY; when that's off the snapshot
// returns 0 and the module shows only its (free) MoonModule cost table. The per-task CPU% counter
// (ulRunTimeCounter) needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS, which costs a timer read on
// every context switch (~5% tick, measured) — so it's gated behind MM_TASK_CPU_STATS and reported
// as kTaskCpuUnmeasured when compiled out.

#include "platform/platform.h"

#include "sdkconfig.h"

#include <cstring>
#include <cstdio>

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"   // heap_caps_calloc — the one-time snapshot scratch
#endif

namespace mm::platform {

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && CONFIG_FREERTOS_USE_TRACE_FACILITY

namespace {
TaskState mapState(eTaskState s) {
    switch (s) {
        case eRunning:   return TaskState::Running;
        case eReady:     return TaskState::Ready;
        case eBlocked:   return TaskState::Blocked;
        case eSuspended: return TaskState::Suspended;
        case eDeleted:   return TaskState::Deleted;
        case eInvalid:   return TaskState::Invalid;
        default:         return TaskState::Unknown;
    }
}
}  // namespace

size_t taskSnapshot(TaskInfo* out, size_t maxTasks) {
    if (!out || maxTasks == 0) return 0;
    // Allocated ONCE, on the first snapshot — not held from boot, and not re-allocated per tick.
    //
    // The no-heap rule applies here: this runs from tick1s, which the Scheduler dispatches inside
    // tick(). One lazy allocation satisfies it — every tick after the first is allocation-free, and
    // the first one happens when the user adds TasksModule, not on the render path at steady state.
    // As a plain `static TaskStatus_t raw[40]` it cost 1440 B of INTERNAL RAM from boot on every
    // board, and TasksModule is opt-in: it appears in no device model, so the overwhelmingly common
    // case paid for a diagnostic it never used (surfaced by check_footprint's STATIC column, where
    // this file read 230 B of code against 1440 B of static).
    //
    // Kept for the process rather than freed per call: freeing would put the allocation back on
    // every tick, which is the thing the no-heap rule forbids. A module that is removed leaves the
    // buffer behind — 1440 B once used, against 1440 B always — and re-adding it reuses the same
    // one. Single-threaded (one render task calls this), so no guard is needed.
    //
    // uxTaskGetSystemState wants room for EVERY task or it returns 0, so the ceiling is generous;
    // exceeding it makes the snapshot empty that tick rather than partial. The walk also briefly
    // suspends the scheduler — an accepted once-per-second cost for an opt-in diagnostic.
    // INTERNAL, not PSRAM, even though 1440 B would fit PSRAM comfortably: uxTaskGetSystemState
    // fills this buffer with the kernel lock held (`prvENTER_CRITICAL_OR_SUSPEND_ALL(&xKernelLock)`
    // around its whole walk, FreeRTOS-Kernel/tasks.c), so every write lands inside a critical
    // section. A PSRAM cache miss there stretches that section — the one place on this chip where
    // a stall is most expensive. The 1440 B is worth spending to keep the walk deterministic.
    static constexpr size_t kScratch = 40;
    static TaskStatus_t* raw = nullptr;
    if (!raw) {
        raw = static_cast<TaskStatus_t*>(
            heap_caps_calloc(kScratch, sizeof(TaskStatus_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (!raw) return 0;   // no scratch, no snapshot: the table shows empty, nothing crashes
    }
    uint32_t totalRunTime = 0;
    const UBaseType_t got = uxTaskGetSystemState(raw, kScratch, &totalRunTime);
    const size_t n = got < maxTasks ? got : maxTasks;
    for (size_t i = 0; i < n; i++) {
        const TaskStatus_t& t = raw[i];
        TaskInfo& o = out[i];
        std::snprintf(o.name, sizeof(o.name), "%s", t.pcTaskName ? t.pcTaskName : "?");
        o.state = mapState(t.eCurrentState);
        o.priority = static_cast<uint8_t>(t.uxCurrentPriority);
        o.stackFreeBytes = t.usStackHighWaterMark;
        // TaskStatus_t.xCoreID exists only when configTASKLIST_INCLUDE_COREID is set (it is not by
        // default, even on a dual-core chip). Without it we can't know the per-task core, so report
        // -1 (unknown); the current-task-per-core view (core0/core1) still works via a separate call.
    #if defined(configTASKLIST_INCLUDE_COREID) && configTASKLIST_INCLUDE_COREID == 1
        o.core = (t.xCoreID == tskNO_AFFINITY) ? -1 : static_cast<int8_t>(t.xCoreID);
    #else
        o.core = -1;
    #endif
    #if defined(MM_TASK_CPU_STATS) && MM_TASK_CPU_STATS
        o.cpuPermille = totalRunTime ? static_cast<uint32_t>(1000ULL * t.ulRunTimeCounter / totalRunTime)
                                     : 0;
    #else
        o.cpuPermille = kTaskCpuUnmeasured;
        (void)totalRunTime;
    #endif
    }
    return n;
}

void currentTaskOnCore(int core, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    if (core < 0 || core > 1) return;
    TaskHandle_t h = xTaskGetCurrentTaskHandleForCore(core);
    if (h) std::snprintf(out, cap, "%s", pcTaskGetName(h));
#else
    (void)core;
#endif
}

// The name of the task calling this. Today everything — the render loop AND the HTTP/WS serialization
// that reads this to decide which task nests the modules — runs in app_main (FreeRTOS "main"), so the
// caller's task IS the render task, and the nesting is correct. This is TRUE ONLY while single-tasked:
// when a dedicated render task lands, serialization would still run in "main"/httpd, so the caller's
// name would NO LONGER be the render task's — this seam must then capture the render task's name from
// inside the render loop (or the Scheduler) rather than from the caller. Flagged with the multi-task
// scheduler work; correct for the single-task present.
const char* renderTaskName() { return pcTaskGetName(nullptr); }

#else  // trace facility off — inert stubs; the module falls back to its cost table only.

size_t taskSnapshot(TaskInfo*, size_t) { return 0; }
void currentTaskOnCore(int, char* out, size_t cap) { if (out && cap) out[0] = '\0'; }
const char* renderTaskName() { return ""; }   // trace facility off — no task view, no nesting anchor

#endif

}  // namespace mm::platform
