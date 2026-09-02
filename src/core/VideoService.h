#pragma once

#include "core/ActiveInstance.h" // the one-active-source seat (RAII vacate on destruct)
#include "core/color.h"          // RGB — the pattern's band colours
#include "core/MoonModule.h"
#include "core/ScratchBuffer.h"
#include "core/VideoFrame.h"
#include "platform/platform.h" // fsSize / fsReadAt / millis

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// The device's video input — one decoded RGB frame per tick, published through the static
/// `latestFrame()`. Decoded once here however many effects read it, and effects hold no pointer
/// to this module.
///
/// Two sources. `test pattern` is a DIAGNOSTIC, not decoration: its coloured border bands make a
/// border-mapped effect's orientation self-evident, so a mis-set `startCorner` shows up as the
/// wrong physical edge lighting rather than as a subtly wrong picture. `file` reads a binary PPM.
///
/// PPM rather than JPEG because there is no software JPEG decoder here — the real capture path uses
/// the P4's JPEG hardware behind the platform layer, and adding one for the desktop build would buy
/// a dependency for a convenience. USB capture lands as a third source filling the same buffer.
///
/// Not auto-wired: the user adds it under the `Services` container
class VideoService : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    // 0 = test pattern, 1 = PPM file, 2 = USB capture (platform::hasUsbVideo only). Appended, so a
    // persisted index keeps its meaning.
    uint8_t source = 0;
    char file[64] = "/frame.ppm";
    uint8_t usbFormat = 0; // index into the device's advertised list; the only USB setting persisted
    uint16_t staleMs = 2000;   // 0 = hold the last frame forever

    static constexpr const char* kSourceOptions[] = {"test pattern", "file", "usb"};
    // A target with no High-Speed USB host or no JPEG decoder cannot capture, so it is not offered
    // the option — the two software sources still work everywhere.
    static constexpr uint8_t kSourceCount = platform::hasUsbVideo ? 3 : 2;

    // Synthesised-pattern extent. Small on purpose: a border effect averages the frame down to a
    // few dozen values, so pixels beyond that buy nothing but bandwidth and decode time. 16:9.
    static constexpr uint16_t kPatternW = 64;
    static constexpr uint16_t kPatternH = 36;
    static constexpr int kBand = kPatternH / 4; // thickness of each coloured edge
    // Sanity ceiling for a loaded file - comfortably past 4K, so a corrupt header is rejected at
    // parse time with a clear message instead of failing later as "too large for memory". What
    // actually bounds the allocation is buf_.resize() failing, which allocate() handles.
    static constexpr uint32_t kMaxDim = 4096;

    /// The live frame. The POINTER is never null - with no source this returns kNoVideoFrame, which
    /// has a null `rgb`. So callers test the frame's contents, never the pointer
    static const VideoFrame* latestFrame() MM_NONBLOCKING {
        VideoService* v = ActiveInstance<VideoService>::active();
        return v ? &v->frame_ : &kNoVideoFrame;
    }

    VideoService() { seat_.claim(); }

    void defineControls() override {
        controls_.addSelect("source", source, kSourceOptions, kSourceCount);
        controls_.addText("file", file, sizeof(file));
        controls_.setHidden(controls_.count() - 1, source != 1);
        controls_.addButton("reload");
        controls_.setHidden(controls_.count() - 1, source != 1);
        // The device decides what is on offer, so there is nothing to type. Until one has
        // enumerated the control still renders — read-only, holding a placeholder — rather than
        // appearing out of nowhere once a cable is plugged in.
        static constexpr const char* kNoDevice[] = {"no device"};
        const bool known = formatCount_ > 0;
        controls_.addSelect("offered", usbFormat, known ? formatOptions_ : kNoDevice,
                            known ? formatCount_ : 1);
        controls_.setHidden(controls_.count() - 1, source != 2);
        controls_.setReadOnly(controls_.count() - 1, !known);
        // How long a gap in frames is tolerated before the lights go dark. 0 holds the last
        // picture instead, for a source that legitimately stops sending.
        controls_.addUint16("staleMs", staleMs, 0, 10000);
        controls_.setHidden(controls_.count() - 1, source != 2);
        MoonModule::defineControls();
    }

    /// A source switch changes what the buffer must hold, so it re-runs the whole build. The reload
    /// button re-reads the same file in place — cheap, and it must NOT tear down the pipeline.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "source") == 0 || std::strcmp(name, "file") == 0 ||
               std::strcmp(name, "offered") == 0;
    }

    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "reload") == 0) loadFile();
        if (std::strcmp(name, "offered") == 0) applyFormat();
        MoonModule::onControlChanged(name);
    }

    /// Cold path: size the frame buffer for the selected source and fill it once, so a frame exists
    /// before the first tick rather than one tick later.
    void prepare() override {
        seat_.claim(); // re-take after a disable/enable cycle — release() vacated it
        platform::videoCaptureDeinit(capture_); // a source switch releases the device
        if (source >= kSourceCount) source = 0;   // a config restored from a capture-capable board
        if (source == 2) {
            // The first open doubles as a probe: a device only lists its formats once it
            // enumerates, which happens inside init — so open, learn what is really on offer, and
            // open again when a restored pick differs. Only the last attempt reports, or a failed
            // probe would leave an error over the retry that fixed it.
            bool open = platform::videoCaptureInit(capture_, usbWidth, usbHeight, usbFps);
            readFormats();
            if (applyFormat()) {
                platform::videoCaptureDeinit(capture_);
                open = platform::videoCaptureInit(capture_, usbWidth, usbHeight, usbFps);
            }
            if (!open) {
                fail("no capture device");
            } else {
                std::snprintf(status_, sizeof(status_), "%ux%u %ufps", usbWidth, usbHeight, usbFps);
                setStatus(status_, Severity::Status);
            }
        } else if (source == 1) {
            loadFile();
        } else {
            if (!allocate(kPatternW, kPatternH)) return;
            renderPattern();
        }
    }

    /// Only the synthesised pattern regenerates per frame, since it animates; a still file keeps the
    /// buffer it already holds. File I/O is blocking and belongs nowhere near this function.
    void tick() MM_NONBLOCKING override {
        // Take an EMPTY seat, so deleting the elected source while a second one runs hands over
        // rather than going permanently dark. claim() only fills an empty seat, never yanks one.
        seat_.claim();
        if (source == 0 && buf_.data()) renderPattern();
        else if (source == 2) readCapture();
        MoonModule::tick();
    }

    void release() override {
        platform::videoCaptureDeinit(capture_);
        seat_.vacate();
        frame_ = VideoFrame{};
        MoonModule::release();
    }

