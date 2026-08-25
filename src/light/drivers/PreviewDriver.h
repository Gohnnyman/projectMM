#pragma once

#include "light/drivers/DriverBase.h"

#include "light/light_types.h"  // lengthType, nrOfLightsType
#include "core/BinaryBroadcaster.h"
#include "platform/platform.h"

#include <limits>  // numeric_limits for the memory-derived point cap

namespace mm {

/// Streams a true-shape 3D preview to the web UI over the binary WebSocket.
///
/// The preview is a POINT LIST, not a dense grid: only the real lights are sent,
/// at their real (x,y,z) positions. This is the proven MoonLight model (virtual
/// grid → physical sparse lights; positions sent once at mapping time, channels
/// per frame). Two message types — PreviewDriver owns both wire formats; the
/// HTTP server is a domain-neutral BinaryBroadcaster that just writes the bytes:
///
// --8<-- [start:wire-format]
///   0x03 coordinate table (sent when the geometry changes — every LUT/layout rebuild
///        via prepare — and when a new client connects, so a refresh gets it; never
///        per-frame):
///        [0x03][count:u32][bx:u8][by:u8][bz:u8][stride:u16][(x,y,z):u8×3 × count]
///        bx/by/bz = bounding-box extent (for client centring); positions are
///        1 byte/axis (a layout box ≤255/axis is the realistic case). count is u32 so a
///        >65535-light panel (big ArtNet/HUB75 walls) isn't capped by the wire format —
///        it matches nrOfLightsType (u32 on PSRAM boards).
///
///   0x02 per-frame channels: [0x02][count:u32][stride:u16][(r,g,b) × count]
///        RGB by driver index, every `stride`-th light. The browser positions
///        triple i at coord-table entry i*stride.
// --8<-- [end:wire-format]
///
/// `count` is the number of points actually kept after lattice downsampling (the
/// lights whose position satisfies `pos ≡ 0 mod stride`) — a client sizes its buffer
/// from this `count`, not from the light total. `stride` rises above 1 only when the
/// point set would exceed the runtime send-buffer cap (`min(display, memory)`); below
/// the cap every light is sent (stride 1), so a sparse layout streams in full.
/// **Its own channel (`/wsp`), and why.** Preview frames are lossy and large; control-plane state is
/// small and latency-sensitive. Sharing one WebSocket made the small messages queue behind the big
/// ones, head-of-line blocking, which users saw as a flickering connection indicator and a UI that
/// stopped responding while a large layout streamed. Separate TCP connections is the standard remedy
/// for that mixed-criticality pairing.
///
/// **Resolution is client-driven.** The browser measures the frame rate that actually arrives and
/// posts the lattice stride it wants as a `[0x51][stride]` uplink; the device serves the coarsest
/// standing request across viewers, full detail when none stands. A fixed point ceiling
/// (`maxPreviewPoints()`: min of the 16384 display cap and the memory cap) bounds what any request
/// can ask for.
///
/// **No subscriber, no work.** `tick()` returns immediately when the channel has no clients, so a
/// dismissed preview pane costs the device nothing, not merely nothing on the wire.
///
/// @card PreviewDriver.png
class PreviewDriver : public DriverBase {
public:
    /// The 3D preview the web UI renders streams from this driver. Deleting or
    /// replacing it from the UI would silently kill that preview, so it opts out
    /// of user-editing — it stays a fixed child of Drivers.
    bool userEditable() const override { return false; }

    /// The frame rate the preview aims for (Hz), independent of render FPS. User-tunable 1-60.
    /// The device never sends faster; the browser's controller trades resolution toward it.
    uint8_t targetFps = 24;

    /// Set the sink each message is pushed to (HttpServerModule, as a
    /// BinaryBroadcaster). Wired in main.cpp. Light depends only on the
    /// interface, not the concrete HTTP server.
    void setBroadcaster(BinaryBroadcaster* b) { broadcaster_ = b; }


    /// The currently served downsample factor (1 = full resolution). Test-only: lets a test pin
    /// that the stride mirrors the standing client requests and nothing else.
    nrOfLightsType downscaleForTest() const { return downscale_; }


