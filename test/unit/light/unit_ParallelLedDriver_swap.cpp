// @module ParallelLedDriver
// @also MultiPinLedDriver, ParlioLedDriver

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "unit/core/conditional_controls.h"   // controlIndex

#include <cstring>
#include <vector>

// Host test of the runtime PERIPHERAL SWAP path — the `peripheral` Select changing on a live driver.
// The borrowed-mock tests (setPeripheralForTest) deliberately do NOT exercise this: a borrowed backend
// is !peripheralOwned_, so ensurePeripheralMatchesSelection() short-circuits and the registry swap
// never runs. To pin the swap we REGISTER two owned mock backends and drive the exact sequence
// Scheduler::setControl runs on a control change:
//
//     applyControlValue writes peripheralSel_  →  rebuildControls()  →  onControlChanged("peripheral")
//
// The bug this pins (found in the parallel-driver consolidation review): rebuildControls() already
// swaps the backend (ensurePeripheralMatchesSelection) and binds its members into the control list,
// so onControlChanged must NOT swap AGAIN — a second swap frees the just-bound backend and leaves
// every backend-owned control dangling into freed memory. Desktop's own registry is empty, so only a
// test that populates it can catch this.

namespace {

// The quiesce-render hook's fire count, and whether it had fired BEFORE the last attached backend was
// freed. The swap-ordering test installs a hook that bumps g_hookFires; SwapMock's destructor snapshots
// (into g_hookFiredBeforeLastDtor) whether the hook had already fired by the time an ATTACHED backend is
// destroyed — the proof that the worker was quiesced before the backend it reads was freed, not merely
// that the hook fired at some point during the swap.
int g_hookFires = 0;
bool g_hookFiredBeforeLastDtor = false;

// A minimal owned backend (created by the registry, deleted by the driver). Reports a real hwBlock so
// the two labels model two distinct peripherals, and counts its own construction/destruction so the
// test can assert no leak and no double-free across a swap.
struct SwapMock : mm::LedPeripheral {
    static inline int live = 0;      // net live instances (ctor++ / dtor--) — 0 at rest, no leak/double-free
    static inline int attachedDtors = 0;   // destructions of a backend that was ATTACHED to a driver (i.e.
                                    // a real selected backend, not a buildPeripheralOptions() probe — probes
                                    // are make()/delete'd without attach()). A control change swaps to ONE
                                    // backend, so across the whole swap sequence the only attached backend
                                    // destroyed is the one being replaced. The double-swap bug destroys an
                                    // EXTRA attached backend per change (it attaches B1 in the rebuild, then
                                    // frees it in onControlChanged before attaching B2) — this counter is the
                                    // discriminator that plain live/ctor counts can't be (probe churn).
    mm::LedHwBlock block;
    explicit SwapMock(mm::LedHwBlock b) : block(b) { live++; }
    // owner_ (protected, set by attach()) is non-null only for a backend the driver actually selected;
    // a buildPeripheralOptions() probe is make()/delete'd without attach(), so it destructs with a null
    // owner_. Counting attached destructions isolates real backend churn from probe churn. For an ATTACHED
    // backend being freed, also snapshot whether the quiesce hook had already fired — the ordering proof
    // the swap-ordering test reads (the worker must be stopped BEFORE the backend it dereferences is freed).
    ~SwapMock() override {
        live--;
        if (owner_) { attachedDtors++; g_hookFiredBeforeLastDtor = (g_hookFires > 0); }
    }

    uint8_t lanesAvailable() const override { return 8; }
    bool powerOfTwoBus() const override { return true; }
    bool loopbackFullWidth() const override { return false; }
    bool supportsPinExpander() const override { return false; }
    const char* initFailMsg() const override { return "mock init failed"; }
    mm::LedHwBlock hwBlock() const override { return block; }

    void addBusControls(mm::ControlList&) override {}
    bool busControlTriggersBuild(const char*) const override { return false; }
    void recordBusPins() override {}
    bool extraBusPinsCurrent() const override { return true; }
    const char* validateBusPins(const uint16_t*, uint8_t) const override { return nullptr; }
    const char* validateBusFatal() const override { return nullptr; }
    uint16_t clockPinForBus() const override { return 99; }

    bool busInit(size_t frameBytes, bool) override { cap_ = frameBytes; buf_.assign(frameBytes, 0); return true; }
    uint8_t* busBuffer(uint8_t i) override { return (i == 0 && !buf_.empty()) ? buf_.data() : nullptr; }
    size_t busCapacity() const override { return cap_; }
    bool busTransmit(uint8_t, size_t) override { transmits_++; return true; }
    bool busWait(uint8_t, uint32_t) override { return true; }
    uint32_t busLastTransmitUs() const override { return 0; }
    void busDeinit() override { cap_ = 0; buf_.clear(); }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) override { return {}; }

    size_t transmits_ = 0;
private:
    std::vector<uint8_t> buf_;
    size_t cap_ = 0;
};

