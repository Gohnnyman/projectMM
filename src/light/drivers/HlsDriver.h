#pragma once
/// HlsDriver, projectMM as an HLS video source.
///
/// The rendered frame, pixel-exact, reaches a TV, VLC or a browser as H.264 over HLS from the
/// device's own HTTP server. Complements NdiDriver: NDI is the pro-tools path, HLS is the
/// consumer-playback path.
///
/// **The driver states numbers; the platform encodes.** This driver packs the corrected frame and
/// hands it over with the geometry, rate and bitrate (`platform::EncoderConfig`); how those become
/// H.264 differs completely per platform and is none of its business. On desktop the platform
/// spawns the `ffmpeg` found on PATH and pipes frames to its stdin, ffmpeg doing the encode and
/// the HLS segmenting; nothing is vendored or linked (GPL x264 / patent-encumbered openh264 stay
/// out of the tree), the same runtime-dependency arrangement as Npcap and the NDI runtime. On the
/// ESP32-P4 the platform drives the chip's hardware H.264 encoder and muxes the segments itself.
/// Per-OS desktop encoder integrations were rejected for the reason NDI beat Spout/Syphon:
/// coverage decides.
///
/// **Pixel-exact contract.** The encoded frame is the grid from the layer, letterboxed by the
/// display. Where `scale` > 1 a light becomes a solid square BLOCK rather than one pixel: still
/// pixel-exact in the sense that matters, since replication invents no color the wall does not
/// have and keeps every light individually visible, unlike an interpolating resize. Scaling
/// exists because the P4's hardware encoder refuses a frame under 80x80 and because a small wall
/// streamed 1:1 is a postage stamp in the player; `autoScale()` therefore lifts a small wall to
/// that floor and leaves everything else at 1:1. One factor for both axes keeps the aspect ratio
/// exact. Latency is HLS's, not ours: the encode adds milliseconds, segmentation plus player
/// buffering adds the seconds (2-5 s live-tuned).
///
/// **Where it runs** (`platform::hasHls`): H.264 needs a desktop-class CPU or a hardware encoder,
/// so desktop and the ESP32-P4. Every other ESP32 reaches viewers through Preview instead.
///
/// **Frame pacing is a fixed schedule**, not a last-sent timestamp: `1000/fps` truncates (30 fps
/// asks for 33 ms, so 30 frames span 990 ms and the stream runs ~1% fast) and re-basing on each
/// frame's arrival lets one late tick shift the schedule for good. Either drifts against the
/// player's clock until it stalls to re-buffer, which is a periodic hiccup rather than an
/// obvious fault. The bitrate is DERIVED for the same reason a control was removed: it follows
/// from pixels x fps (see autoBitrateKbit), and `targetFps` is the knob a user actually wants.
///
/// Prior art: HLS is Apple's (RFC 8216); ffmpeg does the encoding. The frame-pacing, packing and
/// status shape follow NdiDriver, the other driver that turns the rendered buffer into video.
/// Author: projectMM original

