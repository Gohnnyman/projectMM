#pragma once

#include "core/MoonModule.h"
#include "core/AudioFrame.h"
#include "core/AudioLevel.h"
#include "core/AudioBands.h"
#include "core/math8.h"      // beatsin8 / sin8 — the simulated-audio oscillators
#include "light/WLEDAudioSyncPacket.h"   // WLED audio-sync wire format (send/receive)
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>   // snprintf for the read-out strings
#include <cstring>

namespace mm {

/// Acquires an audio source and publishes an `AudioFrame` — an overall sound
/// **level**, a 16-band frequency **spectrum**, and the **dominant peak** — as the
/// producer half of the audio-reactive pipeline
///
/// The frame is available to consumers every render tick, but its analysed values
/// are *recomputed* only when a full sample block has accumulated (a 512-sample
/// block at 22 kHz takes ~23 ms, longer than one tick), so a tick that doesn't
/// complete a block re-publishes the previous `AudioFrame` unchanged rather than
/// re-analysing. `AudioVolumeEffect` and `AudioSpectrumEffect` are the consumers,
/// reaching the live frame through the static `latestFrame()`.
///
/// **Named for what it does** — audio acquisition plus analysis, not for one
/// source. Today the source is a digital I2S MEMS microphone (INMP441-class, the
/// only one wired); the same source-independent analysis pipeline is built to serve
/// other sources (line-in, USB audio, PDM mics, I2C codecs) behind the platform
/// read seam as they are added. Most of the module is the analysis (DC-blocker,
/// RMS level, windowed FFT, band mapping), which is source-independent.
///
/// **User-added Service.** A child of the `Services` container, registered in the
/// factory and added through the UI when wanted, not boot-wired — auto-wiring it
/// forced an I2S init on every board, which on the classic ESP32 hung `setup()` and
/// boot-looped a mic-less device. When added, its pins default to unset (−1, the
/// standard Pin-control sentinel, so GPIO 0 stays a usable mic pin) and it stays
/// idle with a status note until the user enters the real GPIOs. Chip-agnostic:
/// gated on `platform::hasI2sMic`, inert with a status note on targets without I2S
/// and on desktop.
///
/// **The AudioFrame pipeline.** Each `loop()` that completes a block: read a block
/// of samples, DC-blocker high-pass, compute the level, window + FFT, map to bands.
/// The high-pass conditions the raw block once, up front, so both the level and the
/// spectrum see the same cleaned signal. The DSP choices are textbook defaults on
/// purpose — a Hann window, RMS for level, a geometric band split, argmax for the
/// peak — with deliberately no per-frequency correction table (the INMP441 is flat
/// ±3 dB across the range that matters). The level is overall RMS loudness computed
/// independently of the FFT, not derived from the bands.
///
/// **Hardware: INMP441-class digital mic.** A self-clocked I2S MEMS microphone:
/// standard/Philips framing, 24-bit data left-justified in a 32-bit slot, mono. The
/// part is self-clocked from the bit clock; there is no master-clock (MCLK) pin.
/// The bench wiring is SCK=6 (bit clock), WS=4 (word-select/LRCLK), SD=5 (serial data
/// out). It drives the one slot its L/R select pin chooses (tie L/R to GND for the
/// left slot); if `level` stays at the floor with sound present, the mic is filling
/// the other slot — one wire, not firmware.
///
/// **Platform seams.** Only the I2S read and the FFT kernel are platform code
/// (`platform_esp32_i2s.cpp`: IDF's `i2s_std` driver + esp-dsp's float
/// `dsps_fft2r_fc32`, the radix-2 real FFT); everything else is plain domain math
/// that runs in CI on the desktop's reference DFT. The signal math is host-tested
/// domain code (`AudioLevel.h`, `AudioBands.h`); this module owns the lifecycle,
/// the controls, and the two seams.
///
/// **Hot path:** fixed member scratch buffers (sample block + window + magnitudes,
/// ~6 KB DRAM-resident), one float FFT per loop, no per-loop heap. The mic read is
/// non-blocking (the first ~250 ms of power-on settling garbage flows through the
/// first few reads and self-corrects); a bad init leaves the module idle (zeroed
/// frame), never crashing.
///
/// **Prior art:** audio-reactive lighting is a long-standing idea in the LED-controller world
/// (WLED-MM and MoonLight are the closest lineage). This is projectMM's own implementation, designed
/// from the INMP441 datasheet (https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf) and
/// standard DSP rather than traced from any one project — studying, with credit, the thinking of
/// Frank (softhack007, WLED-MM audioreactive), Troy (troyhacks, the esp-dsp FFT + biquad pre-filters
/// path we share), and Damian Schneider (DedeHai, the fixed-point FFT for FPU-less chips). The
/// forward-looking analysis (source-seam extensions — line-in / PDM / analog / I²C codecs — and the
/// adaptive-noise-gate design that would retire the borrowed `floor` squelch) is a design study in
/// docs/backlog/audio-dsp-roadmap.md.
/// @card AudioService.png
class AudioService : public MoonModule {
public:
    /// Block size = FFT size: a power of two. 512 samples at 22050 Hz is ~23 ms of
    /// audio per frame — fine resolution (~43 Hz/bin) at a modest per-tick cost.
    static constexpr size_t kBlock = 512;
    static constexpr size_t kMag = kBlock / 2;   ///< real-FFT magnitude bins

