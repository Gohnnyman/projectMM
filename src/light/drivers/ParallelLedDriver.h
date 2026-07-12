#pragma once

#include "light/drivers/DriverBase.h"        // DriverBase, Correction
#include "light/drivers/LcdSlots.h"        // encodeWs2812LcdSlots (shared encoder)
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared)
#include "platform/platform.h"


namespace mm {

template <class Derived>
/// Base for the parallel WS2812B LED-output drivers — the S3's LCD_CAM i80 bus (LcdLedDriver) and
/// the P4's Parlio peripheral (ParlioLedDriver). Both drive up to 8 strands that clock out
/// SIMULTANEOUSLY, one GPIO lane each, fed consecutive slices of the source buffer.
///
/// **Single-shot autonomous DMA:** both pre-encode the whole frame (a per-ROW fused
/// correct+transpose, the SAME LcdSlots.h encoder — a Parlio bus byte and an i80 bus byte are
/// identical: one word per slot, bit L = data line L) plus a zeroed ≥300 µs latch pad into a
/// platform-owned DMA buffer, then ship it as one autonomous transfer. So there's NO CPU deadline
/// during transmission — the WiFi-induced bit-slip of refill-based drivers cannot occur by
/// construction. (The i80 bus owns the DMA buffer and its max transfer size is fixed at creation, so
/// re-creating the bus IS the buffer resize.) The two drivers were ~250 of ~370 lines byte-for-byte
/// identical; this is the one copy (the No-duplication rule).
///
/// **Buffer slicing across pins:** consecutive slices in `pins` order, sizes from `ledsPerPin`,
/// even-split remainder — identical semantics to RmtLedDriver, parsers shared (PinList.h).
///
/// **CRTP, not a virtual hierarchy:** the base calls back into the derived through
/// `static_cast<Derived*>(this)->busX()` — no vtable, no runtime indirection — so it stays inside
/// the hot-path / data-over-objects rules and keeps the module tree as the one deliberate class
/// hierarchy (the only virtual boundary remains MoonModule → DriverBase). The derived supplies just
/// the peripheral-specific pieces: the bus* platform wrappers, `lanesAvailable()` (the inert-on-
/// wrong-chip `if constexpr` guard), `kExactLaneCount` (i80 needs exactly 8; Parlio runs 1..8), the
/// slot rate `kClockHz`, and any extra pins the i80 driver tracks (WR/DC) that Parlio doesn't.
/// configErr_/failBuf_ come from DriverBase (shared with RmtLedDriver too).
class ParallelLedDriver : public DriverBase {
public:
    /// WS2812/SK6812 strips are GRB-wired, so a fresh parallel LED driver (and its LcdLedDriver
    /// subclass) references the "GRB" preset by default. The user can pick any preset.
    ParallelLedDriver() { this->setDefaultPresetName("GRB"); }

    /// Max parallel lanes = the peripheral's full 16 data lines. The bus width the driver
    /// actually builds is DERIVED from the configured pin count: ≤8 pins → an 8-bit bus (uint8
    /// slots), 9..16 pins → a 16-bit bus (uint16 slots). Both peripherals do 16 data lines
    /// (LCD_CAM `bus_width` 8|16; Parlio `data_width` 8|16 — powers of two only).
    static constexpr uint8_t kMaxLanes = 16;

    /// Light count the loopback self-test drives (or `maxLaneLights_` if the strip is smaller).
    /// Small on purpose: the test verifies the peripheral emits *correct WS2812 bits*, which a few
    /// hundred lights prove fully (encode, fused correct+transpose, single-shot DMA, latch pad) — it
    /// does NOT need the operational grid. A big frame hits two hardware limits: the P4 Parlio rejects
    /// a single non-loop transfer over `PARLIO_LL_TX_MAX_BITS_PER_FRAME` (~0.5 Mbit), and the RMT-RX
    /// capture can't hold a large symbol count (a 128×128 grid = 16384 lights is ~1.2 Mbit TX / ~400k
    /// capture symbols → transfer rejected AND capture returns nothing, surfacing as a misleading "bad
    /// bit 0"). 256 lights = ~18 KB TX / ~6144 capture symbols — comfortably inside both on every
    /// parallel driver, so the test runs identically at any grid size.
    static constexpr nrOfLightsType kLoopbackTestLights = 256;