#include "core/Control.h"
#include "core/ScratchBuffer.h"
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace mm {

/// Driver that publishes the layer as an H.264/HLS stream.
class HlsDriver : public DriverBase {
public:
    static constexpr const char* kTags = "🖥️";
    static constexpr const char* kSegmentDir = "/.hls";   // under the fs mount; served at /hls/
                                                          // (same literal in serveHlsFile and the
                                                          // backup walkers: change all together)

    HlsDriver() : rgb_(*this), corrScratch_(*this) {}

    const char* tags() const override { return kTags; }

    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    void defineDriverControls() override {
        controls_.addControl("targetFps", targetFps, 1, 120);
        // How many video pixels each light becomes. 0 = auto, which picks the smallest factor
        // that clears the encoder's minimum frame size, so a small wall is never rejected and
        // never arrives as a postage stamp in the player. One factor for both axes: the aspect
        // ratio is preserved by construction, and every light stays a clean square block rather
        // than being interpolated across a fractional boundary.
        controls_.addControl("scale", scale, 0, kMaxScale);
        // The video encoder, where the platform has more than one. libx264 exists in every ffmpeg
        // build; the hardware entries offload the encode entirely, worth picking on large grids.
        // One this ffmpeg lacks fails the spawn and the status says so. A platform with a single
        // hardware encoder (the P4) offers no choice, so the control is absent rather than a
        // one-entry dropdown.
        if constexpr (platform::hasEncoderChoice) {
            controls_.addSelect("encoder", encoderSel_, kEncoderOptions, kEncoderOptionCount);
            // Persisted by LABEL: ffmpeg encoder names are stable identities, and editing the
            // option list (vaapi's removal) must never silently remap an index-persisted pick.
            controls_.setPersistLabel(controls_.count() - 1);
        }
        // The playable address, one copy away from VLC or a Safari AirPlay hand-off.
        controls_.addReadOnly("url", urlBuf_, sizeof(urlBuf_));
    }

    /// A pacing or rate change needs a new encode (ffmpeg fixes both at spawn); geometry changes
    /// arrive through prepare() already.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "targetFps") == 0 || std::strcmp(name, "scale") == 0 ||
               std::strcmp(name, "encoder") == 0 || isCorrectionControl(name);
    }

    void prepare() override {
        release();
        if (!layer_) return;

        srcWidth_  = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        srcHeight_ = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        scale_     = scale ? scale : autoScale();
        // Widen BEFORE multiplying, and reject before narrowing. Both operands are individually
        // sane while their product need not be: lengthType is int16_t, so an 821x4 wall at
        // scale 80 wraps to 144x320, the frame buffer is sized from the wrapped number, and the
        // pixel loop then walks the REAL 821x4 source straight past the end of it.
        const uint32_t scaledW = static_cast<uint32_t>(srcWidth_)  * scale_;
        const uint32_t scaledH = static_cast<uint32_t>(srcHeight_) * scale_;
        if (scaledW > kMaxEncodeWidth || scaledH > kMaxEncodeHeight) {
            std::snprintf(statusBuf_, sizeof(statusBuf_),
                          "%ux%u at scale %u exceeds the encoder's %ux%u",
                          static_cast<unsigned>(srcWidth_), static_cast<unsigned>(srcHeight_),
                          static_cast<unsigned>(scale_),
                          static_cast<unsigned>(kMaxEncodeWidth),
                          static_cast<unsigned>(kMaxEncodeHeight));
            setStatus(statusBuf_, Severity::Warning);
            return;
        }
        width_     = static_cast<lengthType>(scaledW);
        height_    = static_cast<lengthType>(scaledH);

        const size_t pixels = static_cast<size_t>(width_) * height_;
        if (!rgb_.resize(pixels * 3)) {
            setStatus("out of memory for the video frame", Severity::Error);
            return;
        }
        if (correction_.outChannels > 3) corrScratch_.resize(correction_.outChannels);

        // Only where the encoder writes segments to disk. A platform that keeps them in RAM
        // (the P4) has no directory to make and drops its ring on encoderStart.
        if constexpr (platform::hasFsSegments) {
            platform::fsMkdir(kSegmentDir);
            clearSegments();                 // a stale playlist must not serve the old geometry
        }
        restartsLeft_ = kMaxRestarts;
        sendEpochMs_ = platform::millis();   // the schedule the frame pacing counts from
        nextSendMs_  = sendEpochMs_;
        frameIndex_  = 0;
        if (!startEncoder()) return;         // startEncoder set the status

        // A RELATIVE url. The device's own address is not the platform layer's to know on an
        // ESP32 (platform::hostIp() is empty there by design: NetworkModule owns the IP), and a
        // hard-coded host would go stale on every DHCP change anyway. The browser already knows
        // which host it loaded from, so it resolves this against that and renders an absolute,
        // clickable link; anything copied out of the UI is therefore correct by construction.
        std::snprintf(urlBuf_, sizeof(urlBuf_), "/hls/stream.m3u8");
        if (scale_ > 1) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "streaming %ux%u as %ux%u at %u fps",
                          static_cast<unsigned>(srcWidth_), static_cast<unsigned>(srcHeight_),
                          static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                          static_cast<unsigned>(targetFps));
        } else {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "streaming %ux%u at %u fps",
                          static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                          static_cast<unsigned>(targetFps));
        }
        setStatus(statusBuf_, Severity::Status);
    }

    void release() override {
        if (open_) {
            platform::encoderStop();
            open_ = false;
            if constexpr (platform::hasFsSegments) clearSegments();   // transient; nothing to keep
        }
        DriverBase::release();
    }

    void tick() MM_NONBLOCKING override {
        // encoderDied_ gates HARD: after a mid-frame failure the pipe holds a PARTIAL frame,
        // and any further write lands shifted: the torn half-and-half image seen live on the
        // bench. Nothing more is written until the restart hands over a clean pipe.
        if (!open_ || encoderDied_ || targetFps == 0 || !sourceBuffer_ || !sourceBuffer_->data()) return;
        // Warm-up: the encoder reads nothing while it initializes (VideoToolbox takes a few
        // hundred ms), and a frame that STARTS into the pipe then cannot complete trips the
        // wedge budget as a false death. Send nothing until the encoder is demonstrably up.
        if (static_cast<int32_t>(platform::millis() - warmupUntilMs_) < 0) return;

        // targetFps is a CEILING, the NdiDriver/PreviewDriver pacing pattern: the render loop
        // runs faster, frames beyond the rate are simply not encoded.
        //
        // Paced on a FIXED schedule rather than from each frame's arrival time. Two reasons, both
        // measured on the bench: `1000/fps` truncates (30 fps asks for 33 ms, so 30 frames span
        // 990 ms and the stream runs ~1% fast), and re-basing on `now` lets every late tick shift
        // the schedule permanently. A player fed segments that drift against its own clock stalls
        // to re-buffer, which is the periodic hiccup this replaced. Milliseconds are accumulated
        // in the numerator so the rate is exact at any fps, and a long stall (a re-prepare, a
        // paused render loop) resyncs rather than trying to catch up with a burst.
        const uint32_t now = platform::millis();
        const uint32_t periodMs = 1000u / targetFps;
        if (static_cast<int32_t>(now - nextSendMs_) < 0) return;
        frameIndex_++;
        nextSendMs_ = sendEpochMs_ + static_cast<uint32_t>(
            (static_cast<uint64_t>(frameIndex_) * 1000u) / targetFps);
        if (static_cast<int32_t>(now - nextSendMs_) > static_cast<int32_t>(periodMs * 4)) {
            // Far behind (a re-prepare, or a render loop that was paused): restart the schedule
            // from this frame rather than firing a burst to catch up. The next frame is due one
            // period out, NOT immediately -- resyncing to `now` makes the very next tick due and
            // sends a second frame straight away (caught by the rate-ceiling test).
            sendEpochMs_ = now;
            frameIndex_  = 0;
            nextSendMs_  = now + periodMs;
        }

        const nrOfLightsType lights = static_cast<nrOfLightsType>(srcWidth_) * srcHeight_;
        const nrOfLightsType have   = sourceBuffer_->count();
        const nrOfLightsType n      = lights < have ? lights : have;
        const size_t frameBytes = static_cast<size_t>(width_) * height_ * 3;
        if (n == 0 || rgb_.count() < frameBytes) return;

        const uint8_t* src   = sourceBuffer_->data();
        const uint8_t  srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t  outCh = correction_.outChannels;
        if (srcCh < 3) return;   // a non-color buffer (DMX roles) has no frame to send

        // Pack tight RGB with the per-driver output correction, exactly as NdiDriver does: the
        // stream shows what the wall shows. At scale_ > 1 each light becomes a solid square
        // block, so the picture is the wall magnified rather than interpolated: no new colors
        // appear and every light stays individually visible.
        uint8_t* dst = &rgb_[0];
        const bool wide = outCh > 3 && corrScratch_.count() >= outCh;
        const size_t rowBytes = static_cast<size_t>(width_) * 3;
        for (nrOfLightsType i = 0; i < n; i++) {
            const uint8_t* s = src + static_cast<size_t>(i) * srcCh;
            uint8_t rgb[3];
            if (outCh == 3) {
                correction_.apply(s, rgb);
            } else if (wide) {
                uint8_t* c = &corrScratch_[0];
                correction_.apply(s, c);
                rgb[0] = c[0]; rgb[1] = c[1]; rgb[2] = c[2];
            } else {
                rgb[0] = s[0]; rgb[1] = s[1]; rgb[2] = s[2];
            }
            const lengthType sx = static_cast<lengthType>(i % srcWidth_);
            const lengthType sy = static_cast<lengthType>(i / srcWidth_);
            // Write the block's first row, then copy it to the rest: one color conversion per
            // light, and the replication is memcpy rather than per-pixel work.
            uint8_t* row0 = dst + (static_cast<size_t>(sy) * scale_) * rowBytes
                                + (static_cast<size_t>(sx) * scale_) * 3;
            for (uint8_t px = 0; px < scale_; px++) {
                row0[px * 3 + 0] = rgb[0];
                row0[px * 3 + 1] = rgb[1];
                row0[px * 3 + 2] = rgb[2];
            }
            for (uint8_t py = 1; py < scale_; py++)
                std::memcpy(row0 + static_cast<size_t>(py) * rowBytes, row0,
                            static_cast<size_t>(scale_) * 3);
        }
        // Lights the source buffer never supplied leave their blocks black.
        if (n < lights) {
            for (nrOfLightsType i = n; i < lights; i++) {
                const lengthType sx = static_cast<lengthType>(i % srcWidth_);
                const lengthType sy = static_cast<lengthType>(i / srcWidth_);
                uint8_t* row0 = dst + (static_cast<size_t>(sy) * scale_) * rowBytes
                                    + (static_cast<size_t>(sx) * scale_) * 3;
                for (uint8_t py = 0; py < scale_; py++)
                    std::memset(row0 + static_cast<size_t>(py) * rowBytes, 0,
                                static_cast<size_t>(scale_) * 3);
            }
        }

        // Non-blocking hand-off: a full pipe drops the frame (the encoder is behind, H.264
        // carries on from the next one), a dead process schedules a restart from tick1s.
        const int wrote = platform::encoderWrite(dst, frameBytes);
        if (wrote == 0) droppedFrames_++;
        else if (wrote < 0) encoderDied_ = true;
    }

    void tick1s() MM_NONBLOCKING override {
        if (!open_) return;
        if (encoderDied_ || !platform::encoderRunning()) {
            encoderDied_ = true;   // stays set until a restart SUCCEEDS: the pipe is dirty
            healthySecs_ = 0;
            // Backoff doubles per attempt (1 s, 2 s, 4 s) so a crash-looping ffmpeg is not
            // respawned every second; the budget replenishes below after sustained health, so
            // transient deaths across a long run never permanently stop the stream.
            if (restartsLeft_ > 0 && restartWaitS_ > 0) { restartWaitS_--; return; }
            if (restartsLeft_ > 0) {
                restartsLeft_--;
                restartWaitS_ = static_cast<uint8_t>(1u << (kMaxRestarts - restartsLeft_));
                if (startEncoder()) {
                    encoderDied_ = false;   // fresh process, clean pipe: writing may resume
                    statusStale_ = true;    // the healthy branch repaints "streaming" next second
                    setStatus("encoder restarted", Severity::Warning);
                    return;
                }
            }
            open_ = false;
            setStatus("encoder exited - check ffmpeg (see the docs)", Severity::Error);
            return;
        }
        if (healthySecs_ < 60) healthySecs_++;
        else { restartsLeft_ = kMaxRestarts; restartWaitS_ = 0; }
        if (statusStale_ || droppedFrames_ != lastReportedDrops_) {
            statusStale_ = false;
            lastReportedDrops_ = droppedFrames_;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "streaming %ux%u, %u frames dropped",
                          static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                          static_cast<unsigned>(droppedFrames_));
            setStatus(statusBuf_, Severity::Status);
        }
    }

    // Controls
    /// Video pixels per light, 0 = auto (see autoScale). One factor for both axes, so the
    /// aspect ratio is preserved and each light stays a square block.
    uint8_t  scale       = 0;
    /// Encode-rate ceiling; the render loop runs faster and extra frames are not encoded.
    uint8_t  targetFps   = 30;
    /// The ffmpeg encoder pick; hardware entries offload the encode (see the control's comment).
    uint8_t  encoderSel_ = 0;   // index into kEncoderOptions; 0 = libx264, the universal default