    ModuleRole role() const override { return ModuleRole::Service; }

    /// Unlike a zero-cost diagnostic peripheral, this module pays a
    /// real per-tick cost (the FFT) that IS the capability, not an optional extra,
    /// so it must not run when the user turns it off. We therefore respect `enabled`
    /// (the default): the Scheduler skips loop() entirely while disabled, so the FFT,
    /// the level, and the read-outs all stop and the cost goes to zero. Enabled runs
    /// the full pipeline; removing the module stops it the same way. The read-outs
    /// hold their last value while disabled (no consumer reads a disabled module).
    /// (respectsEnabled() defaults to true, so we don't override it.)

    // --- controls: three I2S pins, sample rate, the two conditioning knobs, and
    // two read-only read-outs. The pins default to UNSET (-1, the standard Pin-
    // control sentinel — so GPIO 0 stays a usable mic pin): the module is user-added
    // when a board has a mic, and stays idle (no I2S init) until the user enters the
    // real GPIOs, so adding it can't grab arbitrary pins or wedge a board with no
    // mic. The bench INMP441 wiring is SCK=6 / WS=4 / SD=5. Order follows the I2S
    // datasheet convention: clocks (SCK, WS) then data (SD) then the optional MCLK. ---
    int8_t sckPin = -1;          ///< bit clock / BCLK (-1 = unset). Changing it re-creates the I2S channel live (no reboot).
    int8_t wsPin = -1;           ///< word-select / LRCLK (-1 = unset). Changing it re-creates the I2S channel live.
    int8_t sdPin = -1;           ///< serial data in / DOUT (-1 = unset). Changing it re-creates the I2S channel live.
    int8_t mclkPin = -1;         ///< master clock out (-1 = none). Self-clocked MEMS mics (INMP441) leave this
                                 ///< -1; an ADC/codec that needs a master clock (e.g. the MHC-WLED P4 shield's
                                 ///< line-in ADC on GPIO3) sets it so the I2S peripheral drives MCLK. On a codec
                                 ///< board (CodecType != None) the codec's own mclk is used instead (see reinit()).
                                 ///< Changing it re-creates the I2S channel live.
    /// Sample rate is a discrete choice (the standard audio rates), so it's a
    /// dropdown over a fixed set, not a free number. sampleRateSel indexes
    /// kSampleRates; sampleRate() resolves it to Hz. Default index 2 = 22050
    /// (~11 kHz Nyquist covers the range that matters for light). Changing it
    /// re-creates the channel live.
    uint8_t  sampleRateSel = 2;
    // Two knobs condition the spectrum + level:
    uint8_t  floor = 100;        ///< noise floor (dB display floor) — bands/level
                                 ///< below this read as silence. Raise to keep an
                                 ///< ambient room dark, lower for a quiet room.
    uint8_t  gain = 222;         ///< sensitivity — HIGHER = more (a narrower dB window
                                 ///< so a given sound fills more of the bar).
    /// Simulated audio: fill the frame with a synthesized signal so audio-reactive effects are demoable
    /// (and testable) without a mic or music, such as a preview/demo device with no microphone. Two patterns:
    ///   `music` — a plausible song: multi-sine bands + a swelling volume + a periodic beat + a
    ///             sweeping peak. Nice for demos (bars dance, VU breathes, peaks move).
    ///   `sweep` — a single band lit, marching bass→treble on a timer, with the peak frequency and a
    ///             steady volume tracking it. Deterministic — the clean test pattern to check that each
    ///             effect responds across the whole spectrum.
    /// A real mic always wins: when `mode` is a fill-in ("silence") mode it only runs while there's no real signal.
    uint8_t  simulate = 0;       ///< 0 = off, 1 = music (fill silence), 2 = sweep (fill silence),
                                 ///< 3 = music (always), 4 = sweep (always)
    /// WLED audio-sync over UDP (port 11988, broadcast, WLED v2 wire format —
    /// light/WLEDAudioSyncPacket.h). 0 = Off (local audio only, no socket bound);
    /// 1 = Send (broadcast this device's AudioFrame for WLED/MoonLight receivers);
    /// 2 = Receive (bind 11988; a peer's audio overwrites frame_ so effects react to
    /// it, auto-falling-back to the local mic when packets stop for ~1 s). The socket
    /// is bound only in Send/Receive — Off costs nothing. Changing it re-binds live.
    uint8_t  sync = 0;           ///< 0 = off, 1 = send, 2 = receive
    /// The sync UDP port — the Send destination and the Receive listen port. Defaults
    /// to WLED's 11988 (interop with WLED/MoonLight); set it the same on both ends to
    /// run a private projectMM-only sync group on a non-WLED port.
    uint16_t syncPort = WLED_SYNC_PORT;

    static constexpr uint16_t kSampleRates[] = {8000, 16000, 22050, 44100};
    static constexpr uint8_t kSampleRateCount = 4;
    uint32_t sampleRate() const { return kSampleRates[sampleRateSel < kSampleRateCount
                                                       ? sampleRateSel : 2]; }

