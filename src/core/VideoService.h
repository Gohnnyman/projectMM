#pragma once

#include "core/ActiveInstance.h" // the one-active-source seat (RAII vacate on destruct)
#include "core/Scheduler.h"      // requestPrepareTree: a hotplug needs a cold-path rebuild
#include "core/color.h"          // RGB: the pattern's band colors
#include "core/MoonModule.h"
#include "core/ScratchBuffer.h"
#include "core/VideoFrame.h"
#include "platform/platform.h" // fsSize / fsReadAt / millis

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mm {

/// The device's video input: one decoded RGB frame per tick, published through the static
/// `latestFrame()`. Decoded once here however many effects read it, and effects hold no pointer
/// to this module.
///
/// Three sources. `test pattern` is a DIAGNOSTIC, not decoration: its colored border bands make a
/// border-mapped effect's orientation self-evident, so a mis-set `startCorner` shows up as the
/// wrong physical edge lighting rather than as a subtly wrong picture. `file` reads a binary PPM.
/// `usb` captures from an HDMI grabber, where the platform has the hardware for it.
///
/// PPM rather than JPEG because there is no software JPEG decoder here. the real capture path uses
/// the P4's JPEG hardware behind the platform layer, and adding one for the desktop build would buy
/// a dependency for a convenience.
///
/// Ownership of the pixels differs per source: the software ones render into buf_, which this
/// module owns; usb BORROWS the decoder's output buffer, which the platform owns while the device
/// is open. So the device is only ever closed through closeCapture(), which drops the frame first.
///
/// Not auto-wired: the user adds it under the `Services` container
class VideoService : public MoonModule {
public:
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    // Appended, never reordered, so a persisted index keeps its meaning.
    static constexpr uint8_t kSourcePattern = 0;
    static constexpr uint8_t kSourceFile = 1;
    static constexpr uint8_t kSourceUsb = 2; // platform::hasUsbVideo only
    static constexpr const char* kSourceOptions[] = {"test pattern", "file", "usb"};
    // A target with no High-Speed USB host or no JPEG decoder cannot capture, so it is not offered
    // the option: the two software sources still work everywhere.
    static constexpr uint8_t kSourceCount = platform::hasUsbVideo ? 3 : 2;

    uint8_t source = kSourcePattern;
    char file[64] = "/frame.ppm";
    uint8_t usbFormat = 0;   // index into the device's advertised list; the only USB setting persisted
    uint16_t staleMs = 2000; // gap tolerated before the lights go dark

    // The source's transfer curve. MJPEG carries no HDR metadata, so this is declared, not
    // detected.
    static constexpr uint8_t kHdrOff = 0, kHdrPq = 1, kHdrHlg = 2;
    static constexpr const char* kHdrOptions[] = {"off", "HDR10 (PQ)", "HLG"};
    static constexpr uint8_t kHdrCount = 3;
    uint8_t hdr = kHdrOff;
    // PQ is absolute luminance, so it needs a reference white. Too low and bright channels clamp,
    // dragging saturated hues toward their neighbors; too high and the picture reads dim.
    uint16_t hdrNits = 2000;
    // Sweep rate of the test pattern's white block, in PIXELS PER SECOND. 0 parks it, which makes
    // the pattern a still reference for checking a border light against a known color. 17 is the
    // rate it used to be hard-coded to: a sweep every ~4 s.
    uint8_t patternSpeed = 17;

    // Synthesized-pattern extent. Small on purpose: a border effect averages the frame down to a
    // few dozen values, so pixels beyond that buy nothing but bandwidth and decode time. 16:9.
    static constexpr uint16_t kPatternW = 64;
    static constexpr uint16_t kPatternH = 36;
    static constexpr int kBand = kPatternH / 4; // thickness of each colored edge
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

    /// Test seam: the curve as built.
    const uint8_t* toneForTest() const { return tone_; }

    VideoService() { seat_.claim(); }

    void defineControls() override {
        controls_.addSelect("source", source, kSourceOptions, kSourceCount);
        // Live (not in affectsPrepare): the rate changes what the NEXT frame draws, and nothing
        // about the buffer, so it must not tear the pipeline down to take effect.
        controls_.addControl("patternSpeed", patternSpeed, 0, 255);
        controls_.setHidden(controls_.count() - 1, source != kSourcePattern);
        controls_.addText("file", file, sizeof(file));
        controls_.setHidden(controls_.count() - 1, source != kSourceFile);
        controls_.addButton("reload");
        controls_.setHidden(controls_.count() - 1, source != kSourceFile);
        // The device decides what is on offer, so there is nothing to type. Until one has
        // enumerated the control still renders (read-only, holding a placeholder) rather than
        // appearing out of nowhere once a cable is plugged in.
        static constexpr const char* kNoDevice[] = {"no device"};
        const bool known = formatCount_ > 0;
        controls_.addSelect("offered", usbFormat, known ? formatOptions_ : kNoDevice,
                            known ? formatCount_ : 1);
        controls_.setHidden(controls_.count() - 1, source != kSourceUsb);
        controls_.setReadOnly(controls_.count() - 1, !known);
        // How long a gap in frames is tolerated before the lights go dark. Floored above a frame
        // interval, not 0: the render loop outruns the capture, so ordinary gaps between frames
        // would otherwise read as loss and strobe the room.
        controls_.addControl("staleMs", staleMs, 100, 10000);
        controls_.setHidden(controls_.count() - 1, source != kSourceUsb);
        // Live: changes how the buffer is read, not its size. Capture-only; other sources are
        // authored display-encoded.
        controls_.addSelect("hdr", hdr, kHdrOptions, kHdrCount);
        controls_.setHidden(controls_.count() - 1, source != kSourceUsb);
        controls_.addControl("hdrNits", hdrNits, 100, 10000);
        controls_.setHidden(controls_.count() - 1, source != kSourceUsb || hdr != kHdrPq);
        MoonModule::defineControls();
    }

    /// A source switch changes what the buffer must hold, so it re-runs the whole build. The reload
    /// button re-reads the same file in place: cheap, and it must NOT tear down the pipeline.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "source") == 0 || std::strcmp(name, "file") == 0 ||
               std::strcmp(name, "offered") == 0;
    }

    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "reload") == 0) loadFile();
        if (std::strcmp(name, "offered") == 0) applyFormat();
        if (std::strcmp(name, "hdr") == 0 || std::strcmp(name, "hdrNits") == 0) rebuildTone();
        MoonModule::onControlChanged(name);
    }

    /// Cold path: size the frame buffer for the selected source and fill it once, so a frame exists
    /// before the first tick rather than one tick later.
    void prepare() override {
        seat_.claim();  // re-take after a disable/enable cycle: release() vacated it
        if (source >= kSourceCount) source = kSourcePattern; // a config restored from a capture-capable board
        if (source == kSourceUsb) {
            // Resolve the selected row FIRST: usbWidth/Height are what captureCurrent() compares
            // against, and they only ever moved inside openCapture(). A restored usbFormat that
            // arrives after the first prepare would otherwise never reach them, so the check would
            // keep reporting the stale request as current and never reopen.
            applyFormat();
            rebuildTone(); // a restored `hdr` lands as a VALUE, never through onControlChanged
            // Every tree-wide rebuild lands here too (a layout resized, a module added), and the
            // device stays open through those: a reopen drops the published frame and blocks on
            // negotiation. It happens only for what actually changed the request.
            if (!captureCurrent()) {
                closeCapture();
                openCapture();
            }
        } else {
            closeCapture(); // a source switch releases the device, and the frame borrowed from it
            if (source == kSourceFile) {
                loadFile();
            } else if (allocate(kPatternW, kPatternH)) {
                renderPattern();
            }
        }
    }

    /// Only the synthesized pattern regenerates per frame, since it animates; a still file keeps the
    /// buffer it already holds. File I/O is blocking and belongs nowhere near this function.
    void tick() MM_NONBLOCKING override {
        // Take an EMPTY seat, so deleting the elected source while a second one runs hands over
        // rather than going permanently dark. claim() only fills an empty seat, never yanks one.
        seat_.claim();
        if (source == kSourcePattern && buf_.data())
            renderPattern();
        else if (source == kSourceFile && buf_.data())
            publish();
        else if (source == kSourceUsb)
            readCapture();
        MoonModule::tick();
    }

    /// A grabber plugged in (or back in) enumerates on its own, so its format list arrives with
    /// nothing having asked for it. Notice here (one atomic load) and ask for a rebuild, which runs
    /// prepare() on the render thread: the only thread that may open or close the device, and the
    /// one path by which a device that came back is picked up again.
    void tick1s() MM_NONBLOCKING override {
        if (source == kSourceUsb && (platform::videoCaptureFormatGeneration() != formatGen_ || selectionStale()))
            if (Scheduler* s = Scheduler::instance()) s->requestPrepareTree();
        MoonModule::tick1s();
    }

    /// Does the selected row disagree with what is actually open? Boot restores control VALUES
    /// after prepare() has already run, so a persisted `usbFormat` arrives too late to reach the
    /// device: prepare opened row 0 and nothing asked it to look again. Watching the generation
    /// alone never catches that, because no device came or went. Four int compares.
    bool selectionStale() const MM_NONBLOCKING {
        if (!capture_.impl || usbFormat >= formatCount_) return false;
        const platform::VideoCaptureFormat& f = formats_[usbFormat];
        return f.width != opened_.width || f.height != opened_.height || f.fps != opened_.fps;
    }

    void release() override {
        closeCapture(); // drops the published frame too, whichever source it came from
        seat_.vacate();
        MoonModule::release();
    }

