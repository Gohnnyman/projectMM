#pragma once
/// HlsDriver, projectMM as an HLS video source.
///
/// The rendered frame, pixel-exact, reaches a TV, VLC or a browser as H.264 over HLS from the
/// device's own HTTP server. Complements NdiDriver: NDI is the pro-tools path, HLS is the
/// consumer-playback path. Spec: docs/backlog/hls-driver-spec.md.
///
/// **Why an ffmpeg pipe.** One general implementation for every desktop OS and the Pi: the
/// platform spawns the `ffmpeg` found on PATH and this driver pipes raw RGB frames to its stdin;
/// ffmpeg encodes and writes HLS segments the HTTP server serves from `/.hls/`. Nothing is
/// vendored or linked (GPL x264 / patent-encumbered openh264 stay out of the tree), the same
/// runtime-dependency arrangement as Npcap and the NDI runtime. Per-OS encoder integrations were
/// rejected for the same reason NDI beat Spout/Syphon: coverage decides.
///
/// **Pixel-exact contract.** The encoded frame IS the grid, width x height from the layer, no
/// scaling anywhere in the pipeline: every written pixel is one video pixel, letterboxed by the
/// display. Latency is HLS's, not ours: the encode adds milliseconds, segmentation plus player
/// buffering adds the seconds (2-5 s live-tuned). Documented on the card so nobody expects
/// preview-grade feedback.
///
/// **Desktop only** (`platform::hasHls`): H.264 needs a desktop-class CPU or hardware encoder,
/// and there is no process to spawn on ESP32, which reaches viewers through Preview instead.
///
/// Prior art: HLS is Apple's (RFC 8216); ffmpeg does the encoding. The frame-pacing, packing and
/// status shape follow NdiDriver, the other driver that turns the rendered buffer into video.
/// Author: projectMM original