    /// Preview shows the raw logical buffer, no correction.
    bool hasCorrectionControls() const override { return false; }

    /// Bind the controls: `targetFps` (1-60), the frame rate the preview aims for. The device never
    /// sends faster, and when the link cannot sustain it the BROWSER trades resolution to get
    /// closer: a low target keeps full detail at a low rate, a high target accepts a coarser
    /// preview to stay responsive.
    void defineDriverControls() override {
        controls_.addControl("targetFps", targetFps, 1, 60);
    }

    /// Point the driver at the sparse driver buffer the LED/ArtNet drivers also read
    /// (the MappingLUT fills it with exactly the real lights). The driver streams
    /// straight from it — no preview-side copy.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
    }

    /// A rebuild (layout add/replace/remove, resize, modifier change) ran — the
    /// light set / positions may have changed, so rebuild + broadcast the coordinate
    /// table (the MoonLight "positions once at mapping time"). Cancels any in-flight
    /// color send *first*: a resize frees+reallocs the producer buffer, so
    /// a half-sent frame would read freed memory — a use-after-free guard pinned by a
    /// test. This coupling spans PreviewDriver ↔ HttpServerModule ↔ the Layer buffer.
    void prepare() override {
        // A resize frees+reallocs the producer buffer, so any in-flight color send holds
        // a pointer that's about to dangle — cancel it BEFORE the rebuild (the browser discards the
        // half-sent message and gets the fresh table + frame next tick). Guards a use-after-free.
        if (broadcaster_) broadcaster_->cancelBufferedSend();
        else freePreviewBuffers();            // no broadcaster wired: nothing streams, release the buffers
        // downscale_ is NOT reset here: it is the client's standing request, and tick() mirrors the
        // standing requests every pass anyway, the client asks finer when the new geometry deserves it.
        buildAndSendCoordTable();
        refreshStatus();   // surface an index-cache alloc miss in the tab
    }

    void release() override {
        freePreviewBuffers();
        DriverBase::release();
    }

    /// No control changes the transport structure: `targetFps` is a plain value edit, so nothing
    /// here re-runs prepare. Geometry changes come through onRebuild, not a control.
    bool affectsPrepare(const char* /*name*/) const override { return false; }

