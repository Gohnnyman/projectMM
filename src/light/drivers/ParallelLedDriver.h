#pragma once

#include "light/drivers/DriverBase.h"        // DriverBase, Correction
#include "light/drivers/ParallelSlots.h"        // encodeWs2812ParallelSlots (shared encoder)
#include "light/drivers/LedDriverConfig.h"
#include "light/drivers/PinList.h"         // parsePinList / assignCounts (shared)
#include "platform/platform.h"


namespace mm {

template <class Derived>
/// Base for the parallel WS2812B LED-output drivers — the S3's LCD_CAM i80 bus (MultiPinLedDriver) and
/// the P4's Parlio peripheral (ParlioLedDriver). Both drive up to 16 strands that clock out
/// SIMULTANEOUSLY, one GPIO lane each, fed consecutive slices of the source buffer (see kMaxLanes).
///
/// **Single-shot autonomous DMA:** both pre-encode the whole frame (a per-ROW fused
/// correct+transpose, the SAME ParallelSlots.h encoder — a Parlio bus byte and an i80 bus byte are
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
/// wrong-chip `if constexpr` guard), `kPowerOfTwoBus` (i80 needs exactly 8 or 16; Parlio runs 1..16), the
/// slot rate `kClockHz`, and any extra pins the i80 driver tracks (WR/DC) that Parlio doesn't.
/// configErr_/failBuf_ come from DriverBase (shared with RmtLedDriver too).
class ParallelLedDriver : public DriverBase {
public:
    /// WS2812/SK6812 strips are GRB-wired, so a fresh parallel LED driver (and its MultiPinLedDriver
    /// subclass) references the "GRB" preset by default. The user can pick any preset.
    ParallelLedDriver() { this->setDefaultPresetName("GRB"); }

    /// Max parallel lanes = the peripheral's full 16 data lines. The bus width the driver
    /// actually builds is DERIVED from the configured pin count: ≤8 pins → an 8-bit bus (uint8
    /// slots), 9..16 pins → a 16-bit bus (uint16 slots). Both peripherals do 16 data lines
    /// (LCD_CAM `bus_width` 8|16; Parlio `data_width` 8|16 — powers of two only).
    /// This is the BUS WIDTH — a hardware fact. The number of STRANDS can exceed it when a
    /// shift-register expander is fitted (see kMaxStrands).
    static constexpr uint8_t kMaxLanes = 16;

    /// Max strands the driver can drive: every data line fanned out through its '595 chain. Sizes the
    /// per-strand arrays (laneCounts_, laneStart_). In direct mode only the first kMaxLanes are used.
    ///
    /// **64 is a REAL ceiling, and it is the active-strand mask that sets it.** The mask is a
    /// `uint64_t` (encodeWs2812ShiftSlots' `activeMask`), so one row carries at most 64 strands.
    /// parseConfig rejects anything larger rather than silently dropping strands.
    ///
    /// What that allows and blocks — worth knowing before wiring a board:
    ///
    /// | config | strands | frame (256 lights) | |
    /// |---|---|---|---|
    /// | 6 pins ×8  | 48  | ~151 KB | ✅ hpwit's board today |
    /// | 7 pins ×8  | 56  | ~151 KB | ✅ |
    /// | 15 pins ×8 | 120 | ~301 KB | ❌ over the mask (hpwit's headline number) |
    ///
    /// **Lifting it is cheap and NOT blocked by the uint64_t** — that framing would be wrong. The
    /// encoder only ever needs *one physical pin's* strands at a time (it indexes
    /// `p * outputsPerPin() + pos`), so a **per-pin mask of ≤16 bits** removes the ceiling entirely
    /// without ever making the hot-path test a multi-word object. Deliberately not done yet: no board
    /// we have needs >56, and the encode loop is still being profiled (it is the fps bottleneck), so
    /// this is queued rather than guessed at.
    ///
    /// Extra **pins** ride the bus width and are nearly free; extra **shift depth** (cascading '595s)
    /// would double the DMA frame *and* the shift time. hpwit's 120 comes from 15 PINS × 8, not from
    /// cascading. **More pins beats deeper cascades** — which, with the clock arithmetic in
    /// ParallelSlots.h, is why only the ×8 wiring is offered at all.
    static constexpr uint8_t kMaxStrands = 64;

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
    /// Comma-separated lights-per-lane; the unassigned remainder splits evenly over the remaining
    /// lanes. **A lane is a STRAND, not a pin** — which only differ through the expander: direct
    /// mode has one strand per pin, so entry N is pin N's; with `pinExpander` on, each pin fans
    /// out to 8 strands, so the list runs over all `pins × 8` of them (pin 0's eight first, then
    /// pin 1's, …) and entry N is strand N. That is what lets two strands on one '595 have
    /// different lengths. Each lane is clamped to the WS2812 per-pin ceiling
    /// (`kMaxWs2812LedsPerPin`) with a Warning status — it drives that many rather than choking a
    /// whole grid onto one line. Empty default splits this driver's window evenly. Sized for the full
    /// kMaxStrands contract: each strand's value is at most kMaxWs2812LedsPerPin (2048 → 4 digits) plus a
    /// separator, times kMaxStrands, plus the NUL — so a fully-explicit 64-strand list never truncates.
    char ledsPerPin[kMaxStrands * 5 + 1] = "";

    /// Async double-buffer transmit (default ON). When ON, the driver encodes the next frame into a
    /// SECOND DMA buffer while the current one clocks out (deferred-wait — per-tick wall-clock
    /// `max(encode, wire)` instead of `encode + wire`), so the blocking WS2812 wire wait no longer
    /// stalls the render thread. Measured on the P4 at 16×256 lights: it lifts the whole board from
    /// ~48 to ~76 fps (the driver tick drops from ~10.8 ms to ~3.8 ms of CPU — the ~7.7 ms wire moves
    /// into background DMA).
    ///
    /// **ON is simply the better configuration — the switch exists to A/B it, not because some setups
    /// should run it OFF.** The one-frame output latency it adds (~8–20 ms) sits inside the perceptual
    /// audio↔visual sync window and is small next to what the pipeline already spends (FFT window +
    /// render + the 8–16 ms WS2812 wire itself), so there is no user class — sound-reactive included —
    /// that should turn it off for latency. Leave it ON and take the fps.
    ///
    /// **It is per-driver because the resource is per-driver:** each driver's second DMA buffer lives
    /// on its own peripheral, sized by its own lane count and strand length (a 16-lane i80 frame and a
    /// 1-lane RMT strand are wildly different allocations), so a board can afford it for one driver and
    /// not another. If the buffer won't fit, the driver degrades to the synchronous path by itself
    /// rather than failing — the flag is the *measurement* knob; the degrade is automatic.
    ///
    /// Toggling rebuilds the bus (via affectsPrepare) to add or free the second buffer. Distinct from
    /// the container's `multicore` control, and they stack: this hides the WIRE behind DMA within one
    /// core; that hides the ENCODE behind the render on the other core. See docs/history/lessons.md.
    bool doubleBuffer = true;
    /// Streaming-ring source snapshot on/off — an A/B measurement knob, ring path only. ON (default,
    /// the safe behavior) freezes the source into a driver-owned buffer each frame so the ring's refill
    /// (which encodes off the render thread, concurrently with rendering) reads an immutable copy — no
    /// use-after-free on a grid resize, no frame tearing. OFF reads the live source directly, matching
    /// the pre-snapshot behavior, so a bench can A/B the snapshot's effect on ring frame completion live
    /// without a reflash. Live (NOT a prepare trigger): it changes only what tickRing does per frame,
    /// nothing the bus was built with, so the next tick honors the new value. Ignored off the ring path
    /// (the whole-frame paths encode inline on the render thread, no concurrency, so they never snapshot).
    bool ringSnapshot = true;
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
    /// Which STRAND carries the test pattern (shift mode only; ignored in direct mode, where the
    /// pattern always rides lane 0).
    ///
    /// With a 74HCT595 expander the jumper comes off a register OUTPUT, and which output is
    /// physically easiest to reach varies by board — while which STRAND that output carries depends
    /// on the '595's shift order, which is exactly the kind of thing that is easy to get wrong on
    /// paper. So rather than force the wire to strand 0, point the TEST at the wire: tap any output,
    /// set this to the strand it drives, and the self-test puts its pattern there. A mismatch then
    /// shows up as a bit FAIL, and walking this control 0..N-1 until the test PASSES *identifies*
    /// the strand empirically — turning "which output is Q7?" from a guess into a measurement.
    uint8_t  loopbackStrand = 0;
    /// Jumper this to the TX lane for the self-test (unset = -1 by default).
    ///
    /// **With a 74HCT595 expander (`pinExpander` on) the jumper comes from a DIFFERENT place, and
    /// getting it wrong can damage the ESP32.** In direct mode you jumper a data GPIO straight to
    /// this pin. In shift mode a data GPIO carries the fast serial stream *into* the register, not
    /// pixel data — so the wire must come from the register's OUTPUT side instead:
    ///
    ///  - **Which output:** the test pattern is driven on the strand named by **`loopbackStrand`** (which
    ///    defaults to 0 but can be any strand — pick the '595 output that is physically easiest to reach,
    ///    e.g. a spare one that drives no panel). Strand S is data pin `S / 8`'s register at shift
    ///    position `S % 8`. A '595 shifts MSB-first — the first bit clocked in lands on the LAST output —
    ///    so shift position `p` appears on output **Q(7 − p)**. Strand 0 (position 0) is therefore Q7;
    ///    strand 7 (position 7) is Q0; and so on. Feed that output back.
    ///  - **⚠ LEVEL-SHIFT IT.** The '595 runs at 5 V, so its output swings to **5 V**, and an ESP32 GPIO
    ///    is **not 5 V tolerant** — a direct wire can damage the pin. Use a divider (e.g. 1 kΩ from the
    ///    output to the GPIO, 2 kΩ from the GPIO to GND, giving 5 V × 2/3 ≈ 3.3 V) or any 5 V→3.3 V shifter.
    ///  - **No continuity pre-check runs in shift mode** — it would drive the TX pin and expect this
    ///    pin to follow, which a shift register does not do (that takes 8 clocks + a latch). So a
    ///    mis-wired jumper shows up as a bit FAIL, not as "jumper not detected".
    ///
    /// The payoff is a *stronger* test than direct mode: it verifies the whole chain — encode → i80
    /// bus → shift register → latch → output — rather than only the ESP32 half.
    int8_t   loopbackRxPin = -1;

