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
///   0x03 coordinate table (sent ONLY in answer to a client's [0x52] request; the client
///        caches tables per (epoch, stride), so a stride change to a known rung asks nothing):
///        [0x03][count:u32][bx:u8][by:u8][bz:u8][stride:u16][epoch:u8][(x,y,z):u8x3 x count]
///        bx/by/bz = bounding-box extent (for client centring); positions are
///        1 byte/axis (scaled when an axis exceeds 255). count is u32 so a >65535-light
///        panel isn't capped by the wire format; epoch bumps on every geometry rebuild
///        and keys the client's cache.
///
///   0x02 per-frame channels: [0x02][count:u32][stride:u16][epoch:u8][drops:u8][(r,g,b) x count]
///        RGB of every kept light, in the coord table's order. drops = frames discarded
///        at the source since the last delivered one, the congestion signal the client's
///        controller adapts on.
///
///   0x04 per-frame AIM, sent only for a rig whose fixtures carry pan/tilt (a moving-head rig);
///        a plain LED wall never emits it and costs nothing for it:
///        [0x04][count:u32][stride:u16][epoch:u8][reserved:u8][(pan,tilt):u8x2 x count]
///        Same order and same (epoch, stride) key as 0x02, so aim[k] belongs to the light the
///        color frame's k-th entry colors. The BROWSER decides how to draw it (today a beam
///        line from the fixture): the wire carries where a head points, never a rendered look,
///        so a richer visual later is a shader change and not a protocol change.
///
///   Client requests (masked WS frames, unmasked by core, interpreted only here):
///        [0x51][stride][fps]  standing frame request (most conservative across viewers wins;
///                             the targetFps control is the ceiling)
///        [0x52][stride]       one-shot: send me the coordinate table
// --8<-- [end:wire-format]
///
/// `count` is the number of points actually kept after lattice downsampling (the
/// lights whose position satisfies `pos ≡ 0 mod stride`) — a client sizes its buffer
/// from this `count`, not from the light total. `stride` rises above 1 when a client
/// requests it, or when the memory cap forces a floor; with no cap in play every light
/// is sent (stride 1), so a sparse layout streams in full.
/// **Its own channel (`/wsp`), and why.** Preview frames are lossy and large; control-plane state is
/// small and latency-sensitive. Sharing one WebSocket made the small messages queue behind the big
/// ones, head-of-line blocking, which users saw as a flickering connection indicator and a UI that
/// stopped responding while a large layout streamed. Separate TCP connections is the standard remedy
/// for that mixed-criticality pairing.
///
/// **Resolution is client-driven.** The browser reads the drops counter each frame carries (the
/// device's own congestion signal) and posts the `[0x51][stride][fps]` standing request it wants;
/// the device serves the most conservative request across viewers. The memory cap
/// (`maxPreviewPoints()`) is the only floor a request cannot go finer than.
///
/// **No request, no work.** `tick()` returns immediately when no standing request exists, so a
/// dismissed preview pane (or a hidden tab) costs the device nothing, not merely nothing on the
/// wire.
///
/// @card PreviewDriver.png
class PreviewDriver : public DriverBase, public BinaryBroadcaster::ClientMessageSink {
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
    /// interface, not the concrete HTTP server; the driver registers itself
    /// as the channel's inbound-message sink (the pull model's request path).
    void setBroadcaster(BinaryBroadcaster* b) {
        broadcaster_ = b;
        if (b) b->setClientMessageSink(this);
    }

    /// The pull protocol, this producer's whole request vocabulary:
    ///   [0x51][stride][fps]  standing frame request: serve stride s at rate f
    ///                        (fps 0/absent = the targetFps control's value).
    ///   [0x52][stride]       one-shot: send me the coordinate table (for the served stride).
    /// Arrives on the transport thread; single-byte slot fields, so the encode-thread reader
    /// tolerates the race (lossy-channel rule). Out-of-range bytes are ignored at the store:
    /// requests aggregate conservatively, so one hostile value must not mask every real one.
    void onClientMessage(int slot, const uint8_t* payload, int len) override {
        if (slot < 0 || slot >= kMaxRequestSlots || len < 2) return;
        if (payload[0] == 0x51) {
            const uint8_t stride = payload[1];
            if (stride < 1 || stride > 64) return;
            reqStride_[slot] = stride;
            reqFps_[slot] = (len >= 3 && payload[2] >= 1 && payload[2] <= 25) ? payload[2] : 0;
        } else if (payload[0] == 0x52) {
            tableRequested_ = true;
        }
    }
    void onClientGone(int slot) override {
        if (slot < 0 || slot >= kMaxRequestSlots) return;
        reqStride_[slot] = 0;   // a dead client's request dies with its slot
        reqFps_[slot] = 0;
    }