// The peripheral registry is a static set built at static-init in production. On desktop the three
// real backends (i80/MoonI80/Parlio) DO register (their headers compile into this binary) but report
// 0 lanes, so they fill registry slots while being filtered out of the option list — leaving no room
// for two test mocks (kMaxPeripherals == 4). So a swap test SAVES the registry, installs exactly its
// two mocks, runs, and RESTORES it — scoped, and it can't perturb any other test's view of the set.
struct RegistryScope {
    mm::ParallelLedDriver::PeripheralEntry saved[mm::ParallelLedDriver::kMaxPeripherals];
    uint8_t savedCount;
    RegistryScope() {
        savedCount = mm::ParallelLedDriver::peripheralRegistryCount_;
        for (uint8_t i = 0; i < savedCount; i++) saved[i] = mm::ParallelLedDriver::peripheralRegistry_[i];
        mm::ParallelLedDriver::peripheralRegistryCount_ = 0;
        // "swapA" (Parlio block) and "swapB" (LcdCam block) model two distinct peripherals.
        mm::ParallelLedDriver::registerPeripheral("swapA", []() -> mm::LedPeripheral* {
            return new SwapMock(mm::LedHwBlock::Parlio);
        });
        mm::ParallelLedDriver::registerPeripheral("swapB", []() -> mm::LedPeripheral* {
            return new SwapMock(mm::LedHwBlock::LcdCam);
        });
    }
    ~RegistryScope() {
        mm::ParallelLedDriver::peripheralRegistryCount_ = savedCount;
        for (uint8_t i = 0; i < savedCount; i++) mm::ParallelLedDriver::peripheralRegistry_[i] = saved[i];
    }
};

// Drive the Scheduler control-change sequence for the `peripheral` Select: write the new index through
// the bound control pointer (as applyControlValue does), rebuild controls (swap #1, binds new backend),
// then fire onControlChanged (must be a no-op swap, not a second free-and-rebuild).
void changePeripheralTo(mm::ParallelLedDriver& d, uint8_t index) {
    int i = mm::test::controlIndex(d, "peripheral");
    REQUIRE(i >= 0);
    *static_cast<uint8_t*>(d.controls()[static_cast<uint8_t>(i)].ptr) = index;
    d.rebuildControls();
    d.onControlChanged("peripheral");
}

} // namespace

TEST_CASE("ParallelLedDriver: a peripheral swap does not double-free or dangle the control list") {
    RegistryScope registry;   // install swapA/swapB, restore the real set on scope exit
    const int liveBefore = SwapMock::live;

    {
        mm::ParallelLedDriver d;
        mm::Buffer src;
        mm::Correction corr;
        // Fresh driver seeds the first registered backend (swapA). Wire a working config.
        std::strcpy(d.pins, "1,2");
        REQUIRE(src.allocate(64, 3) == true);
        mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
        d.defineControls();
        d.setSourceBuffer(&src);
        d.correctionForTest() = corr;
        d.applyState();

        // Exactly ONE backend is live for a driver holding a single peripheral.
        CHECK(SwapMock::live == liveBefore + 1);

        // Resolve labels → board-filtered indices via the driver's own option list (order is registry
        // order, which the test does not hardcode). A Select stores its options in aux and its option
        // COUNT in max (hi = max-1), the shape defineDriverControls uses for the peripheral Select.
        int aIdx = -1, bIdx = -1;
        const auto& cs = d.controls();
        int selIdx = mm::test::controlIndex(d, "peripheral");
        REQUIRE(selIdx >= 0);
        const char* const* opts = reinterpret_cast<const char* const*>(cs[static_cast<uint8_t>(selIdx)].aux);
        const int optCount = cs[static_cast<uint8_t>(selIdx)].max;   // addSelect stores optionCount in max
        for (int k = 0; k < optCount; k++) {
            if (opts[k] && std::strcmp(opts[k], "swapA") == 0) aIdx = k;
            if (opts[k] && std::strcmp(opts[k], "swapB") == 0) bIdx = k;
        }
        REQUIRE(aIdx >= 0);
        REQUIRE(bIdx >= 0);

        // Swap A → B. The bug: rebuildControls() swaps in B1 (binding its members into the control list),
        // then onControlChanged swaps AGAIN — freeing B1 and building B2 — so the control list dangles into
        // freed B1. The tell is the CONSTRUCTION count: the buggy path builds TWO backends for one change
        // (B1 orphaned + B2), the fixed path builds exactly ONE (onControlChanged is a no-op swap). live
        // count alone can't see it (both paths net one live backend); `built` delta is the discriminator.
        int dtorsBefore = SwapMock::attachedDtors;
        changePeripheralTo(d, static_cast<uint8_t>(bIdx));
        // A→B destroys exactly ONE attached backend (the outgoing A). The double-swap bug destroys TWO
        // (A replaced by B1 in the rebuild, then B1 replaced by B2 in onControlChanged) — so a delta > 1
        // is the bug's fingerprint. This is what live/ctor counts miss: probe churn hides the ctor delta,
        // and net-live is 1 either way; only counting ATTACHED destructions isolates the extra free.
        CHECK(SwapMock::attachedDtors - dtorsBefore == 1);
        CHECK(SwapMock::live == liveBefore + 1);
        // busInit succeeds (a memory buffer, not an inert desktop stub), so the bus comes up and the
        // inited_-gated hwBlock() reports B's block (LcdCam) — proving the LIVE backend is B, not a
        // dangling A: if the double-swap freed the rebuild-bound backend, hwBlock() would read freed memory.
        CHECK(d.hwBlock() == mm::LedHwBlock::LcdCam);

        // The driver must still run: a tick after the swap encodes+transmits through the LIVE backend,
        // not a freed one. If the control list dangled, this is a use-after-free (ASan/TSan would catch;
        // the plain assert catches the "transmitted through a stale object" case).
        d.tick20ms();
        d.tick();   // one render tick — reaches the live backend's busTransmit

        // Swap back B → A, then A → B again: repeated swaps must stay balanced (no accumulation), and
        // hwBlock() tracks the live backend each time (A = Parlio, B = LcdCam).
        changePeripheralTo(d, static_cast<uint8_t>(aIdx));
        CHECK(SwapMock::live == liveBefore + 1);
        CHECK(d.hwBlock() == mm::LedHwBlock::Parlio);
        changePeripheralTo(d, static_cast<uint8_t>(bIdx));
        CHECK(SwapMock::live == liveBefore + 1);
        CHECK(d.hwBlock() == mm::LedHwBlock::LcdCam);
    }

    // Driver destroyed → its owned backend freed → back to the starting live count (no leak).
    CHECK(SwapMock::live == liveBefore);
}