    /// Is a 74HCT595 shift-register expander board fitted? OFF (the default) = strands wired straight
    /// to the GPIOs; ON = each data pin feeds one '595, so the driver drives `pins` × 8 strands.
    ///
    /// A **checkbox, not a fan-out number**: the 8 is the '595's own width, not a free parameter, so
    /// there are exactly two wirings and a boolean says which one the board has. (An earlier
    /// `outputsPerPin` number invited half-configured values like 4 and, bound as a Select, stored the
    /// menu *index* — which made `1` mean *eight* in a catalog entry.)
    ///
    /// The expander buys pins, not memory: the '595 is serial-in, so presenting 8 bits costs 8 shift
    /// cycles, and the DMA frame grows ×8 (each WS2812 slot becomes 8 bus words). Extra lanes on an
    /// already-fanned-out pin are then free (they ride the bus width) — which is why 15×256 and
    /// 48×256 cost the SAME frame. The bus also clocks ×8 faster (see kShiftPclkHz).
    /// Needs a `latchPin` and the LCD_CAM i80 path (ESP32-S3 / -P4); refused with a status elsewhere.
    ///
    /// **⚠️ EXPERIMENTAL, and size-limited today — the limit depends on the backend.** The ×8 frame is
    /// ~1.1 KB/light, so it exceeds internal DMA RAM (~110 KB, less once WiFi has taken its share) above
    /// roughly **96 lights per strand**. The **esp_lcd i80** backend must stream one contiguous frame, so
    /// it is capped there (above it the frame falls to PSRAM, which the S3's GDMA cannot sustain at the
    /// expander's 26.67 MHz clock — the frame never completes). The **MoonI80** backend lifts that with a
    /// streaming ring (small internal buffers refilled as the DMA drains them, so PSRAM is never on the
    /// path): it drives **128 lights/strand reliably**. Above 128 the ring reuses buffers and a residual
    /// refill-vs-DMA race still stalls it — the current working ceiling is 128/strand via MoonI80, 96 via
    /// i80.
    ///
    /// Full status, the reuse-race + concurrency follow-ups, and the measurements behind these limits:
    /// [the analysis](https://github.com/MoonModules/projectMM/blob/main/docs/backlog/shift-register-driver-analysis.md)
    /// and the ring items in `docs/backlog/backlog-light.md`.
    bool     pinExpander = false;
    /// The 74HCT595 LATCH (RCLK) line — pulsed once the shifted byte is in, presenting it on the
    /// '595 outputs. Unlike the shift clock (the peripheral's own WR pin), this is a DATA lane:
    /// it occupies one bit of every bus word, so it must NOT collide with a data pin or clockPin.
    /// Shown only when the expander is on. Unset (-1) until the user wires it.
    int8_t   latchPin = -1;

    /// Is the shift-register expander engaged? (Reads the control; the alias keeps the intent
    /// readable at the call sites that ask "am I encoding through a register?")
    bool pinExpanderMode() const { return pinExpander; }
    /// Strands driven per physical data pin: 1 direct, or the '595's width through the expander.
    /// The multiplier every size/clock/slot computation below is expressed in.
    uint8_t outputsPerPin() const { return pinExpander ? kPinExpanderOutputs : uint8_t(1); }

    /// Bind the driver's controls: the window (start/count), the `pins` and
    /// `ledsPerPin` text lists, any derived-supplied bus controls (i80 adds
    /// clockPin/dcPin, Parlio none), and the loopback self-test controls (TX/RX pin
    /// overrides always bound but shown only in test mode).
    void defineDriverControls() override {
        addWindowControls();   // start / count — the slice of the shared buffer this driver outputs
        controls_.addText("pins", pins, sizeof(pins));
        controls_.addText("ledsPerPin", ledsPerPin, sizeof(ledsPerPin));
        controls_.addBool("doubleBuffer", doubleBuffer);   // double-buffer on/off (latency opt-out)
        // Streaming-ring source-snapshot A/B knob (ring path only; inert on the whole-frame paths). Shown
        // unconditionally: the precise "only when ringing" gate would key on wantsRing(), but that reads
        // frameBytes_, which is 0 at defineControls() time (the source buffer is wired AFTER the schema is
        // built at boot) — so a wantsRing() gate hid it exactly when it was needed. Backlogged: hide the
        // ring-inert controls once the schema can be rebuilt post-source-wiring. See backlog-light.md.
        controls_.addBool("ringSnapshot", ringSnapshot);
        // Read-only KPI: the measured DMA wire time + the fps ceiling it implies. The pure output
        // floor (independent of render load), so it shows how much headroom remains as the pipeline
        // improves — and it reflects an overclocked slot rate directly. Refreshed in tick1s().
        controls_.addReadOnly("frameTime", frameTimeStr_, sizeof(frameTimeStr_));
        // A checkbox: the expander is fitted or it isn't. The '595's width (8) is the chip's, not a
        // setting, so there is nothing to type — and a boolean can't be half-configured.
        controls_.addBool("pinExpander", pinExpander);
        // The bus pins sit UNDER the expander toggle because for a driver that owns its own GPIO
        // routing they are '595 pins: MoonI80 routes WR only when a shift register needs it as SRCLK,
        // and hides the control otherwise. (I80 goes through esp_lcd, which mandates a valid WR *and*
        // DC GPIO whatever the mode, so it shows both unconditionally — same hook, different answer.)
        derived()->addBusControls();
        // Always bound, shown only when the expander is on — the conditional-control shape
        // (bound regardless so a saved latchPin survives a round-trip through direct mode).
        controls_.addPin("latchPin", latchPin);
        controls_.setHidden(controls_.count() - 1, !pinExpanderMode());
        controls_.addBool("loopbackTest", loopbackTest);
        // Always bound, shown only in test mode — the conditional-control shape.
        controls_.addPin("loopbackTxPin", loopbackTxPin);
        // Direct mode only. In shift mode the captured strand is decided by which '595 OUTPUT the
        // jumper is on (loopbackStrand), not by which GPIO transmits — runLoopbackSelfTest ignores
        // the override there — so showing it would only invite a mis-set control.
        controls_.setHidden(controls_.count() - 1, !loopbackTest || pinExpanderMode());
        controls_.addPin("loopbackRxPin", loopbackRxPin);
        controls_.setHidden(controls_.count() - 1, !loopbackTest);
        // Which strand carries the pattern — shift mode only (in direct mode it is always lane 0).
        // Lets the jumper come off ANY '595 output, including a spare one that drives no panel.
        controls_.addUint8("loopbackStrand", loopbackStrand, 0, kMaxStrands - 1);
        controls_.setHidden(controls_.count() - 1, !loopbackTest || !pinExpanderMode());
    }

    /// A change to the pins, per-lane counts, the window, a derived bus control (clockPin/dcPin on
    /// i80), or `doubleBuffer` re-parses and re-inits the bus live via the prepare sweep.
    /// doubleBuffer is a prepare trigger because it drives ALLOCATION: OFF allocates one DMA buffer,
    /// ON (the default) allocates a second for the double-buffer — so toggling it must rebuild the bus
    /// to add or free that buffer (a board that turns it off pays no second-buffer memory).
    /// pinExpander / latchPin are prepare triggers too: the expander multiplies frameBytes_ ×8 and
    /// re-maps lanes onto pins, and the latch claims a bus bit — all of which is decided at bus init.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "pins") == 0 || std::strcmp(name, "ledsPerPin") == 0
            || std::strcmp(name, "doubleBuffer") == 0
            || std::strcmp(name, "pinExpander") == 0 || std::strcmp(name, "latchPin") == 0
            || isWindowControl(name)
            || derived()->busControlTriggersBuild(name);   // clockPin/dcPin on i80
    }

    /// React to a control change off the render loop. loopbackTest is a persistent
    /// on/off mode: while ON, the self-test re-runs on every relevant change (with a
    /// lane-config refresh first, since onControlChanged precedes the prepare sweep);
    /// turning it OFF clears the verdict and re-derives the real driver status.
    void onControlChanged(const char* name) override {
        const bool isTestControl = std::strcmp(name, "loopbackTest") == 0;
        // Every control that reshapes the lane config the self-test transmits through. pinExpander
        // and latchPin belong here for the same reason `pins` does: they change laneList_,
        // frameBytes_ and latchBit_, so a test left running across such an edit would otherwise
        // re-transmit through a stale bus and report a verdict for a configuration that is gone.
        const bool isPinControl  = std::strcmp(name, "pins") == 0
                                || std::strcmp(name, "pinExpander") == 0
                                || std::strcmp(name, "latchPin") == 0
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
        deinit();   // drains in-flight first, so the ring's refill has stopped reading the snapshot
        if (snapshotBuf_) { platform::free(snapshotBuf_); snapshotBuf_ = nullptr; snapshotCap_ = 0; }
        encodeSrc_ = nullptr;
        DriverBase::release();   // frees the correction scratch, clears failBuf_ + configErr_
    }

    /// Pure build (see MoonModule::prepare): re-parse the lanes and (re)init the bus off the
    /// hot path. No enabled() check — core's applyState() calls this only when effectively-enabled
    /// and routes to release() (bus + DMA buffer freed) otherwise, so a shared GPIO is released
    /// when the driver, or a parent, is disabled.
    void prepare() override {
        // Drain any in-flight transfer BEFORE parseConfig, not just before reinit's rebuild. parseConfig
        // reallocates the per-row scratch (prepareWire → ensureWire frees-then-allocs on a channel-count
        // change) and zeroes laneCounts_ — state the streaming ring's REFILL TASK reads concurrently on
        // its own core for the ~5 ms wire window. Draining first guarantees that reader has stopped, so a
        // grid resize or an RGB→RGBW switch mid-wire can't free the block the refill task is encoding
        // into. A no-op when idle, and correct for the whole-frame paths too (they had no second reader,
        // so it never mattered there — the ring is the first path that needs it).
        drainInFlight();
        parseConfig();
        reinit();
    }

    /// RGB<->RGBW changes the bytes-per-light and therefore the frame size, so re-parse and re-init the
    /// bus — but ONLY when the frame size actually changed. Skipped while (effectively) disabled (would
    /// re-grab the bus). Drains in-flight first (before parseConfig reallocates the scratch the ring's
    /// refill task reads) — see prepare().
    ///
    /// **The frame-size gate is what stops a redundant double-reinit.** The Drivers container's prepare()
    /// pushes every child's correction (passBufferToDrivers → rebuildCorrection → this) and THEN the same
    /// prepareTree sweep runs each child's own prepare() (which reinits unconditionally). Without the gate,
    /// a plain `ledsPerPin`/window edit — which changes the LIGHT COUNT but not the per-light CHANNELS, so
    /// frameBytes_ is unchanged by the correction push — would reinit here AND again in prepare(), tearing
    /// the bus down and rebuilding it twice in rapid succession. On the shift-mode LCD_CAM ring that second
    /// rapid rebuild came up with its GDMA EOF interrupt dead (the "output stalled" wedge). Gating on a real
    /// frameBytes_ change makes this a no-op in that path (only prepare() rebuilds), while a genuine RGB↔RGBW
    /// channel-count change — which prepare() does NOT necessarily follow (a standalone preset/whiteMode edit
    /// doesn't trigger prepareTree) — still rebuilds here.
    void onCorrectionChanged() override {
        if (!effectivelyEnabled()) return;
        const size_t before = frameBytes_;
        drainInFlight();
        parseConfig();
        if (frameBytes_ != before) reinit();   // rebuild only on a real frame-size change (see above)
    }