    /// Comma-separated GPIO list, one parallel lane per pin — up to kMaxLanes strands clocked out
    /// SIMULTANEOUSLY, fed consecutive slices of this driver's window. Shared control shape with
    /// RmtLedDriver (parsers in PinList.h). Defaults live on the derived (chip-specific safe pins),
    /// so the derived sets them after construction; the base just declares them. i80 needs exactly
    /// 8 OR 16 real pins (a partial bus is rejected — a sub-16 board parks unused lanes + WR/DC on
    /// spare GPIOs); Parlio runs on 1..16. Sized for 16 two-digit GPIOs + separators.
    char pins[64] = "";
    /// Comma-separated lights-per-lane, matched to `pins` by position; the unassigned remainder
    /// splits evenly over the remaining lanes. Each lane is clamped to the WS2812 per-pin ceiling
    /// (`kMaxWs2812LedsPerPin`) with a Warning status — it drives that many rather than choking a
    /// whole grid onto one line. Empty default splits this driver's window evenly. Sized for 16
    /// per-lane counts + separators.
    char ledsPerPin[96] = "";

    /// On-device loopback self-test — jumper a lane's TX to `loopbackRxPin`, tick to transmit a
    /// known WS2812 pattern and bit-verify the capture, proving the peripheral emits correct bytes
    /// on real silicon. A persistent on/off mode (see onControlChanged): while on it re-runs on every
    /// relevant change; off clears the verdict. The verdict lands in the status slot.
    bool     loopbackTest = false;
    /// Optional TX override for the self-test: when set (>= 0), the loopback transmits on THIS pin
    /// in place of lane 0 (laneList_[0]); other lanes are unchanged. Lets the bench loopback run on
    /// a dedicated jumper pin without re-typing the operational `pins`. Falls back to laneList_[0]
    /// when unset (-1). The loopback only drives lane 0 with the test pattern, so a single override
    /// pin is all it needs. Test-only — normal output always uses the real `pins`. int8_t + addPin
    /// (not uint16): the standard single-GPIO control, and -1 = unset keeps GPIO 0 usable as a
    /// loopback pin.
    int8_t   loopbackTxPin = -1;
    /// Jumper this to the TX lane for the self-test (unset = -1 by default).
    int8_t   loopbackRxPin = -1;

    /// Bind the driver's controls: the window (start/count), the `pins` and
    /// `ledsPerPin` text lists, any derived-supplied bus controls (i80 adds
    /// clockPin/dcPin, Parlio none), and the loopback self-test controls (TX/RX pin
    /// overrides always bound but shown only in test mode).
    void defineDriverControls() override {
        addWindowControls();   // start / count — the slice of the shared buffer this driver outputs
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        derived()->addBusControls();   // i80 adds clockPin/dcPin here; Parlio none
        controls_.addBool("loopbackTest", loopbackTest);
        // Always bound, shown only in test mode — the conditional-control shape.
        controls_.addPin("loopbackTxPin", loopbackTxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        controls_.addPin("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
    }

    /// A change to the pins, per-lane counts, the window, or a derived bus control
    /// (clockPin/dcPin on i80) re-parses and re-inits the bus live via the
    /// prepare sweep.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || isWindowControl(name)
            || derived()->busControlTriggersBuild(name);   // clockPin/dcPin on i80
    }

    /// React to a control change off the render loop. loopbackTest is a persistent
    /// on/off mode: while ON, the self-test re-runs on every relevant change (with a
    /// lane-config refresh first, since onControlChanged precedes the prepare sweep);
    /// turning it OFF clears the verdict and re-derives the real driver status.
    void onControlChanged(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "loopbackTxPin") == 0
                                || std::strcmp(name, "loopbackRxPin") == 0;
        if (isTestControl && !loopbackTest) {
            // Toggling the test off clears the loopback's own verdict, then
            // re-derives the real driver status — a config/init error must
            // survive, which a blind clearStatus() would hide.
            clearFailBuf();
            clearStatus();
            parseConfig();
            reinit();
        } else if (loopbackTest && (isTestControl || isPinControl)) {
            // A pin edit changes laneList_/laneCount_/frameBytes_, but onControlChanged runs
            // BEFORE the prepare() sweep (and loopbackRxPin doesn't trigger that
            // sweep at all), so refresh the lane config here before testing it —
            // otherwise the self-test would build its private bus from stale pins.
            if (isPinControl) { parseConfig(); reinit(); }
            runLoopbackSelfTest();
        }
        // Chain to the base so a correction-control edit (localBrightness / preset / whiteMode)
        // rebuilds this driver's correction LUT — without this the LED driver's brightness/preset
        // controls were dead (only the global-brightness push reached the LUT).
        DriverBase::onControlChanged(name);
    }