private:
    // The one-active-source election. Claimed at CONSTRUCTION (an effect resolves latestFrame()
    // during its own build, before this module's prepare()), re-claimed in prepare() after a
    // disable/enable, and in tick() so a survivor inherits an empty seat.
    ActiveInstance<VideoService> seat_{*this};

    // --- USB capture source -------------------------------------------------------------------
    /// Open the device at the selected format. The first open doubles as a probe: a device only
    /// lists its formats once it enumerates, which happens inside init, so open, learn what is
    /// really on offer, and open again when a restored pick differs. Only the last attempt reports,
    /// or a failed probe would leave an error over the retry that fixed it.
    void openCapture() {
        bool open = platform::videoCaptureInit(capture_, usbWidth, usbHeight, usbFps);
        readFormats();
        if (applyFormat()) {
            closeCapture();
            open = platform::videoCaptureInit(capture_, usbWidth, usbHeight, usbFps);
        }
        if (!open) {
            fail("no capture device");
            return;
        }
        opened_ = {usbWidth, usbHeight, usbFps};
        // What was ASKED for, until a frame arrives: the device negotiates, and readCapture()
        // replaces this with the dimensions actually being decoded.
        std::snprintf(status_, sizeof(status_), "asked %ux%u", usbWidth, usbHeight);
        setStatus(status_, Severity::Status);
        shownW_ = shownH_ = 0;
    }

    /// Whether the open device already serves the selection: the same format list (a replug, even
    /// of the same grabber, publishes a new generation) and the same requested format. A (re)open
    /// is worth its cost only when one of those moved.
    bool captureCurrent() const {
        return capture_.impl && platform::videoCaptureFormatGeneration() == formatGen_ &&
               opened_.width == usbWidth && opened_.height == usbHeight && opened_.fps == usbFps;
    }

    /// Release the device. The published frame borrows one of ITS buffers (platform.h,
    /// videoCaptureFrame), so it is dropped first: this is the one place that order is decided,
    /// and every teardown path goes through here.
    void closeCapture() {
        frame_ = VideoFrame{};
        opened_ = {};
        platform::videoCaptureDeinit(capture_);
    }

    /// Resolve the selected row into the request fields. True when that changed something: the
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
    /// defineControls(), which must stay pure. it only reads what this leaves behind.
    void readFormats() {
        const uint32_t gen = platform::videoCaptureFormatGeneration();
        const bool changed = gen != formatGen_;
        formatGen_ = gen;
        formatCount_ = static_cast<uint8_t>(platform::videoCaptureFormats(formats_, kMaxFormats));
        for (uint8_t i = 0; i < formatCount_; i++) {
            std::snprintf(formatLabels_[i], sizeof(formatLabels_[i]), "%ux%u %ufps", formats_[i].width,
                          formats_[i].height, formats_[i].fps);
            formatOptions_[i] = formatLabels_[i];
        }
        // Only once there IS a list. An empty one means the device has not enumerated yet, not
        // that the pick is invalid: clamping against 0 threw away a restored index every boot, and
        // the reopen that would have applied it never ran, so the stream stayed on row 0.
        if (formatCount_ && usbFormat >= formatCount_) usbFormat = 0;
        // On the GENERATION, not the count: a replacement device advertising the same number of
        // different formats overwrites the labels in place, and a client with no schema resync
        // would go on offering the old ones.
        if (changed) rebuildControls();
    }

    /// Publish the newest decoded frame. Unlike the other sources this does not fill buf_: the
    /// JPEG decoder owns its output buffer (it writes it by DMA, with its own alignment), so the
    /// frame borrows that instead. The borrow is safe for as long as the device stays open, which
    /// closeCapture() is the only thing to end, and it drops the frame first.
    void readCapture() MM_NONBLOCKING {
        uint16_t w = 0, h = 0;
        const uint8_t* rgb = platform::videoCaptureFrame(capture_, w, h);
        if (rgb) {
            lastFrameMs_ = platform::millis();
            frame_.rgb = rgb;
            frame_.width = w;
            frame_.height = h;
            publish();
            // What the device actually negotiated, once per change (so, in practice, once):
            // compared against what was last SHOWN rather than the frame, which a stale drop resets.
            if (w != shownW_ || h != shownH_) {
                shownW_ = w;
                shownH_ = h;
                std::snprintf(status_, sizeof(status_), "%ux%u", w, h);
                setStatus(status_, Severity::Status);
            }
            return;
        }
        // A gap of one tick is normal: the decoder runs at its own rate. A long one means the
        // source stopped (a console asleep, a cable out), and holding the last picture would leave
        // the room lit by a frozen frame. Dropping it makes every effect fall back to black.
        if (frame_.rgb && platform::millis() - lastFrameMs_ > staleMs) frame_ = VideoFrame{};
    }

    platform::VideoCaptureHandle capture_;
    platform::VideoCaptureFormat opened_ = {}; // the request the open device was made with

    // Derived from the selected row, never typed: what actually gets requested of the device, and
    // the opening bid before one has listed its formats. 640x480 because almost every UVC device
    // offers it, and a bid nothing offers costs a full uvc_host_stream_open timeout (3 s) at every
    // boot before the real list can be read: an MS2130 grabber has no 848x480 at all.
    //
    // It is 4:3, and a 16:9 source letterboxes into it, so the top and bottom zones would average
    // bars rather than picture. That only applies to this first probe: the moment the device lists
    // its formats, `usbFormat` picks the row, and a 16:9 one should be chosen there.
    uint16_t usbWidth = 640;
    uint16_t usbHeight = 480;
    uint8_t usbFps = 60;

    uint32_t lastFrameMs_ = 0;
    uint16_t shownW_ = 0, shownH_ = 0; // the dimensions the status last reported

    static constexpr int kMaxHeaderBytes = 256; // room for a comment, and its own error if not
    static constexpr uint8_t kMaxFormats = platform::kVideoCaptureMaxFormats;
    platform::VideoCaptureFormat formats_[kMaxFormats] = {};
    char formatLabels_[kMaxFormats][24] = {};
    const char* formatOptions_[kMaxFormats] = {};
    uint8_t formatCount_ = 0;
    uint32_t formatGen_ = 0;            // the platform generation formats_ was read at
    ScratchBuffer<uint8_t> buf_{*this}; // width*height*3, accounted in dynamicBytes()
    VideoFrame frame_;
    uint32_t seq_ = 0;
    uint8_t tone_[256] = {}; // the published curve; only read while hdr != off
    // Sweep position in 1/1000 px and the millis() it was last advanced at. Milli-pixels because a
    // per-second rate sampled per tick rounds to zero motion in whole pixels at 1 px/s.
    uint32_t sweepMilliPx_ = 0;
    uint32_t sweepAtMs_ = 0;
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

    /// Publish the buffer as a NEW frame: the sequence bump is what tells a consumer the pixels
    /// changed, so every producer path ends here (see VideoFrame::seq).
    void publish() {
        // Per frame, so a curve change lands on the next one with nothing to remember.
        frame_.tone = (source == kSourceUsb && hdr != kHdrOff) ? tone_ : nullptr;
        frame_.seq = ++seq_;
    }

    /// Fill `tone_`: transfer curve -> linear, scale to the reference white, re-encode sRGB. Cold
    /// path: 256 float evaluations per edit or prepare, never per frame.
    void rebuildTone() {
        if (hdr == kHdrOff) return;
        for (int i = 0; i < 256; i++) {
            const float e = static_cast<float>(i) / 255.0f;
            float lin;
            if (hdr == kHdrHlg) {
                // ARIB STD-B67 inverse OETF. Relative, so hdrNits does not apply. No OOTF: a
                // second-order tilt that border averages do not need.
                constexpr float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f;
                lin = e <= 0.5f ? (e * e) / 3.0f : (std::exp((e - c) / a) + b) / 12.0f;
            } else {
                // SMPTE ST 2084 (PQ) EOTF: absolute nits, referred to hdrNits.
                constexpr float m1 = 2610.0f / 16384.0f;
                constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
                constexpr float c1 = 3424.0f / 4096.0f;
                constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
                constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
                const float p = std::pow(e, 1.0f / m2);
                const float num = p > c1 ? p - c1 : 0.0f;
                const float den = c2 - c3 * p; // > 0 across the whole domain
                const float nits = 10000.0f * std::pow(num / den, 1.0f / m1);
                lin = nits / static_cast<float>(hdrNits ? hdrNits : 1);
            }
            if (lin < 0.0f) lin = 0.0f;
            if (lin > 1.0f) lin = 1.0f;
            const float v = lin <= 0.0031308f ? 12.92f * lin : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
            tone_[i] = static_cast<uint8_t>(v * 255.0f + 0.5f);
        }
    }

    // Four colored border bands and a sweeping white block. Integer-only and allocation-free: it
    // runs on the render tick.
    void renderPattern() {
        uint8_t* p = buf_.data();
        if (!p) return;
        // An accumulator fed by elapsed time, not a position derived from millis(): a derived one
        // jumps the block the instant the rate changes and cannot express "stopped" at all.
        const uint32_t now = platform::millis();
        if (sweepAtMs_ == 0) sweepAtMs_ = now; // first frame: no elapsed time to charge for
        const uint32_t elapsed = now - sweepAtMs_;
        sweepAtMs_ = now;
        if (patternSpeed) sweepMilliPx_ = (sweepMilliPx_ + elapsed * patternSpeed) % (kPatternW * 1000u);
        const int sweepX = static_cast<int>(sweepMilliPx_ / 1000u);
        for (int y = 0; y < kPatternH; y++) {
            for (int x = 0; x < kPatternW; x++) {
                // A white block riding the top edge: shows liveness, and which way "forward" runs.
                const bool onSweep = y < kBand && x >= sweepX && x < sweepX + 4;
                const RGB c = onSweep ? RGB{255, 255, 255} : bandColor(x, y);
                uint8_t* px = p + (static_cast<size_t>(y) * kPatternW + x) * 3;
                px[0] = c.r;
                px[1] = c.g;
                px[2] = c.b;
            }
        }
        publish();
    }

    /// Color of the pattern at (x, y): one hue per edge, black interior.
    static RGB bandColor(int x, int y) {
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

        // Netpbm allows comments and any run of whitespace between tokens, so a valid header is
        // not a fixed length. A ceiling is still needed since the file is user-supplied, but it has
        // to be generous enough for the comment GIMP writes and to fail with its own message rather
        // than looking like a format error.
        char header[kMaxHeaderBytes] = {};
        const int headerLen = platform::fsReadAt(file, 0, header, sizeof(header) - 1);
        uint16_t w = 0, h = 0;
        const int pixOff = parsePpmHeader(header, headerLen, w, h);
        if (pixOff < 0)
            return fail(headerLen >= static_cast<int>(sizeof(header)) - 1
                            ? "PPM header too long"
                            : "not a binary PPM (P6, maxval 255)");
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
        if (maxval != 255) return -1; // 16-bit samples are two big-endian bytes: another format
        // Netpbm requires ONE whitespace byte here. Accepting whatever is present would eat a
        // pixel: "P6\n2 2\n255X" would read as valid with the X swallowed.
        if (cur.pos >= len || !HeaderCursor::isBlank(buf[cur.pos])) return -1;

        w = static_cast<uint16_t>(ww);
        h = static_cast<uint16_t>(hh);
        return cur.pos + 1; // the pixels begin straight after that one byte
    }

private:
    /// Position within an ASCII header. Netpbm separates tokens with any run of whitespace and `#`
    /// comments to end-of-line, so both readers skip those first.
    struct HeaderCursor {
        const char* buf;
        int len;
        int pos = 0;

        /// Spelled out rather than isspace(), which is locale-dependent and undefined for a
        /// signed char above 127.
        static bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

        void skipBlanks() {
            while (pos < len) {
                if (buf[pos] == '#') {
                    while (pos < len && buf[pos] != '\n') pos++;
                } else if (isBlank(buf[pos])) {
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