    void onBuildControls() override {
        controls_.addPin("sckPin", sckPin);
        controls_.addPin("wsPin", wsPin);
        controls_.addPin("sdPin", sdPin);
        controls_.addPin("mclkPin", mclkPin);
        static constexpr const char* kRateOptions[] = {"8000", "16000", "22050", "44100"};
        controls_.addSelect("sampleRate", sampleRateSel, kRateOptions, kSampleRateCount);
        // floor/gain condition the LOCAL FFT/level mapping. They stay visible in every sync
        // mode — including Receive, because auto-blend falls back to the local mic when the
        // peer goes quiet, and floor/gain govern that fallback. (They're only transiently
        // dead while a peer's audio is actively driving the frame — not permanently, so
        // hiding them by the sync setting would wrongly hide a control the fallback needs.)
        controls_.addUint8("floor", floor, 0, 255);
        controls_.addUint8("gain", gain, 1, 255);
        static constexpr const char* kSimulateOptions[] = {
            "off", "music (silence)", "sweep (silence)", "music (always)", "sweep (always)"};
        controls_.addSelect("simulate", simulate, kSimulateOptions, 5);
        // WLED audio sync — only on builds with an IP stack (WiFi OR Ethernet: the UDP
        // send/receive works over either, so an Ethernet-only board like the MHC-WLED
        // shield still gets it). A no-network build hides the controls entirely.
        if constexpr (platform::hasNetwork) {
            static constexpr const char* kSyncOptions[] = {"off", "send", "receive"};
            controls_.addSelect("sync", sync, kSyncOptions, 3);
            // The UDP port — the Send destination and the Receive listen port. Defaults to
            // WLED's 11988 (interop with WLED/MoonLight); change it on BOTH ends to run a
            // private projectMM-only sync group on a non-WLED port. Shown whenever sync is on
            // (Send or Receive), hidden in Off. A `sync` change re-runs this method (it's in
            // controlChangeTriggersBuildState) so the row toggles live.
            controls_.addUint16("syncPort", syncPort, 1, 65535);
            controls_.setHidden(controls_.count() - 1, sync == 0);
            controls_.addReadOnly("sync status", syncStr_, sizeof(syncStr_));
        }
        // Read-only live read-outs (formatted in loop1s). Derived every second,
        // nothing to persist, so ReadOnly (the display-only type) not a flipped
        // Text — same idiom as SystemModule's uptime/fps.
        // "level RMS" = the RMS loudness; the DISPLAYED number is its peak over the 1-second window
        // (loop1s publishes levelPeak_, the max of the per-block RMS level, then resets it), so a
        // beat that lands between samples still registers. The live frame_.level the LEDs use is the
        // instantaneous RMS, recomputed every audio block — this read-out is the human-readable
        // summary of it, not a separate statistic.
        controls_.addReadOnly("level RMS", levelStr_, sizeof(levelStr_));
        controls_.addReadOnly("peakHz", peakStr_, sizeof(peakStr_));
        MoonModule::onBuildControls();
    }

    /// A pin or rate change rebuilds the I2S channel (live, no reboot); a `sync` /
    /// `syncPort` change re-binds/unbinds the UDP socket AND re-toggles the port row's
    /// visibility (all flow through onBuildState → rebuildControls).
    bool controlChangeTriggersBuildState(const char* name) const override {
        return std::strcmp(name, "wsPin") == 0 || std::strcmp(name, "sdPin") == 0
            || std::strcmp(name, "sckPin") == 0 || std::strcmp(name, "mclkPin") == 0
            || std::strcmp(name, "sampleRate") == 0 || std::strcmp(name, "sync") == 0
            || std::strcmp(name, "syncPort") == 0;
    }

    void onBuildState() override { reinit(); syncReinit(); MoonModule::onBuildState(); }
    void setup() override {
        if (active_ == nullptr) active_ = this;   // first live mic wins; a 2nd mic is captured but not read
        reinit();
        syncReinit();
    }
    void teardown() override {
        deinit();
        if constexpr (platform::hasNetwork) { syncSock_.close(); syncOpen_ = false; }
        if (active_ == this) active_ = nullptr;   // vacate; a surviving module re-elects itself in loop()
    }

    /// The latest analysed frame — what effects read. Always valid (zeroed until
    /// the first successful read), so a consumer never dereferences null and a
    /// mic-less build just sees silence.
    const AudioFrame* audioFrame() const { return &frame_; }

    // --- Test seams (host unit tests only; mirror DevicesModule::injectPacketForTest) ---
    // Read-only views of the sync socket lifecycle so unit_AudioService_sync can assert it
    // through the public loop() without befriending the class or exposing internals broadly.
    bool syncOpenForTest() const { return syncOpen_; }
    uint8_t syncFrameCounterForTest() const { return syncFrameCounter_; }
    const char* syncStatusForTest() const { return syncStr_; }
    static constexpr uint32_t syncSendIntervalMsForTest() { return kSyncSendIntervalMs; }
    static constexpr uint32_t syncFallbackMsForTest() { return kSyncFallbackMs; }
    static constexpr uint32_t syncOpenRetryMsForTest() { return kSyncOpenRetryMs; }