    /// One-time wiring only (parse the lane lists into members); the bus acquire lives in
    /// prepare(), the sole resource gate. Enabled-independent — the acquire happens in the
    /// prepareTree sweep that always follows.
    void setup() override { parseConfig(); }
    /// Deinit the bus, then clear the shared fail/config-error state
    /// (DriverBase::release()).
    void release() override {
        deinit();
        DriverBase::release();   // clears failBuf_ + configErr_
    }

    /// Pure build (see MoonModule::prepare): re-parse the lanes and (re)init the bus off the
    /// hot path. No enabled() check — core's applyState() calls this only when effectively-enabled
    /// and routes to release() (bus + DMA buffer freed) otherwise, so a shared GPIO is released
    /// when the driver, or a parent, is disabled.
    void prepare() override {
        parseConfig();
        reinit();
    }

    /// RGB<->RGBW changes the bytes-per-light and therefore the frame size, so
    /// re-parse and re-init the bus. Skipped while (effectively) disabled (would re-grab the bus).
    void onCorrectionChanged() override { if (!effectivelyEnabled()) return; parseConfig(); reinit(); }

    /// Point the driver at the source frame buffer and re-parse the lane config.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; parseConfig(); }

    /// Per-tick output: a fused per-ROW pass corrects the same light index of every
    /// active lane and transposes it into 3-slot bus bytes in the platform-owned DMA
    /// buffer, then ships the frame as one autonomous transfer. Inert off this chip
    /// and idle until inited with a source buffer + correction.
    void tick() override {
        if constexpr (Derived::lanesAvailable() == 0) return;  // inert off this chip
        if (!inited_ || !dmaBuf_ || !sourceBuffer_ || !sourceBuffer_->data()
            || laneCount_ == 0 || maxLaneLights_ == 0) return;
        const uint8_t outCh = correction_.outChannels;
        if (outCh == 0 || frameBytes_ > derived()->busCapacity()) return;

        // Fused per-ROW pass, one runtime branch on the bus width: ≤8 lanes clock an
        // 8-bit bus (uint8 slots, the transposeLanes8x8 core); 9..16 lanes clock a
        // 16-bit bus (uint16 slots, transposeLanes16x8). The branch is OUTSIDE the
        // row loop, so each width's inner loop stays branch-free.
        if (laneCount_ <= 8) encodeRows<uint8_t>(outCh);
        else                 encodeRows<uint16_t>(outCh);

        // The latch pad after the rows is zeroed at reinit and never written
        // here, so the transfer ends holding every lane LOW for >=300 µs. Wait
        // only when the transfer actually started: a failed transmit gives no
        // done-callback, so an unconditional wait would block the full 1000 ms
        // timeout every tick. Drop the frame and retry next tick (self-heals).
        if (derived()->busTransmit(frameBytes_))
            derived()->busWait(1000 /* ms */);
    }

    // Encode every row into the DMA buffer as `Slot`-wide bus words (uint8_t for the
    // 8-bit bus, uint16_t for the 16-bit bus). Correct the same light index of every
    // active lane into the wire block, then transpose+emit its 3 slots. No heap,
    // integer math only. `dmaBuf_` is raw bytes at the seam; a 16-bit frame views it
    // as uint16 slots (the buffer was sized ×2 by frameBytesFor). Called from tick().
    template <class Slot>
    void encodeRows(uint8_t outCh) {
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        auto* out = reinterpret_cast<Slot*>(dmaBuf_);
        uint8_t wire[kMaxLanes * 4];
        for (nrOfLightsType row = 0; row < maxLaneLights_; row++) {
            Slot mask = 0;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row >= laneCounts_[lane]) continue;   // short strand: idle LOW
                mask |= static_cast<Slot>(Slot(1) << lane);
                // winStart_ shifts this driver's whole slice; laneStart_ is the
                // per-lane offset within it.
                correction_.apply(src + (winStart_ + laneStart_[lane] + row) * srcCh,
                                   wire + lane * 4);
            }
            encodeWs2812LcdSlots<Slot>(wire, mask, outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3;   // 3 slots × 8 bits × channels, in Slot elements
        }
    }

