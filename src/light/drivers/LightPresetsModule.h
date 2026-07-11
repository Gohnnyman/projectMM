#pragma once

#include "core/MoonModule.h"
#include "core/ActiveInstance.h"     // the singleton seat drivers resolve the library through
#include "core/ScratchBuffer.h"      // the dynamic (no-cap) role pool
#include "core/JsonSink.h"
#include "core/JsonUtil.h"           // restoreList: recursive reader for the persisted array
#include "light/ChannelRole.h"
#include "light/drivers/Correction.h"  // LightPreset + fillRolesFromPreset + the derived offsets

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

/// The reusable light-preset library — a Drivers submodule (child role `preset`) that owns a set
/// of NAMED channel-role wirings, each editable in its own row and referenced by many drivers.
///
/// A "light preset" is a channel-role layout: role `r` at channel `i` says channel `i` of a light
/// carries role `r` (Red/Green/Blue/White/WarmWhite/Yellow/UV, and the fixture roles Pan/Tilt/…).
/// A curated set of real fixtures is SEEDED as read-only (`locked`) rows on first boot: the colour
/// orders (RGB, GRB, BGR, RGBW, GRBW, WRGB), the multi-channel LED/par fixtures (Curtain GRB6,
/// Lightbar RGBWYP, RGBCCT, IRGB), and moving heads (MH BeeEyes 15, MH BeTopper 32, MH 19x15W-24).
/// A user adds custom named wirings alongside them. A
/// driver stores only a preset's STABLE id; it resolves that id here at cold-path rebuild time into
/// its own `Correction` (see `deriveCorrection`) and holds nothing else. So building a wiring once
/// makes it reusable across every driver, and reordering / deleting other presets never disturbs a
/// reference (the id is invariant; the row index is not).
///
/// **Storage is uncapped.** A preset is exactly as wide as its fixture — role bytes live in one
/// dynamic pool (`rolePool_`, a ScratchBuffer), each preset a `{poolOffset, channelCount}` slice.
/// No fixed channel ceiling: a moving head can declare as many channels as it has. The pool is
/// touched only on the cold path (edit / restore); nothing binds a control to it, so it is free to
/// reallocate — the control-bind stable-address rule that pins DriverBase's arrays does not apply.
///
/// It's the first consumer of the editable-list primitive (`EditableListSource`): the whole
/// add / delete / reorder / per-field-edit UI comes from that, reused rather than re-built (custom
/// palettes reuse the same primitive later). The per-row fields are the preset's name, its channel
/// count, and one role picker per channel.
///
/// **Hot-path guarantee.** The render loop never reads this module. A driver resolves its preset id
/// into its own `Correction` offset cache ONCE per config change (`deriveCorrection`); per-light
/// `apply()` reads only that cache. A library preset costs exactly what an inline one did per light.
/// @card lightpresets.png
class LightPresetsModule : public MoonModule, public ListSource {
public:
    ModuleRole role() const override { return ModuleRole::Generic; }

    // A boot-wired singleton (exactly one, added under Drivers at boot): not user-deletable, the same
    // as the boot-wired PreviewDriver. Drivers accepts only `driver` children, so a deleted preset
    // library could never be re-added — and every driver resolves its preset through this one, so
    // losing it would break them all. The one library is permanent; the presets INSIDE it are editable.
    bool userEditable() const override { return false; }

    static constexpr uint8_t kMaxPresets = 32;   // bounded row count; a device won't wire more light types

    /// The boot library (exactly one under Drivers). A driver resolves its preset reference through
    /// this static seam — no compile-time dependency on the module's address, same shape as
    /// DevicesModule::active() / AudioService::latestFrame().
    static LightPresetsModule* active() { return ActiveInstance<LightPresetsModule>::active(); }

    /// The id of the first preset (a safe default reference for a fresh driver / a dangling id).
    /// Always valid once the built-ins are seeded.
    uint32_t defaultId() const { return count_ ? presets_[0].id : 0; }

    /// Row count / name / id accessors so a driver can build its `preset` Select (names for the
    /// options, ids for the stable reference it stores). The Select's chosen INDEX maps to a preset
    /// id via idAt(index); a driver keeps the id, not the index — so reorder/delete never re-points
    /// it. indexOfId() is the inverse, for rendering the Select at the currently-referenced preset.
    uint8_t presetCount() const { return count_; }
    const char* nameAt(uint8_t i) const { return i < count_ ? presets_[i].name : ""; }
    uint32_t idAt(uint8_t i) const { return i < count_ ? presets_[i].id : 0; }
    uint8_t indexOfId(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (presets_[i].id == id) return i;
        return 0;   // a dangling id renders at the first preset; rebuildCorrection falls back to it too
    }