    /// Point the driver at the source frame buffer and re-parse the lane config.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; parseConfig(); }

    /// Per-tick output (deferred-wait double-buffer): a fused per-ROW pass corrects the same
    /// light index of every active lane and transposes it into 3-slot bus words in the DMA buffer,
    /// then ships it as one autonomous transfer. Two paths, chosen by whether a second DMA buffer is
    /// allocated (which `doubleBuffer` drives at init): SYNCHRONOUS (default, one buffer — the
    /// original encode→transmit→wait, 0 added latency, provably no regression) or DEFERRED-WAIT
    /// DOUBLE-BUFFER (doubleBuffer ON — encode N+1 while N clocks out, `max(encode, wire)` per tick,
    /// +1 frame latency, +1 DMA buffer). Inert off this chip and idle until inited with a source
    /// buffer + correction. (The double-buffer defaults ON — it overlaps the blocking wire wait and
    /// lifted the P4 whole-board rate 48→76 fps; OFF is the sound-reactive 0-latency opt-out and pays
    /// for exactly one buffer — see the doubleBuffer control + docs/history/lessons.md.)
    void tick() override {
        if constexpr (Derived::lanesAvailable() == 0) return;  // inert off this chip
        // Loopback mode owns the peripheral EXCLUSIVELY. While it is on, the render loop must not
        // transmit — the loopback tears the bus down, drives its own private frame, and rebuilds it.
        // KNOWN BUG (backlog): after toggling OFF, the shift-mode bus does not deliver frames until a
        // reboot ("output stalled") — a no-reboot-principle violation still under investigation.
        if (loopbackTest) return;
        if (!inited_ || !dmaBuf_ || !sourceBuffer_ || !sourceBuffer_->data()
            || laneCount_ == 0 || maxLaneLights_ == 0) return;
        const uint8_t outCh = correction_.outChannels;
        if (outCh == 0) return;
        // The per-row scratch must be allocated and sized for this channel count (prepareWire, off the
        // hot path). If it isn't (alloc failed / a shrunk realloc pending), idle rather than overrun.
        if (!wire_ || wireCap_ < static_cast<size_t>(kMaxStrands) * outCh) return;

        // Three explicitly-separate paths. RING (MoonI80's oversize-shift path) streams the frame from
        // small internal buffers refilled by the platform, so it does NOT hold the whole frame and the
        // busCapacity() guard below (which the other two need) does not apply. The whole-frame paths keep
        // their guard and their proven behavior byte-for-byte.
        if (derived()->busIsRing()) { tickRing(outCh); return; }
        if (frameBytes_ > derived()->busCapacity()) return;

        // Two explicitly-separate whole-frame paths so the OFF path is PROVABLY the pre-Step-1.5 behavior
        // (no regression) and pays nothing for the double-buffer it isn't using. The mode is fixed by
        // whether the second buffer was allocated (busInit(async) — ON allocs it, OFF doesn't), so a
        // stale flag can't route a single-buffer bus down the async path.
        if (derived()->busBuffer(1)) tickAsync(outCh);   // double-buffer (doubleBuffer ON)
        else                         tickSync(outCh);    // synchronous (doubleBuffer OFF / no 2nd buf)
    }

    // Synchronous single-buffer path — the ORIGINAL tick, verbatim: encode buffer 0, transmit, wait
    // right here. One DMA buffer, no alternation, no deferred-wait bookkeeping, 0 added latency. This
    // is the default (doubleBuffer OFF) and its timing is exactly the pre-double-buffer driver's.
    void tickSync(uint8_t outCh) {
        if (busGaveUp()) return;
        // A previous frame's wait may have timed out, leaving the DMA still reading buffer 0 — re-wait
        // rather than encoding over a live transfer. (Normally a no-op: the wait below completes.)
        if (!busWaitIfBusy(0)) return;
        uint8_t* buf = derived()->busBuffer(0);
        if (!buf) return;
        // Branch on the BUS WIDTH (slotBytes), not the strand count — with a '595 expander the
        // strands ride the shift cycles, so 48 strands on 6 pins is still an 8-bit bus.
        if (slotBytes() == 1) encodeRows<uint8_t>(outCh, buf);
        else                  encodeRows<uint16_t>(outCh, buf);
        // The latch pad (zeroed at reinit, never written here) ends the transfer holding every lane
        // LOW >=300 µs. Wait only when the transfer started — a failed transmit gives no done-callback,
        // so an unconditional wait would block the full timeout. A timed-out wait leaves the buffer
        // marked in-flight so the next tick re-waits instead of corrupting the live transfer.
        if (derived()->busTransmit(0, frameBytes_)) {
            inFlight_[0] = true;
            busWaitIfBusy(0);   // synchronous: wait it out here; clears the flag on completion
        }
    }

    // Deferred-wait double-buffer path (doubleBuffer ON) — encode frame N+1 into the back buffer
    // while frame N clocks out of the front, so the per-tick wall-clock is max(encode, wire) instead
    // of encode + wire. Costs the second DMA buffer + 1 frame of output latency. See the class doc.
    void tickAsync(uint8_t outCh) {
        if (busGaveUp()) return;
        // 1. Finish the transfer that last used the buffer we're about to encode into, so the encode
        //    never overwrites a frame still clocking out (no-op on the first tick). If that wait TIMES
        //    OUT the DMA is still reading the buffer — skip this frame entirely rather than encoding
        //    into a live transfer (which would corrupt the frame on the wire). The buffer stays marked
        //    in-flight, so the next tick re-waits; a genuinely wedged DMA therefore idles the driver
        //    instead of emitting garbage, and it self-heals the moment the transfer completes.
        if (!busWaitIfBusy(active_)) return;
        // 2. Fused per-ROW encode into buffer `active_`, one branch on the bus width (see encodeRows).
        uint8_t* buf = derived()->busBuffer(active_);
        if (!buf) return;
        // Branch on the BUS WIDTH (slotBytes), not the strand count — with a '595 expander the
        // strands ride the shift cycles, so 48 strands on 6 pins is still an 8-bit bus.
        if (slotBytes() == 1) encodeRows<uint8_t>(outCh, buf);
        else                  encodeRows<uint16_t>(outCh, buf);
        // 3. Kick this buffer's DMA and return WITHOUT waiting; flip to the other buffer for next tick.
        if (derived()->busTransmit(active_, frameBytes_)) {
            inFlight_[active_] = true;
            active_ ^= 1;
        }
    }

    // Streaming-ring path (MoonI80, oversize shift mode) — the frame is too big to hold, so the platform
    // streams it from a few small internal buffers, refilled by a pinned task (ringEncodeTrampoline →
    // encodeRows) as the DMA drains them.
    //
    // **ASYNC per tick, like tickAsync — the render thread must NOT block on the wire.** The refill task
    // has a hard real-time deadline (the DMA drains one buffer in ~360 µs, so a slice must be re-encoded
    // within that), and it runs on the same core as the render loop. If tickRing BLOCKED here waiting out
    // the whole ~6 ms wire, the render thread would hold the core across that window — and any work that
    // piled on (a `/api/state` serialize from a UI refresh, a WiFi burst) would starve the refill task
    // past its deadline, the DMA would hit an un-refilled buffer, and the frame would stall (the
    // "freezes when I refresh the UI" bug). So: WAIT for the previous frame (a no-op if it finished while
    // the render ran), START the next, and RETURN — the wire and its refills proceed in the background
    // with the core free. One frame of latency, exactly as the double-buffer trades.
    void tickRing(uint8_t /*outCh*/) {
        if (busGaveUp()) return;
        // Finish the PREVIOUS ring frame before starting the next (a strand receives one frame at a time;
        // the ring reports completion on slot 0). Normally already done — the whole render tick elapsed
        // since we kicked it. A timed-out wait leaves the frame in-flight so the next tick re-waits rather
        // than starting a second frame over a live one.
        if (!busWaitIfBusy(0)) return;
        // Freeze the source for this frame BEFORE kicking the transfer: busTransmitRing primes the first
        // ring buffers synchronously (trampoline → encodeRows) and its refill re-encodes the rest off the
        // render thread across the ~6 ms wire, so both must read an immutable copy, not the live Layer
        // buffer the render loop is free to overwrite. A failed snapshot (source gone / OOM) skips the
        // frame rather than encoding from a moving source. The `ringSnapshot` A/B knob turns this off to
        // read the live source directly (matches pre-snapshot behavior) — a bench measurement lever, not a
        // shipping opt-out; it defaults ON.
        if (ringSnapshot) { if (!snapshotSourceForRing()) return; }
        else              encodeSrc_ = nullptr;   // OFF: encodeRows reads the live sourceBuffer_
        if (derived()->busTransmitRing()) {
            inFlight_[0] = true;   // kicked; DO NOT wait here — the next tick waits, freeing the core now
        }
    }

    /// Refresh the read-only `frameTime` KPI once a second (off the hot path): the last measured DMA
    /// wire time and the fps ceiling it implies (1e6 / frameTime). This is the pure WS2812 output floor —
    /// the render loop can never beat it, so it's the target the multicore work drives the system tick
    /// toward, and it tracks an overclocked slot rate directly. "—" until the first transfer completes.
    void tick1s() override {
        const uint32_t us = derived()->busLastTransmitUs();
        if (us == 0) std::snprintf(frameTimeStr_, sizeof(frameTimeStr_), "—");
        else std::snprintf(frameTimeStr_, sizeof(frameTimeStr_), "%u µs (%u fps max)",
                           static_cast<unsigned>(us), static_cast<unsigned>(1000000u / us));
        derived()->refreshBusKpi();   // per-backend extra read-only KPIs (MoonI80's ring diagnostic); base no-op
    }

    /// CRTP hook: refresh any backend-specific read-only KPI once a second (off the hot path). Base is a
    /// no-op; MoonLedDriver overrides it to publish its ring diagnostic counters (see ringDbg).
    void refreshBusKpi() {}

