#pragma once

// Header-only (exception to the core-module .h+.cpp rule): a small read-only diagnostic that only
// reads the live module tree — no platform reach, no translation-unit bulk — same shape and rationale
// as TasksModule.h, I2cScanModule.h and DevicesModule.h.

#include "core/MoonModule.h"
#include "core/Scheduler.h"   // instance()->moduleCount()/module(i) — the roots of the tree to walk
#include "core/JsonSink.h"    // writeListRow emits its row as JSON into the sink
#include "core/Sort.h"        // insertionSort — order the map by GPIO (Device-Manager keying)
#include "light/drivers/PinList.h"  // parsePinList — the shared "18,19,20" CSV parser the drivers use

#include <cstdint>
#include <cstdio>
#include <cstring>   // strcmp / strstr — match control names to a role

namespace mm {

/// A core, domain-neutral diagnostic that shows **which module owns each GPIO, for what role** — the
/// device's pin ownership map, keyed by physical GPIO number the way an OS Device Manager, a Tasmota
/// template, or a GPIOViewer diagram is. It walks the live module tree and collects every claimed pin:
/// each `ControlType::Pin` control (a mic's `sckPin`/`wsPin`/`sdPin`, an Ethernet PHY's `ethMdcGpio`,
/// a driver's `loopbackTxPin`) and each `"pins"` text control (an LED driver's `"18,19,20"` lane CSV).
///
/// **One nested list: `pins`.** Each row is a claimed GPIO (`gpio`, `owner`, `role`); a GPIO claimed by
/// more than one control shows the first owner in the summary and every claimant in the row detail, so a
/// double-claim is *visible at a glance* — the exact class of bug the GPIO-46 loopback corruption was (an
/// output role driven onto a strap). Surfacing only: this reads the tree, it does not arbitrate. The
/// controls are already the pin registry; this module is a reader over them, holding no state of its own.
///
/// **Inversion vs the central-manager pattern.** MoonLight's `ModuleIO`
/// (https://github.com/MoonModules/MoonLight) is the *central* pin manager — it owns a JSON pin table and
/// assigns GPIOs. projectMM inverts that: **each module owns its own pins** (as its own controls), and this
/// one central module only *observes* and coordinates. So the reusable idea taken from ModuleIO is its
/// per-pin *report* — owner + usage — not its ownership mechanism; here the owner and role are read back
/// out of the live controls, never from a central table. The role column is name-derived (see `roleFor`),
/// projectMM's equivalent of ModuleIO's `usage` enum without a central vocabulary.
///
/// **Fixed System module.** Wired by code as a child of SystemModule in `main.cpp` (like Tasks and I2cScan),
/// not user-added — you don't delete the pin map. Base `Generic` role so no container accepts it as a
/// user-editable child; `markWiredByCode()` exempts it from the persistence trim. Read-only: no editable
/// controls. The list refreshes on `loop1s()` (a periodic sample, never the hot path), so a live pin change
/// shows on the next second with no reboot. The list uses `ControlType::List` + `ListSource`, the read-only
/// data-source/adapter shape (UITableView's data source, Qt's `QAbstractItemModel`) as TasksModule.
class PinsModule : public MoonModule {
public:
    void onBuildControls() override {
        MoonModule::onBuildControls();
        controls_.addList("pins", pins_);
    }

    /// Re-walk the tree once a second (off the hot path) to rebuild the claim map. Each claim copies its
    /// owner name + role into its own storage, so the rows serialize safely even if the owning module is
    /// deleted between refreshes — the snapshot holds no pointers into module memory. The next refresh
    /// rebuilds from the live tree, so a deleted module's claim drops within a second.
    void loop1s() override {
        MoonModule::loop1s();
        pins_.refresh();
    }

private:
    /// The pin ownership map: a ListSource over a fixed snapshot rebuilt on loop1s. A claim is one GPIO
    /// staked by one control; multiple claims on the same GPIO are kept (a conflict must stay visible, not
    /// be merged away). No allocation — a fixed `Claim[kMaxClaims]`, filled or left at count 0.
    struct PinListSource : ListSource {
        // 64 claims covers any realistic board (a P4 has ~55 GPIOs; a fully-loaded tree of drivers + mic +
        // Ethernet claims far fewer). A diagnostic, so overflow just stops adding rather than allocating.
        static constexpr uint8_t kMaxClaims = 64;
        struct Claim {
            uint8_t gpio;         // the physical GPIO number (row key)
            char owner[16];       // owning module's name(), COPIED in — not a pointer into module storage,
                                  // which a delete frees between refreshes (the UI serializes state right
                                  // after a delete op, so a borrowed pointer would be a live use-after-free,
                                  // not a rare race). 16 matches MoonModule's name_[16] cap.
            char role[14];        // the role, copied in (own storage): a name-derived label ("BCLK",
                                  // "LED lane 0") or the control name. Owned per-claim, not a shared
                                  // buffer, so N lane claims don't collapse onto one string. 14 = the
                                  // worst case "LED lane 255"+NUL (laneIdx is a uint8) and "loopback Tx".
        };
        Claim claims_[kMaxClaims];
        uint8_t count_ = 0;