    /// Resolve `id` into `out` — fill its brightness LUT + channel-role offsets from the preset's
    /// wiring. Returns true if the id resolved; on a missing id (a deleted preset) returns false and
    /// leaves `out` untouched, so the caller keeps its safe default (never crashes). COLD PATH: a
    /// driver calls this from rebuildCorrection, never from the render loop. This is the single point
    /// a preset's roles become a driver's flat `Correction` cache — the render path never sees a role.
    /// Does the preset carry a White channel? A driver uses this to hide its whiteMode control when
    /// the referenced preset has no white to synthesise (an RGB/GRB strip). Missing id → false.
    bool presetHasWhite(uint32_t id) const {
        const Preset* p = find(id);
        if (!p) return false;
        const uint8_t* r = roleAt(*p);
        for (uint8_t c = 0; c < p->channelCount; c++) {
            const ChannelRole role = static_cast<ChannelRole>(r[c]);
            if (role == ChannelRole::White || role == ChannelRole::WarmWhite) return true;
        }
        return false;
    }

    bool deriveCorrection(uint32_t id, uint8_t brightness, Correction& out) const {
        const Preset* p = find(id);
        if (!p) return false;
        // The pool stores role bytes as indices into kChannelRoleOptions, which are index-aligned
        // with the ChannelRole enum (enum : uint8_t) — so a role byte IS a ChannelRole value. The
        // reinterpret is a view of the same bytes, no copy (the array can be wide — a moving head).
        out.rebuild(brightness, reinterpret_cast<const ChannelRole*>(roleAt(*p)), p->channelCount);
        return true;
    }

    // The singleton seat is claimed at CONSTRUCTION, not in prepare() — a driver resolves the library
    // via active() while building its `preset` Select in defineControls() (phase 1) and rebuildControls
    // (phase 2b), both BEFORE the module's own prepare() (phase 4). Claiming later left active() null
    // then, so every driver's preset dropdown showed "(none)". Exactly one instance exists (boot-wired
    // singleton), so claim-at-construction is unambiguous; the RAII vacate on destruct still guards
    // the dangling-static case.
    LightPresetsModule() { seat_.claim(); }

    void setup() override {
        MoonModule::setup();
        if (count_ == 0) seedBuiltins();   // belt-and-braces; defineControls already seeds if empty
        refreshStatus();
    }

    void prepare() override { seat_.claim(); }   // idempotent — no-op while we already hold the seat
    void release() override { seat_.vacate(); MoonModule::release(); }

    void defineControls() override {
        MoonModule::defineControls();
        // Seed the curated built-ins if the set is empty, so a driver building its preset Select in
        // this same phase-1 pass sees real names (not "(none)"). restoreList (phase 2) replaces the
        // set with the persisted rows — which INCLUDE the built-ins — so this never double-seeds.
        if (count_ == 0) seedBuiltins();
        controls_.addList("presets", *this);   // this module is the (editable) ListSource
    }

    // --- ListSource (editable) ---------------------------------------------------------
    uint8_t listRowCount() const override { return count_; }

    // The row SUMMARY is also the PERSISTED form (only the List value round-trips to the saved file,
    // not the detail), so it must be complete: id, name, channel count, the roles array, and locked.
    // restoreList reads exactly these fields back — a custom preset's full wiring survives a reboot.
    void writeListRow(JsonSink& sink, uint8_t row) const override {
        const Preset& p = presets_[row];
        const uint8_t* roles = roleAt(p);
        sink.appendf("{\"id\":%lu,\"name\":\"%s\",\"channels\":%u,\"roles\":[",
                     static_cast<unsigned long>(p.id), p.name, static_cast<unsigned>(p.channelCount));
        for (uint8_t c = 0; c < p.channelCount; c++)
            sink.appendf("%s%u", c ? "," : "", static_cast<unsigned>(roles[c]));
        sink.append("]");
        if (p.locked) sink.append(",\"locked\":true");
        sink.append("}");
    }

