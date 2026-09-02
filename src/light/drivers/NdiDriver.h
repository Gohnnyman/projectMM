#pragma once
/// NdiDriver, projectMM as an NDI video source.
///
/// The rendered frame reaches OBS, Resolume, TouchDesigner or any other NDI receiver, on this
/// machine or another.
///
/// **Why NDI and not Spout/Syphon.** One implementation covers Windows, macOS, Linux and ARM, it
/// discovers by name, and it crosses machines. Spout (Windows) and Syphon (macOS) share a GPU texture
/// zero-copy and are bit-exact, but they are same-machine only, are TWO platform-specific
/// implementations, and leave Linux and the Pi with nothing. At LED-wall pixel counts the latency
/// difference sits far below one frame of the render loop, so coverage decides, not latency.
///
/// **Desktop only** (`platform::hasNdi`): the NDI runtime is a closed binary built only for Intel
/// and ARM (SSSE3 / NEON floor), so no ESP32 can load one and there is no source to port. An ESP32
/// reaches the same receivers over Art-Net / sACN / DDP, which projectMM implements itself.
///
/// **The runtime is the user's.** projectMM is GPL-3.0 and the NDI runtime is proprietary, so it is
/// never bundled or linked, the platform layer resolves it on demand, exactly as it does Npcap for
/// the panel-card driver. A machine without it runs normally and this driver says so in its status.
/// The whole NDI surface lives behind `platform::` (see platform.h § NDI); no NDI type appears here.
///
/// Prior art: the NDI protocol and SDK are NewTek/Vizrt's; this driver is our own code against the
/// documented C API. The frame-pacing and status shape follow PreviewDriver, the other driver that
/// turns the rendered buffer into frames for a remote consumer.
/// Author: projectMM original

#include "core/Control.h"
#include "core/ScratchBuffer.h"
#include "light/drivers/DriverBase.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Driver that publishes the layer as an NDI video source.
class NdiDriver : public DriverBase {
public:
    static constexpr const char* kTags = "🖥️";

    // ScratchBuffer registers with its owning module, so it takes *this — that registration is what
    // puts these buffers in the memory report rather than leaving them untracked.
    NdiDriver() : rgb_(*this), corrScratch_(*this) {}

    const char* tags() const override { return kTags; }

    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    void defineDriverControls() override {
        // The name a receiver lists. Blank means the device's own name, which is what a user
        // scanning OBS's source list expects to see.
        controls_.addText("sourceName", sourceName, sizeof(sourceName));
        controls_.addControl("fps", fps, 1, 120);
    }

    /// A geometry change resizes the frame; a name change re-creates the sender (NDI has no rename).
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "sourceName") == 0 || isCorrectionControl(name);
    }

    void prepare() override {
        release();
        if (!layer_) return;

        width_  = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        height_ = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;

        if (!platform::ndiAvailable()) {
            // Not an error: the feature is simply not installed, and the fix is a user action.
            setStatus("NDI runtime not installed - see the docs", Severity::Warning);
            return;
        }
        if (!platform::ndiSenderOpen(sourceName[0] ? sourceName : nullptr)) {
            setStatus("could not create the NDI source", Severity::Error);
            return;
        }
        // Set BEFORE the staging below, so release() owns the sender from the moment it exists: a
        // later failure here would otherwise leave the source advertised on the network forever
        // with nothing ever sent, since release() closes only what open_ claims.
        open_ = true;

        // Size the staging off the hot path: one tight-RGB frame, plus a per-light correction
        // scratch when the wiring emits more channels than the three NDI carries.
        const size_t pixels = static_cast<size_t>(width_) * height_;
        if (!rgb_.resize(pixels * 3)) {
            release();                       // closes the sender we just opened
            setStatus("out of memory for the NDI frame", Severity::Error);
            return;
        }
        if (correction_.outChannels > 3) corrScratch_.resize(correction_.outChannels);

        std::snprintf(statusBuf_, sizeof(statusBuf_), "sending %ux%u at %u fps",
                      static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                      static_cast<unsigned>(fps));
        setStatus(statusBuf_, Severity::Status);
    }

    void release() override {
        if (open_) { platform::ndiSenderClose(); open_ = false; }
    }

    void tick() MM_NONBLOCKING override {
        if (!open_ || fps == 0 || !sourceBuffer_ || !sourceBuffer_->data()) return;

        // fps is a CEILING, as in PreviewDriver: NDI's own clock_video paces the receiver, this
        // stops us building frames faster than we declared.
        const uint32_t now = platform::millis();
        if (now - lastSendMs_ < 1000u / fps) return;
        lastSendMs_ = now;

        const nrOfLightsType want = static_cast<nrOfLightsType>(width_) * height_;
        const nrOfLightsType have = sourceBuffer_->count();
        const nrOfLightsType n    = want < have ? want : have;
        if (n == 0) return;

        // Pack to tight RGB for the seam. The correction (brightness, preset, white mode) is the
        // per-driver output correction every driver applies, so an NDI receiver sees what the wall
        // sees. Falls back to the raw channels when correction is not usable, matching
        // NetworkSendDriver's guard rather than inventing a second policy.
        if (rgb_.count() < static_cast<size_t>(want) * 3) return;
        uint8_t* dst = &rgb_[0];
        const uint8_t* src   = sourceBuffer_->data();
        const uint8_t  srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t  outCh = correction_.outChannels;
        if (srcCh < 3) return;   // a non-color buffer (DMX roles) has no frame to send

        // outChannels == 3 corrects straight into the destination; a wider wiring (RGBW and up)
        // corrects into a one-light scratch and takes the first three, since NDI carries RGB only.
        const bool wide = outCh > 3 && corrScratch_.count() >= outCh;
        for (nrOfLightsType i = 0; i < n; i++) {
            const uint8_t* s = src + static_cast<size_t>(i) * srcCh;
            uint8_t* d = dst + static_cast<size_t>(i) * 3;
            if (outCh == 3) {
                correction_.apply(s, d, srcCh);
            } else if (wide) {
                uint8_t* c = &corrScratch_[0];
                correction_.apply(s, c, srcCh);
                d[0] = c[0]; d[1] = c[1]; d[2] = c[2];
            } else {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];   // passthrough, same fallback as NetworkSend
            }
        }
        // A layer smaller than the frame leaves the tail from the previous send; blank it so a
        // shrunk layout cannot show stale pixels.
        if (n < want) std::memset(dst + static_cast<size_t>(n) * 3, 0,
                                  static_cast<size_t>(want - n) * 3);

        platform::ndiSendFrame(dst, static_cast<uint16_t>(width_),
                               static_cast<uint16_t>(height_), fps);
    }

    // Controls
    /// The name a receiver lists this source under. Blank uses the device's own name, which is what
    /// a user scanning OBS's source list expects to find.
    char    sourceName[32] = "";
    /// Frame-rate ceiling. The driver sends no faster than this and declares the rate in each frame;
    /// the link may deliver fewer.
    uint8_t fps            = 30;

private:
    Buffer*        sourceBuffer_ = nullptr;
    lengthType     width_  = 0;
    lengthType     height_ = 0;
    bool           open_   = false;
    uint32_t       lastSendMs_ = 0;
    ScratchBuffer<uint8_t> rgb_;          // tight RGB staging, sized in prepare()
    ScratchBuffer<uint8_t> corrScratch_;  // one corrected light, when the wiring is wider than RGB
    char           statusBuf_[64]{};
};

}  // namespace mm