    // Wait for buffer `i`'s in-flight transfer to finish. Returns TRUE when the buffer is free to
    // reuse — either nothing was outstanding (first tick, or a transmit that failed to start, so an
    // unconditional wait can't block the full timeout) or the transfer completed. Returns FALSE when
    // the wait TIMED OUT: the DMA may still be reading, so `inFlight_` stays set and the caller must
    // not touch the buffer. Used by tickAsync (the deferred-wait double-buffer path) on the buffer it
    // is about to REUSE; the synchronous path (tickSync) waits inline and never uses this.
    bool busWaitIfBusy(uint8_t i) {
        if (!inFlight_[i]) return true;
        if (!derived()->busWait(i, waitBudgetMs())) {
            // The transfer did not complete within many times its own wire time, so it is not going
            // to. Count it: a bus that keeps doing this is broken, and the ONLY thing that matters
            // then is that the driver stops spending the render thread on it (see deadFrames_).
            if (deadFrames_ < kDeadFramesBeforeGiveUp) deadFrames_++;
            return false;   // still in flight — do not reuse the buffer
        }
        deadFrames_ = 0;   // a completed transfer clears the strike count: the bus is alive again
        inFlight_[i] = false;
        // If we'd already reported give-up ("output stalled"), a completed transfer means the bus
        // recovered — re-derive the real status so the UI stops showing a fault the device no longer has.
        // parseConfig() (not a bare clearStatus) is what RESTORES the normal "driving N of M lights" info;
        // clearStatus alone would wipe the error but leave the status blank forever (the info is only set
        // in parseConfig, which the retry path doesn't otherwise re-run).
        if (gaveUpReported_) { gaveUpReported_ = false; parseConfig(); }
        return true;
    }

    /// How long a transfer gets before we call it dead. **Derived from the frame, never a constant.**
    /// The wire time is `frameBytes / pclk` by construction, so a healthy transfer completes in a few
    /// ms; a fixed 1 s timeout (what this used) is 20× a whole tick budget, so a single undelivered
    /// frame would block the render thread for 20 ticks' worth of CPU — enough to starve WiFi off the
    /// air. That is how a merely *misconfigured* driver made a device unreachable (bench, 2026-07-14),
    /// which is a robustness bug in its own right: no LED setting may cost the user the device.
    ///
    /// **A broken bus must not cost the user the device.** Every stalled frame is a wait on the RENDER
    /// thread, so a bus that never delivers doesn't merely fail to light LEDs — it eats the CPU that
    /// WiFi, HTTP and the UI need, and the device drops off the network with no way back in. That is
    /// how a *misconfigured LED driver* (a frame the DMA can't sustain from PSRAM) made a bench board
    /// unreachable and unrecoverable-without-a-cable, 2026-07-14.
    ///
    /// So after kDeadFramesBeforeGiveUp consecutive dead transfers, stop transmitting: report the failure
    /// and let the tick return immediately. The LEDs go dark — but the device stays *reachable*, so the
    /// user can see the status and fix the setting that caused it. Degraded, not crashed; the
    /// *Robustness* rule ([architecture.md](../../../docs/architecture.md#robustness)) says a bad input
    /// may leave the output idle, never the device wedged.
    ///
    /// It self-heals: any completed transfer clears the strike count, and a config change re-inits the
    /// bus (prepare → reinit → deadFrames_ = 0), so fixing the setting brings the LEDs straight back with
    /// no reboot.
    ///
    /// **Give-up is not permanent — it periodically RETRIES.** A *misconfigured* bus never delivers and
    /// stays given-up (correct: stop spending the render thread). But a *transient* stall — the streaming
    /// ring's refill missing one deadline because a `/api/state` serialise stole the core for a few ms —
    /// is recoverable: the next frame would succeed. Latching give-up until a reinit turns a momentary
    /// hiccup into "LEDs dark until you touch a control or reboot," which is itself a robustness failure.
    /// So once given up, let ONE frame through every `kGiveUpRetryTicks` ticks: if it completes, the
    /// strike count clears and output resumes on its own; if it doesn't, we fall straight back to
    /// given-up, having spent one frame's wait per ~`kGiveUpRetryTicks` ticks — cheap enough not to
    /// starve the network, unlike retrying every tick.
    bool busGaveUp() {
        if (deadFrames_ < kDeadFramesBeforeGiveUp) return false;
        if (!gaveUpReported_) {
            gaveUpReported_ = true;
            setStatus("no LED output — the driver is not sending frames; check pins and LED count",
                      Severity::Error);
        }
        // Periodic retry window: every kGiveUpRetryTicks-th call, return false so the caller attempts one
        // transfer. A completed transfer clears deadFrames_ (busWaitIfBusy) and lifts the give-up; a
        // failed one re-increments and we stay given-up until the next window.
        if (++giveUpRetry_ >= kGiveUpRetryTicks) {
            giveUpRetry_ = 0;
            deadFrames_ = kDeadFramesBeforeGiveUp - 1;   // one strike below the threshold: let this frame try
            // ABANDON the wedged in-flight transfer so the retry starts CLEAN. After this many timeouts
            // the transfer is not going to complete; leaving inFlight_ set would make the tick's
            // busWaitIfBusy() wait on that dead transfer (and time out again) instead of arming a fresh
            // one. Clearing the flags lets the next tick encode+transmit a replacement — the transmit
            // re-arms the bus, which is what actually recovers it.
            inFlight_[0] = inFlight_[1] = false;
            // Do NOT clear the "output stalled" status here — the retry has not SUCCEEDED yet, it is only
            // about to try. Leave gaveUpReported_ set so the status stays honest during the attempt: if the
            // retry frame completes, busWaitIfBusy() re-derives the real "driving N of M" status (via
            // parseConfig); if it fails again, the error was correctly still showing the whole time. This
            // is the fix for the earlier bug where an eager clear wiped the status to blank forever (the
            // driving-info is only re-set on a successful transfer, not on the retry attempt itself).
            return false;
        }
        return true;
    }

    /// Computed from the WS2812 wire contract, which is the same on every backend and needs no
    /// platform constant: one light clocks `channels × 8` bits, each bit is 3 slots, and a slot is
    /// ~375 ns of WIRE time — the expander changes how many bus words fill a slot, not how long the
    /// strand takes. So `maxLaneLights_` (the longest strand) sets the frame's wire time directly.
    ///
    /// 4× that, floored at 20 ms (a tiny frame still tolerates scheduling jitter) and capped at
    /// 100 ms (even a huge frame cannot blow a tick budget). Generous enough never to kill a healthy
    /// transfer; small enough that a dead one costs a frame, not a second.
    uint32_t waitBudgetMs() const {
        constexpr uint32_t kSlotNs = 375;   // WS2812 wire slot; 3 per bit (ParallelSlots.h)
        const uint32_t bits = static_cast<uint32_t>(maxLaneLights_) * correction_.outChannels * 8u;
        const uint32_t wireMs = (bits * 3u * kSlotNs) / 1'000'000u;
        const uint32_t budget = wireMs * 4u;
        return budget < 20u ? 20u : (budget > 100u ? 100u : budget);
    }

    // Drain BOTH buffers' in-flight transfers before a reinit/release frees them — a live DMA reading
    // a buffer that's about to be freed is a use-after-free. Safe to call any time (busWaitIfBusy is a
    // no-op on an idle buffer). If a wait times out here we cannot simply skip: the buffers are about
    // to go away, so the peripheral itself is torn down (deinit stops the unit / deletes the bus,
    // which cancels any transfer) — the flag is cleared so the teardown proceeds rather than spinning.
    void drainInFlight() {
        if (!busWaitIfBusy(0)) inFlight_[0] = false;   // deinit below cancels the wedged transfer
        if (!busWaitIfBusy(1)) inFlight_[1] = false;
    }

    /// Prefill both DMA buffers' shift-mode constants (a no-op in direct mode, where every slot word
    /// already carries data or is a zero the buffer's memset provides). Called from reinit(), the cold
    /// path, whenever the buffers are (re)established or re-zeroed.
    void prefillShiftConstantsIfNeeded() {
        if (!pinExpanderMode() || !inited_) return;
        const uint8_t outCh = correction_.outChannels;
        if (outCh == 0 || maxLaneLights_ == 0) return;
        for (uint8_t i = 0; i < 2; i++) {
            uint8_t* buf = derived()->busBuffer(i);
            if (!buf) continue;   // buffer 1 is null in single-buffer mode
            if (slotBytes() == 1) prefillShiftFrame<uint8_t>(outCh, buf);
            else                  prefillShiftFrame<uint16_t>(outCh, buf);
        }
    }

    /// Write the shift-mode frame CONSTANTS into every DMA buffer, once, off the hot path.
    ///
    /// Of the three words a WS2812 slot emits per shift cycle, two are the same for every light: the
    /// pulse-start (which strands are live) and the pulse-tail (all-LOW). Only the middle word carries
    /// pixel data. Pre-filling the constants here means `encodeRows` writes **one word instead of
    /// three** — two thirds of the encoder's stores, gone from the per-frame path. (Measured on an S3:
    /// ~9.7 µs/light → ~3 µs.) hpwit does the same with `putdefaultones()` at buffer init; studied,
    /// then written fresh against our own layout.
    ///
    /// **Short strands make this row-dependent, not frame-uniform.** A strand that runs out mid-frame
    /// drops out of the active set at that row, and its pulse-start word must stop asserting from
    /// there on — otherwise the exhausted strand flashes white at full brightness (the bug this
    /// morning). So the frame is prefilled in RUNS: rows sharing an active mask are one call, and a
    /// new run starts wherever a strand ends. Equal-length strands — the common case — are a single run.
    template <class Slot>
    void prefillShiftFrame(uint8_t outCh, uint8_t* dst) {
        prefillShiftRows<Slot>(outCh, dst, 0, maxLaneLights_);
    }