    /// The row's editable field descriptors: name (text), channels (the fixture width), and one role
    /// Select per channel — rendered generically by the editable-list primitive. Every channel is
    /// shown (including unmapped "—" ones), so the editor mirrors the fixture's full width. A locked
    /// (built-in) row still emits its fields; the row's `locked` flag makes the UI render them
    /// read-only. (A sparse editor — showing only mapped channels + an add-channel affordance — is a
    /// future refinement; kept dense here so it's straightforward and reliable.)
    void writeListRowDetail(JsonSink& sink, uint8_t row) const override {
        const Preset& p = presets_[row];
        const uint8_t* roles = roleAt(p);
        sink.append("{\"fields\":[");
        sink.appendf("{\"name\":\"name\",\"type\":\"text\",\"value\":\"%s\"},", p.name);
        sink.appendf("{\"name\":\"channels\",\"type\":\"uint8\",\"value\":%u,\"min\":1,\"max\":255}",
                     static_cast<unsigned>(p.channelCount));
        for (uint8_t c = 0; c < p.channelCount; c++) {
            sink.appendf(",{\"name\":\"ch%u\",\"type\":\"select\",\"value\":%u,\"options\":[",
                         static_cast<unsigned>(c), static_cast<unsigned>(roles[c]));
            for (uint8_t o = 0; o < kChannelRoleCount; o++)
                sink.appendf("%s\"%s\"", o ? "," : "", kChannelRoleOptions[o]);
            sink.append("]}");
        }
        sink.append("]}");
    }

    bool isEditableList() const override { return true; }

    bool addListRow(uint32_t& outId) override {
        if (count_ >= kMaxPresets) return false;
        Preset& p = presets_[count_];
        p = Preset{};
        p.id = nextId_++;
        p.channelCount = 3;
        std::snprintf(p.name, sizeof(p.name), "preset %lu", static_cast<unsigned long>(p.id));
        count_++;
        rebuildPool();                       // give the new preset its slice (defaults R,G,B)
        uint8_t* r = roleAtMut(p);
        r[0] = static_cast<uint8_t>(ChannelRole::Red);
        r[1] = static_cast<uint8_t>(ChannelRole::Green);
        r[2] = static_cast<uint8_t>(ChannelRole::Blue);
        outId = p.id;
        refreshStatus();
        return true;
    }

    bool deleteListRow(uint32_t id) override {
        int i = indexOf(id);
        if (i < 0 || presets_[i].locked) return false;   // a seeded built-in is protected
        for (uint8_t j = static_cast<uint8_t>(i); j + 1 < count_; j++) presets_[j] = presets_[j + 1];
        count_--;
        rebuildPool();
        refreshStatus();
        return true;
    }

    bool moveListRow(uint32_t id, uint8_t to) override {
        int i = indexOf(id);
        if (i < 0) return false;
        // The seeded built-ins are a FIXED block at the top: a locked row can't be moved, and a
        // custom row can't be moved up INTO the locked block (customs reorder freely among
        // themselves, below the built-ins). This keeps "RGB is always first" stable, matching the
        // built-in-vs-user grouping of a normal preset library.
        if (presets_[i].locked) return false;
        const uint8_t firstCustom = lockedCount();
        if (to < firstCustom) to = firstCustom;      // clamp a custom above the built-ins back down
        if (to >= count_) to = static_cast<uint8_t>(count_ - 1);
        Preset moved = presets_[i];
        if (to > i) for (int j = i; j < to; j++) presets_[j] = presets_[j + 1];
        else        for (int j = i; j > to; j--) presets_[j] = presets_[j - 1];
        presets_[to] = moved;
        rebuildPool();                       // pool order follows preset order
        return true;
    }

    bool setListRowField(uint32_t id, const char* field, const char* valueJson) override {
        int i = indexOf(id);
        if (i < 0 || presets_[i].locked) return false;   // built-ins are read-only
        Preset& p = presets_[i];
        if (std::strcmp(field, "name") == 0) {
            mm::json::parseString(valueJson, "value", p.name, sizeof(p.name));
            return true;
        }
        if (std::strcmp(field, "channels") == 0) {
            int v = mm::json::parseInt(valueJson, "value");
            if (v < 1 || v > 255) return false;
            setChannelCount(p, static_cast<uint8_t>(v));
            return true;
        }
        if (field[0] == 'c' && field[1] == 'h' && field[2] >= '0' && field[2] <= '9') {
            const int c = std::atoi(field + 2);
            if (c < 0 || c >= p.channelCount) return false;
            int v = mm::json::parseInt(valueJson, "value");
            if (v < 0 || v >= kChannelRoleCount) return false;
            roleAtMut(p)[c] = static_cast<uint8_t>(v);
            return true;
        }
        return false;
    }