    /// Process-wide accessor for the consumers (audio effects). There is one mic,
    /// and an effect can be added/removed via the UI at any time, so it can't rely
    /// on a boot-time setter — it asks here
    ///
    /// Returns the live mic's frame while one exists, else a static all-silent
    /// frame, so an effect added before/without a mic still reads valid silence
    /// instead of null. The FIRST live module claims the seat in `setup()`, vacates
    /// it in `teardown()`, and any running module re-claims an empty seat in
    /// `loop()` — so a device with two mics reads the first consistently, and
    /// removing the active one lets a survivor take over. Add/remove in any order
    /// leaves a coherent answer (the robustness rule).
    static const AudioFrame* latestFrame() {
        static const AudioFrame kSilence{};
        return active_ ? &active_->frame_ : &kSilence;
    }

    void loop() override {
        // Self-elect as the active mic if the seat is empty. setup() gives it to the first live module
        // and teardown() vacates it, but removing the active module while a second one is still running
        // would otherwise leave the seat empty (effects go silent). A running module re-claiming an
        // empty seat here keeps latestFrame() pointing at a live frame for ANY add/remove order — the
        // survivor takes over on its next tick (robustness).
        //
        if (active_ == nullptr) active_ = this;

        // WLED audio sync. Send broadcasts the current frame_ (throttled). Receive drains
        // the socket into frame_ and, while a peer's audio is fresh, RETURNS so the local
        // mic analysis below is skipped (the received frame drives the effects); once the
        // peer goes quiet (~1 s) it falls through and the local mic resumes (auto-blend).
        if constexpr (platform::hasNetwork) {
            if (sync != 0 && syncEnsureSocket()) {   // lazy-open once the network is up
                if (sync == 1) syncSend();
                else if (sync == 2 && syncReceive()) return;
            }
        }

        // Simulated audio (see the `simulate` control): 0=off, 1=music-on-silence, 2=sweep-on-silence,
        // 3=music-always, 4=sweep-always. Real mic input always wins in the "on-silence" modes — the
        // mic path below resets realQuietMs_ whenever a block carries signal, and synthesizeFrame()
        // no-ops while that timer says the mic is live. Off I2S (desktop / mic-less) or on a bad init
        // there's no mic, so the sim is the only possible source: run it and return.
        const bool simSweep = (simulate == 2 || simulate == 4);
        const bool simAlways = (simulate >= 3);
        if constexpr (!platform::hasI2sMic) {
            if (simulate != 0) synthesizeFrame(simSweep);   // the only possible source off I2S
            return;
        } else {
            if (!inited_) { if (simulate != 0) synthesizeFrame(simSweep); return; }
            if (simAlways) { synthesizeFrame(simSweep); return; }   // forced: skip the mic entirely
        }

        // Drain whatever the DMA holds this tick (non-blocking) into the free tail
        // of the block accumulator. A full kBlock takes ~23 ms to arrive (longer
        // than one render tick), so each tick contributes a partial; we analyse
        // once the accumulator is full, then reset it.
        const size_t n = platform::audioMicRead(mic_, samples_ + filled_, kBlock - filled_);
        // Mic-health tallies (read here, before the DC-blocker rewrites samples_): the samples the I2S
        // read delivered, and whether any were non-zero, accumulated over the 1 s window and diagnosed
        // in loop1s(). The two together isolate the two clean failure modes of a mic bring-up: samples
        // arriving proves the clocks (BCLK/WS) + DMA; a non-zero among them proves the data line (SD).
        // All-zero samples = clocks fine, SD dead — otherwise a silent "RMS 0" with no clue which wire.
        micSamples1s_ += n;
        for (size_t i = 0; i < n; i++) if (samples_[filled_ + i] != 0) { micNonzero1s_++; break; }
        if (n == 0) return;                            // nothing ready this tick
        filled_ += n;
        if (filled_ < kBlock) return;                  // wait for a whole block
        filled_ = 0;                                   // consumed below; refill next

        // DC-blocker high-pass (~40 Hz): removes the constant offset + sub-bass
        // rumble before any analysis, so they can't leak into the low bands. The
        // filter is continuous across blocks (state in dc_).
        dc_.process(samples_, kBlock);

        // Level: overall loudness (RMS), independent of the FFT — it fluctuates
        // with how loud the room is. Uses a gentler floor than the bands (half),
        // so the VU keeps moving with volume instead of being gated hard like the
        // per-band display.
        computeLevel(samples_, kBlock, static_cast<uint8_t>(floor / 2), gain, frame_);

        // Smoothed level: a one-pole exponential moving average of the raw `level`, so effects that
        // want a calm, breathing VU (rather than the raw value's snap-to-transient) read a value that
        // lags and rounds off sudden changes. 3/4 old + 1/4 new is the textbook light smoothing —
        // fast enough to follow the music, slow enough to hide per-block jitter. Integer-only, one
        // block behind, off the per-light path. (WLED's `volume`/`volumeSmth` to our raw `level`.)
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);