    /// Per-tick: (re)stream the coordinate table when the geometry or client set
    /// changed, then stream one color frame if the previous one finished draining.
    /// The frame rate self-limits to what the link sustains (sheds rate first, then
    /// spatial resolution via adaptive downscale), so a large grid never stalls the
    /// loop or tears — it always delivers a complete frame.
    // REPORTED AS BLOCKING, deliberately: sendFrame() writes to a socket and
    // buildAndSendCoordTable() resizes keptIdx_. Both are real and both are on the render path,
    // so clang-hotpath lists them rather than hiding them. Backlogged (backlog-core: hot path).
    void tick() MM_NONBLOCKING override {
        if (targetFps == 0) return;
        // Nobody watching → do NOTHING. The gather, the downsample and the send all cost render-path
        // time, and a frame with no subscriber is pure waste. This is what makes closing the preview
        // pane actually free the device, rather than the traffic continuing unseen.
        if (!broadcaster_ || !broadcaster_->hasSubscribers()) return;
        uint32_t now = platform::millis();
        uint32_t interval = 1000 / targetFps;
        if (now - lastSendTime_ < interval) return;  // targetFps CEILING (max rate); link may be slower

        // Hold the sender for this WHOLE tick, because under the multicore split this runs on core 1
        // while the transport drains and pushes state on core 0. The bracket must span the entire
        // message set below — a coordinate table streams as begin/push/end, and another core's write
        // landing between those parts would corrupt the WS frame; arming a frame likewise must not
        // race the drain reading the slot. TRY-acquire (never block: we are on the encode thread) —
        // busy means the transport is mid-drain, so we SKIP this slot, which is exactly the back-off
        // the adaptive frame rate already takes when the link is behind. A skipped preview frame is
        // invisible; a blocked encode thread would stall the LEDs.
        SendLease lease{broadcaster_};
        if (!lease) return;

        lastSendTime_ = now;   // only after we own the sender — a skipped slot must retry next tick

        // The coordinate table is (re)streamed only when the geometry changes (prepare — a
        // resize / LUT rebuild), when a new client connects (clientGeneration bump, so a page
        // refresh gets positions immediately), when the adaptive factor changes, or while a
        // previous stream didn't reach every client (coordPending_ retry). NOT per frame: the
        // color frames below reference the last-streamed positions. coordCount_==0 = cold start.
        // CLIENT-DRIVEN RESOLUTION, adopted BEFORE this tick can arm a new frame. The device no
        // longer guesses link quality from its socket (a controller doing that cycled for a whole
        // bench day; the sender can only see its own buffer): the RECEIVER measures the true
        // end-to-end rate and posts the stride it wants as an uplink hint; the coarsest request
        // across clients wins (the HLS/DASH shape). The stride IS the standing requests, no live
        // request (a fresh client, an old UI, every requester gone) means full detail, so a dead
        // client's coarsening dies with its slot. Gated on the no-splice idle rule below; ordering
        // matters: at the end of the tick, sendFrame() has just re-armed a buffered frame, so the
        // idle gate could never pass while frames stream back-to-back, the device then never
        // adopts, the client's coarsen never pays, and its controller honestly settles on
        // full-detail-slow (bench: stuck at 1/1 with targetFps ignored).
        uint32_t gen = broadcaster_ ? broadcaster_->clientGeneration() : 0;
        {
            const uint32_t req = broadcaster_->maxClientHint();
            const nrOfLightsType want = (req >= 1 && req <= 64) ? static_cast<nrOfLightsType>(req) : 1;
            if (want != downscale_ && broadcaster_->bufferedSendIdle()) {
                downscale_ = want;
                buildAndSendCoordTable();
                lastClientGen_ = gen;   // this build streamed to everyone: the coord block below need not repeat it
            }
        }

        // …and NEVER while a buffered color frame is mid-drain: the table streams synchronously,
        // and a whole message spliced into a half-drained one corrupts every client's framing
        // (bench: "Invalid frame header" reconnect loop on WiFi, where a frame drains for many
        // ticks). The need re-evaluates next tick, gen/coordPending_ are only consumed on send.
        const bool needCoords = coordCount_ == 0 || gen != lastClientGen_ || coordPending_;
        if (needCoords && broadcaster_->bufferedSendIdle()) {
            lastClientGen_ = gen;
            buildAndSendCoordTable();   // streams positions; sets coordPending_ if not all clients got it
        }

        // ADAPTIVE FRAME RATE. The full-res color frame streams resumably (sendBufferedFrame drains
        // across transport ticks), so a frame only starts once the previous one fully drained. We
        // gate on that: idle → send the next frame now; still draining → skip this slot. The
        // EFFECTIVE targetFps therefore self-limits to what the link sustains, fast links hit the targetFps
        // ceiling, slow links naturally drop to a few targetFps, with NO loop stall either way. The slot
        // we skip sheds frame rate FIRST; resolution is the rate controller's last resort.
        if (!coordPending_) {
            // Backpressure: never queue a frame behind one still draining, a lossy stream drops,
            // it does not build a backlog. Effective rate self-limits to what the link sustains.
            const bool idle = !broadcaster_ || broadcaster_->bufferedSendIdle();
            if (idle) sendFrame();
        }

    }