    /// Persist: the presets List round-trips as a JSON array (each row carries its roles array).
    /// Restore repopulates presets_ + the role pool; custom presets survive a reboot, and setup()
    /// re-seeds the built-ins only when the restore left the list empty.
    bool restoreList(const char* json, const char* key) override {
        mm::json::JsonDoc doc;
        if (!mm::json::parse(json, doc)) return false;
        const mm::json::JsonNode* arr = mm::json::member(doc, doc.rootNode(), key);
        if (!arr || arr->type != mm::json::JsonType::Array) return false;
        count_ = 0;
        const int n = mm::json::arraySize(doc, arr);
        for (int r = 0; r < n && count_ < kMaxPresets; r++) {
            const mm::json::JsonNode* row = mm::json::element(doc, arr, r);
            Preset& p = presets_[count_];
            p = Preset{};
            p.id = static_cast<uint32_t>(mm::json::readInt(mm::json::member(doc, row, "id"), 0));
            mm::json::readString(mm::json::member(doc, row, "name"), p.name, sizeof(p.name));
            int ch = mm::json::readInt(mm::json::member(doc, row, "channels"), 3);
            p.channelCount = static_cast<uint8_t>(ch < 1 ? 1 : ch > 255 ? 255 : ch);
            p.locked = mm::json::readBool(mm::json::member(doc, row, "locked"), false);
            if (p.id >= nextId_) nextId_ = p.id + 1;   // never reissue a persisted id
            count_++;
        }
        rebuildPool();
        // Second pass: fill each preset's roles now that the pool is sized.
        for (int r = 0, idx = 0; r < n && idx < count_; r++, idx++) {
            const mm::json::JsonNode* row = mm::json::element(doc, arr, r);
            const mm::json::JsonNode* roles = mm::json::member(doc, row, "roles");
            uint8_t* dst = roleAtMut(presets_[idx]);
            const int rn = (roles && roles->type == mm::json::JsonType::Array)
                               ? mm::json::arraySize(doc, roles) : 0;
            for (uint8_t c = 0; c < presets_[idx].channelCount; c++)
                dst[c] = (c < rn) ? static_cast<uint8_t>(mm::json::readInt(mm::json::element(doc, roles, c), 0)) : 0;
        }
        return true;
    }

private:
    // A preset row: identity + a slice into the shared role pool. The roles themselves live in
    // rolePool_ (packed in preset order), so a Preset is a fixed small struct with no per-row array.
    struct Preset {
        uint32_t id = 0;
        char     name[16] = {};
        uint8_t  channelCount = 3;
        uint32_t poolOffset = 0;   // start of this preset's roles in rolePool_ (set by rebuildPool)
        uint32_t poolLen = 0;      // bytes currently allocated to it in rolePool_ (the OLD slice size
                                   // rebuildPool copies from; distinct from channelCount, which may
                                   // already hold a new value mid-change). 0 for a fresh preset.
        bool     locked = false;
    };

    Preset   presets_[kMaxPresets] = {};
    uint8_t  count_ = 0;
    uint32_t nextId_ = 1;
    ScratchBuffer<uint8_t> rolePool_{*this};   // Σ channelCount role bytes, packed in preset order
    char     statusBuf_[24] = {};
    ActiveInstance<LightPresetsModule> seat_{*this};

    const Preset* find(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (presets_[i].id == id) return &presets_[i];
        return nullptr;
    }
    int indexOf(uint32_t id) const {
        for (uint8_t i = 0; i < count_; i++) if (presets_[i].id == id) return i;
        return -1;
    }
    // The seeded built-ins occupy the top of the list (they're added first and never move), so the
    // number of locked rows is also the index of the first custom row.
    uint8_t lockedCount() const {
        uint8_t n = 0;
        while (n < count_ && presets_[n].locked) n++;
        return n;
    }
    const uint8_t* roleAt(const Preset& p) const { return rolePool_.data() + p.poolOffset; }
    uint8_t*       roleAtMut(const Preset& p)     { return rolePool_.data() + p.poolOffset; }