        // Spectrum: window -> FFT -> 16 log bands, same floor/gain mapping.
        uint16_t peakHz = 0, peakMag = 0;
        applyWindow(samples_, kBlock, windowed_);
        platform::audioFft(windowed_, kBlock, mag_);
        magnitudesToBands(mag_, kMag, sampleRate(), floor, gain,
                          frame_.bands, peakHz, peakMag);

        // Peak frequency: the exact-Hz FFT bin, held when there's no real signal so
        // it doesn't wander in silence.
        if (peakMag > 8) { frame_.peakHz = peakHz; frame_.peakMag = peakMag; }

        // Track the PEAK level across the 1 s display window. frame_.level is recomputed every
        // ~23 ms audio block, but the UI string is snapshotted only once a second — sampling the
        // instantaneous value lands in the gaps between beats and reads 0 even while the LEDs (driven
        // live every render tick) move with the music. The window peak is the representative reading.
        // Display-only — the live frame_.level the effects/LEDs use is untouched.
        if (frame_.level > levelPeak_) levelPeak_ = frame_.level;

        // Auto fill-in (simulate 1/2): if the mic has been quiet for a bit, synthesize so the effects
        // still have something to react to. `level` this block re-arms the "real audio" grace period;
        // once it lapses, the sim takes over and yields again the instant real sound returns.
        if (simulate == 1 || simulate == 2) {
            if (frame_.level > kSimRealThreshold) realBlocks_ = kSimRealGraceBlocks;
            else if (realBlocks_ > 0) realBlocks_--;
            if (realBlocks_ == 0) synthesizeFrame(simulate == 2);
        }
    }

    /// Fill frame_ with a synthesized signal. sweep=false → a plausible "song" (each band its own
    /// oscillator, a swelling volume, a periodic beat, a drifting peak); sweep=true → one band lit
    /// marching bass→treble on a timer (the deterministic test pattern). All integer LUT math (sin8),
    /// once per tick, off the per-light path. Also runs the same levelSmoothed EMA the mic path does.
    void synthesizeFrame(bool sweep) {
        const uint32_t t = platform::millis();
        if (sweep) {
            // One band lit at a time, stepping bass→treble every ~250 ms and wrapping. The lit band
            // ramps up-and-down (triangle) so it's not a hard on/off, and the peak frequency + volume
            // track the swept band so frequency-mapped and volume effects follow it too.
            const uint8_t pos = static_cast<uint8_t>((t / 250u) % 16u);
            const uint8_t env = triwave8(static_cast<uint8_t>((t % 250u) * 255u / 250u));  // 0..255 within a step
            for (uint8_t b = 0; b < 16; b++) frame_.bands[b] = (b == pos) ? env : 0;
            frame_.level = env;
            frame_.peakHz = static_cast<uint16_t>(80 + pos * 700);   // bass→~10.6 kHz across the 16 steps
            frame_.peakMag = env;
        } else {
            // Musical "song": each band an independent sine at its own rate/phase (bass slow, treble
            // fast), a slow overall volume swell, and a periodic beat pulse that briefly lifts the low
            // bands + volume so beat-reactive effects fire. A drifting peak sweeps the dominant tone.
            const uint8_t beat = (t % 600u < 90u) ? static_cast<uint8_t>(triwave8(static_cast<uint8_t>((t % 600u) * 255u / 90u))) : 0;
            uint16_t sum = 0;
            for (uint8_t b = 0; b < 16; b++) {
                // Per-band oscillator: rate rises with b (treble flickers faster), phase spread by b.
                const uint8_t rate = static_cast<uint8_t>(1 + b);                   // BPM-ish multiplier
                const uint8_t osc = sin8(static_cast<uint8_t>(t * rate / 8u + b * 24u));
                uint16_t v = static_cast<uint16_t>((osc * 3u) / 4u);               // 0..191 base
                if (b < 4) v = static_cast<uint16_t>(v + beat / 2u);               // beat lifts the bass
                frame_.bands[b] = static_cast<uint8_t>(v > 255 ? 255 : v);
                sum = static_cast<uint16_t>(sum + frame_.bands[b]);
            }
            const uint8_t swell = sin8(static_cast<uint8_t>(t / 24u));             // slow volume breath
            uint16_t lvl = static_cast<uint16_t>(swell / 2u + sum / 32u + beat / 2u);
            frame_.level = lvl > 255 ? 255 : lvl;
            // Peak drifts across the spectrum so freq-mapped effects move.
            frame_.peakHz = static_cast<uint16_t>(80 + sin8(static_cast<uint8_t>(t / 40u)) * 40u);
            frame_.peakMag = frame_.level;
        }
        frame_.levelSmoothed = static_cast<uint16_t>((frame_.levelSmoothed * 3 + frame_.level) / 4);
        // Feed the same 1 s peak-hold the mic path uses, so the "level RMS" display tracks the
        // synthesized level instead of reading a stale 0 (the display is peak-over-window, not live).
        if (frame_.level > levelPeak_) levelPeak_ = static_cast<uint8_t>(frame_.level);
    }