private:
    // The one-active-source election. Claimed at CONSTRUCTION (an effect resolves latestFrame()
    // during its own build, before this module's prepare()), re-claimed in prepare() after a
    // disable/enable, and in tick() so a survivor inherits an empty seat.
    ActiveInstance<VideoService> seat_{*this};

    /// Resolve the selected row into the request fields. True when that changed something — the
    /// index survives a reboot but the list behind it does not, so this is how a restored pick
    /// reaches the device.
    bool applyFormat() {
        if (usbFormat >= formatCount_) return false;
        const platform::VideoCaptureFormat& f = formats_[usbFormat];
        const bool changed = f.width != usbWidth || f.height != usbHeight || f.fps != usbFps;
        usbWidth = f.width;
        usbHeight = f.height;
        usbFps = f.fps;
        return changed;
    }

    /// Cold path: cache what the device advertises as dropdown labels. Kept out of
    /// defineControls(), which must stay pure — it only reads what this leaves behind.
    void readFormats() {
        const uint8_t was = formatCount_;
        formatCount_ = static_cast<uint8_t>(platform::videoCaptureFormats(formats_, kMaxFormats));
        for (uint8_t i = 0; i < formatCount_; i++) {
            std::snprintf(formatLabels_[i], sizeof(formatLabels_[i]), "%ux%u %ufps", formats_[i].width,
                          formats_[i].height, formats_[i].fps);
            formatOptions_[i] = formatLabels_[i];
        }
        if (usbFormat >= formatCount_) usbFormat = 0;
        if (formatCount_ != was) rebuildControls(); // the dropdown appeared, or its length changed
    }

    /// Publish the newest decoded frame. Unlike the other sources this does not fill buf_ — the
    /// JPEG decoder owns its output buffer (it writes it by DMA, with its own alignment), so the
    /// frame borrows that instead.
    void readCapture() MM_NONBLOCKING {
        uint16_t w = 0, h = 0;
        const uint8_t* rgb = platform::videoCaptureFrame(capture_, w, h);
        if (rgb) {
            lastFrameMs_ = platform::millis();
            frame_.rgb = rgb;
            frame_.width = w;
            frame_.height = h;
            publish();
            return;
        }
        // A gap of one tick is normal — the decoder runs at its own rate. A long one means the
        // source stopped (a console asleep, a cable out), and holding the last picture would leave
        // the room lit by a frozen frame. Dropping it makes every effect fall back to black.
        if (staleMs && frame_.rgb && platform::millis() - lastFrameMs_ > staleMs) frame_ = VideoFrame{};
    }

    platform::VideoCaptureHandle capture_;

    // Derived from the selected row, never typed — what actually gets requested of the device, and
    // the opening bid before one has listed its formats. 16:9 on purpose: a 4:3 capture makes a
    // 16:9 source letterbox into it, and the border zones then average bars instead of picture.
    uint16_t usbWidth = 848;
    uint16_t usbHeight = 480;
    uint8_t usbFps = 60;

    uint32_t lastFrameMs_ = 0;

    static constexpr uint8_t kMaxFormats = 24;
    platform::VideoCaptureFormat formats_[kMaxFormats] = {};
    char formatLabels_[kMaxFormats][24] = {};
    const char* formatOptions_[kMaxFormats] = {};
    uint8_t formatCount_ = 0;
    ScratchBuffer<uint8_t> buf_{*this}; // width*height*3, accounted in dynamicBytes()
    VideoFrame frame_;
    uint32_t seq_ = 0;
    char status_[24] = {};

    /// Drop the published frame and say why. Returns false so every failing path reads as one line,
    /// `return fail("...")`, and none can forget to un-publish the stale frame.
    bool fail(const char* why) {
        frame_ = VideoFrame{};
        setStatus(why, Severity::Error);
        return false;
    }

    /// Size the buffer and point the published frame at it. False on any failure, so a too-large
    /// image degrades to "no video" rather than to a crash.
    bool allocate(uint16_t w, uint16_t h) {
        if (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim) return fail("frame size out of range");
        if (!buf_.resize(static_cast<uint32_t>(w) * h * 3u)) return fail("frame too large for memory");
        frame_.rgb = buf_.data();
        frame_.width = w;
        frame_.height = h;
        // Published here, not from a tick: dimensions only change on a resize, so this keeps the
        // snprintf off the render path.
        std::snprintf(status_, sizeof(status_), "%ux%u", w, h);
        setStatus(status_, Severity::Status);
        return true;
    }

    /// Publish the buffer as a NEW frame — the sequence bump is what tells a consumer the pixels
    /// changed, so every producer path ends here (see VideoFrame::seq).
    void publish() { frame_.seq = ++seq_; }

    // Four coloured border bands and a sweeping white block. Integer-only and allocation-free — it
    // runs on the render tick.
    void renderPattern() {
        uint8_t* p = buf_.data();
        if (!p) return;
        // One sweep every ~4 s, so motion is obvious without being frantic.
        const int sweepX = static_cast<int>((platform::millis() / 60u) % kPatternW);
        for (int y = 0; y < kPatternH; y++) {
            for (int x = 0; x < kPatternW; x++) {
                // A white block riding the top edge: shows liveness, and which way "forward" runs.
                const bool onSweep = y < kBand && x >= sweepX && x < sweepX + 4;
                const RGB c = onSweep ? RGB{255, 255, 255} : bandColour(x, y);
                uint8_t* px = p + (static_cast<size_t>(y) * kPatternW + x) * 3;
                px[0] = c.r;
                px[1] = c.g;
                px[2] = c.b;
            }
        }
        publish();
    }

    /// Colour of the pattern at (x, y): one hue per edge, black interior.
    static RGB bandColour(int x, int y) {
        if (y < kBand) return {255, 0, 0};              // top    → red
        if (y >= kPatternH - kBand) return {0, 0, 255}; // bottom → blue
        if (x < kBand) return {255, 255, 0};            // left   → yellow
        if (x >= kPatternW - kBand) return {0, 255, 0}; // right  → green
        return {0, 0, 0};                               // interior stays dark
    }

    // --- PPM (P6) file source -----------------------------------------------------------------
    /// Read the header, size the buffer, then read the pixel block straight into it. Cold path only
    /// (prepare / the reload button): this blocks on the filesystem.
    bool loadFile() {
        const long size = platform::fsSize(file);
        if (size <= 0) return fail("file not found");

        char header[64] = {};
        const int headerLen = platform::fsReadAt(file, 0, header, sizeof(header) - 1);
        uint16_t w = 0, h = 0;
        const int pixOff = parsePpmHeader(header, headerLen, w, h);
        if (pixOff < 0) return fail("not a binary PPM (P6, maxval 255)");
        if (!allocate(w, h)) return false; // allocate() already reported why

        const uint32_t need = static_cast<uint32_t>(w) * h * 3u;
        if (static_cast<uint32_t>(size - pixOff) < need) return fail("PPM truncated");

        const int read = platform::fsReadAt(file, pixOff, reinterpret_cast<char*>(buf_.data()), need);
        if (read < 0 || static_cast<uint32_t>(read) != need) return fail("PPM read failed");

        setStatus(status_, Severity::Status); // allocate() formatted the size; restore it over an error
        publish();
        return true;
    }