    // Re-pack the role pool so it holds Σ channelCount bytes with each preset's slice at its new
    // poolOffset, PRESERVING existing role bytes across the re-pack. resize() reallocates + zero-
    // fills, so the old bytes are copied into a transient buffer first, then back at the new offsets.
    // Cold path only (add / delete / move / channel-count change / restore); a transient alloc here
    // is fine. No channel cap — the pool grows to whatever the presets need.
    void rebuildPool() {
        const uint32_t oldPoolLen = static_cast<uint32_t>(rolePool_.count());
        uint32_t newOff[kMaxPresets] = {};
        uint32_t total = 0;
        for (uint8_t i = 0; i < count_; i++) { newOff[i] = total; total += presets_[i].channelCount; }

        // Copy each preset's currently-allocated bytes (its `poolLen` at `poolOffset`, both from the
        // PREVIOUS layout) into a temp laid out at the NEW offsets, then resize + copy back. The copy
        // length is min(poolLen, channelCount): a grow keeps the old bytes + leaves the tail zeroed
        // for the caller to fill, a shrink keeps the surviving head. poolLen (the real old slice size,
        // tracked explicitly) is what makes this robust — never inferred from neighbours or from
        // channelCount, which may already hold the new value. A fresh preset has poolLen 0 (nothing
        // to keep). After the repack, poolOffset/poolLen are updated to the new layout.
        uint8_t* keep = total ? static_cast<uint8_t*>(platform::alloc(total)) : nullptr;
        if (keep) {
            std::memset(keep, 0, total);
            if (rolePool_.data()) {
                for (uint8_t i = 0; i < count_; i++) {
                    const uint32_t oldStart = presets_[i].poolOffset;
                    const uint32_t oldLen   = presets_[i].poolLen;
                    const uint32_t newLen   = presets_[i].channelCount;
                    const uint32_t copy     = oldLen < newLen ? oldLen : newLen;   // surviving overlap
                    if (copy && oldStart + copy <= oldPoolLen)
                        std::memcpy(keep + newOff[i], rolePool_.data() + oldStart, copy);
                }
            }
        }
        rolePool_.resize(total);
        for (uint8_t i = 0; i < count_; i++) {
            presets_[i].poolOffset = newOff[i];
            presets_[i].poolLen    = presets_[i].channelCount;   // slice now matches the channel count
        }
        if (keep) {
            if (rolePool_.data()) std::memcpy(rolePool_.data(), keep, total);
            platform::free(keep);
        }
    }

    // Change a preset's channel count, preserving existing role picks; new channels default to
    // R,G,B,W then None. Re-packs the pool (offsets shift), then fills the new tail.
    void setChannelCount(Preset& p, uint8_t n) {
        const uint8_t was = p.channelCount;
        p.channelCount = n;
        rebuildPool();                       // pool now holds n bytes for this preset (old kept, tail zeroed)
        uint8_t* r = roleAtMut(p);
        for (uint8_t c = was; c < n; c++)
            r[c] = c < 4 ? static_cast<uint8_t>(c + 1) : 0;   // 1=R,2=G,3=B,4=W, then None
    }