private:
    static constexpr uint8_t kMaxRestarts = 3;
    /// The smallest frame the strictest supported encoder accepts (the P4 hardware block's
    /// 80x80 floor; ffmpeg has no such limit, but one rule keeps the platforms interchangeable).
    static constexpr uint16_t kMinEncodeSize = 80;
    /// The largest frame the strictest supported encoder accepts (the P4 hardware block).
    /// Checked against the SCALED geometry, in a wide type: see prepare().
    static constexpr uint16_t kMaxEncodeWidth  = 1920;
    static constexpr uint16_t kMaxEncodeHeight = 2032;
    /// A ceiling on the blow-up, so a slider cannot ask for a frame no encoder will take (the
    /// P4 tops out at 1920x2032). It must be at least kMinEncodeSize, or auto could not lift a
    /// 1-light-wide wall to the floor and would hand the encoder a frame it refuses -- the
    /// feature failing exactly where it is needed most.
    static constexpr uint8_t kMaxScale = 80;
    static_assert(kMaxScale >= kMinEncodeSize,
                  "auto-scale must be able to reach the encoder's minimum from a 1-pixel axis");

    static constexpr uint32_t kWarmupMs = 750;   // encoder init headroom before the first frame
    // vaapi is deliberately absent: it needs -vaapi_device + hwupload filter plumbing this
    // argv does not build, so the row would be a guaranteed dead end; Linux uses libx264
    // (or nvenc) until that plumbing is its own change. The listed hardware encoders accept
    // system-memory frames, ffmpeg auto-inserting the pixel-format conversion.
    static constexpr const char* kEncoderOptions[] = {
        "libx264",            // software; practically every ffmpeg distribution ships it
        "h264_videotoolbox",  // macOS media engine
        "h264_v4l2m2m",       // Raspberry Pi
        "h264_nvenc",         // NVIDIA
    };
    static constexpr uint8_t kEncoderOptionCount = 4;

    /// The smallest whole factor that lifts BOTH axes to the encoder's minimum frame size. The
    /// P4's hardware encoder refuses anything under 80x80, and a player showing a 32x32 stream
    /// renders a postage stamp, so the default blows a small wall up rather than failing or
    /// under-filling. Upscaling is integer, so each light is a solid square and the picture stays
    /// exactly what the wall shows -- 1:1 in the sense that matters, just bigger.
    uint8_t autoScale() const {
        uint32_t f = 1;
        while (f < kMaxScale &&
               (srcWidth_ * f < kMinEncodeSize || srcHeight_ * f < kMinEncodeSize)) f++;
        return static_cast<uint8_t>(f);
    }

    /// The encode bitrate, derived rather than asked for. There is no bitrate control: the value
    /// follows from what the picture costs (bits-per-pixel-per-frame x pixels x fps), and the one
    /// knob a user actually wants for bandwidth is `targetFps`, which is the more meaningful trade
    /// for LED content. 0.1 bpp is the usual working figure for H.264 on this kind of material
    /// (flat regions, strong temporal correlation): a 128x128 grid at 30 fps lands near 500 kbit,
    /// a 512x512 near 8 Mbit. Clamped so a tiny grid still looks clean and a huge one cannot ask
    /// for more than an encoder accepts.
    uint16_t autoBitrateKbit() const {
        const uint64_t pixels = static_cast<uint64_t>(width_) * height_;
        const uint64_t kbit = (pixels * targetFps) / 10000u;   // 0.1 bpp, expressed in kbit
        if (kbit < 500) return 500;
        if (kbit > 40000) return 40000;
        return static_cast<uint16_t>(kbit);
    }

    bool startEncoder() {
        char outDir[192];
        std::snprintf(outDir, sizeof(outDir), "%s%s", platform::fsRootPath(), kSegmentDir);
        platform::EncoderConfig cfg{};
        cfg.width       = static_cast<uint16_t>(width_);
        cfg.height      = static_cast<uint16_t>(height_);
        cfg.fps         = targetFps;
        cfg.bitrateKbit = autoBitrateKbit();
        cfg.encoderName = kEncoderOptions[encoderSel_ < kEncoderOptionCount ? encoderSel_ : 0];
        cfg.outDir      = outDir;
        if (!platform::encoderStart(cfg)) {
            // Why it failed differs per platform, and a wrong reason sends the user hunting: on
            // desktop the encoder is an ffmpeg that may not be installed, while a hardware
            // encoder is always present and refuses only a frame size it cannot do (the P4's is
            // 80x80 to 1920x2032, in even steps).
            if constexpr (platform::hasEncoderChoice) {
                setStatus("ffmpeg not found - see the docs", Severity::Warning);
            } else if (width_ < 80 || height_ < 80 || width_ > 1920 || height_ > 2032) {
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              "%ux%u is outside the encoder's 80x80 to 1920x2032",
                              static_cast<unsigned>(width_), static_cast<unsigned>(height_));
                setStatus(statusBuf_, Severity::Warning);
            } else {
                setStatus("the hardware encoder did not start", Severity::Error);
            }
            return false;
        }
        warmupUntilMs_ = platform::millis() + kWarmupMs;
        open_ = true;
        return true;
    }

    void clearSegments() {
        // Collect first, remove after: deleting entries while fsList walks the same directory
        // is unspecified (std::filesystem::directory_iterator underneath on desktop).
        struct Names { char items[16][48]; uint8_t count = 0; } names;
        platform::fsList(kSegmentDir, [](const char* name, bool isDir, uint32_t, void* user) {
            auto* n = static_cast<Names*>(user);
            if (!isDir && n->count < 16) {
                std::snprintf(n->items[n->count], sizeof(n->items[0]), "%s", name);
                n->count++;
            }
        }, &names);
        for (uint8_t i = 0; i < names.count; i++) {
            char path[96];
            std::snprintf(path, sizeof(path), "%s/%s", kSegmentDir, names.items[i]);
            platform::fsRemove(path);
        }
    }

    Buffer*        sourceBuffer_ = nullptr;
    lengthType     srcWidth_  = 0;   // the wall
    lengthType     srcHeight_ = 0;
    lengthType     width_  = 0;      // the encoded frame: the wall times scale_
    lengthType     height_ = 0;
    uint8_t        scale_  = 1;      // the factor actually in use (the control, or autoScale)
    // tick() runs on the encode worker while tick1s() runs on the render task: the fields both
    // touch are atomics (each an independent flag/counter; the default ordering is plenty).
    std::atomic<bool>     open_{false};
    std::atomic<bool>     encoderDied_{false};
    bool           statusStale_ = false;   // tick1s-only
    uint8_t        restartsLeft_ = kMaxRestarts;
    uint8_t        restartWaitS_ = 0;    // backoff countdown, in tick1s steps
    uint8_t        healthySecs_  = 0;    // sustained-health counter that replenishes the budget
    // Frame pacing, tick-only. A fixed schedule (epoch + frame count) rather than a
    // last-sent timestamp, so the rate is exact and a late tick cannot shift it (see tick()).
    uint32_t       sendEpochMs_ = 0;
    uint32_t       nextSendMs_  = 0;
    uint32_t       frameIndex_  = 0;
    std::atomic<uint32_t> warmupUntilMs_{0};
    std::atomic<uint32_t> droppedFrames_{0};
    uint32_t       lastReportedDrops_ = 0; // tick1s-only
    ScratchBuffer<uint8_t> rgb_;          // tight RGB staging, sized in prepare()
    ScratchBuffer<uint8_t> corrScratch_;  // one corrected light, when the wiring is wider than RGB
    char           urlBuf_[64]{};
    char           statusBuf_[64]{};
};

}  // namespace mm
