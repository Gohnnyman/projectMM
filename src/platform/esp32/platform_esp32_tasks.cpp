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
    // Fixed static scratch, NOT a malloc: this runs from loop1s, which the Scheduler dispatches inside
    // tick() — the hot path, where the no-heap rule applies. uxTaskGetSystemState wants a buffer for
    // every task (a short one returns 0), so the scratch is sized to a generous ceiling; if the system
    // ever exceeds it uxTaskGetSystemState returns 0 and the snapshot is simply empty that tick. The
    // walk still briefly suspends the scheduler — an accepted once-per-second cost for a diagnostic the
    // user opted into by adding the module; it is not per-frame. Single-threaded access (one render
    // task calls this), so a plain static needs no guard.
    static constexpr size_t kScratch = 40;
    static TaskStatus_t raw[kScratch];
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