    void loop1s() override {
        std::snprintf(levelStr_, sizeof(levelStr_), "%u", static_cast<unsigned>(levelPeak_));
        std::snprintf(peakStr_, sizeof(peakStr_), "%u Hz", static_cast<unsigned>(frame_.peakHz));
        levelPeak_ = 0;   // reset for the next window

        // Mic-health diagnosis from the 1 s tallies (see the read path). Only for a live *direct* mic
        // (inited, not a sync receiver, no codec) — a codec/sync/unset-pin path has its own status. The
        // split turns a silent mic into a wire-specific verdict:
        //   no samples at all  → the I2S clocks aren't running — check sckPin / wsPin.
        //   samples, all zero  → clocks fine, data line dead — check sdPin (SD/DOUT) + power.
        //   samples, non-zero  → data is flowing; clear any prior diagnosis.
        const bool directMicLive = inited_ && sync != 2
                                   && platform::audioCodecType == platform::CodecType::None;
        if (directMicLive) {
            if (micSamples1s_ == 0)
                setStatus("mic: no samples — check sckPin / wsPin (I2S clocks)", Severity::Warning);
            else if (micNonzero1s_ == 0)
                setStatus("mic: data line silent — check sdPin (SD/DOUT) + mic power", Severity::Warning);
            else if (micStatusStale_)
                setStatus("", Severity::Status);   // data flowing again — clear a prior diagnosis
            micStatusStale_ = (micSamples1s_ == 0 || micNonzero1s_ == 0);
        }
        micSamples1s_ = 0;
        micNonzero1s_ = 0;
        // Live sync status: "sending" / "receiving" (peer audio fresh) / "listening"
        // (bound, no peer) / "off". While the socket isn't open yet, leave the baseline
        // syncReinit/syncEnsureSocket set ("waiting for network" / "…failed"); only once
        // open do we report the moment-to-moment send/receive state.
        if constexpr (platform::hasNetwork) {
            if (sync == 0) std::snprintf(syncStr_, sizeof(syncStr_), "off");
            else if (syncOpen_) {
                if (sync == 1) std::snprintf(syncStr_, sizeof(syncStr_), "sending");
                else std::snprintf(syncStr_, sizeof(syncStr_),
                              (lastSyncRecv_ != 0
                               && platform::millis() - lastSyncRecv_ < kSyncFallbackMs)
                                  ? "receiving" : "listening");
            }
        }
        MoonModule::loop1s();
    }

private:
    // The mic that latestFrame() hands to effects. One in practice; the last one
    // to setup() wins, teardown() clears it. inline so the header stays standalone.
    static inline AudioService* active_ = nullptr;

    platform::AudioMicHandle mic_;
    bool inited_ = false;
    size_t filled_ = 0;         // samples accumulated toward the next full block
    DcBlocker dc_;              // ~40 Hz high-pass, continuous across blocks

    // Fixed hot-path scratch — sized once, never reallocated. ~6 KB total
    // (2 KB samples + 2 KB windowed + 1 KB magnitudes), DRAM-resident.
    int32_t samples_[kBlock] = {};
    float windowed_[kBlock] = {};
    float mag_[kMag] = {};

    AudioFrame frame_;

    char levelStr_[12] = {};
    char peakStr_[12] = {};
    uint8_t levelPeak_ = 0;   // peak frame_.level across the current 1 s display window (UI only)

    // Mic-health tallies over the 1 s window (see the read path + loop1s diagnosis). micSamples1s_ =
    // raw samples the I2S read delivered (proves clocks/DMA); micNonzero1s_ = reads that carried a
    // non-zero sample (proves the SD data line). micStatusStale_ = a diagnosis is currently posted, so
    // loop1s clears it once the mic recovers rather than clearing every second.
    uint32_t micSamples1s_ = 0;
    uint32_t micNonzero1s_ = 0;
    bool     micStatusStale_ = false;

    // Auto fill-in (simulate 1/2): treat the mic as "live" for a grace window after any block above
    // the threshold, so brief gaps between beats don't flip to the sim. ~2 s of blocks (≈86/block·23ms).
    static constexpr uint16_t kSimRealThreshold  = 4;    // a block level above this counts as real sound
    static constexpr uint16_t kSimRealGraceBlocks = 86;  // ~2 s at ~23 ms/block before the sim takes over
    uint16_t realBlocks_ = 0;   // grace countdown: >0 = mic was recently live, hold off the sim

    // WLED audio sync (light/WLEDAudioSyncPacket.h). One socket, bound only in Send/Receive.
    platform::UdpSocket syncSock_;
    uint32_t lastSyncSend_ = 0;      // millis of the last broadcast (send throttle)
    uint32_t lastSyncRecv_ = 0;      // millis of the last received packet (receive auto-blend)
    uint8_t  syncFrameCounter_ = 0;  // increments per send (the packet's dup/reorder field)
    bool     syncOpen_ = false;      // socket opened for the current mode (lazy-open latch)
    uint32_t lastSyncOpenFailMs_ = 0;  // millis of the last failed open (0 = none); bring-up backoff
    char     syncStr_[32] = {};      // "sync status" read-out
    static constexpr uint32_t kSyncSendIntervalMs = 25;   // ~40/s — WLED-friendly, well under a flood
    static constexpr uint32_t kSyncFallbackMs = 1000;     // no packet this long → resume local mic
    static constexpr uint32_t kSyncOpenRetryMs = 1000;    // pause between socket bring-up retries after a failure
    static constexpr int kSyncMaxRecvPerTick = 8;         // bounded non-blocking drain (sync is low-rate)