    // The curated built-in wirings, each a dense role array of exactly its fixture's width. Data,
    // not code: a preset of any width seeds directly (the old LightPreset-enum path capped at 4
    // channels via a roles[4] — a wide moving head couldn't be expressed). The colour orders are the
    // real ones (WS2812 GRB, ws2814 WRGB, sk6812 GRBW, …); RBG/GBR/BRG are deliberately omitted — no
    // real fixture ships them, so they'd be permutation noise in the built-in list (a user adds a
    // custom preset if ever needed). The moving-head maps are migrated from MoonLight's DriverNode
    // offset tables; a channel whose role this project doesn't drive yet (RGBW sub-cells, and any
    // fixture function past Pan/Tilt/Zoom/Gobo/Dimmer) stays None — a correct DMX map whose extra
    // channels are inert until the moving-head effect writers land.
    void seedBuiltins() {
        using R = ChannelRole;
        static constexpr R kRGB[]    = {R::Red, R::Green, R::Blue};
        static constexpr R kGRB[]    = {R::Green, R::Red, R::Blue};
        static constexpr R kBGR[]    = {R::Blue, R::Green, R::Red};
        static constexpr R kRGBW[]   = {R::Red, R::Green, R::Blue, R::White};
        static constexpr R kGRBW[]   = {R::Green, R::Red, R::Blue, R::White};
        static constexpr R kWRGB[]   = {R::White, R::Red, R::Green, R::Blue};                     // ws2814
        static constexpr R kGRB6[]   = {R::Green, R::Red, R::Blue, R::None, R::None, R::None};     // curtain
        static constexpr R kRGBWYP[] = {R::Red, R::Green, R::Blue, R::White, R::Yellow, R::UV};    // lightbar
        static constexpr R kRGBCCT[] = {R::Red, R::Green, R::Blue, R::White, R::WarmWhite};        // cold+warm
        static constexpr R kIRGB[]   = {R::Dimmer, R::Red, R::Green, R::Blue};                     // CH1 master intensity
        // Moving heads (MoonLight offset maps → dense arrays). N = None.
        static constexpr R N = R::None;
        static constexpr R kMHBeeEyes15[] = {   // 15ch: Pan,Tilt,-,Dim,-,Gobo,-,Zoom,Dim,-,R,G,B,-,-
            R::Pan, R::Tilt, N, R::Dimmer, N, R::Gobo, N, R::Zoom, R::Dimmer, N, R::Red, R::Green, R::Blue, N, N};
        static constexpr R kMHBeTopper32[] = {  // 32ch: Pan,-,Tilt,-,-,Zoom,Dim,-,-,R,G,B,… (RGBW cells → None)
            R::Pan, N, R::Tilt, N, N, R::Zoom, R::Dimmer, N, N, R::Red, R::Green, R::Blue,
            N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N, N};
        static constexpr R kMH19x15W24[] = {    // 24ch: Pan,Tilt,-,Dim,R,G,B,W,…,Zoom@17 (RGBW cells → None)
            R::Pan, R::Tilt, N, R::Dimmer, R::Red, R::Green, R::Blue, R::White,
            N, N, N, N, N, N, N, N, N, R::Zoom, N, N, N, N, N, N};

        struct Builtin { const char* name; const R* roles; uint8_t channelCount; };
        static constexpr Builtin kBuiltins[] = {
            {"RGB", kRGB, 3}, {"GRB", kGRB, 3}, {"BGR", kBGR, 3},
            {"RGBW", kRGBW, 4}, {"GRBW", kGRBW, 4}, {"WRGB", kWRGB, 4},
            {"Curtain GRB6", kGRB6, 6}, {"Lightbar RGBWYP", kRGBWYP, 6},
            {"RGBCCT", kRGBCCT, 5}, {"IRGB", kIRGB, 4},
            {"MH BeeEyes 15", kMHBeeEyes15, 15},
            {"MH BeTopper 32", kMHBeTopper32, 32},
            {"MH 19x15W-24", kMH19x15W24, 24},
        };
        // A built-in name that overflows Preset::name would truncate silently (and break a lookup by
        // name), so fail LOUD at BUILD time if one is too long — the fix is a shorter name, not a
        // wider buffer (the buffer is ×kMaxPresets standing DRAM on classic ESP32). constexpr scan
        // over the table, checked by static_assert; a bad name never reaches a boot.
        constexpr auto namesFit = [](const Builtin* t, size_t n) {
            for (size_t i = 0; i < n; i++) {
                size_t len = 0; while (t[i].name[len]) len++;
                if (len >= sizeof(Preset::name)) return false;
            }
            return true;
        };
        static_assert(namesFit(kBuiltins, sizeof(kBuiltins) / sizeof(kBuiltins[0])),
                      "a built-in preset name exceeds Preset::name — shorten it");
        for (const Builtin& b : kBuiltins) {
            if (count_ >= kMaxPresets) break;
            Preset& p = presets_[count_];
            p = Preset{};
            p.id = nextId_++;
            p.locked = true;
            std::snprintf(p.name, sizeof(p.name), "%s", b.name);
            p.channelCount = b.channelCount;
            count_++;
            rebuildPool();
            uint8_t* dst = roleAtMut(p);
            for (uint8_t c = 0; c < b.channelCount; c++) dst[c] = static_cast<uint8_t>(b.roles[c]);
        }
    }

    void refreshStatus() {
        std::snprintf(statusBuf_, sizeof(statusBuf_), "%u preset%s", count_, count_ == 1 ? "" : "s");
        setStatus(statusBuf_);
    }
};

} // namespace mm
