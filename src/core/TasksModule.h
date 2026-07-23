#pragma once

// Header-only (exception to the core-module .h+.cpp rule): a small read-only diagnostic whose only
// platform reach is the domain-neutral taskSnapshot seam — same shape and rationale as I2cScanModule.h
// and DevicesModule.h, no translation-unit bulk to warrant a .cpp.

#include "core/MoonModule.h"
#include "core/Scheduler.h"   // instance()->moduleCount()/module(i) — the modules to profile
#include "core/JsonSink.h"    // writeListRow emits its row as JSON into the sink
#include "platform/platform.h" // taskSnapshot / TaskInfo — the RTOS task view (behind the boundary)

#include <algorithm>  // std::sort — stable row order so the list doesn't jump each refresh
#include <cstdint>
#include <cstdio>
#include <cstring>   // strcmp — match a task row to the render task

namespace mm {

/// A core, domain-neutral diagnostic that shows **what runs where**: the FreeRTOS tasks, with the
/// projectMM MoonModules that run inside each nested beneath it and their live cost. It is the
/// observability foundation for core-affinity / task-assignment work — you can't
/// optimise which module runs on which core until you can see the cost of each and where it runs.
///
/// **One nested list: `tasks`.** Each row is a FreeRTOS task (name, state, core, priority, stack
/// high-water-mark; CPU% only in a profiling build); expanding a row reveals the MoonModules that
/// run inside that task, each with its live cost — `us` (avg loop time), `class` (instance bytes),
/// `heap` (dynamic bytes). Those three are projectMM's already-collected self-report
/// (`tickTimeUs()`/`classSize()`/`dynamicBytes()`, see MoonModule), read straight off the live tree,
/// so the cost view is free. Two read-only fields (`core0`/`core1`) name the task on each core.
///
/// **Prior art.** MoonLight's `ModuleTasks` (https://github.com/MoonModules/MoonLight, doc
/// https://moonmodules.org/MoonLight/moonbase/tasks/) is a flat FreeRTOS task table via
/// `uxTaskGetSystemState` — the textbook RTOS-introspection pattern (behind the platform boundary
/// here: `platform::taskSnapshot`, no FreeRTOS type in the module). projectMM adds the per-module
/// cost nesting MoonLight lacks. The list uses `ControlType::List` + `ListSource`, the read-only
/// data-source/adapter shape (UITableView's data source, Qt's `QAbstractItemModel`) as DevicesModule.
///
/// **Bespoke, with reason.** Nesting modules *under* a task is projectMM-specific: its Scheduler runs
/// many modules cooperatively inside one render task (`tick()` calls every `tick()`), whereas in a
/// normal RTOS a task *is* the unit. Today every module runs in the one render task, so that task's
/// detail is the whole module list and other tasks are leaves; the nesting structure is ready for the
/// future core-affinity split, when modules spread across tasks. Built agile, in steps.
///
/// **Fixed System module.** Wired by code as a child of SystemModule in `main.cpp` (like Improv
/// under Network), not user-added — you don't delete Task Manager. It keeps the base `Generic` role
/// so no container accepts it as a user-editable child, and `markWiredByCode()` exempts it from the
/// persistence trim. Read-only: no user-editable controls.
class TasksModule : public MoonModule {
public:
    void defineControls() override {
        MoonModule::defineControls();
        // One list: the RTOS tasks, each row expanding to the MoonModules that run in it (the
        // per-module cost lives in that detail, not a separate flat list — the nested tree IS the
        // view). core0/core1 name the task on each core; the platform getter leaves them empty on a
        // single-core chip or a target without an RTOS, so the controls are harmless there.
        controls_.addList("tasks", tasks_);
        controls_.addReadOnly("core0", core0_, sizeof(core0_));
        controls_.addReadOnly("core1", core1_, sizeof(core1_));
    }

    /// Refresh the RTOS snapshot + the current-per-core names once a second (off the hot path). The
    /// module cost table needs no refresh — it reads the live tree on each serialize.
    void tick1s() override {
        MoonModule::tick1s();
        tasks_.refresh();
        platform::currentTaskOnCore(0, core0_, sizeof(core0_));
        platform::currentTaskOnCore(1, core1_, sizeof(core1_));
    }

private:
    /// The RTOS task table, a ListSource over a fixed snapshot the platform layer fills. Kept as a
    /// nested source (a dedicated ListSource for the `tasks` control, distinct from the module class).
    /// `refresh()` re-snapshots on tick1s; the rows serialize from
    /// the buffer. No allocation — a fixed `TaskInfo[kMaxTasks]`, filled or left at count 0 (desktop
    /// / no trace facility → the table is simply empty).
    struct TaskListSource : ListSource {
        static constexpr uint8_t kMaxTasks = 32;
        platform::TaskInfo rows_[kMaxTasks];
        uint8_t count_ = 0;