    /// Build (or rebuild) the cached coordinate table from the layout's real lights
    /// and broadcast it (the `0x03` message). Above the point cap — `min(display,
    /// memory)`, memory from `maxAllocBlock()` — lights are kept on a spatial lattice
    /// (position ≡ 0 mod stride), sampling positions not indices so there is no moiré.
    /// Public so tests can drive it deterministically.
    void buildAndSendCoordTable() {
        coordCount_ = 0;
        if (!layer_ || !layer_->layouts()) return;
        Layouts* layouts = layer_->layouts();
        nrOfLightsType n = layouts->totalLightCount();
        if (n == 0) return;

        // Box EXTENT = the maximum coordinate the positions reach, which is (size − 1): placeLights
        // emits x in [0, width−1], so an 8-wide grid spans 0..7 and its extent is 7, NOT 8. The
        // header carries these extents and the browser centres the cloud by dividing by the largest,
        // so they must match the packed coordinates' span exactly — using the size (8) instead drew
        // the wireframe box one cell too large and shifted the lights off-centre.
        auto extent = [](lengthType size) -> lengthType { return size > 0 ? size - 1 : 0; };
        const lengthType ex = extent(layer_->physicalWidth());
        const lengthType ey = extent(layer_->physicalHeight());
        const lengthType ez = extent(layer_->physicalDepth());
        // Positions are 1 byte/axis. To support layouts whose extent exceeds 255 on an axis (a
        // 512-wide grid, say), scale every axis by the same factor so the largest edge maps to 255 —
        // preserving aspect ratio. For extents ≤255/axis the factor is 1 (exact integer positions).
        lengthType maxEdge = ex;
        if (ey > maxEdge) maxEdge = ey;
        if (ez > maxEdge) maxEdge = ez;
        if (maxEdge < 1) maxEdge = 1;
        posScale_ = (maxEdge > 255) ? maxEdge : 0;   // 0 = no scaling (1:1)
        bx_ = scaleAxis(ex);
        by_ = scaleAxis(ey);
        bz_ = scaleAxis(ez);

        // Per-axis downsample step s (lattice skip x%s && y%s && z%s). The cell count of the
        // bounding box is the upper bound on kept lights, so grow s until it fits the cap — but
        // ONLY when the layout has more lights than the cap (a sparse layout — big box, few
        // lights — fits at s==1 and must not be downsampled for its box size alone). The wire
        // "stride" field carries s to the browser (1 = full res; >1 = "1/s shown, link limited").
        const lengthType ax = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        const lengthType ay = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        const lengthType az = layer_->physicalDepth()  > 0 ? layer_->physicalDepth()  : 1;
        nrOfLightsType s = 1;
        const nrOfLightsType cap = maxPreviewPoints();   // memory-derived this rebuild
        if (n > cap) {
            auto latticeCount = [&](nrOfLightsType step) {
                nrOfLightsType cx = (ax + step - 1) / step, cy = (ay + step - 1) / step,
                               cz = (az + step - 1) / step;
                return static_cast<uint32_t>(cx) * cy * cz;
            };
            while (latticeCount(s) > cap) s++;
        }
        if (s < downscale_) s = downscale_;   // adaptive: never finer than the link sustains
        previewStride_ = s;

        // Count the lights the lattice keeps. A dense grid in natural order (no LUT) is a regular
        // box, so the kept count is closed-form: ceil(size/s) per axis — no walk. A sparse/mapped
        // layout (LUT) has an arbitrary index↔position map, so it's counted by one placeLights
        // pass applying the same lattice predicate the color/coord passes use (color[k] ↔ coord[k]
        // line up by shared order, no stored index map).
        if (denseGrid()) {
            const nrOfLightsType cx = (ax + s - 1) / s, cy = (ay + s - 1) / s, cz = (az + s - 1) / s;
            coordCount_ = static_cast<nrOfLightsType>(static_cast<uint32_t>(cx) * cy * cz);
        } else {
            struct CountCtx { nrOfLightsType s, out; };
            CountCtx cc{s, 0};
            // A gap is a real preview position (drawn dark at its (x,y,z)), so count/emit it like any
            // light — blackCb null → blackPixel falls back to the same handler.
            layouts->placeLights(CoordSink{[](void* c, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<CountCtx*>(c);
                if (x % p->s == 0 && y % p->s == 0 && z % p->s == 0) p->out++;
            }, nullptr, &cc});
            coordCount_ = cc.out;
            // Size the kept-index cache to EXACTLY this count (grow-only) BEFORE the emit pass fills it,
            // so the cache can never truncate: coordCount_ is recomputed every rebuild (adaptive stride,
            // memory-driven cap), so sizing it here — not lazily to a stale point-cap — is what keeps
            // keptCount_ == coordCount_ and the per-frame gather complete. An alloc miss leaves the
            // cache too small; the gather then falls back to the full lattice walk (correct, slower).
            if (keptIdxCap_ < coordCount_) {
                auto* grown = static_cast<nrOfLightsType*>(platform::alloc(coordCount_ * sizeof(nrOfLightsType)));
                if (grown) {
                    if (keptIdx_) platform::free(keptIdx_);
                    keptIdx_ = grown;
                    keptIdxCap_ = coordCount_;
                    keptIdxAllocFailed_ = false;
                    publishHeapBytes();   // the index cache grew — refresh the memory readout
                } else {
                    keptIdxAllocFailed_ = true;   // degraded: the gather walks placeLights per frame
                }
            }
        }
        if (coordCount_ == 0) { coordPending_ = false; return; }

        // 0x03 app header: [type][count:u32 LE][bx][by][bz][stride:u16 LE] (10 bytes).
        uint8_t h[10];
        h[0] = 0x03;
        h[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        h[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        h[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        h[5] = bx_; h[6] = by_; h[7] = bz_;
        h[8] = static_cast<uint8_t>(s & 0xFF);
        h[9] = static_cast<uint8_t>(s >> 8);

        if (!broadcaster_) { coordPending_ = true; return; }
        broadcaster_->beginBinaryFrame(sizeof(h) + static_cast<size_t>(coordCount_) * 3);
        broadcaster_->pushBinaryFrame(h, sizeof(h));
        // Push the kept lights' scaled positions in small slices through a stack scratch. A dense
        // grid strides its box directly (closed-form, no walk over skipped cells); a sparse/mapped
        // layout walks placeLights with the lattice predicate. BOTH visit the kept lights in the
        // SAME order the color pass uses, so color[k] ↔ coord[k] line up. The C callback can't
        // capture, so it shares PosCtx (used by both the dense loop and the sparse callback).
        struct PosCtx {
            PreviewDriver* self; mm::BinaryBroadcaster* bc; nrOfLightsType s;
            uint8_t buf[1536]; uint16_t fill;
            void emit(lengthType x, lengthType y, lengthType z) {
                buf[fill++] = self->scaleAxis(x);
                buf[fill++] = self->scaleAxis(y);
                buf[fill++] = self->scaleAxis(z);
                if (fill > sizeof(buf) - 3) { bc->pushBinaryFrame(buf, fill); fill = 0; }
            }
        };
        PosCtx pc{this, broadcaster_, s, {}, 0};
        keptCount_ = 0;   // rebuilt below for the sparse path; dense gathers closed-form, no index map
        if (denseGrid()) {
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s) pc.emit(x, y, z);
        } else {
            // While emitting coords, CACHE the kept lights' buffer indices — the per-frame color
            // gather then loops this index map instead of re-walking placeLights over every light
            // (an O(total-lights) callback walk per firing, measured ~8 ms at 12K lights on the
            // encode worker). The map's lifecycle IS the coord table's: same pass, same invalidation.
            layouts->placeLights(CoordSink{[](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<PosCtx*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                PreviewDriver* self = p->self;
                if (self->keptIdx_ && self->keptCount_ < self->keptIdxCap_)
                    self->keptIdx_[self->keptCount_++] = idx;
                p->emit(x, y, z);
            }, nullptr, &pc});
        }
        if (pc.fill) broadcaster_->pushBinaryFrame(pc.buf, pc.fill);
        // The coord table must reach the browser before color frames carrying the new count (the
        // browser skips a count-mismatched 0x02). endBinaryFrame() reports whether every client got
        // it; tick() retries while coordPending_ and withholds color frames until it lands.
        coordPending_ = !broadcaster_->endBinaryFrame();
    }

    /// Stream one per-frame `0x02` RGB message straight from the producer buffer — no
    /// intermediate copy. Returns whether every client got it (false → tick() drives
    /// adaptive downscaling). Public so tests can drive it without tick()'s rate-limit.
    bool sendFrame() {
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return false;
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        const nrOfLightsType n = sourceBuffer_->count();
        const nrOfLightsType s = previewStride_;

        // Header: [0x02][count:u32 LE][stride:u16 LE]  (7 bytes). count = the kept lights.
        uint8_t header[7];
        header[0] = 0x02;
        header[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        header[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>(s & 0xFF);
        header[6] = static_cast<uint8_t>(s >> 8);

        if (s == 1 && cpl == 3 && coordCount_ <= n) {
            // FULL RES, RGB: the producer buffer IS the payload. Hand it to the RESUMABLE buffered
            // send (header copied, body = the producer buffer, a stable pointer) — it drains across
            // transport ticks without a copy and without spinning this loop, the fix for the
            // large-frame stall. The common case (any grid ≤ cap, incl. 16K on a no-PSRAM classic).
            // prepare cancels it before a resize frees the buffer (use-after-free guard).
            return broadcaster_->sendBufferedFrame(header, sizeof(header),
                                                   src, static_cast<size_t>(coordCount_) * 3);
        }

        // Downsampled (s>1) or non-RGB (cpl≠3): the producer buffer is not the payload, so gather
        // the kept lights' RGB and push it through the SYNCHRONOUS begin/push/end stream. The
        // downsampled body is small (the point of coarsening), so the whole message fits one tick.
        // The kept subset + order MUST match the coord table's, so color[k] ↔ coord[k] line up
        // (the browser drops a count/stride-mismatched frame). A dense grid strides its box
        // directly: light (x,y,z) is at buffer index z·H·W + y·W + x, closed-form, no walk over
        // skipped cells. A sparse/mapped layout walks placeLights with the same lattice predicate.
        const size_t bodyBytes = static_cast<size_t>(coordCount_) * 3;
        struct ColCtx {
            mm::BinaryBroadcaster* bc; const uint8_t* src; nrOfLightsType n; uint8_t cpl;
            uint8_t buf[1536]; uint16_t fill;
            void put(uint8_t b) {
                buf[fill++] = b;
                if (fill > sizeof(buf) - 1) { bc->pushBinaryFrame(buf, fill); fill = 0; }
            }
            void emit(nrOfLightsType idx) {
                const uint8_t* px = (idx < n) ? src + static_cast<size_t>(idx) * cpl : nullptr;
                put(px ? px[0] : 0);
                put((px && cpl >= 2) ? px[1] : 0);
                put((px && cpl >= 3) ? px[2] : 0);
            }
        };
        broadcaster_->beginBinaryFrame(sizeof(header) + bodyBytes);
        broadcaster_->pushBinaryFrame(header, sizeof(header));
        ColCtx col{broadcaster_, src, n, cpl, {}, 0};
        if (denseGrid()) {
            const lengthType W = layer_->physicalWidth(), H = layer_->physicalHeight();
            const lengthType az = layer_->physicalDepth() > 0 ? layer_->physicalDepth() : 1;
            const lengthType ay = H > 0 ? H : 1, ax = W > 0 ? W : 1;
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s)
                        col.emit(static_cast<nrOfLightsType>(static_cast<size_t>(z) * H * W
                                                             + static_cast<size_t>(y) * W + x));
        } else if (keptIdx_ && keptCount_ == coordCount_) {
            // The index map cached at coord-table build: a tight gather over the kept lights only.
            for (nrOfLightsType k = 0; k < keptCount_; k++) col.emit(keptIdx_[k]);
        } else {
            // Fallback (index-map alloc miss): the full lattice walk, s as the FULL stride (not
            // clamped) — must match buildAndSendCoordTable's.
            struct Skip { ColCtx* col; nrOfLightsType s; } sk{&col, s};
            layer_->layouts()->placeLights(CoordSink{[](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<Skip*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                p->col->emit(idx);
            }, nullptr, &sk});
        }
        if (col.fill) broadcaster_->pushBinaryFrame(col.buf, col.fill);
        return broadcaster_->endBinaryFrame();
    }

private:
    /// Free the preview buffers + refresh the memory readout. Cancels any in-flight send first, so
    /// a drain can never outlive the buffer it reads (the use-after-free guard).
    void freePreviewBuffers() {
        if (broadcaster_) broadcaster_->cancelBufferedSend();
        if (keptIdx_) { platform::free(keptIdx_); keptIdx_ = nullptr; keptIdxCap_ = 0; keptCount_ = 0; }
        publishHeapBytes();
    }


    /// Publish the preview's operating status: PLAIN "previewing N points" normally, or a WARNING naming
    /// the degradation when the index cache could not allocate (RAM-tight board) so the tab shows
    /// WHY it fell back — the synchronous send returns (blocking socket writes on the encode thread, the
    /// LED-hitch this optimization removed) or the sparse gather walks placeLights per frame. Called from
    /// the cold path (prepare) and refreshed on the coord rebuild, never the render loop.
    void refreshStatus() {
        if (keptIdxAllocFailed_) {
            setStatus("preview degraded — index cache alloc failed, gathering per frame (slower)",
                      Severity::Warning);
        } else if (lastClients_ > 0) {
            // The observability the bench work had to reconstruct with socket probes: who is
            // watching, and at what resolution they asked to be served.
            std::snprintf(statusBuf_, sizeof(statusBuf_), "%d watching · 1/%u",
                          lastClients_, static_cast<unsigned>(downscale_));
            setStatus(statusBuf_, Severity::Status);
        } else {
            clearStatus();
        }
    }

    /// Housekeeping cadence: refresh the watcher count in the status when it changes. The change
    /// guard keeps the common tick at two integer compares.
    void tick1s() MM_NONBLOCKING override {
        const int c = broadcaster_ ? broadcaster_->subscriberCount() : 0;
        if (c != lastClients_ || downscale_ != lastShownStride_) {
            lastClients_ = c;
            lastShownStride_ = downscale_;
            refreshStatus();
        }
        MoonModule::tick1s();
    }
    int lastClients_ = 0;
    nrOfLightsType lastShownStride_ = 0;
    char statusBuf_[40]{};
    bool keptIdxAllocFailed_ = false;     // index cache couldn't allocate → gather walks per frame
    nrOfLightsType* keptIdx_ = nullptr;   // sparse layouts: kept lights' buffer indices, coord-table order
    nrOfLightsType keptIdxCap_ = 0, keptCount_ = 0;

protected:
    // Matches DriverBase's visibility — a private override would silently hide the hook from any
    // future caller holding a DriverBase*. ParallelLedDriver keeps it protected for the same reason.
    /// This driver's heap = the base scratch + the kept-index cache, summed for the per-module
    /// memory readout (see DriverBase::driverHeapBytes). PreviewDriver holds no wire_ scratch, but
    /// chaining to the base keeps the rule uniform.
    size_t driverHeapBytes() const override {
        return DriverBase::driverHeapBytes()
             + static_cast<size_t>(keptIdxCap_) * sizeof(nrOfLightsType);
    }

private:

    // Frame cap: the most points one preview frame carries before the spatial-lattice downsample
    // engages — derived at runtime from free contiguous memory, not a fixed per-board constant
    // (architecture.md § Scaling to available memory: "sizes determined at runtime based on
    // available memory"). There is no per-frame buffer; the cap bounds the transient work the coord
    // table build (3 bytes/point in flight to the socket) imposes. So
    // a fragmented classic downscales SOONER (less contiguous RAM) while a roomy PSRAM board goes
    // far higher — one rule, every board, measured not assumed. The spatial-lattice downsample is
    // the graceful fallback above the cap.
    // True when the source is a dense grid in natural box order (no mapping LUT): driver index i is
    // exactly box cell i, so the kept-light set + each light's buffer index are CLOSED-FORM from the
    // box dimensions and the stride: no placeLights walk needed (the count, the coord positions,
    // and the downsampled colors all stride the box directly). A LUT means a sparse / serpentine /
    // modified layout whose index↔position map is arbitrary, so those paths must walk placeLights.
    // Mirrors the Layer's own dense-vs-LUT decision (Layer::isNaturalOrder gates lut_.setIdentity),
    // so the two agree: no LUT ⇔ Drivers passed the dense box buffer ⇔ closed-form is valid here.
    bool denseGrid() const { return layer_ && !layer_->lut().hasLUT(); }

    nrOfLightsType maxPreviewPoints() const {
        // TWO independent bounds, take the smaller:
        //  (1) DISPLAY cap — a preview is a browser canvas a few hundred px wide; beyond ~4096
        //      points the lights are sub-pixel and indistinguishable, so MORE points only cost link
        //      bandwidth (a 16K-point 49 KB frame streams at <1 targetFps even on Ethernet). Capping to a
        //      display-sensible count is what makes a big-RAM board (P4) downsample to a frame the
        //      LINK can actually push fast — the bottleneck here is throughput, not memory. WLED-MM
        //      caps its live preview the same way. The lattice downsample (and the browser's status)
        //      handle anything larger gracefully.
        //  (2) MEMORY cap — derived from maxAllocBlock() so a tight/fragmented board downsamples even
        //      SOONER than the display cap (architecture.md § Scaling to available memory).
        // min(display, memory): the display cap normally wins (it's the smaller); the memory cap
        // only bites on a board too tight to stream even 4096 points.
        // The display ceiling is now EARNED, not fixed. It starts at kDisplayCapBase and rises while
        // frames drain promptly on this link, because the preview has its own channel, a bigger
        // frame no longer delays the control plane, so the old flat 4096 was leaving resolution on
        // the table for a board and link that can carry more.
        // see noteFrameOutcome() for how it moves and what bounds it.
        constexpr uint32_t kDisplayCap = 16384;   // fixed: the most a preview canvas resolves
        constexpr size_t kReserve = 32u * 1024u;     // leave this much contiguous headroom
        constexpr size_t kBytesPerPoint = 3u;        // RGB on the wire / position bytes in the table
        constexpr nrOfLightsType kFloor = 1024;      // always previewable (hard-downsampled) on any board
        const size_t block = platform::maxAllocBlock();
        // maxAllocBlock() returns 0 = "unlimited / not reported" (desktop, test default): memory is
        // not the limit there, so the display cap governs.
        uint32_t memPts;
        if (block == 0) {
            memPts = kDisplayCap;
        } else {
            const size_t usable = block > kReserve ? block - kReserve : 0;
            memPts = static_cast<uint32_t>(usable / kBytesPerPoint);
            if (memPts < kFloor) memPts = kFloor;
        }
        uint32_t pts = memPts < kDisplayCap ? memPts : kDisplayCap;
        // Clamp into the board's nrOfLightsType range (u16 on a no-PSRAM classic).
        constexpr uint32_t kTypeMax = static_cast<uint32_t>(std::numeric_limits<nrOfLightsType>::max());
        if (pts > kTypeMax) pts = kTypeMax;
        return static_cast<nrOfLightsType>(pts);
    }

    // Map an axis coordinate into the 0..255 byte range. posScale_ == 0 means
    // the box already fits (1:1, exact integer positions); otherwise scale by
    // 255/posScale_ (posScale_ = the largest box edge), preserving aspect ratio
    // so a >255 axis isn't silently flattened onto the 255 plane.
    uint8_t scaleAxis(lengthType v) const {
        if (v < 0) return 0;
        int32_t s = posScale_ ? (static_cast<int32_t>(v) * 255 / posScale_) : v;
        return s > 255 ? 255 : static_cast<uint8_t>(s);
    }

    Buffer* sourceBuffer_ = nullptr;
    BinaryBroadcaster* broadcaster_ = nullptr;
    nrOfLightsType coordCount_ = 0;        // lights the lattice keeps = the streamed 0x03/0x02 count
    nrOfLightsType previewStride_ = 1;     // wire field: the lattice/downscale factor (1 = full res)
    bool coordPending_ = false;            // coord table not yet delivered; tick() retries it
    uint8_t bx_ = 0, by_ = 0, bz_ = 0;
    int32_t posScale_ = 0;            // 0 = positions 1:1; else largest box edge (>255) to scale by
    uint32_t lastSendTime_ = 0;
    uint32_t lastClientGen_ = 0;   // last seen broadcaster_->clientGeneration() — re-send coords on change

    // The served per-axis lattice stride: the coarsest standing client request (1 = full
    // resolution, the value with no standing request). An extra floor on top of the cap
    // downsample; rides the wire stride field to the browser's status line.
    nrOfLightsType downscale_ = 1;
};

} // namespace mm
