// @module TasksModule

// Pins TasksModule's shape: one `tasks` List (rows = RTOS tasks) whose row-detail nests the
// MoonModules running in that task, plus core0/core1. On the host there is no FreeRTOS, so
// platform::taskSnapshot returns 0 → the tasks list is empty; the ESP32 path (a populated list
// with the render task's detail nesting every module) is verified on hardware. So the host test
// covers the module's control set + that the empty-snapshot path is safe, not the populated rows.

#include "doctest.h"
#include "core/TasksModule.h"
#include "core/Scheduler.h"
#include "core/JsonSink.h"

#include <cstring>
#include <string>

using namespace mm;

namespace {

// A minimal module with a fixed reported cost, to appear nested under the render task.
struct FakeModule : MoonModule {
    FakeModule(const char* n, size_t klass, size_t heap) {
        setName(n); setClassSize(klass); setDynamicBytes(heap);
    }
};

// Find a control by name in a module's control list.
bool hasControl(const MoonModule& m, const char* name) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, name) == 0) return true;
    return false;
}

// Reach the `tasks` control's ListSource (the inner TaskListSource) so the row/detail JSON is
// exercised directly — the same access unit_Control_list uses (descriptor.ptr → ListSource*).
const ListSource* tasksSource(const MoonModule& m) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, "tasks") == 0)
            return static_cast<const ListSource*>(m.controls()[i].ptr);
    return nullptr;
}

} // namespace

TEST_CASE("TasksModule: exposes a tasks list + core0/core1, no separate modules list") {
    Scheduler scheduler;
    TasksModule tasks;
    scheduler.addModule(&tasks);
    scheduler.setup();

    CHECK(hasControl(tasks, "tasks"));
    CHECK(hasControl(tasks, "core0"));
    CHECK(hasControl(tasks, "core1"));
    // No standalone `modules` list — the per-module cost lives in the tasks row-detail.
    CHECK_FALSE(hasControl(tasks, "modules"));
}

TEST_CASE("TasksModule: a fixed System module (Generic role, no delete affordance)") {
    TasksModule tasks;
    // Tasks is wired-by-code as a System child (main.cpp), not user-added — so it keeps the base
    // Generic role, which no container accepts as a user-editable child. That's what makes the UI
    // render no delete/replace: a fixed inspection module, like Task Manager you don't delete.
    CHECK(tasks.role() == ModuleRole::Generic);
}

TEST_CASE("TasksModule: the empty desktop snapshot is safe (no RTOS on host)") {
    // loop1s refreshes the (empty on host) task snapshot + fills core0/core1 (empty strings). This
    // just confirms the refresh path doesn't crash and the tasks list serializes to an empty array.
    platform::setTestTaskSnapshot(nullptr, 0, "");
    Scheduler scheduler;
    TasksModule tasks;
    scheduler.addModule(&tasks);
    scheduler.setup();
    tasks.loop1s();   // platform::taskSnapshot returns 0 on desktop → 0 rows, no crash
    CHECK(true);      // reaching here without a crash is the assertion
}

TEST_CASE("TasksModule: the tasks list renders the injected RTOS tasks with their fields") {
    // Inject a canned snapshot (desktop test seam) → the `tasks` List serializes those rows with
    // name/state/core/prio/stack, and the CPU% field appears only when it's a real measurement.
    const platform::TaskInfo snap[] = {
        {"main",  platform::TaskState::Running, 0, 1, 2340, 479 /* 47.9% */},
        {"IDLE1", platform::TaskState::Ready,   1, 0, 1100, platform::kTaskCpuUnmeasured},
    };
    platform::setTestTaskSnapshot(snap, 2, "main");

    Scheduler scheduler;
    TasksModule tasks;
    scheduler.addModule(&tasks);
    scheduler.setup();
    tasks.loop1s();   // pulls the injected snapshot into the ListSource

    const ListSource* src = tasksSource(tasks);
    REQUIRE(src != nullptr);
    REQUIRE(src->listRowCount() == 2);

    JsonSink r0; src->writeListRow(r0, 0);
    std::string row0(r0.data());
    CHECK(row0.find("\"name\":\"main\"") != std::string::npos);
    CHECK(row0.find("\"state\":\"running\"") != std::string::npos);
    CHECK(row0.find("\"core\":0") != std::string::npos);
    CHECK(row0.find("\"prio\":1") != std::string::npos);
    CHECK(row0.find("\"stack\":2340") != std::string::npos);
    CHECK(row0.find("\"cpu\":47.9") != std::string::npos);   // real measurement → shown

    JsonSink r1; src->writeListRow(r1, 1);
    std::string row1(r1.data());
    CHECK(row1.find("\"name\":\"IDLE1\"") != std::string::npos);
    CHECK(row1.find("\"cpu\":") == std::string::npos);       // kTaskCpuUnmeasured → field omitted
}

TEST_CASE("TasksModule: the render task's detail nests the modules + the ∑/tick cross-check") {
    // The render task ("main") detail lists the scheduled modules with their cost, plus the closing
    // "∑ modules … / tick …" line; a NON-render task's detail is an empty modules array.
    const platform::TaskInfo snap[] = {
        {"main",  platform::TaskState::Running, 0, 1, 2340, platform::kTaskCpuUnmeasured},
        {"IDLE1", platform::TaskState::Ready,   1, 0, 1100, platform::kTaskCpuUnmeasured},
    };
    platform::setTestTaskSnapshot(snap, 2, "main");

    Scheduler scheduler;
    FakeModule a("Alpha", 128, 0);
    FakeModule b("Beta", 384, 4096);
    TasksModule tasks;
    scheduler.addModule(&a);
    scheduler.addModule(&b);
    scheduler.addModule(&tasks);
    scheduler.setup();
    tasks.loop1s();

    const ListSource* src = tasksSource(tasks);
    REQUIRE(src != nullptr);

    // Row 0 ("main") = the render task → its detail nests the modules + the cross-check line.
    JsonSink d0; src->writeListRowDetail(d0, 0);
    std::string det0(d0.data());
    CHECK(det0.find("Alpha") != std::string::npos);
    CHECK(det0.find("class=") == std::string::npos);        // modules are scalar strings, not objects
    CHECK(det0.find("Alpha \xC2\xB7 0us \xC2\xB7 128B \xC2\xB7 0heap") != std::string::npos);
    CHECK(det0.find("Beta \xC2\xB7 0us \xC2\xB7 384B \xC2\xB7 4096heap") != std::string::npos);
    CHECK(det0.find("\xE2\x88\x91 modules") != std::string::npos);   // the ∑ cross-check line
    CHECK(det0.find("outside modules") != std::string::npos);

    // Row 1 ("IDLE1") = NOT the render task → empty modules array, no cross-check.
    JsonSink d1; src->writeListRowDetail(d1, 1);
    std::string det1(d1.data());
    CHECK(det1.find("Alpha") == std::string::npos);
    CHECK(det1.find("\"modules\":[]") != std::string::npos);
}