        void refresh() {
            count_ = static_cast<uint8_t>(platform::taskSnapshot(rows_, kMaxTasks));
            // uxTaskGetSystemState walks the RTOS ready/blocked lists, so a task's position in the
            // snapshot shifts as its state changes — the SAME task lands on a different row each refresh,
            // and the once-a-second list re-render makes the rows jump so you can't read them. Sort so
            // every task holds a fixed row (only the live cpu/stack values update in place), AND float
            // projectMM's OWN tasks to the top (the render task "main" and the "mm"-prefixed workers like
            // mmEncode / mmSnap — the ones the user is here to watch), sinking the RTOS system tasks
            // (IDLE, Tmr, ipc, esp_timer, …) below. Alphabetical within each group.
            std::sort(rows_, rows_ + count_, [](const platform::TaskInfo& a, const platform::TaskInfo& b) {
                const bool oursA = isProjectMMTask(a.name), oursB = isProjectMMTask(b.name);
                if (oursA != oursB) return oursA;                 // our tasks first
                return std::strcmp(a.name, b.name) < 0;           // then alphabetical, stable
            });
        }

        // A projectMM-created task: the render task (whatever platform::renderTaskName() reports — the
        // same seam writeListRowDetail uses to nest the modules, NOT a hardcoded "main") or a worker we
        // spawn (our convention names them with an "mm" prefix — mmEncode, mmSnap). Everything else is an
        // RTOS/system task.
        static bool isProjectMMTask(const char* name) {
            const char* render = platform::renderTaskName();
            return (render[0] && std::strcmp(name, render) == 0) || std::strncmp(name, "mm", 2) == 0;
        }
        uint8_t listRowCount() const override { return count_; }
        void writeListRow(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const platform::TaskInfo& t = rows_[row];
            sink.append("{\"name\":");
            sink.writeJsonString(t.name);
            sink.appendf(",\"state\":\"%s\",\"core\":%d,\"prio\":%u,\"stack\":%u",
                         stateGlyph(t.state), static_cast<int>(t.core),
                         static_cast<unsigned>(t.priority),
                         static_cast<unsigned>(t.stackFreeBytes));
            // CPU% only when it's a real measurement (run-time-stats compiled in); else omit the
            // field so the UI shows no column rather than a misleading 0.
            if (t.cpuPermille != platform::kTaskCpuUnmeasured)
                sink.appendf(",\"cpu\":%u.%u", static_cast<unsigned>(t.cpuPermille / 10u),
                             static_cast<unsigned>(t.cpuPermille % 10u));
            sink.append("}");
        }

        // Row detail = the projectMM modules running in this task. Today projectMM is single-threaded:
        // Scheduler::tick() runs every module's tick() on the one render task, so the render task's
        // detail is the whole module list and every other RTOS task has none. When a future multi-task
        // split lands, this predicate broadens to "the modules assigned to this task" — the nesting
        // structure is already here, only the mapping changes.
        //
        // Each module is a SCALAR STRING ("Name · Nus · NB · Nheap"), not a nested object, so the row
        // renders in the generic list-detail UI (fillListDetail, which shows a scalar array as chips
        // but can't display an array of objects). The dot-separated shape matches the summary rows'
        // "name · state · core · …" so the two read alike.
        void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const bool isRenderTask = platform::renderTaskName()[0] &&
                                      std::strcmp(rows_[row].name, platform::renderTaskName()) == 0;
            sink.append("{\"modules\":[");
            if (isRenderTask) {
                Scheduler* s = Scheduler::instance();
                const uint8_t mc = s ? s->moduleCount() : 0;
                uint32_t sumUs = 0;
                for (uint8_t i = 0; i < mc; i++) {
                    MoonModule* m = s->module(i);
                    if (!m) continue;
                    if (i) sink.append(",");
                    // Sum TOP-LEVEL module loop times only: a parent's tickTimeUs already includes its
                    // children's (the Scheduler times the whole parent tick(), which runs the children
                    // inside it), so summing top-level is the correct non-double-counting total.
                    sumUs += m->tickTimeUs();
                    char line[64];
                    std::snprintf(line, sizeof(line), "%s \xC2\xB7 %uus \xC2\xB7 %uB \xC2\xB7 %uheap",
                                  m->name(),
                                  static_cast<unsigned>(m->tickTimeUs()),
                                  static_cast<unsigned>(m->classSize()),
                                  static_cast<unsigned>(m->dynamicBytes()));
                    sink.writeJsonString(line);
                }
                // The closed cross-check: the top-level module loop times should account for nearly all
                // of the render tick — the small remainder is the blend/map/output + scheduler overhead
                // that isn't a module. A large remainder means significant work outside module loops.
                const uint32_t tick = s ? s->tickTimeUs() : 0;
                const uint32_t untracked = tick > sumUs ? tick - sumUs : 0;
                if (mc) sink.append(",");
                char calc[80];
                std::snprintf(calc, sizeof(calc),
                              "\xE2\x88\x91 modules %uus / tick %uus \xC2\xB7 %uus outside modules",
                              static_cast<unsigned>(sumUs), static_cast<unsigned>(tick),
                              static_cast<unsigned>(untracked));
                sink.writeJsonString(calc);
            }
            sink.append("]}");
        }
        static const char* stateGlyph(platform::TaskState s) {
            switch (s) {
                case platform::TaskState::Running:   return "running";
                case platform::TaskState::Ready:     return "ready";
                case platform::TaskState::Blocked:   return "blocked";
                case platform::TaskState::Suspended: return "suspended";
                case platform::TaskState::Deleted:   return "deleted";
                default:                             return "?";
            }
        }
    };

    TaskListSource tasks_;
    char core0_[16] = {};   // task currently on core 0 (multi-core only)
    char core1_[16] = {};   // task currently on core 1
};

} // namespace mm