        void refresh() {
            count_ = 0;
            Scheduler* s = Scheduler::instance();
            const uint8_t mc = s ? s->moduleCount() : 0;
            for (uint8_t i = 0; i < mc; i++)
                collect(s->module(i));
            // Order by GPIO so the map reads like a Device Manager (ascending pin), stable for the eye and
            // for drift-tracked scenario snapshots. Insertion sort: tiny n, no allocation.
            insertionSort(claims_, count_, [](const Claim& a, const Claim& b) { return a.gpio < b.gpio; });
        }

        uint8_t listRowCount() const override { return count_; }

        void writeListRow(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const Claim& c = claims_[row];
            sink.appendf("{\"gpio\":%u,\"owner\":", static_cast<unsigned>(c.gpio));
            sink.writeJsonString(c.owner);
            sink.append(",\"role\":");
            sink.writeJsonString(c.role);
            sink.append("}");
        }

        // Row detail = every claim on this row's GPIO. With one claimant it repeats the summary; with two
        // or more it lists them all as "owner · role" chips — the visible double-claim (read-only surfacing,
        // no enforcement, which is phase 2). Scalar strings, so the generic list-detail UI renders them.
        void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
            if (row >= count_) { sink.append("{}"); return; }
            const uint8_t gpio = claims_[row].gpio;
            sink.append("{\"claims\":[");
            bool first = true;
            for (uint8_t i = 0; i < count_; i++) {
                if (claims_[i].gpio != gpio) continue;
                if (!first) sink.append(",");
                first = false;
                char line[80];
                std::snprintf(line, sizeof(line), "%s \xC2\xB7 %s", claims_[i].owner, claims_[i].role);
                sink.writeJsonString(line);
            }
            sink.append("]}");
        }

    private:
        // Recurse a module + its children, recording every claimed pin. Depth-first so children's pins list
        // under the whole tree, not just the roots.
        void collect(MoonModule* m) {
            if (!m) return;
            const ControlList& cl = m->controls();
            for (uint8_t i = 0; i < cl.count(); i++) {
                const ControlDescriptor& d = cl[i];
                if (d.type == ControlType::Pin) {
                    const int8_t v = *static_cast<int8_t*>(d.ptr);
                    if (v >= 0) addPinClaim(static_cast<uint8_t>(v), m->name(), roleFor(d.name));
                } else if (d.type == ControlType::Text && std::strcmp(d.name, "pins") == 0) {
                    // The LED-driver lane CSV ("18,19,20"): one claim per pin, role "LED lane N". parsePinList
                    // dedups within the one string, so a lane list never self-collides here.
                    uint16_t pins[kMaxClaims];
                    uint8_t n = 0;
                    if (!parsePinList(static_cast<const char*>(d.ptr), pins, kMaxClaims, n))
                        for (uint8_t p = 0; p < n; p++)
                            // parsePinList accepts up to 65535 (a typo like "300"); skip anything past the
                            // chip's GPIO ceiling so a bad entry never wraps to a real GPIO in the map.
                            if (pins[p] <= MM_MAX_GPIO)
                                addLaneClaim(static_cast<uint8_t>(pins[p]), m->name(), p);
                }
            }
            for (uint8_t i = 0; i < m->childCount(); i++)
                collect(m->child(i));
        }

        // The claim-adders copy BOTH owner and role into the claim's own buffers, so no claim holds a
        // pointer into module or shared storage — the snapshot survives a module delete between refreshes.
        Claim* reserve(uint8_t gpio, const char* owner) {
            if (count_ >= kMaxClaims) return nullptr;   // diagnostic: drop past the cap, never allocate
            Claim& c = claims_[count_++];
            c.gpio = gpio;
            std::strncpy(c.owner, owner ? owner : "", sizeof(c.owner) - 1);
            c.owner[sizeof(c.owner) - 1] = '\0';
            return &c;
        }
        void addPinClaim(uint8_t gpio, const char* owner, const char* role) {
            if (Claim* c = reserve(gpio, owner)) {
                std::strncpy(c->role, role, sizeof(c->role) - 1);
                c->role[sizeof(c->role) - 1] = '\0';
            }
        }
        void addLaneClaim(uint8_t gpio, const char* owner, uint8_t laneIdx) {
            if (Claim* c = reserve(gpio, owner))
                std::snprintf(c->role, sizeof(c->role), "LED lane %u", static_cast<unsigned>(laneIdx));
        }

        // Name → role: projectMM's name-derived equivalent of ModuleIO's usage enum. A small display
        // convenience, not a vocabulary authority — an unmatched name falls through to itself, which is
        // still informative. Returns a static literal (or the control name, which lives in static/module
        // storage); the caller copies it into the claim, so the returned pointer needn't outlive the call.
        static const char* roleFor(const char* name) {
            struct Entry { const char* suffix; const char* role; };
            static constexpr Entry kRoles[] = {
                {"sckPin",        "BCLK"},        {"wsPin",         "WS"},
                {"sdPin",         "data"},        {"mclkPin",       "MCLK"},
                {"loopbackTxPin", "loopback Tx"}, {"loopbackRxPin", "loopback Rx"},
                {"ethMdcGpio",    "MDC"},         {"ethMdioGpio",   "MDIO"},
                {"sda",           "I\xC2\xB2""C SDA"}, {"scl",       "I\xC2\xB2""C SCL"},
            };
            for (const Entry& e : kRoles)
                if (std::strcmp(name, e.suffix) == 0) return e.role;
            return name;   // fall through to the control name — still informative
        }
    };

    PinListSource pins_;
};

} // namespace mm