public:
    /// Parse a binary-PPM header (Netpbm). Returns the byte offset where pixel data begins, or -1
    /// if `buf` is not one. Pure: `len` bounds the read, so a truncated file is rejected rather than
    /// parsed into whatever follows it.
    static int parsePpmHeader(const char* buf, int len, uint16_t& w, uint16_t& h) {
        // P6 <width> <height> <maxval> <ONE whitespace byte> <pixels>
        HeaderCursor cur{buf, len};
        cur.skipBlanks();
        if (cur.pos + 1 >= len || buf[cur.pos] != 'P' || buf[cur.pos + 1] != '6') return -1;
        cur.pos += 2;

        const long ww = cur.readInt();
        const long hh = cur.readInt();
        const long maxval = cur.readInt();
        if (ww <= 0 || ww > static_cast<long>(kMaxDim)) return -1;
        if (hh <= 0 || hh > static_cast<long>(kMaxDim)) return -1;
        if (maxval != 255) return -1;  // 16-bit samples are two big-endian bytes — another format
        if (cur.pos >= len) return -1; // no separator byte, so no pixel data can follow

        w = static_cast<uint16_t>(ww);
        h = static_cast<uint16_t>(hh);
        return cur.pos + 1; // one whitespace byte separates the header from the pixels
    }

private:
    /// Position within an ASCII header. Netpbm separates tokens with any run of whitespace and `#`
    /// comments to end-of-line, so both readers skip those first.
    struct HeaderCursor {
        const char* buf;
        int len;
        int pos = 0;

        void skipBlanks() {
            while (pos < len) {
                if (buf[pos] == '#') {
                    while (pos < len && buf[pos] != '\n') pos++;
                } else if (buf[pos] == ' ' || buf[pos] == '\t' || buf[pos] == '\n' || buf[pos] == '\r') {
                    pos++;
                } else {
                    break;
                }
            }
        }

        /// Next decimal token, or -1 when the next token is not one. Capped well above any real
        /// dimension purely so a long digit run cannot overflow; the true bounds are the caller's.
        long readInt() {
            skipBlanks();
            if (pos >= len || buf[pos] < '0' || buf[pos] > '9') return -1;
            long v = 0;
            while (pos < len && buf[pos] >= '0' && buf[pos] <= '9') {
                v = v * 10 + (buf[pos] - '0');
                if (v > 100000) return -1;
                pos++;
            }
            return v;
        }
    };
};

} // namespace mm