    /// The currently served downsample factor (1 = full resolution). Test-only: lets a test pin
    /// that the stride mirrors the standing client requests and nothing else.
    nrOfLightsType downscaleForTest() const { return downscale_; }


    /// Preview shows the raw logical buffer, no correction.
    bool hasCorrectionControls() const override { return false; }

    /// Bind the controls: `targetFps` (1-25), the frame rate the preview aims for. The device never
    /// sends faster, and when the link cannot sustain it the BROWSER trades resolution to get
    /// closer: a low target keeps full detail at a low rate, a high target accepts a coarser
    /// preview to stay responsive.
    void defineDriverControls() override {
        controls_.addControl("targetFps", targetFps, 1, 25);
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
        // A rebuild is a NEW epoch: frames start carrying it, every client's table cache misses,
        // and each asks via [0x52]. The device never volunteers a table (the pull model).
        epoch_++;
        buildCoordTable();
        // Pre-size the staging and index buffers for the FINEST stride this geometry can be
        // served at (stride 1, bounded by the memory cap). Both are grow-only, so every later
        // stride adopt on the render tick reuses this capacity and allocates nothing: the one
        // allocation the tick path could reach moves here, the cold rebuild seam.
        if (layer_ && layer_->layouts()) {
            const nrOfLightsType finest = layer_->layouts()->totalLightCount();
            const nrOfLightsType capPts = maxPreviewPoints();
            const size_t maxPts = finest < capPts ? finest : capPts;
            ensureStaging(maxPts * 3u);
            if (!denseGrid() && keptIdxCap_ < maxPts) {
                auto* grown = static_cast<nrOfLightsType*>(platform::alloc(maxPts * sizeof(nrOfLightsType)));
                if (grown) {
                    if (keptIdx_) platform::free(keptIdx_);
                    keptIdx_ = grown;
                    keptIdxCap_ = static_cast<nrOfLightsType>(maxPts);
                    publishHeapBytes();
                }
            }
        }
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
    // buildCoordTable() resizes keptIdx_. Both are real and both are on the render path,
    // so clang-hotpath lists them rather than hiding them. Backlogged (backlog-core: hot path).
    void tick() MM_NONBLOCKING override {
        // THE PULL MODEL: the device serves standing client requests and volunteers nothing.
        // No standing request (no viewer, every pane closed, a tab hibernating) means no gather,
        // no downsample, no send, nothing: closing the preview genuinely frees the device.
        if (!broadcaster_) return;
        nrOfLightsType wantStride = 0;
        uint8_t wantFps = 255;
        for (int i = 0; i < kMaxRequestSlots; i++) {
            const uint8_t rs = reqStride_[i];
            if (!rs) continue;
            if (rs > wantStride) wantStride = rs;                       // coarsest wins
            const uint8_t rf = reqFps_[i] ? reqFps_[i] : targetFps;
            if (rf < wantFps) wantFps = rf;                             // slowest wins
        }
        if (wantStride == 0) return;                                    // nobody asked: no work
        if (wantFps > targetFps) wantFps = targetFps;                   // the control is the ceiling
        if (wantFps == 0) return;

        uint32_t now = platform::millis();
        if (now - lastSendTime_ < 1000u / wantFps) return;              // rate: the served request

        // Hold the sender for this tick: under the multicore split this runs on core 1 while the
        // transport drains on core 0, and ARMING a message (the only socket-adjacent thing this
        // thread ever does now) must not race the drain reading the slot. TRY-acquire, never
        // block: busy means the transport is mid-drain, so we SKIP this slot, the same back-off a
        // busy link already gets. A skipped preview frame is invisible; a blocked encode thread
        // would stall the LEDs.
        SendLease lease{broadcaster_};
        if (!lease) return;

        lastSendTime_ = now;   // only after we own the sender: a skipped slot must retry next tick

        // Adopt the served stride. The lattice rebuild is local bookkeeping (counts + index
        // cache + staging), gated on an idle slot only because the staging buffer must not be
        // rewritten under a live drain. No table is sent here: frames carrying the new
        // (epoch, stride) make every client's cache miss, and each asks via [0x52] when it needs
        // the positions, the pull model's answer to the re-stream storms the push design fed.
        const bool idle = broadcaster_->bufferedSendIdle();
        if ((wantStride != downscale_ || coordCount_ == 0) && idle) {
            downscale_ = wantStride;
            buildCoordTable();
        }
        if (coordCount_ == 0) return;   // nothing previewable (empty layout / staging alloc miss)

        // A requested table outranks the next frame for the slot: the asker cannot render one
        // frame until it lands.
        // Rebuild before sending: the staging buffer is shared with the frame gather, so the
        // table's bytes are only valid straight after its build.
        if (tableRequested_) {
            if (idle) {
                buildCoordTable();
                if (sendCoordTable()) tableRequested_ = false;
            }
            return;
        }

        // One frame in the slot at a time (drop-new): a frame offered while one drains is DROPPED
        // at the source, the frame-dropping every lossy stream does, and the drop is REPORTED in
        // the next frame's header so the client adapts on the sender's own congestion signal
        // instead of probing. Rate self-limits to what the link drains; nothing waits, ever.
        if (idle) {
            // Color and aim ALTERNATE rather than both going out per tick: the transport keeps one
            // send in flight and drops a second, so calling them back to back meant every aim frame
            // was rejected and the beams never moved. Alternating halves each stream's rate, which
            // a fixture rig can afford (a head sweeps far slower than a pixel changes) and a rig
            // with no motion never pays, since sendAim declines before claiming a turn.
            if (aimTurn_ && sendAim()) {
                aimTurn_ = false;
            } else {
                if (!sendFrame() && dropsSinceLast_ < 255) dropsSinceLast_++;
                aimTurn_ = true;
            }
        } else if (dropsSinceLast_ < 255) {
            dropsSinceLast_++;
        }
    }

    /// Build (or rebuild) the cached coordinate table from the layout's real lights
    /// and broadcast it (the `0x03` message). Above the point cap — `min(display,
    /// memory)`, memory from `maxAllocBlock()` — lights are kept on a spatial lattice
    /// (position ≡ 0 mod stride), sampling positions not indices so there is no moiré.
    /// Public so tests can drive it deterministically.
    void buildCoordTable() {
        coordCount_ = 0;
        if (!layer_ || !layer_->layouts()) return;
        Layouts* layouts = layer_->layouts();
        nrOfLightsType n = layouts->totalLightCount();
        if (n == 0) return;

        // Box EXTENT = the maximum coordinate the positions reach, which is (size − 1): placeLights
        // emits x in [0, width−1], so an 8-wide grid spans 0..7 and its extent is 7, NOT 8. The
        // header carries these extents and the browser centers the cloud by dividing by the largest,
        // so they must match the packed coordinates' span exactly — using the size (8) instead drew
        // the wireframe box one cell too large and shifted the lights off-center.
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
        if (coordCount_ == 0) return;

        // The table body is built COMPLETE into the staging buffer, ready for sendCoordTable to
        // hand to the one resumable send when a client asks. The buffer is stable for a drain's
        // lifetime (freed only behind cancelBufferedSend), and rewriting it is gated on an idle
        // slot by every caller, so a drain never reads a half-rewritten table.
        if (!ensureStaging(static_cast<size_t>(coordCount_) * 3)) {
            coordCount_ = 0;   // alloc miss: nothing previewable until memory frees; retried next adopt
            return;
        }
        // Emit the kept lights' scaled positions. A dense grid strides its box directly
        // (closed-form, no walk over skipped cells); a sparse/mapped layout walks placeLights with
        // the lattice predicate. BOTH visit the kept lights in the SAME order the color pass uses,
        // so color[k] ↔ coord[k] line up. The C callback can't capture, so PosCtx is shared.
        struct PosCtx {
            PreviewDriver* self; uint8_t* out; size_t at; nrOfLightsType s;
            void emit(lengthType x, lengthType y, lengthType z) {
                out[at++] = self->scaleAxis(x);
                out[at++] = self->scaleAxis(y);
                out[at++] = self->scaleAxis(z);
            }
        };
        PosCtx pc{this, staging_, 0, s};
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
        stagingUsed_ = pc.at;   // the built table's byte length, what sendCoordTable ships
    }

    /// Answer a [0x52] table request: one 0x03 message for the SERVED stride, through the same
    /// resumable slot as everything else. Returns whether the send was accepted (drop-new: false
    /// = the slot was busy; the caller keeps the request standing and retries next tick).
    /// Header: [0x03][count u32 LE][bx][by][bz][stride u16 LE][epoch] (11 bytes).
    bool sendCoordTable() {
        if (!broadcaster_ || coordCount_ == 0 || !staging_) return false;
        uint8_t h[11];
        h[0] = 0x03;
        h[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        h[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        h[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        h[5] = bx_; h[6] = by_; h[7] = bz_;
        h[8] = static_cast<uint8_t>(previewStride_ & 0xFF);
        h[9] = static_cast<uint8_t>(previewStride_ >> 8);
        h[10] = epoch_;
        return broadcaster_->sendBufferedFrame(h, sizeof(h), staging_, stagingUsed_);
    }

    /// Stream one per-frame `0x02` RGB message straight from the producer buffer — no
    /// intermediate copy. Returns whether every client got it (false → tick() drives
    /// adaptive downscaling). Public so tests can drive it without tick()'s rate-limit.
    /// Stream one per-frame `0x04` AIM message, so the preview can draw where each moving head
    /// points. Returns false when there is nothing to send, which is the ordinary case.
    ///
    /// COSTS NOTHING ON A RIG WITHOUT MOTION: the first line is a flag test resolved when the
    /// fixture layout was published, so an LED wall never builds a payload, never allocates and
    /// never touches the socket. That is the requirement this feature had to meet, since most
    /// rigs are strips and panels.
    bool sendAim() {
        Layer* l = layer();
        if (!l) return false;
        const FixtureChannels& fc = l->fixtureChannels();
        if (!fc.movable()) return false;                 // the common case: one branch, then out
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return false;

        const uint8_t* src = sourceBuffer_->data();
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        const nrOfLightsType n = sourceBuffer_->count();
        if (cpl == 0 || n == 0) return false;
        // The staging buffer is sized for RGB (3 bytes/light) at the coord-table build, and aim
        // needs 2, so it always fits. Bail rather than overrun if that ever stops being true.
        if (!staging_ || static_cast<size_t>(coordCount_) * 2 > stagingCap_) return false;

        uint8_t header[9];
        header[0] = 0x04;
        header[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        header[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>(previewStride_ & 0xFF);
        header[6] = static_cast<uint8_t>(previewStride_ >> 8);
        header[7] = epoch_;
        header[8] = 0;   // reserved: keeps the header the same width as 0x02's

        // Gather in the COORD TABLE's order, so aim[k] belongs to the same light color[k] colors.
        // This must walk the lattice exactly as sendFrame does: a flat `i += stride` agrees only
        // on a dense 1D buffer, and on a mapped or sparse layout it silently pairs each beam with
        // a different fixture's aim. A missing axis sends center (128), not 0, which would aim
        // every such head hard over.
        const size_t bodyBytes = static_cast<size_t>(coordCount_) * 2;
        if (!staging_ || stagingCap_ < bodyBytes) return false;   // alloc miss: skip, lossy channel
        const nrOfLightsType s = previewStride_;
        struct AimCtx {
            uint8_t* out; size_t at; const uint8_t* src; nrOfLightsType n; uint8_t cpl;
            uint8_t panOff, tiltOff;
            void emit(nrOfLightsType idx) {
                const uint8_t* px = (idx < n) ? src + static_cast<size_t>(idx) * cpl : nullptr;
                out[at++] = (px && panOff  != FixtureChannels::kAbsent && panOff  < cpl)
                                ? px[panOff]  : 128;
                out[at++] = (px && tiltOff != FixtureChannels::kAbsent && tiltOff < cpl)
                                ? px[tiltOff] : 128;
            }
        };
        AimCtx aim{staging_, 0, src, n, cpl, fc.pan, fc.tilt};
        if (denseGrid()) {
            const lengthType W = layer_->physicalWidth(), H = layer_->physicalHeight();
            const lengthType az = layer_->physicalDepth() > 0 ? layer_->physicalDepth() : 1;
            const lengthType ay = H > 0 ? H : 1, ax = W > 0 ? W : 1;
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s)
                        aim.emit(static_cast<nrOfLightsType>(static_cast<size_t>(z) * H * W
                                                             + static_cast<size_t>(y) * W + x));
        } else if (keptIdx_ && keptCount_ == coordCount_) {
            for (nrOfLightsType k = 0; k < keptCount_; k++) aim.emit(keptIdx_[k]);
        } else {
            struct Skip { AimCtx* aim; nrOfLightsType s; } sk{&aim, s};
            layer_->layouts()->placeLights(CoordSink{[](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<Skip*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                p->aim->emit(idx);
            }, nullptr, &sk});
        }
        return broadcaster_->sendBufferedFrame(header, sizeof(header), staging_, aim.at);
    }

    bool sendFrame() {
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return false;
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        const nrOfLightsType n = sourceBuffer_->count();
        const nrOfLightsType s = previewStride_;

        // Header: [0x02][count:u32 LE][stride:u16 LE][epoch][drops] (9 bytes). count = the kept
        // lights; (epoch, stride) is the client's table-cache key; drops = frames discarded at
        // the source since the last delivered one, the sender-side congestion signal the client's
        // controller adapts on.
        uint8_t header[9];
        header[0] = 0x02;
        header[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        header[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>(s & 0xFF);
        header[6] = static_cast<uint8_t>(s >> 8);
        header[7] = epoch_;
        header[8] = dropsSinceLast_;

        if (s == 1 && cpl == 3 && coordCount_ <= n) {
            // FULL RES, RGB: the producer buffer IS the payload. Hand it to the RESUMABLE buffered
            // send (header copied, body = the producer buffer, a stable pointer) — it drains across
            // transport ticks without a copy and without spinning this loop, the fix for the
            // large-frame stall. The common case (any grid ≤ cap, incl. 16K on a no-PSRAM classic).
            // prepare cancels it before a resize frees the buffer (use-after-free guard).
            const bool ok = broadcaster_->sendBufferedFrame(header, sizeof(header),
                                                            src, static_cast<size_t>(coordCount_) * 3);
            if (ok) dropsSinceLast_ = 0;
            return ok;
        }

        // Downsampled (s>1) or non-RGB (cpl≠3): the producer buffer is not the payload, so gather
        // the kept lights' RGB into the staging buffer (sized at the coord-table build; every
        // caller gates on an idle slot, so no drain is reading it) and hand THAT to the same
        // resumable send the full-res path uses. The gather is a few thousand byte moves on this
        // thread; every socket byte moves on the transport tick. The kept subset + order MUST
        // match the coord table's, so color[k] ↔ coord[k] line up (the browser drops a
        // count/stride-mismatched frame). A dense grid strides its box directly: light (x,y,z) is
        // at buffer index z·H·W + y·W + x, closed-form, no walk over skipped cells. A sparse or
        // mapped layout walks placeLights with the same lattice predicate.
        const size_t bodyBytes = static_cast<size_t>(coordCount_) * 3;
        if (!staging_ || stagingCap_ < bodyBytes) return false;   // alloc miss: skip, lossy channel
        struct ColCtx {
            uint8_t* out; size_t at; const uint8_t* src; nrOfLightsType n; uint8_t cpl;
            void emit(nrOfLightsType idx) {
                const uint8_t* px = (idx < n) ? src + static_cast<size_t>(idx) * cpl : nullptr;
                out[at++] = px ? px[0] : 0;
                out[at++] = (px && cpl >= 2) ? px[1] : 0;
                out[at++] = (px && cpl >= 3) ? px[2] : 0;
            }
        };
        ColCtx col{staging_, 0, src, n, cpl};
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
        const bool ok = broadcaster_->sendBufferedFrame(header, sizeof(header), staging_, col.at);
        if (ok) dropsSinceLast_ = 0;
        return ok;
    }

private:
    /// Free the preview buffers + refresh the memory readout. Cancels any in-flight send first, so
    /// a drain can never outlive the buffer it reads (the use-after-free guard).
    void freePreviewBuffers() {
        if (broadcaster_) broadcaster_->cancelBufferedSend();
        if (keptIdx_) { platform::free(keptIdx_); keptIdx_ = nullptr; keptIdxCap_ = 0; keptCount_ = 0; }
        if (staging_) { platform::free(staging_); staging_ = nullptr; stagingCap_ = 0; }
        publishHeapBytes();
    }

    /// Grow-only staging for the coord-table and gathered-frame bodies: the ONE stable buffer the
    /// resumable drain reads across transport ticks. Rewritten only behind an idle slot; freed only
    /// behind cancelBufferedSend (freePreviewBuffers).
    bool ensureStaging(size_t bytes) {
        if (stagingCap_ >= bytes) return staging_ != nullptr;
        auto* grown = static_cast<uint8_t*>(platform::alloc(bytes));
        if (!grown) return false;
        if (staging_) platform::free(staging_);
        staging_ = grown;
        stagingCap_ = bytes;
        publishHeapBytes();
        return true;
    }


    /// Publish the preview's operating status: who is watching and at what stride normally, or a
    /// WARNING when the index cache could not allocate (RAM-tight board), so the tab shows WHY the
    /// sparse gather fell back to walking placeLights per frame. Called from the cold path
    /// (prepare) and refreshed on the coord rebuild, never the render loop.
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
             + static_cast<size_t>(keptIdxCap_) * sizeof(nrOfLightsType)
             + stagingCap_;
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
        // NO display cap: the only bounds are MEMORY (staging + index tables must fit this
        // board's largest free block) and the index type. Everything else self-degrades where it
        // actually binds: a link that cannot carry full-res frames reports drops and the client
        // asks coarser; a browser that cannot RENDER the points measures its own low fps and asks
        // coarser too. Pre-capping "for the client's sake" only withheld detail from clients that
        // could take it (deduction: the S31's RAM and a desktop GPU both dwarf any fixed number).
        constexpr size_t kReserve = 32u * 1024u;     // leave this much contiguous headroom
        constexpr size_t kBytesPerPoint = 3u;        // RGB on the wire / position bytes in the table
        constexpr nrOfLightsType kFloor = 1024;      // always previewable (hard-downsampled) on any board
        constexpr uint32_t kTypeMax = static_cast<uint32_t>(std::numeric_limits<nrOfLightsType>::max());
        const size_t block = platform::maxAllocBlock();
        // maxAllocBlock() returns 0 = "unlimited / not reported" (desktop, test default).
        if (block == 0) return static_cast<nrOfLightsType>(kTypeMax);
        const size_t usable = block > kReserve ? block - kReserve : 0;
        uint32_t memPts = static_cast<uint32_t>(usable / kBytesPerPoint);
        if (memPts < kFloor) memPts = kFloor;
        if (memPts > kTypeMax) memPts = kTypeMax;
        return static_cast<nrOfLightsType>(memPts);
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
    bool aimTurn_ = false;          // alternates color/aim so each gets its own send slot
    uint8_t* staging_ = nullptr;           // stable body for table + gathered frames (see ensureStaging)
    size_t stagingCap_ = 0;
    nrOfLightsType coordCount_ = 0;        // lights the lattice keeps = the streamed 0x03/0x02 count
    nrOfLightsType previewStride_ = 1;     // wire field: the lattice/downscale factor (1 = full res)
    uint8_t bx_ = 0, by_ = 0, bz_ = 0;
    int32_t posScale_ = 0;            // 0 = positions 1:1; else largest box edge (>255) to scale by
    uint32_t lastSendTime_ = 0;
    // The pull model's standing state, written on the transport thread (onClientMessage /
    // onClientGone), read on the encode thread: single bytes, benign to race on a lossy channel.
    static constexpr int kMaxRequestSlots = 8;
    volatile uint8_t reqStride_[kMaxRequestSlots] = {};   // 0 = no standing request in this slot
    volatile uint8_t reqFps_[kMaxRequestSlots] = {};      // 0 = use the targetFps control
    volatile bool tableRequested_ = false;                // a [0x52] is owed the table
    uint8_t epoch_ = 0;             // bumped per geometry rebuild; the table-cache key's half
    uint8_t dropsSinceLast_ = 0;    // frames discarded at the source since the last delivered one
    size_t stagingUsed_ = 0;        // byte length of the last-built table in staging_

    // The served per-axis lattice stride: the coarsest standing client request (1 = full
    // resolution, the value with no standing request). An extra floor on top of the cap
    // downsample; rides the wire stride field to the browser's status line.
    nrOfLightsType downscale_ = 1;
};

} // namespace mm