    // Encode the loopback test frame at `Slot` width: the same pattern on lane 0 in every
    // row (activeMask = bit 0). Matches the private loopback bus's width so the DMA stride
    // is right. `frame` is raw bytes; viewed as Slot elements.
    template <class Slot>
    void encodeLoopbackFrame(uint8_t* frame, const uint8_t* wire, uint8_t outCh,
                             nrOfLightsType lights) {
        auto* out = reinterpret_cast<Slot*>(frame);
        for (nrOfLightsType row = 0; row < lights; row++) {
            encodeWs2812LcdSlots<Slot>(wire, Slot(1), outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3;
        }
    }

    /// Test-only accessors — pin the lane slicing and frame-size arithmetic on the
    /// host (unit_{Lcd,Parlio}LedDriver.cpp); the hardware half is proven on device.
    uint8_t laneCount() const { return laneCount_; }
    /// Lights on lane `i` (0 if out of range). Test-only.
    nrOfLightsType laneLightCount(uint8_t i) const { return i < laneCount_ ? laneCounts_[i] : 0; }
    /// First light index of lane `i`'s slice (0 if out of range). Test-only.
    nrOfLightsType laneStart(uint8_t i) const { return i < laneCount_ ? laneStart_[i] : 0; }
    /// Length of the longest lane — the frame's row count. Test-only.
    nrOfLightsType maxLaneLights() const { return maxLaneLights_; }
    /// Total DMA frame size in bytes (rows + latch pad). Test-only.
    size_t frameBytes() const { return frameBytes_; }

protected:
    Derived* derived() { return static_cast<Derived*>(this); }
    const Derived* derived() const { return static_cast<const Derived*>(this); }

    Buffer* sourceBuffer_ = nullptr;

    LedDriverConfig cfg_;
    bool inited_ = false;
    uint8_t* dmaBuf_ = nullptr;          // platform-owned; cached for the encode
    uint16_t laneList_[kMaxLanes] = {};
    nrOfLightsType laneCounts_[kMaxLanes] = {};
    nrOfLightsType laneStart_[kMaxLanes] = {};
    nrOfLightsType winStart_ = 0;   // first source-buffer light this driver reads (the window)
    nrOfLightsType winLen_ = 0;     // window length (lights), clamped to the buffer
    uint8_t laneCount_ = 0;
    nrOfLightsType maxLaneLights_ = 0;
    size_t frameBytes_ = 0;
    uint16_t busPins_[kMaxLanes] = {};   // data pins the live bus/unit was built
                                         // with — a pin change must rebuild even
                                         // when the buffer still fits
    uint8_t  busLaneCount_ = 0;          // lane count the live bus was built with —
                                         // a lane-count change (e.g. Parlio 8→4)
                                         // can keep the same frameBytes_ yet needs
                                         // a rebuild, so the fast path checks it too

    static constexpr uint8_t maxLanesForTarget() {
        return (Derived::lanesAvailable() > 0 && Derived::lanesAvailable() < kMaxLanes)
                   ? Derived::lanesAvailable()
                   : kMaxLanes;
    }

    /// CRTP hook (default: no extra bus pins to validate). A derived driver whose
    /// peripheral commits its own GPIOs beyond the data lanes (the i80 bus's WR/DC)
    /// HIDES this to flag a data lane that overlaps them. Returns a WARNING string
    /// (the driver keeps running — see LcdLedDriver::validateBusPins for why it's a
    /// warning, not a blocker) or null when the data pins are clean. Parlio has no
    /// such pins, so it uses this default.
    const char* validateBusPins(const uint16_t* /*lanes*/, uint8_t /*n*/) const { return nullptr; }

    // Frame bytes: longest lane × channels × 24 slots, plus a zeroed latch pad of
    // >=300 µs at the slot rate (800 slots) with clock-tolerance slack (64), each
    // scaled by `slotBytes` (1 for an 8-bit bus, 2 for a 16-bit bus — a slot is one
    // bus WORD, so a 16-lane frame is 2 bytes/slot), rounded up to the bus's 64-byte
    // alignment. 0 when there's nothing to send. The pad scales too: it's a count of
    // idle bus words, and its LOW duration is measured in slots, so an unscaled pad
    // would be half the latch time on a 16-bit bus and could latch a strand mid-frame.
    static size_t frameBytesFor(nrOfLightsType maxLights, uint8_t outCh, uint8_t slotBytes) {
        if (maxLights == 0 || outCh == 0) return 0;
        const size_t latchPad = static_cast<size_t>(800 + 64) * slotBytes;
        const size_t bytes = static_cast<size_t>(maxLights) * outCh * 24 * slotBytes + latchPad;
        return (bytes + 63) & ~static_cast<size_t>(63);
    }

    // Bytes per bus slot: 1 for the 8-bit bus (≤8 lanes), 2 for the 16-bit bus.
    uint8_t slotBytes() const { return laneCount_ > 8 ? 2 : 1; }

    // Re-derive lanes/counts/starts/frame size from the controls and the wired
    // buffer/correction. Off the hot path; on error the driver idles with the
    // parse literal in the status slot (same shape as RmtLedDriver).
    bool parseConfig() {
        laneCount_ = 0;
        maxLaneLights_ = 0;
        frameBytes_ = 0;
        uint8_t n = 0;
        const char* err = parsePinList(pins, laneList_, maxLanesForTarget(), n);
        // i80 (kExactLaneCount) requires a real GPIO on every data line up to the
        // bus width, and the bus width is power-of-two only (8 or 16) — a partial
        // bus is rejected at esp_lcd_new_i80_bus(). So LCD accepts EXACTLY 8 or 16
        // pins; a sub-16 board parks unused lanes on spare GPIOs. Parlio accepts
        // 1..16 (unused lanes idle NC), so it sets kExactLaneCount=false and skips this.
        if constexpr (Derived::kExactLaneCount) {
            if (!err && n != 8 && n != 16) err = "LCD bus needs exactly 8 or 16 pins";
        }
        // Peripheral-specific data-pin check (i80's WR/DC on a data lane routes two
        // output signals to one pin, so that lane emits the clock/DC waveform, not
        // pixel data). This is a WARNING, not a blocker: on a board that wires all
        // 8/16 lanes but drives fewer strands, an unused data pin is a legitimate
        // WR/DC candidate — only the lanes that actually drive a strand would show
        // garbage, and the user opted into that. Parlio has no WR/DC and returns null.
        // Kept a CRTP hook so the base stays peripheral-neutral.
        const char* warn = err ? nullptr : derived()->validateBusPins(laneList_, n);
        if (!err) {
            // Distribute over this driver's window slice, not the whole buffer.
            // assignCounts clamps each lane to kMaxWs2812LedsPerPin (drives that many
            // rather than choking a whole grid onto one WS2812 line). Its own warning
            // (a clamped lane) wins over the WR/DC one only if it sets warn non-null.
            const nrOfLightsType bufN = sourceBuffer_ ? sourceBuffer_->count() : 0;
            windowSlice(bufN, winStart_, winLen_);
            const char* clampWarn = nullptr;
            err = assignCounts(ledsPerPin, n, winLen_, laneCounts_, kMaxWs2812LedsPerPin, &clampWarn);
            if (clampWarn) warn = clampWarn;
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        laneCount_ = n;
        nrOfLightsType start = 0;
        for (uint8_t i = 0; i < laneCount_; i++) {
            laneStart_[i] = start;
            start = static_cast<nrOfLightsType>(start + laneCounts_[i]);
            if (laneCounts_[i] > maxLaneLights_) maxLaneLights_ = laneCounts_[i];
        }
        const uint8_t outCh = correction_.outChannels;
        // laneCount_ is set above, so slotBytes() reflects the derived bus width (1 for
        // ≤8 lanes, 2 for the 16-bit bus) — a 16-lane frame is twice the bytes.
        frameBytes_ = frameBytesFor(maxLaneLights_, outCh, slotBytes());
        clearConfigErr();
        // A lane clamped to the WS2812 ceiling still drives — Warning, not error (see RmtLed).
        setConfigWarn(warn);
        // With nothing more urgent to show AND lights actually driven, report how many
        // this driver consumes (Σ laneCounts_ = `start`) of its window — so the user sees
        // real consumption instead of guessing from grid × pins. A clamp warning wins; an
        // idle driver (no pins / empty buffer) stays statusless, not "driving 0 of 0".
        if (!warn && start > 0) setDrivingInfo(start, winLen_);
        return true;
    }

    // --- bus + DMA buffer (hardware; this peripheral's targets only). Fused on
    // purpose: max_transfer_bytes is fixed at bus creation, so growing the frame
    // means re-creating the bus and its buffer together. Grow-only; the buffer is
    // re-zeroed on every reinit so a shrunken frame's latch pad can't hold stale
    // row bytes. ---

    void reinit() {
        if constexpr (Derived::lanesAvailable() == 0) return;
        if (laneCount_ == 0 || frameBytes_ == 0) { deinit(); return; }
        // Reuse the existing bus ONLY when the frame is the EXACT same size and on the same pins/lanes.
        // Grow OR shrink both rebuild: a peripheral whose single-transfer size is fixed at bus creation
        // (Parlio: max_transfer_size sets a hardware bit-length cap) becomes INVALID if the buffer is
        // reused at a different size — a shrunk frame kept an oversized unit whose configured max
        // exceeded the hardware frame limit, so every transmit silently failed (tx=0, no output, no
        // error). Exact-match reuse (not `>=`) keeps the bus always valid-or-rebuilt. The pin check
        // matters (a pin edit keeps the size but must move the GPIOs); the lane-count check matters for
        // Parlio (8→4 keeps frameBytes but the bus was built for 8 lanes).
        if (inited_ && derived()->busCapacity() == frameBytes_ && busPinsCurrent()
            && busLaneCount_ == laneCount_) {
            std::memset(dmaBuf_, 0, derived()->busCapacity());   // clear stale latch-pad bytes
            return;
        }
        deinit();
        inited_ = derived()->busInit(frameBytes_);
        dmaBuf_ = inited_ ? derived()->busBuffer() : nullptr;
        if (inited_) {
            for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
            busLaneCount_ = laneCount_;
            derived()->recordBusPins();   // i80 also stores WR/DC; Parlio no-op
        }
        if (!inited_) {
            clearFailBuf();
            setStatus(Derived::kInitFailMsg, Severity::Error);
        } else if (status() == Derived::kInitFailMsg) {
            clearStatus();
        }
    }

    void deinit() {
        if constexpr (Derived::lanesAvailable() == 0) return;
        if (inited_) derived()->busDeinit();
        inited_ = false;
        dmaBuf_ = nullptr;
        busLaneCount_ = 0;
    }

    bool busPinsCurrent() const {
        for (uint8_t i = 0; i < laneCount_; i++)
            if (busPins_[i] != laneList_[i]) return false;
        return derived()->extraBusPinsCurrent();   // i80 also checks WR/DC
    }

    // --- loopback self-test (control-driven; same status shapes as RMT). Builds
    // the REAL frame with the test pattern on lane 0, deinits the live bus, and
    // hands the platform a private TX path + RMT-RX capture that transmits the
    // genuine frame back to back and verifies every bit. ---

    void runLoopbackSelfTest() {
        if constexpr (Derived::lanesAvailable() == 0) {
            clearFailBuf();
            setStatus("loopback: not supported on this platform", Severity::Warning);
            return;
        }
        if (laneCount_ == 0) {
            clearFailBuf();
            setStatus("loopback: no valid pins", Severity::Warning);
            return;
        }
        // RX pin must be set — an unset rxPin (-1) has nothing to capture on, and the
        // uint16_t cast at the busLoopback call would turn -1 into a bogus pin.
        if (loopbackRxPin < 0) {
            clearFailBuf();
            setStatus("loopback: set loopbackRxPin (jumper it to the TX pin)", Severity::Status);
            return;
        }
        const uint8_t outCh = correction_.outChannels;
        if (frameBytes_ == 0 || maxLaneLights_ == 0 || outCh == 0) {
            clearFailBuf();
            setStatus("loopback: no lights to encode", Severity::Warning);
            return;
        }
        // Cap the test frame to kLoopbackTestLights (see its declaration for why a big frame
        // overruns the P4 Parlio transfer limit + the RMT-RX capture buffer).
        const nrOfLightsType lights =
            maxLaneLights_ < kLoopbackTestLights ? maxLaneLights_ : kLoopbackTestLights;
        // The loopback frame width is per-driver (kLoopbackFullWidth): the i80 loopback rebuilds
        // the FULL-WIDTH private bus (it can't do a 1-lane bus), so a 16-lane driver's frame must
        // be 16-bit slots to match — and this then genuinely exercises the 16-bit transpose+DMA on
        // real silicon. Parlio builds a 1-lane private unit, so its loopback stays 8-bit regardless.
        const uint8_t sb = Derived::kLoopbackFullWidth ? slotBytes() : 1;
        const size_t perLightBytes = static_cast<size_t>(outCh) * 8 * 3 * sb;   // 3 slots/bit × slotBytes
        const size_t testFrameBytes = static_cast<size_t>(lights) * perLightBytes;
        // Build the REAL frame with the test pattern in every row on lane 0 only;
        // the platform transmits the genuine transfer (size, DMA chain, latch pad)
        // back to back and verifies every captured bit, so the test covers what
        // the render loop actually sends. Heap alloc is fine: control-driven, off
        // the hot path.
        auto* frame = static_cast<uint8_t*>(platform::alloc(testFrameBytes));
        if (!frame) {
            clearFailBuf();
            setStatus("loopback: out of memory", Severity::Error);
            return;
        }
        std::memset(frame, 0, testFrameBytes);
        uint8_t wire[kMaxLanes * 4] = {};
        wire[0] = 0xA5; wire[1] = 0x00; wire[2] = 0xFF;   // wire[3] stays 0 (RGBW)
        // Encode the pattern on lane 0 at the operational width. A wider bus adds only
        // idle high lanes (activeMask = bit 0); the private bus + loopbackTxPin override
        // carry lane 0's signal to the jumpered RX pin. Sized-per-slot so the 16-bit
        // buffer stride matches the 16-bit private bus.
        if (sb == 1) encodeLoopbackFrame<uint8_t>(frame, wire, outCh, lights);
        else         encodeLoopbackFrame<uint16_t>(frame, wire, outCh, lights);
        // dataBytes drives the RX capture's WS2812-bit count (kBits = dataBytes/3),
        // which is the wire signal on lane 0's RX pin — the SAME bit count at any bus
        // width. So it is width-INDEPENDENT (no ×slotBytes): a wider bus fattens each
        // slot in the DMA buffer (that's testFrameBytes/frameBytes, ×sb) but clocks
        // the same number of WS2812 bits out of lane 0.
        const size_t dataBytes = static_cast<size_t>(lights) * outCh * 24;
        deinit();   // free the live bus; the test builds its own on the data pins
        // TX override: the loopback drives lane 0 only, so when loopbackTxPin is
        // set, transmit on it instead of laneList_[0] (the operational LED pin),
        // letting the test run on a dedicated jumper without re-typing `pins`. The
        // subclass busLoopback reads laneList_, so substitute lane 0 around the call
        // and restore it after, so the following reinit() rebuilds the real bus.
        const uint16_t realLane0 = laneList_[0];
        if (loopbackTxPin >= 0) laneList_[0] = static_cast<uint16_t>(loopbackTxPin);
        const auto r = derived()->busLoopback(frame, testFrameBytes, dataBytes,
                                              static_cast<uint8_t>(outCh * 8));
        laneList_[0] = realLane0;
        platform::free(frame);
        // Loopback result first, then reinit: if rebuilding the real bus fails
        // afterwards, kInitFailMsg overwrites the verdict — an unusable driver
        // matters more than a passed self-test.
        if (!r.jumperDetected) {
            clearFailBuf();
            setStatus("loopback: jumper not detected", Severity::Warning);
        } else if (r.pass) {
            clearFailBuf();
            setStatus("loopback PASS", Severity::Status);
        } else if (failBufEnsure()) {
            // Name the first corrupted light: the loopback reports the first
            // mismatching bit; rowBits = outCh*8, so light = firstBadBit / rowBits.
            const unsigned rowBits = static_cast<unsigned>(outCh) * 8u;
            const unsigned badLight = rowBits ? r.firstBadBit / rowBits : 0u;
            std::snprintf(failBuf_, kFailBufLen,
                          "loopback FAIL: bad bit %u/%u (light %u)",
                          static_cast<unsigned>(r.firstBadBit),
                          static_cast<unsigned>(r.bitsChecked), badLight);
            setStatus(failBuf_, Severity::Error);
        } else {
            setStatus("loopback FAIL", Severity::Error);
        }
        reinit();
    }
};

} // namespace mm