#include "core/Control.h"
#include "core/HttpServerModule.h"   // servedPort: the url control must not lie after a port change
#include "core/ScratchBuffer.h"
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace mm {

/// Driver that publishes the layer as an H.264/HLS stream via a spawned ffmpeg.
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
        // Bitrate is an identity-like magnitude a user types, not sweeps: a number field.
        controls_.addControl("bitrate", bitrateKbit, 500, 40000);
        controls_.setNumberField(controls_.count() - 1);
        // The ffmpeg video encoder. libx264 exists in every ffmpeg build; the hardware entries
        // offload the encode entirely, worth picking on large grids. One this ffmpeg lacks
        // fails the spawn and the status says so.
        controls_.addSelect("encoder", encoderSel_, kEncoderOptions, kEncoderOptionCount);
        // The playable address, one copy away from VLC or a Safari AirPlay hand-off.
        controls_.addReadOnly("url", urlBuf_, sizeof(urlBuf_));
    }

    /// A pacing or rate change needs a new encode (ffmpeg fixes both at spawn); geometry changes
    /// arrive through prepare() already.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "targetFps") == 0 || std::strcmp(name, "bitrate") == 0 ||
               std::strcmp(name, "encoder") == 0 || isCorrectionControl(name);
    }

    void prepare() override {
        release();
        if (!layer_) return;

        width_  = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        height_ = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;

        const size_t pixels = static_cast<size_t>(width_) * height_;
        if (!rgb_.resize(pixels * 3)) {
            setStatus("out of memory for the video frame", Severity::Error);
            return;
        }
        if (correction_.outChannels > 3) corrScratch_.resize(correction_.outChannels);

        platform::fsMkdir(kSegmentDir);
        clearSegments();                     // a stale playlist must not serve the old geometry
        restartsLeft_ = kMaxRestarts;
        if (!startEncoder()) return;         // startEncoder set the status

        const uint16_t port = HttpServerModule::servedPort();
        std::snprintf(urlBuf_, sizeof(urlBuf_), "http://%s:%u/hls/stream.m3u8",
                      platform::hostIp(), port ? port : 8080u);
        std::snprintf(statusBuf_, sizeof(statusBuf_), "streaming %ux%u at %u fps",
                      static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                      static_cast<unsigned>(targetFps));
        setStatus(statusBuf_, Severity::Status);
    }

    void release() override {
        if (open_) {
            platform::encoderStop();
            open_ = false;
            clearSegments();                 // transient output; nothing to keep
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
        const uint32_t now = platform::millis();
        if (now - lastSendMs_ < 1000u / targetFps) return;
        lastSendMs_ = now;

        const nrOfLightsType want = static_cast<nrOfLightsType>(width_) * height_;
        const nrOfLightsType have = sourceBuffer_->count();
        const nrOfLightsType n    = want < have ? want : have;
        if (n == 0 || rgb_.count() < static_cast<size_t>(want) * 3) return;

        const uint8_t* src   = sourceBuffer_->data();
        const uint8_t  srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t  outCh = correction_.outChannels;
        if (srcCh < 3) return;   // a non-color buffer (DMX roles) has no frame to send

        // Pack tight RGB with the per-driver output correction, exactly as NdiDriver does: the
        // stream shows what the wall shows.
        uint8_t* dst = &rgb_[0];
        const bool wide = outCh > 3 && corrScratch_.count() >= outCh;
        for (nrOfLightsType i = 0; i < n; i++) {
            const uint8_t* s = src + static_cast<size_t>(i) * srcCh;
            uint8_t* d = dst + static_cast<size_t>(i) * 3;
            if (outCh == 3) {
                correction_.apply(s, d);
            } else if (wide) {
                uint8_t* c = &corrScratch_[0];
                correction_.apply(s, c);
                d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
            } else {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
        }
        if (n < want) std::memset(dst + static_cast<size_t>(n) * 3, 0,
                                  static_cast<size_t>(want - n) * 3);

        // Non-blocking hand-off: a full pipe drops the frame (the encoder is behind, H.264
        // carries on from the next one), a dead process schedules a restart from tick1s.
        const int wrote = platform::encoderWrite(dst, static_cast<size_t>(want) * 3);
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

    /// The exact argv handed to ffmpeg, exposed for the unit test that pins it: raw RGB in at the
    /// grid size, zerolatency x264, 1 s segments with a short rolling playlist (the live tuning
    /// that puts glass-to-glass at 2-5 s), segments deleted as they fall off the playlist.
    size_t buildArgs(const char* argv[], size_t cap, char* geo, size_t geoCap,
                     char* rate, size_t rateCap, char* gop, size_t gopCap,
                     char* bv, size_t bvCap, char* out, size_t outCap) const {
        std::snprintf(geo, geoCap, "%ux%u", static_cast<unsigned>(width_),
                      static_cast<unsigned>(height_));
        std::snprintf(rate, rateCap, "%u", static_cast<unsigned>(targetFps));
        // GOP = one segment: hls_time can only cut on a keyframe, so a 2x-fps GOP silently
        // doubles every segment (and the latency) past the 1 s design.
        std::snprintf(gop, gopCap, "%u", static_cast<unsigned>(targetFps));
        std::snprintf(bv, bvCap, "%uk", static_cast<unsigned>(bitrateKbit));
        std::snprintf(out, outCap, "%s%s/stream.m3u8", platform::fsRootPath(), kSegmentDir);
        // Assembled by index so the x264-only tuning flags stay off other encoders
        // (h264_videotoolbox rejects -tune) without duplicated slots.
        const char* encoder = kEncoderOptions[encoderSel_ < kEncoderOptionCount ? encoderSel_ : 0];
        const bool x264 = std::strcmp(encoder, "libx264") == 0;
        size_t i = 0;
        auto add = [&](const char* a) { if (i + 1 < cap) argv[i++] = a; };
        for (const char* a : std::initializer_list<const char*>{
                 "ffmpeg", "-hide_banner", "-loglevel", "error",
                 "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", geo,
                 "-r", rate, "-i", "-",
                 "-c:v", encoder }) add(a);
        if (x264) { add("-preset"); add("veryfast"); add("-tune"); add("zerolatency"); }
        for (const char* a : std::initializer_list<const char*>{
                 "-g", gop, "-b:v", bv,
                 "-f", "hls", "-hls_time", "1", "-hls_list_size", "6",
                 "-hls_flags", "delete_segments+temp_file", out }) add(a);   // temp_file: the playlist lands by RENAME, never served half-written
        argv[i] = nullptr;
        return i;
    }

    // Controls
    /// Encode-rate ceiling; the render loop runs faster and extra frames are not encoded.
    uint8_t  targetFps   = 30;
    /// H.264 target bitrate in kbit/s. 8000 carries a 512x512 grid comfortably.
    uint16_t bitrateKbit = 8000;
    /// The ffmpeg encoder pick; hardware entries offload the encode (see the control's comment).
    uint8_t  encoderSel_ = 0;   // index into kEncoderOptions; 0 = libx264, the universal default

private:
    static constexpr uint8_t kMaxRestarts = 3;

    static constexpr uint32_t kWarmupMs = 750;   // encoder init headroom before the first frame
    static constexpr const char* kEncoderOptions[] = {
        "libx264",            // software, in every ffmpeg build: the safe default
        "h264_videotoolbox",  // macOS media engine
        "h264_vaapi",         // Linux VAAPI
        "h264_v4l2m2m",       // Raspberry Pi
        "h264_nvenc",         // NVIDIA
    };
    static constexpr uint8_t kEncoderOptionCount = 5;

    bool startEncoder() {
        const char* argv[40];
        char geo[16], rate[8], gop[8], bv[12], out[192];
        buildArgs(argv, sizeof(argv) / sizeof(argv[0]), geo, sizeof(geo), rate, sizeof(rate),
                  gop, sizeof(gop), bv, sizeof(bv), out, sizeof(out));
        if (!platform::encoderStart(argv)) {
            setStatus("ffmpeg not found - see the docs", Severity::Warning);
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
    lengthType     width_  = 0;
    lengthType     height_ = 0;
    bool           open_   = false;
    bool           encoderDied_ = false;
    bool           statusStale_ = false;
    uint8_t        restartsLeft_ = kMaxRestarts;
    uint8_t        restartWaitS_ = 0;    // backoff countdown, in tick1s steps
    uint8_t        healthySecs_  = 0;    // sustained-health counter that replenishes the budget
    uint32_t       lastSendMs_ = 0;
    uint32_t       warmupUntilMs_ = 0;
    uint32_t       droppedFrames_ = 0;
    uint32_t       lastReportedDrops_ = 0;
    ScratchBuffer<uint8_t> rgb_;          // tight RGB staging, sized in prepare()
    ScratchBuffer<uint8_t> corrScratch_;  // one corrected light, when the wiring is wider than RGB
    char           urlBuf_[64]{};
    char           statusBuf_[64]{};
};

}  // namespace mm