    /// Prefill the shift constants for rows [firstRow, firstRow + rowCount) into `dst`, written
    /// dst-RELATIVE (row `firstRow` lands at dst+0) — so a ring buffer holding one slice gets its
    /// constants laid down exactly as the whole-frame buffer does for the same rows. `rowCount == 0`
    /// means "to the end". The whole-frame path calls this as [0, all); the ring calls it per slice
    /// (which is why the constants land on a recycled ring buffer that encodeRows alone would leave
    /// stale — encodeRows writes only the DATA word, relying on these constants being present).
    template <class Slot>
    void prefillShiftRows(uint8_t outCh, uint8_t* dst, nrOfLightsType firstRow, nrOfLightsType rowCount) {
        auto* out = reinterpret_cast<Slot*>(dst);
        const size_t slotsPerRow = static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
        const nrOfLightsType lastRow = (rowCount == 0 || firstRow + rowCount > maxLaneLights_)
                                           ? maxLaneLights_
                                           : static_cast<nrOfLightsType>(firstRow + rowCount);
        nrOfLightsType row = firstRow;
        while (row < lastRow) {
            // The active set for this row, and how many rows keep it (until the next strand ends).
            uint64_t mask = 0;
            nrOfLightsType runEnd = lastRow;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row < laneCounts_[lane]) {
                    mask |= uint64_t(1) << lane;
                    if (laneCounts_[lane] < runEnd) runEnd = laneCounts_[lane];   // this strand ends first
                }
            }
            if (runEnd <= row) runEnd = lastRow;   // no strand ends ahead in range: one run to lastRow
            const uint32_t rows = static_cast<uint32_t>(runEnd - row);
            // dst-relative: row `firstRow` is at out+0, so offset by (row - firstRow).
            prefillWs2812ShiftConstants<Slot>(mask, physPins_, latchBit_, outputsPerPin(), outCh, rows,
                                              out + static_cast<size_t>(row - firstRow) * slotsPerRow);
            row = runEnd;
        }
    }

    // Encode rows [firstRow, firstRow + rowCount) into `dst` as `Slot`-wide bus words (uint8_t for the
    // 8-bit bus, uint16_t for the 16-bit bus). Correct the same light index of every active lane into
    // the wire block, then transpose+emit its 3 slots. No heap, integer math only. `dst` is the raw-byte
    // DMA buffer this tick owns; a 16-bit frame views it as uint16 slots (sized ×2 by frameBytesFor).
    //
    // **A ROW RANGE, not always the whole frame** — the whole-frame call is just the range [0, all).
    // The streaming ring (MoonI80's phase-2 path) encodes one slice at a time straight into the small
    // internal buffer the DMA is about to read, so the big encoded frame is never materialised at all.
    // Slicing is why it can: the loop was already per-row, so a range costs nothing extra.
    //
    // `rowCount == 0` means "to the end". Only the LAST slice closes the frame with the latch pad, so a
    // partial slice must not emit it — hence `closeFrame`.
    template <class Slot>
    void encodeRows(uint8_t outCh, uint8_t* dst,
                    nrOfLightsType firstRow = 0, nrOfLightsType rowCount = 0,
                    bool closeFrame = true) {
        const nrOfLightsType lastRow = (rowCount == 0 || firstRow + rowCount > maxLaneLights_)
                                           ? maxLaneLights_
                                           : static_cast<nrOfLightsType>(firstRow + rowCount);
        // The streaming ring encodes OFF the render thread (its refill runs while the DMA drains and the
        // render loop is free to overwrite the source buffer), so it reads from an immutable per-frame
        // SNAPSHOT (encodeSrc_) instead of the live sourceBuffer_ — no use-after-free on a resize, no
        // frame tearing. encodeSrc_ is snapshotSourceForRing()'s windowed copy, bias-corrected by
        // -winStart_ so the index math below (winStart_ + laneStart_ + row) is unchanged either way. The
        // sync/async whole-frame paths encode inline on the render thread with no such hazard, so they
        // leave encodeSrc_ null and read the live buffer as before.
        const uint8_t* src = encodeSrc_ ? encodeSrc_ : sourceBuffer_->data();
        const uint8_t srcCh = sourceBuffer_->channelsPerLight();
        auto* out = reinterpret_cast<Slot*>(dst);
        // Per-lane wire slots, lane-major with stride `outCh` (wire_[lane * outCh + ch]). Sized to the
        // channel count off the hot path (prepareWire), so a light of any channel count (RGB / RGBW /
        // RGBCCT / an N-channel fixture) is laid out without overrun. Zeroed each frame so a
        // short-strand lane's slot (skipped below) contributes no set bit (the activeMask rule already
        // gates it, but this removes the read-of-uninitialised footgun).
        std::memset(wire_, 0, wireCap_);
        const size_t stride = outCh;
        const bool shift = pinExpanderMode();
        // The active-strand mask is 64-bit because a '595 expander drives more strands than the bus
        // is wide (up to kMaxStrands); in direct mode only the low `laneCount_` bits are ever set,
        // and it narrows to Slot for the direct encoder.
        for (nrOfLightsType row = firstRow; row < lastRow; row++) {
            uint64_t mask = 0;
            for (uint8_t lane = 0; lane < laneCount_; lane++) {
                if (row >= laneCounts_[lane]) continue;   // short strand: idle LOW
                mask |= uint64_t(1) << lane;
                // winStart_ shifts this driver's whole slice; laneStart_ is the
                // per-lane offset within it.
                correction_.apply(src + (winStart_ + laneStart_[lane] + row) * srcCh,
                                   wire_ + lane * stride);
            }
            if (shift) {
                // DATA WORDS ONLY. The pulse-start and pulse-tail words of every slot are frame
                // constants and were written once by prefillShiftFrame() — two thirds of the stores,
                // hoisted straight out of the hot path (see prefillWs2812ShiftConstants).
                encodeWs2812ShiftData<Slot>(wire_, mask, physPins_, latchBit_, outputsPerPin(), outCh, out);
                out += static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
            } else {
                encodeWs2812ParallelSlots<Slot>(wire_, static_cast<Slot>(mask), outCh, out);
                out += static_cast<size_t>(outCh) * 8 * 3;   // 3 slots × 8 bits × channels, in Slot elements
            }
        }
        // Close the frame: one latch-only word at the head of the (zeroed) latch pad, so the final
        // byte is actually presented and every strand idles LOW through the pad — the WS2812 reset.
        // Without it a strand whose last wire byte is ODD holds HIGH for the whole pad and never
        // resets (see encodeWs2812ShiftLatchPad). Direct mode needs nothing: a zeroed pad IS a LOW line.
        if (shift && closeFrame) encodeWs2812ShiftLatchPad<Slot>(latchBit_, out);
    }

    // Encode the loopback test frame at `Slot` width: the same pattern on lane 0 in every
    // row (activeMask = bit 0). Matches the private loopback bus's width so the DMA stride
    // is right. `frame` is raw bytes; viewed as Slot elements.
    template <class Slot>
    void encodeLoopbackFrame(uint8_t* frame, const uint8_t* wire, uint8_t outCh,
                             nrOfLightsType lights) {
        auto* out = reinterpret_cast<Slot*>(frame);
        for (nrOfLightsType row = 0; row < lights; row++) {
            encodeWs2812ParallelSlots<Slot>(wire, Slot(1), outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3;
        }
    }

    // The shift-register sibling: the same test pattern, but encoded through the '595 fan-out so the
    // frame the private bus transmits is byte-for-byte what the render loop sends in shift mode.
    // activeMask = bit 0 → STRAND 0, which is data pin 0's register at shift position 0. Because a
    // '595 shifts MSB-first, that strand appears on the register's LAST output (Q7) — that is the pin
    // the jumper must come from. The latch rides its own bus bit (latchBit_), as it does at runtime.
    template <class Slot>
    void encodeLoopbackFrameShift(uint8_t* frame, const uint8_t* wire, uint8_t outCh,
                                  nrOfLightsType lights) {
        auto* out = reinterpret_cast<Slot*>(frame);
        // The pattern rides `loopbackStrand` — whichever '595 output the jumper is actually on. It
        // need not be a strand the render loop drives: a spare output (e.g. the 16th of two '595s on
        // a 15-panel board) is the ideal tap, since nothing else loads it.
        const uint8_t s = (loopbackStrand < kMaxStrands) ? loopbackStrand : uint8_t{0};
        const uint64_t mask = uint64_t(1) << s;
        // `wire` holds ONE light's bytes at index 0; the shift encoder indexes it by strand, so
        // point strand `s`'s slot at those bytes.
        for (nrOfLightsType row = 0; row < lights; row++) {
            encodeWs2812ShiftSlots<Slot>(wire, mask, physPins_, latchBit_,
                                         outputsPerPin(), outCh, out);
            out += static_cast<size_t>(outCh) * 8 * 3 * outputsPerPin();
        }
        encodeWs2812ShiftLatchPad<Slot>(latchBit_, out);   // close the frame — see encodeRows
    }

    /// Test-only accessors — pin the lane slicing and frame-size arithmetic on the
    /// host (unit_{I80,Parlio}LedDriver.cpp); the hardware half is proven on device.
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
    uint8_t* dmaBuf_ = nullptr;          // platform-owned buffer 0; cached as the "inited" flag
                                         // for tick()'s guard (the per-tick encode target is
                                         // busBuffer(active_), which alternates 0/1)
    uint8_t active_ = 0;                 // which DMA buffer this tick encodes into (0/1); stays 0
                                         // in single-buffer mode (no second buffer allocated)
    bool inFlight_[2] = {};              // is buffer i's DMA transfer outstanding (awaiting its wait)
    // Consecutive frames whose DMA transfer never completed within its budget. NOT the `stallUs`
    // render-split KPI (which is a healthy core-0 wait) — this counts DEAD transfers, and is 0 on any
    // working bus. See busGaveUp(): a bus that keeps failing gets switched off rather than allowed to
    // spend the render thread and starve the network.
    uint8_t deadFrames_ = 0;
    bool gaveUpReported_ = false;        // one status write per give-up, not one per tick
    static constexpr uint8_t kDeadFramesBeforeGiveUp = 8;
    // Given-up retry cadence: once given up, let one frame try every this-many ticks so a TRANSIENT stall
    // self-recovers without a reinit (see busGaveUp). ~50 ticks ≈ 1 s of retries — rare enough not to
    // starve the network with dead-frame waits, frequent enough that recovery feels immediate.
    uint16_t giveUpRetry_ = 0;
    static constexpr uint16_t kGiveUpRetryTicks = 50;
    // Per-row correction scratch (wire_ / wireCap_ live on DriverBase — the grow-only lifecycle is
    // shared with RmtLedDriver; only the SIZE differs). Here it is kMaxLanes × outChannels bytes,
    // lane-major (wire_[lane*outCh+ch]) — sized to the channel count off the hot path, so a light of
    // any channel count fits (RGB=3, RGBW=4, RGBCCT=5, an N-channel fixture; limit is memory). A fixed
    // 4-byte-stride stack array here overflowed for >4-channel corrections → the SE16 bootloop.
    char frameTimeStr_[40] = "—";             // read-only `frameTime` KPI text (refreshed in tick1s); sized
                                         // for the worst case "<us> µs (<fps> fps max)" + the 3-byte
                                         // UTF-8 µ, so the snprintf never truncates (-Wformat-truncation)
    uint16_t laneList_[kMaxLanes] = {};              // physical data GPIOs (bus width bound)
    uint16_t busPinBuf_[kMaxLanes] = {};             // data pins + latch — the list busInit() builds from
    nrOfLightsType laneCounts_[kMaxStrands] = {};    // per-STRAND light count (expander bound)
    nrOfLightsType laneStart_[kMaxStrands] = {};     // per-STRAND offset into the window
    nrOfLightsType winStart_ = 0;   // first source-buffer light this driver reads (the window)
    nrOfLightsType winLen_ = 0;     // window length (lights), clamped to the buffer
    uint8_t laneCount_ = 0;      // STRAND lanes driven = physPins_ × outputsPerPin() (expanded)
    uint8_t physPins_ = 0;       // physical data GPIOs (== laneCount_ in direct mode)
    uint8_t latchBit_ = 0;       // bus-bit index of the latch line (shift mode only)
    nrOfLightsType maxLaneLights_ = 0;
    size_t frameBytes_ = 0;

    // Per-frame source SNAPSHOT for the streaming ring. The ring's refill encodes OFF the render thread,
    // concurrently with the render loop overwriting the source buffer (the Drivers output buffer, or the
    // layer buffer in the zero-copy identity case), so it must read an immutable copy. Only THIS DRIVER'S
    // WINDOW is copied — `winLen_ × srcCh` bytes from `winStart_`, not the whole source — so on a large
    // display split across many drivers each copies only the slice it reads, not the entire frame. (The
    // source is the raw pixel array: 3 B/light, so 48 KB at 16K lights — the window keeps a per-driver
    // copy proportional to that driver's strands, ~11.5 KB for a 16×256 driver.) snapshotSourceForRing()
    // fills it at transmit time and points encodeSrc_ at it (biased by -winStart_ so encodeRows' index is
    // unchanged); encodeRows reads encodeSrc_ when set. Grow-only, freed in release() with the scratch.
    uint8_t* snapshotBuf_ = nullptr;      // driver-owned copy of the source WINDOW (ring only)
    size_t   snapshotCap_ = 0;            // allocated capacity, grows to fit the window
    const uint8_t* encodeSrc_ = nullptr;  // when non-null, encodeRows reads this (bias-corrected) instead of sourceBuffer_

    /// (Re)size the ring snapshot to hold this driver's whole window (`winLen_ × srcCh`), OFF the hot path.
    /// Called from the ring build in reinit(), so snapshotSourceForRing() on the render thread only memcpys
    /// — never allocates (the hot-path no-alloc rule). Grow-only; a larger window later re-grows here, not
    /// mid-frame. Returns false if it can't allocate (the ring build then degrades like any alloc failure).
    bool ensureSnapshotCap() {
        if (!sourceBuffer_) return true;   // sized on the first build that has a source; harmless if absent
        const size_t bytes = static_cast<size_t>(winLen_) * sourceBuffer_->channelsPerLight();
        if (bytes == 0 || snapshotCap_ >= bytes) return true;
        uint8_t* grown = static_cast<uint8_t*>(platform::alloc(bytes));
        if (!grown) return false;
        if (snapshotBuf_) platform::free(snapshotBuf_);
        snapshotBuf_ = grown;
        snapshotCap_ = bytes;
        return true;
    }

    /// Copy THIS DRIVER'S WINDOW of the source into the driver-owned snapshot and route the ring's encode
    /// at it, so the refill (off the render thread) reads a frozen frame. NO ALLOCATION here — the buffer
    /// was sized in reinit() (ensureSnapshotCap); this is memcpy-only, safe for the render hot path.
    /// Returns false (encodeSrc_ null, caller bails) if the source is missing, the window is empty, or the
    /// snapshot wasn't sized (a reconfigure that shrank the buffer below the window — clamped defensively).
    /// Sets encodeSrc_ = snapshotBuf_ - winStart_ * srcCh, so encodeRows' index (winStart_ + laneStart_ +
    /// row) lands in the windowed copy WITHOUT changing the formula. The bias pointer is only ever
    /// dereferenced at indices ≥ winStart_ * srcCh (never below the real buffer), so it never reads OOB.
    bool snapshotSourceForRing() {
        encodeSrc_ = nullptr;
        if (!sourceBuffer_ || !sourceBuffer_->data() || !snapshotBuf_) return false;
        const size_t srcCh = sourceBuffer_->channelsPerLight();
        // Clamp the copy to what the live buffer AND the sized snapshot both hold: winLen_/winStart_ come
        // from config and the source can have shrunk since reinit (a grid resize past the render thread is
        // exactly the hazard this snapshot guards), so never read past the source nor write past snapshotCap_.
        const nrOfLightsType count = sourceBuffer_->count();
        if (winStart_ >= count) return false;
        nrOfLightsType winLights = (winStart_ + winLen_ > count)
                                       ? static_cast<nrOfLightsType>(count - winStart_)
                                       : winLen_;
        size_t bytes = static_cast<size_t>(winLights) * srcCh;
        if (bytes > snapshotCap_) bytes = snapshotCap_;   // never overrun the buffer sized in reinit
        if (bytes == 0) return false;
        std::memcpy(snapshotBuf_, sourceBuffer_->data() + static_cast<size_t>(winStart_) * srcCh, bytes);
        // Bias so the unchanged index (winStart_ + laneStart_ + row) * srcCh addresses into the windowed
        // copy: snapshotBuf_[0] holds the light at winStart_, so subtract winStart_ * srcCh.
        encodeSrc_ = snapshotBuf_ - static_cast<size_t>(winStart_) * srcCh;
        return true;
    }

    /// The two wirings that exist: strands on the GPIOs, or a 74HCT595 per GPIO.
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
    /// (the driver keeps running — see MultiPinLedDriver::validateBusPins for why it's a
    /// warning, not a blocker) or null when the data pins are clean. Parlio has no
    /// such pins, so it uses this default.
    const char* validateBusPins(const uint16_t* /*lanes*/, uint8_t /*n*/) const { return nullptr; }

    /// FATAL bus-pin check → the ERROR path (idles the driver), for a bus-pin misconfig the peripheral
    /// can't init at all (MultiPinLedDriver's clockPin==dcPin). Distinct from validateBusPins' per-lane
    /// warnings. Default null; a peripheral with bus control pins overrides it. Parlio has none.
    const char* validateBusFatal() const { return nullptr; }

    /// CRTP ring hooks (default: this peripheral has no streaming ring, so it never rings). Only MoonI80
    /// — which owns its DMA — overrides these to stream a frame too big for internal RAM (see its
    /// busInitRing / the tickRing path). The esp_lcd I80 (the memory-capped reference) and Parlio use
    /// these defaults: busIsRing() is always false, so tick() never selects tickRing and the ring transmit
    /// is never called. reinit() consults wantsRing() to decide whether to attempt a ring build at all.
    bool wantsRing() const { return false; }                        // should reinit try the ring for this config?
    bool busInitRing(size_t /*rowBytes*/, uint32_t /*totalRows*/, size_t /*padBytes*/) { return false; }
    bool busIsRing() const { return false; }
    bool busTransmitRing() { return false; }

    /// CRTP hook: the GPIO a SPARE bus lane is parked on when the pin list is narrower than the bus
    /// width (shift mode — the board decides the data-pin count, the peripheral decides the width).
    /// The i80 driver hides this to return its WR pin, which the peripheral already drives; a
    /// peripheral that accepts NC lanes (Parlio) never calls it. Default: lane 0's pin, so a spare
    /// lane is at worst a harmless duplicate of a pin the bus already owns.
    uint16_t clockPinForBus() const { return laneList_[0]; }

    // Frame bytes: longest lane × channels × 24 slots, plus a zeroed latch pad of
    // >=300 µs at the slot rate (800 slots) with clock-tolerance slack (64), each
    // scaled by `slotBytes` (1 for an 8-bit bus, 2 for a 16-bit bus — a slot is one
    // bus WORD, so a 16-lane frame is 2 bytes/slot), rounded up to the bus's 64-byte
    // alignment. 0 when there's nothing to send. The pad scales too: it's a count of
    // idle bus words, and its LOW duration is measured in slots, so an unscaled pad
    // would be half the latch time on a 16-bit bus and could latch a strand mid-frame.
    //
    // `outPerPin` (1, or kPinExpanderOutputs with a '595 expander) multiplies the ROW slots: a
    // shift register is serial-in, so presenting each WS2812 slot costs outPerPin shift
    // cycles, each its own bus word. The latch pad is NOT multiplied — it is a LOW-time
    // measured in bus words, and the bus already clocks outPerPin× faster in shift mode,
    // so an unscaled pad holds the same wall-clock idle... which would be outPerPin× too
    // SHORT. Scale it by outPerPin to keep the ≥300 µs strand-latch window intact.
    // Bytes ONE light-row encodes to: outCh channels × 24 slots (3 per bit × 8 bits) × slotBytes ×
    // outPerPin shift words. The single source of the row geometry — frameBytesFor and the streaming
    // ring both derive from it, so the ring's per-slice size can never drift from the whole-frame size.
    static size_t rowBytesFor(uint8_t outCh, uint8_t slotBytes, uint8_t outPerPin) {
        return static_cast<size_t>(outCh) * 24 * slotBytes * outPerPin;
    }
    // The trailing latch pad: 800 idle bus words (≥300 µs at the slot rate) + 64 clock-tolerance slack,
    // scaled by slotBytes and outPerPin (see the note above — the shift bus clocks faster, so the pad
    // must scale to hold the same wall-clock LOW). One frame carries exactly one pad, at its end.
    static size_t padBytesFor(uint8_t slotBytes, uint8_t outPerPin) {
        return static_cast<size_t>(800 + 64) * slotBytes * outPerPin;
    }

    static size_t frameBytesFor(nrOfLightsType maxLights, uint8_t outCh, uint8_t slotBytes,
                                uint8_t outPerPin = 1) {
        if (maxLights == 0 || outCh == 0 || outPerPin == 0) return 0;
        const size_t bytes = static_cast<size_t>(maxLights) * rowBytesFor(outCh, slotBytes, outPerPin)
                             + padBytesFor(slotBytes, outPerPin);
        return (bytes + 63) & ~static_cast<size_t>(63);
    }

    // Bytes per bus slot: 1 for the 8-bit bus, 2 for the 16-bit bus. Keys on the PHYSICAL
    // pin count, not laneCount_ — with a '595 expander the lanes ride the shift cycles, not
    // extra bus bits, so 48 lanes on 6 pins is still an 8-bit bus. (In direct mode the two
    // counts are equal, so this is the original behaviour.)
    uint8_t slotBytes() const { return busWidthPins() > 8 ? 2 : 1; }

    // The i80 bus width is a POWER OF TWO (8 or 16) — a peripheral fact, independent of how many
    // strands the board drives. In direct mode the pin list already fills it exactly. In shift mode
    // the board decides the data-pin count (how many '595s are populated), so the driver rounds up to
    // the next legal width and pads the spare lanes itself.
    uint8_t busWidthPins() const {
        // Data pins, plus the latch lane when a '595 expander is in use.
        const uint8_t needed = static_cast<uint8_t>(physPins_ + (pinExpanderMode() ? 1 : 0));
        return needed <= 8 ? uint8_t{8} : uint8_t{16};
    }

    // The GPIO list the PERIPHERAL is built from — always busWidthPins() entries long, because the bus
    // is 8 or 16 bits wide however many pins the board actually drives:
    //   - bus bits 0..physPins_-1 are the data pins (bit L = the L-th entry of `pins`, the
    //     ParallelSlots contract),
    //   - in shift mode, bus bit latchBit_ (== physPins_) is the LATCH — a real lane the peripheral
    //     drives,
    //   - every REMAINING lane up to the bus width is parked on the WR/clock pin. The i80 layer
    //     rejects an NC data pin, so a spare lane must be *some* real GPIO; WR is the safe choice
    //     because the peripheral already drives it and the board already wires it (hpwit's trick,
    //     and exactly what platform_esp32_i80 does for lanes beyond laneCount). Those lanes carry no
    //     strand — their bits are never set in activeMask — so they are inert.
    // The padding is what lets a board name ONLY the pins it drives, at any count: an 8-bit bus with 5
    // strands is 5 data lanes and 3 parked on WR. Returning the raw laneList_ in direct mode would hand
    // the peripheral a short array while busPinCount() claims the full width — a read past the end.
    // Kept in the base (one owner of the latch + the padding), so both derived busInit()s just pass
    // these two calls.
    const uint16_t* busPinList() {
        const uint8_t width = busWidthPins();
        for (uint8_t i = 0; i < width && i < kMaxLanes; i++) {
            if (i < physPins_)                        busPinBuf_[i] = laneList_[i];   // data
            else if (pinExpanderMode() && i == latchBit_)   busPinBuf_[i] = static_cast<uint16_t>(latchPin);
            else                                      busPinBuf_[i] = derived()->clockPinForBus();
        }
        return busPinBuf_;
    }
    uint8_t busPinCount() const { return busWidthPins(); }
    // Bus clock: a '595 must be fed kPinExpanderOutputs shift cycles per WS2812 slot, so the bus clocks
    // that much faster to hold the same 375 ns slot on the wire. The platform picks the exact rate
    // its clock tree can divide to (see platform_esp32_i80.cpp); this is the multiplier.
    uint8_t busClockMultiplier() const { return outputsPerPin(); }

    // (Re)size the per-row correction scratch to kMaxLanes × outCh bytes. Grows-only, off the hot
    // path (called from parseConfig). Sizing to outCh — not a fixed 4 — is what lets encodeRows lay
    // out any channel count without overrun. Failure leaves wire_ null; tick() then idles.
    void prepareWire(uint8_t outCh) {
        // Sized for STRANDS, not bus lanes: with a '595 expander the wire block is indexed by
        // expanded lane (up to kMaxStrands), which is what encodeRows fills and the shift encoder
        // reads. Grows-only, so a direct-mode driver that later enables the expander just grows.
        ensureWire(static_cast<size_t>(kMaxStrands) * (outCh ? outCh : 1));   // DriverBase owns the lifecycle
    }

    // Re-derive lanes/counts/starts/frame size from the controls and the wired
    // buffer/correction. Off the hot path; on error the driver idles with the
    // parse literal in the status slot (same shape as RmtLedDriver).
    bool parseConfig() {
        laneCount_ = 0;
        physPins_ = 0;
        maxLaneLights_ = 0;
        frameBytes_ = 0;
        uint8_t n = 0;
        const char* err = parsePinList(pins, laneList_, maxLanesForTarget(), n);
        // (Nothing to validate about the fan-out itself: pinExpander is a bool, so the only two
        // wirings that physically exist are the only two it can express.)
        // The shift-register expander needs a backend that can DMA the 8× frame from PSRAM:
        // the LCD_CAM i80 path (S3 / P4). Refuse it elsewhere rather than emit a waveform the hardware
        // can't sustain — classic-ESP32 i80 is internal-DMA-only (it walls ~76 KB; its route in is the
        // PSRAM refill ring), and Parlio caps a single transfer at 65,535 B.
        if (!err && pinExpanderMode() && !Derived::kSupportsPinExpander)
            err = "the 74HCT595 expander needs the LCD_CAM i80 bus (ESP32-S3 / -P4)";
        // The latch is a real GPIO and a real bus bit; without it the '595s never present a byte.
        if (!err && pinExpanderMode() && latchPin < 0) err = "the 74HCT595 expander needs a latchPin";
        // **The BUS width is a peripheral fact; the PIN COUNT is a board fact. They are not the same
        // number, and the driver — not the user — reconciles them.** The i80 bus is 8 or 16 bits wide
        // (lcd_ll_set_data_wire_width takes nothing else), but nothing says every bit must reach a
        // GPIO: the matrix routes each data signal wherever we point it, and busPinList() parks the
        // spare lanes on WR, where the peripheral already drives and nothing reads them. So the user
        // configures only the pins that drive something, at ANY count, and the driver rounds the bus
        // up around them.
        //
        // In shift mode the LATCH also occupies a bus bit, so it costs one of the width's lanes —
        // hence kMaxLanes - 1 data pins there against kMaxLanes here.
        if constexpr (Derived::kPowerOfTwoBus) {
            const uint8_t maxData = static_cast<uint8_t>(kMaxLanes - (pinExpanderMode() ? 1 : 0));
            if (!err && (n == 0 || n > maxData))
                err = pinExpanderMode() ? "shift mode needs 1..15 data pins (one per populated 74HCT595)"
                                  : "i80 bus needs 1..16 pins";
        }
        // The latch drives its own bus bit, so it must not double as a data pin (that lane would
        // carry the latch waveform instead of pixel data). clockPin/dcPin overlap is the derived
        // driver's validateBusPins/validateBusFatal job, and the latch is checked against them there.
        if (!err && pinExpanderMode()) {
            for (uint8_t i = 0; i < n; i++)
                if (laneList_[i] == static_cast<uint16_t>(latchPin)) {
                    err = "latchPin collides with a data pin";
                    break;
                }
        }
        // Fatal bus-pin misconfig (MultiPinLedDriver's clockPin==dcPin — the i80 bus can't init) → the
        // error path below, which idles the driver. Checked before the per-lane WARNINGS: a broken
        // bus is worse than a garbled lane, so it wins the status.
        if (!err) err = derived()->validateBusFatal();
        // Peripheral-specific data-pin check (i80's WR/DC on a data lane routes two
        // output signals to one pin, so that lane emits the clock/DC waveform, not
        // pixel data). This is a WARNING, not a blocker: on a board that wires all
        // 8/16 lanes but drives fewer strands, an unused data pin is a legitimate
        // WR/DC candidate — only the lanes that actually drive a strand would show
        // garbage, and the user opted into that. Parlio has no WR/DC and returns null.
        // Kept a CRTP hook so the base stays peripheral-neutral.
        const char* warn = err ? nullptr : derived()->validateBusPins(laneList_, n);
        // STRAND lanes: each physical pin fans out to outputsPerPin() strands through its '595.
        // This is the count ledsPerPin distributes over and the encoder indexes — from here on
        // "lane" means a strand, and physPins_ holds the GPIO count.
        const uint8_t lanes = static_cast<uint8_t>(n * outputsPerPin());
        if (!err && lanes > kMaxStrands) err = "too many strands (pins × 8 through the expander)";
        if (!err) {
            // Distribute over this driver's window slice, not the whole buffer.
            // assignCounts clamps each lane to kMaxWs2812LedsPerPin (drives that many
            // rather than choking a whole grid onto one WS2812 line). Its own warning
            // (a clamped lane) wins over the WR/DC one only if it sets warn non-null.
            const nrOfLightsType bufN = sourceBuffer_ ? sourceBuffer_->count() : 0;
            windowSlice(bufN, winStart_, winLen_);
            const char* clampWarn = nullptr;
            err = assignCounts(ledsPerPin, lanes, winLen_, laneCounts_, kMaxWs2812LedsPerPin,
                               &clampWarn);
            if (clampWarn) warn = clampWarn;
        }
        if (err) {
            setConfigErr(err);
            return false;
        }
        physPins_ = n;
        laneCount_ = lanes;
        // The latch takes the bus bit just above the data pins (data pins are bus bits 0..n-1,
        // matching the ParallelSlots contract that bus bit L = the L-th entry of `pins`).
        latchBit_ = n;
        nrOfLightsType start = 0;
        for (uint8_t i = 0; i < laneCount_; i++) {
            laneStart_[i] = start;
            start = static_cast<nrOfLightsType>(start + laneCounts_[i]);
            if (laneCounts_[i] > maxLaneLights_) maxLaneLights_ = laneCounts_[i];
        }
        const uint8_t outCh = correction_.outChannels;
        // physPins_ and pinExpander are set above, so slotBytes() reflects the real BUS width (data pins
        // + the latch bit), not the strand count — 48 lanes on 6 pins is still an 8-bit bus. The
        // ×8 lands in the slot COUNT instead (outputsPerPin()), which is what grows the frame.
        frameBytes_ = frameBytesFor(maxLaneLights_, outCh, slotBytes(), outputsPerPin());
        // Size the per-row correction scratch to kMaxLanes × outCh (grows-only, off the hot path).
        // Stride outCh, so any channel count fits without the old fixed-4-byte overflow.
        prepareWire(outCh);
        clearConfigErr();
        // A lane clamped to the WS2812 ceiling still drives — Warning, not error (see RmtLed).
        setConfigWarn(warn);
        // With nothing more urgent to show AND lights actually driven, report how many
        // this driver consumes (Σ laneCounts_ = `start`) of its window — so the user sees
        // real consumption instead of guessing from grid × pins. A clamp warning wins; an
        // idle driver (no pins / empty buffer) stays statusless, not "driving 0 of 0".
        if (!warn && start > 0) setDrivingInfo(start, winLen_, outCh);
        return true;
    }

    // --- bus + DMA buffer (hardware; this peripheral's targets only). Fused on
    // purpose: max_transfer_bytes is fixed at bus creation, so growing the frame
    // means re-creating the bus and its buffer together. Grow-only; the buffer is
    // re-zeroed on every reinit so a shrunken frame's latch pad can't hold stale
    // row bytes. ---

    void reinit() {
        if constexpr (Derived::lanesAvailable() == 0) return;
        // A rebuild is the user fixing the setting that broke the bus: give the new bus a clean slate
        // so a driver that gave up starts transmitting again (the live-reconfiguration rule — no reboot).
        deadFrames_ = 0;
        gaveUpReported_ = false;
        giveUpRetry_ = 0;
        // Drain any in-flight DMA before touching the buffers: a rebuild (or the exact-match
        // re-zero below) must not race a transfer still reading the old buffer. No-op when idle.
        drainInFlight();
        if (laneCount_ == 0 || frameBytes_ == 0) { deinit(); return; }
        // Reuse the existing bus ONLY when the frame is the EXACT same size and on the same pins/lanes.
        // Grow OR shrink both rebuild: a peripheral whose single-transfer size is fixed at bus creation
        // (Parlio: max_transfer_size sets a hardware bit-length cap) becomes INVALID if the buffer is
        // reused at a different size — a shrunk frame kept an oversized unit whose configured max
        // exceeded the hardware frame limit, so every transmit silently failed (tx=0, no output, no
        // error). Exact-match reuse (not `>=`) keeps the bus always valid-or-rebuilt. The pin check
        // matters (a pin edit keeps the size but must move the GPIOs); the lane-count check matters for
        // Parlio (8→4 keeps frameBytes but the bus was built for 8 lanes).
        // Also rebuild when the second-buffer PRESENCE no longer matches doubleBuffer: toggling the
        // flag adds (ON) or frees (OFF) buffer 1, which only busInit/deinit can do. `haveSecond`
        // reflects what's currently allocated; `wantSecond` what the flag now asks for.
        // RING PATH (MoonI80, oversize shift mode): the whole-frame reuse/alloc logic below does not
        // apply — a ring holds no whole frame, so busCapacity() is one small slice and the exact-match
        // check would never fire anyway. Build the ring fresh; on failure fall through to the whole-frame
        // path, which then idles with a status if the frame won't fit either (the always-available
        // degrade). The ring owns its own buffers + refill task, so a plain deinit()+rebuild is correct;
        // ring-reuse is a later optimization (a config change already forces a rebuild).
        if (derived()->wantsRing()) {
            deinit();
            const uint8_t outCh = correction_.outChannels;
            const size_t rowBytes = rowBytesFor(outCh, slotBytes(), outputsPerPin());
            const size_t padBytes = padBytesFor(slotBytes(), outputsPerPin());
            if (derived()->busInitRing(rowBytes, static_cast<uint32_t>(maxLaneLights_), padBytes)) {
                ensureSnapshotCap();   // size the per-frame snapshot here, OFF the hot path (tickRing only memcpys)
                inited_ = true;
                dmaBuf_ = derived()->busBuffer(0);   // ring[0] — a real pointer, the "inited" sentinel
                for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
                busLaneCount_ = laneCount_;
                derived()->recordBusPins();
                if (status() == Derived::kInitFailMsg) clearStatus();
                return;
            }
            // Ring build failed (even the small buffers didn't fit) — fall through to whole-frame.
        }

        const bool haveSecond = derived()->busBuffer(1) != nullptr;
        const bool wantSecond = doubleBuffer;
        if (inited_ && !derived()->busIsRing() && derived()->busCapacity() == frameBytes_
            && busPinsCurrent() && busLaneCount_ == laneCount_ && haveSecond == wantSecond) {
            // Clear stale latch-pad bytes in BOTH buffers (buffer 1 is null in single-buffer mode).
            std::memset(dmaBuf_, 0, derived()->busCapacity());
            if (uint8_t* b1 = derived()->busBuffer(1)) std::memset(b1, 0, derived()->busCapacity());
            prefillShiftConstantsIfNeeded();   // the zeroing above wiped them
            return;
        }
        deinit();
        // Pass doubleBuffer so busInit allocates the second buffer only when the double-buffer is
        // wanted — OFF (default) costs exactly one DMA buffer, no async overhead, no second alloc.
        inited_ = derived()->busInit(frameBytes_, doubleBuffer);
        dmaBuf_ = inited_ ? derived()->busBuffer(0) : nullptr;
        if (inited_) {
            for (uint8_t i = 0; i < kMaxLanes; i++) busPins_[i] = laneList_[i];
            busLaneCount_ = laneCount_;
            derived()->recordBusPins();   // i80 also stores WR/DC; Parlio no-op
            prefillShiftConstantsIfNeeded();
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
        // Free is a use-after-free if a transfer is still reading the buffer — drain first.
        // (reinit already drains before calling here; release()/onCorrectionChanged reach
        // deinit directly, so the guard belongs here too. Idempotent no-op when idle.)
        drainInFlight();
        if (inited_) derived()->busDeinit();
        inited_ = false;
        dmaBuf_ = nullptr;
        active_ = 0;              // next init starts on buffer 0
        inFlight_[0] = inFlight_[1] = false;
        busLaneCount_ = 0;
        // NOTE: wire_ is NOT freed here — deinit() runs inside reinit()'s rebuild path (before busInit),
        // and parseConfig (which sized wire_) already ran, so freeing it would strand the encode. It is
        // freed in release() (the true teardown), and grows-only across reinits.
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
        // The expander multiplies the SLOT COUNT (a '595 is serial-in: each WS2812 slot is shifted
        // out over 8 bus words), exactly as frameBytesFor does for the operational frame. Omitting it
        // here would build a frame 8× too small and transmit a truncated waveform.
        const uint8_t opp = outputsPerPin();
        const size_t perLightBytes = static_cast<size_t>(outCh) * 8 * 3 * sb * opp;
        // Shift mode closes the frame with one trailing latch word (encodeWs2812ShiftLatchPad, written
        // unconditionally after the last row), so the buffer must hold one Slot beyond the rows or that
        // write runs off the end of the heap block. Direct mode writes no pad, so it stays row-exact.
        const size_t testFrameBytes = static_cast<size_t>(lights) * perLightBytes
                                      + (pinExpanderMode() ? sb : 0);
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
        // One light's wire bytes, indexed BY STRAND (`wire[strand * outCh + ch]`, the encoder's
        // layout). Direct mode drives lane 0, so byte 0 is enough — but shift mode can put the
        // pattern on ANY strand (loopbackStrand, so the jumper can come off a spare '595 output), and
        // the encoder would then read at `strand * outCh`. Size for the full strand range or that is
        // an out-of-bounds read. Zeroed, so every strand but the chosen one is black.
        const uint8_t patStrand = pinExpanderMode() && loopbackStrand < kMaxStrands ? loopbackStrand
                                                                              : uint8_t{0};
        const size_t wireBytes = static_cast<size_t>(patStrand + 1) * outCh;
        auto* wire = static_cast<uint8_t*>(platform::alloc(wireBytes));
        if (!wire) { platform::free(frame); clearFailBuf(); setStatus("loopback: out of memory", Severity::Error); return; }
        std::memset(wire, 0, wireBytes);
        uint8_t* pat = wire + static_cast<size_t>(patStrand) * outCh;   // the chosen strand's slot
        pat[0] = 0xA5; if (outCh > 1) pat[1] = 0x00; if (outCh > 2) pat[2] = 0xFF;   // R/G/B pattern
        // Encode the pattern on lane 0 at the operational width. A wider bus adds only
        // idle high lanes (activeMask = bit 0); the private bus + loopbackTxPin override
        // carry lane 0's signal to the jumpered RX pin. Sized-per-slot so the 16-bit
        // buffer stride matches the 16-bit private bus.
        //
        // In SHIFT mode the pattern lands on strand 0 — which is data pin 0's '595, shift position 0,
        // i.e. output **Q7** of that register (a '595 shifts MSB-first: the first bit clocked in ends
        // up on the last output). That is the pin to jumper back; see the wiring note on the
        // loopbackRxPin control. Everything else is identical, so the same rig verifies the whole
        // chain through the shift register.
        if (pinExpanderMode()) {
            if (sb == 1) encodeLoopbackFrameShift<uint8_t>(frame, wire, outCh, lights);
            else         encodeLoopbackFrameShift<uint16_t>(frame, wire, outCh, lights);
        } else if (sb == 1) {
            encodeLoopbackFrame<uint8_t>(frame, wire, outCh, lights);
        } else {
            encodeLoopbackFrame<uint16_t>(frame, wire, outCh, lights);
        }
        platform::free(wire);
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
        // The TX override is a DIRECT-mode affordance only. In shift mode the captured signal comes
        // off a '595 output, not off a GPIO the driver can re-point — the strand is determined by
        // which register output the jumper is on, not by which pin transmits. Substituting a pin here
        // would just build the bus on the wrong GPIO, so ignore the override (busPinList() copies
        // laneList_, and the '595s must stay wired to the real data pins for the test to mean
        // anything).
        const uint16_t realLane0 = laneList_[0];
        if (loopbackTxPin >= 0 && !pinExpanderMode()) laneList_[0] = static_cast<uint16_t>(loopbackTxPin);
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
            if (r.bitsChecked == 0) {
                // The capture came up short — the verify never ran, so "bad bit" would blame the
                // waveform for what is a transport/wiring/capture fault. Report the numbers that
                // separate those: symbols captured, the RX line's idle level, and the first
                // transmit's wall-vs-expected time (a wall time far above expected is a stalled or
                // underrun transfer, measured — not inferred).
                std::snprintf(failBuf_, kFailBufLen,
                              "no capture: %u sym idle=%d tx %u/%uus",
                              static_cast<unsigned>(r.capturedSymbols),
                              static_cast<int>(r.rxIdleLevel),
                              static_cast<unsigned>(r.txWallUs),
                              static_cast<unsigned>(r.txExpectUs));
            } else {
                // Name the first corrupted light: the loopback reports the first
                // mismatching bit; rowBits = outCh*8, so light = firstBadBit / rowBits.
                const unsigned rowBits = static_cast<unsigned>(outCh) * 8u;
                const unsigned badLight = rowBits ? r.firstBadBit / rowBits : 0u;
                std::snprintf(failBuf_, kFailBufLen,
                              "loopback FAIL: bad bit %u/%u (light %u)",
                              static_cast<unsigned>(r.firstBadBit),
                              static_cast<unsigned>(r.bitsChecked), badLight);
            }
            setStatus(failBuf_, Severity::Error);
        } else {
            setStatus("loopback FAIL", Severity::Error);
        }
        reinit();
    }
};

} // namespace mm