    static constexpr const char* kInitFailMsg = "mic init failed — check pins / rate";

    /// (Re)create the I2S channel for the current pins + rate. On a codec board the
    /// I2S peripheral drives MCLK first, then the codec is configured over I2C. Any
    /// unset pin (-1) or missing I2S support leaves the module idle with a status
    /// note rather than attempting an init. Called from setup() and onBuildState().
    void reinit() {
        if constexpr (!platform::hasI2sMic) {
            setStatus("mic: no I2S on this platform", Severity::Warning);
            return;
        }
        deinit();
        // Any pin unset (-1, the default until the user wires a mic): stay idle,
        // don't attempt an I2S init — initialising I2S on unset pins is what hung a
        // mic-less board's boot. GPIO 0 IS a valid mic pin now (the sentinel is -1,
        // not 0), so the guard tests < 0, not == 0.
        if (sckPin < 0 || wsPin < 0 || sdPin < 0) {
            setStatus("mic: set sckPin / wsPin / sdPin", Severity::Status);
            return;
        }
        // Bring up the I2S channel FIRST. Where MCLK comes from depends on the board:
        //  - Codec board (an analog mic behind an I2S codec, e.g. the S31's ES8311): the
        //    codec's MCLK pin from the per-target config (platform::audioCodecPins.mclk).
        //    The codec won't even answer I2C until that clock runs, so I2S precedes the
        //    codec config below.
        //  - No codec: the runtime `mclkPin` control — −1 for a self-clocked MEMS mic
        //    (INMP441), or a real pin for an I2S ADC that needs a master clock (the
        //    MHC-WLED P4 shield's line-in ADC on GPIO3, WLED's SR_DMTYPE=4).
        const int16_t mclk = platform::audioCodecType == platform::CodecType::None
                           ? mclkPin : static_cast<int16_t>(platform::audioCodecPins.mclk);
        inited_ = platform::audioMicInit(mic_, static_cast<uint16_t>(wsPin),
                                         static_cast<uint16_t>(sdPin),
                                         static_cast<uint16_t>(sckPin), mclk, sampleRate());
        if (!inited_) {
            setStatus(kInitFailMsg, Severity::Error);
            return;
        }
        // Now configure the codec over I2C (MCLK is running). A no-op returning true
        // on direct-mic boards, so the call is uniform. The codec then streams its ADC
        // onto the I2S bus the read above drains.
        if (!platform::audioCodecInit(platform::audioCodecType, platform::audioCodecPins,
                                      sampleRate())) {
            deinit();   // tear the I2S channel back down — we couldn't bring the codec up
            setStatus("mic: codec init failed — check I2C wiring", Severity::Error);
            return;
        }
        dc_.reset();   // start the high-pass clean for the new stream
        // The INMP441 emits ~250 ms of power-on settling garbage after the clock
        // starts. The read is non-blocking (hot-path rule), so we can't drain a
        // fixed sample count here at init — the DMA has barely filled. Instead the
        // settling samples flow through the first few loop() reads and the level /
        // bands self-correct within that quarter-second; no separate discard is
        // needed, and the frame stays valid (zeroed) until then.
        // Clear any prior status now the mic is live — not just kInitFailMsg, but
        // also the "set wsPin / sdPin / sckPin" note from the unset-pin path, which
        // would otherwise persist and mislead after the user fills the pins in.
        clearStatus();
    }

    void deinit() {
        if constexpr (!platform::hasI2sMic) return;
        if (inited_) platform::audioMicDeinit(mic_);
        platform::audioCodecDeinit();   // releases the codec + its I2C bus (no-op if none)
        inited_ = false;
        filled_ = 0;
        // Publish silence: latestFrame() hands frame_ to consumers whenever this is
        // the active mic, independent of inited_. Without this, a mic that worked
        // and then lost its bus (a failed reinit after a pin edit, or teardown)
        // would leave the last real frame frozen on the LEDs instead of going dark.
        frame_ = AudioFrame{};
    }

    // --- WLED audio sync (guarded: only compiled where platform::hasNetwork) ---

    /// Reset the sync socket to the current mode. Called from setup()/onBuildState()
    /// so a `sync` control change applies live (no reboot). This only CLOSES the socket
    /// and records the mode — it never opens one, because setup() runs at boot before
    /// NetworkModule brings an interface up, and any lwip socket call before then asserts
    /// (the core mutex is still null). The actual open() is deferred to syncEnsureSocket(),
    /// which runs from the tick path once platform::networkReady() is true.
    void syncReinit() {
        if constexpr (!platform::hasNetwork) return;
        syncSock_.close();                 // syncEnsureSocket() re-opens per mode when the net is up
        syncOpen_ = false;
        lastSyncOpenFailMs_ = 0;           // a mode change retries bring-up immediately (no stale backoff)
        lastSyncRecv_ = 0;
        std::snprintf(syncStr_, sizeof(syncStr_),
                      sync == 1 ? "send: waiting for network"
                      : sync == 2 ? "receive: waiting for network"
                      : "off");
    }