// Regression (pre-merge Reviewer BLOCKER): a LIVE peripheral swap frees the old backend, and the core-1
// encode worker dereferences that backend (busBuffer/busTransmit) — so swapPeripheral MUST stop the
// worker (fire the quiesce-render hook) BEFORE deleting the backend, or it is a use-after-free, the same
// class the structural-mutation quiesce fixes. This pins the ORDERING, not merely that the hook fired:
// SwapMock's destructor snapshots (into g_hookFiredBeforeLastDtor) whether the quiesce hook had already
// fired at the moment the outgoing attached backend is freed. A swap that deleted the backend BEFORE
// quiescing would leave that flag false even though g_hookFires ends up > 0.
TEST_CASE("ParallelLedDriver: a live peripheral swap quiesces the render worker before freeing the backend") {
    RegistryScope registry;
    g_hookFires = 0;
    g_hookFiredBeforeLastDtor = false;

    // The hook records each fire; SwapMock's destructor records whether it had fired before an attached
    // backend was freed — the ordering proof.
    mm::MoonModule::setQuiesceRenderHook([] { g_hookFires++; });

    {
        mm::ParallelLedDriver d;
        mm::Buffer src;
        mm::Correction corr;
        std::strcpy(d.pins, "1,2");
        REQUIRE(src.allocate(64, 3) == true);
        mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
        d.defineControls();
        d.setSourceBuffer(&src);
        d.correctionForTest() = corr;
        d.applyState();

        const auto& cs = d.controls();
        int selIdx = mm::test::controlIndex(d, "peripheral");
        REQUIRE(selIdx >= 0);
        const char* const* opts = reinterpret_cast<const char* const*>(cs[static_cast<uint8_t>(selIdx)].aux);
        const int optCount = cs[static_cast<uint8_t>(selIdx)].max;
        int bIdx = -1;
        for (int k = 0; k < optCount; k++) if (opts[k] && std::strcmp(opts[k], "swapB") == 0) bIdx = k;
        REQUIRE(bIdx >= 0);

        // Reset the fire count to ZERO immediately before the swap so the dtor's "had the hook fired?"
        // snapshot reflects ONLY this swap — not an earlier fire from applyState/rebuild. Otherwise any
        // prior fire would make g_hookFiredBeforeLastDtor true even if THIS swap freed A before quiescing.
        g_hookFires = 0;
        g_hookFiredBeforeLastDtor = false;
        changePeripheralTo(d, static_cast<uint8_t>(bIdx));   // the live swap: frees the old backend (A)
        // The swap fired the quiesce hook at all...
        CHECK(g_hookFires > 0);
        // ...AND it fired BEFORE the outgoing backend was freed. This is the ordering the finding pins:
        // the SwapMock dtor recorded whether the hook had fired (this swap) when the attached backend (A)
        // was destroyed. A swap that deleted the backend first would leave this false despite the hook
        // firing later in the same swap.
        CHECK(g_hookFiredBeforeLastDtor);
    }

    mm::MoonModule::setQuiesceRenderHook(nullptr);
}