    /// Lazily open the sync socket for the current mode, once the network stack is up.
    /// Idempotent: opens exactly once per mode (syncOpen_ latch), re-armed by syncReinit()
    /// on a mode change. Returns true when the socket is ready to use this tick. Off is a
    /// no-op (socket stays closed, zero overhead). Send connects to the LAN broadcast;
    /// Receive binds the port. Mirrors NetworkSendDriver/NetworkReceiveEffect, but deferred
    /// past boot so a boot-present AudioService can't touch lwip before it exists.
    bool syncEnsureSocket() {
        if constexpr (!platform::hasNetwork) return false;
        if (sync == 0) return false;
        if (syncOpen_) return true;
        if (!platform::networkReady()) return false;   // interface not up yet — try again next tick
        // Back off between failed bring-ups: loop() runs every tick, so without this a
        // persistent open/bind failure (e.g. the port is busy) would retry — one socket()
        // syscall per tick — dozens of times a second. lastSyncOpenFailMs_ stamps the last
        // failure; hold off until kSyncOpenRetryMs has passed (same throttle form as syncSend).
        const uint32_t now = platform::millis();
        if (lastSyncOpenFailMs_ != 0 && now - lastSyncOpenFailMs_ < kSyncOpenRetryMs) return false;
        if (sync == 1) {                   // send → broadcast destination (configurable port)
            char bcast[16]; formatDottedQuad(bcast, kBroadcast_);
            if (syncSock_.open() && syncSock_.connect(bcast, syncPort)) {
                syncOpen_ = true;
                std::snprintf(syncStr_, sizeof(syncStr_), "sending");
            } else {
                syncSock_.close();
                std::snprintf(syncStr_, sizeof(syncStr_), "send: socket failed");
            }
        } else {                           // receive → bind the port (WLED 11988, or a custom group)
            if (syncSock_.open() && syncSock_.bind(syncPort)) {
                syncOpen_ = true;
                std::snprintf(syncStr_, sizeof(syncStr_), "listening");
            } else {
                syncSock_.close();
                std::snprintf(syncStr_, sizeof(syncStr_), "receive: bind failed");
            }
        }
        // Stamp a failure (or clear the timer on success). now==0 is nudged to 1 so the
        // "!=0 means a failure is pending" sentinel holds even at millis()==0.
        lastSyncOpenFailMs_ = syncOpen_ ? 0 : (now == 0 ? 1 : now);
        return syncOpen_;
    }

    /// Broadcast the current frame_ as a WLED v2 packet, throttled to ~40/s. Called
    /// from loop() in Send mode. Cheap: builds a 44-byte packet, one non-blocking sendTo.
    void syncSend() {
        if constexpr (!platform::hasNetwork) return;
        const uint32_t now = platform::millis();
        if (now - lastSyncSend_ < kSyncSendIntervalMs) return;
        lastSyncSend_ = now;
        // samplePeak hint: a beat is a raw level notably above the smoothed average.
        const bool peak = frame_.level > frame_.levelSmoothed + kSyncPeakMargin;
        uint8_t pkt[WLED_SYNC_PACKET_SIZE];
        buildWledAudioSync(pkt, frame_, syncFrameCounter_++, peak);
        syncSock_.sendTo(pkt, WLED_SYNC_PACKET_SIZE);
    }

    /// Drain the sync socket (bounded, non-blocking) in Receive mode. A valid v2 packet
    /// overwrites frame_ and stamps lastSyncRecv_. Returns true while a peer's audio is
    /// FRESH (within kSyncFallbackMs) so loop() skips the local mic analysis — false once
    /// the peer goes quiet, letting the local mic resume (auto-blend).
    bool syncReceive() {
        if constexpr (!platform::hasNetwork) return false;
        uint8_t pkt[WLED_SYNC_PACKET_SIZE + 8];   // a little slack over the 44-byte v2
        uint8_t srcIp[4] = {};
        for (int i = 0; i < kSyncMaxRecvPerTick; i++) {
            const int n = syncSock_.recvFrom(pkt, sizeof(pkt), srcIp);
            if (n <= 0) break;                     // -1 = nothing pending
            AudioFrame rf;
            if (parseWledAudioSync(pkt, static_cast<size_t>(n), rf)) {
                frame_ = rf;                       // received audio drives the effects
                lastSyncRecv_ = platform::millis();
                // Feed the peer level into the same 1 s peak window the local mic uses, so the
                // "level RMS" read-out (loop1s → levelStr_) reflects received audio too — otherwise
                // it freezes at the last local value while a peer is driving the effects.
                if (frame_.level > levelPeak_)
                    levelPeak_ = static_cast<uint8_t>(frame_.level > 255 ? 255 : frame_.level);
            }
            // else: a v1 / foreign packet — ignore, keep draining.
        }
        // Fresh received audio → skip local mic. Stale (peer quiet) → fall through.
        return lastSyncRecv_ != 0
            && (platform::millis() - lastSyncRecv_) < kSyncFallbackMs;
    }

    static constexpr uint16_t kSyncPeakMargin = 8;   // level over smoothed = a beat (samplePeak hint)
    static constexpr uint8_t kBroadcast_[4] = {255, 255, 255, 255};   // LAN broadcast for send
};

} // namespace mm
