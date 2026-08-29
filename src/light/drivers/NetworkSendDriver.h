#pragma once

#include "light/drivers/DriverBase.h"

#include "light/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/DdpPacket.h"      // shared DDP wire format
#include "light/E131Packet.h"     // shared E1.31/sACN wire format
#include "core/IpList.h"          // parseIpList — the destination-list parser (core primitive)
#include "light/drivers/PinList.h"  // assignCounts — the same window-split idiom as ledsPerPin
#include "platform/platform.h"


namespace mm {

/// Output driver: streams the buffer over UDP — one driver, three industry protocols selected by a
/// control. The single-node-multiple-protocols shape follows MoonLight's D_NetworkOut (architecture
/// studied, not copied). Byte layouts live in ArtNetPacket.h / E131Packet.h / DdpPacket.h, shared
/// with the receiver so the two sides cannot drift.
///
/// **Addressing:** one driver feeds N receivers (`ips`), each taking a contiguous run of the window
/// (`lightsPerIp`, the same broadcasting idiom as an LED driver's `ledsPerPin`) and each addressed
/// only by itself. So a wall of tubes is one driver, not N — the driver-level twin of one LED driver
/// fanning out to N GPIO lanes. Unicast is the default, not an option among equals; broadcast
/// survives only as legacy compatibility.
///
/// **Synchronous send:** this driver's WINDOW (`start`/`count`, not necessarily the whole buffer)
/// goes out inline in tick() — 38 ms Ethernet to 155 ms WiFi for a full-buffer window at 128×128
/// ArtNet (the spread is the lwIP/WiFi buffer pool, not the protocol: performance.md § ArtNet send),
/// less for DDP and proportionally less for a narrower window. A decoupling send task is a
/// PSRAM-gated backlog item. Added per board via the catalog like the LED drivers; applies the same
/// shared Correction, so network and wired outputs show identical colors.
///
/// The deep dives are under *More info*, below the attribute/method lists:
/// @xref{why-unicast-is-the-default|why unicast is the default},
/// @xref{liveness-and-what-is-not-here|liveness, multicast and E1.31 framing}.
/// @card NetworkSendDriver.png
///
/// @moreinfo
///
/// ## Why unicast is the default
///
/// The Art-Net 4 spec leaves no room here: *"ArtDmx packets must be unicast to subscribers of the
/// specific universe contained in the ArtDmx packet… There are no conditions in which broadcast is
/// allowed."* Broadcast survives only as legacy compatibility (type a broadcast address and it still
/// works, `SO_BROADCAST` is set) — it is never the default.
///
/// **The reason is receive cost, and it is asymmetric.** An Art-Net universe number lives in the
/// PAYLOAD, not in any header a switch or NIC can act on, so a receiver can only discard a universe
/// it does not own AFTER its stack has carried the packet all the way up and parsed it. Broadcast
/// therefore makes *every* host on the segment pay the receive cost of *every* universe, including
/// hosts with nothing to do with lighting. Artistic Licence (the protocol's authors) say it plainly:
/// *"Broadcast data floods the entire network and appears at every node whether it needs it or not.
/// Too much broadcast data overloads switches and nodes alike"* — and Art-Net II added node
/// discovery precisely so a controller could switch to unicast, which they credit with a "massive"
/// reduction in network loading. Two independent sources put the practical broadcast ceiling at
/// ~15 universes (Quasar Science; WLED issue #3297). A 128x128 grid is ~97 universes x 50 fps ~=
/// 4,850 pkt/s (~20 Mbit/s) — measured on the bench starving an ESP32's network stack until its HTTP
/// stopped answering. On WiFi it is worse: broadcast goes out at the lowest basic rate, unACKed, to
/// every station, waking them all from power-save (RFC 9119).
///
/// **What the choice is NOT about.** "Unicast means sending the same data N times" only bites when
/// several nodes need the SAME universe (mirroring). When each node owns a DIFFERENT slice — the
/// normal case, and what this driver models — unicast duplicates nothing: the sender emits exactly
/// as many packets as a broadcast stream would, and each reaches only its owner. So per-node unicast
/// costs the sender the same and costs every other receiver nothing. Mirroring is the one case where
/// a broadcast address is genuinely the better tool.
///
/// ## Liveness, and what is not here
///
/// UDP is fire-and-forget, so a dead receiver is invisible to the sender: the send loop simply
/// tolerates it (a failed send drops that packet and moves on, so one dark tube cannot stall the
/// others) rather than pretending to detect it. Real detection is ArtPoll/ArtPollReply discovery —
/// the spec's own mechanism, and the next increment (backlog).
///
/// "E1.31 multicast" sends to sACN's native per-universe group (239.255.{hi}.{lo}) instead of the
/// configured host: opt-in, because a switch WITHOUT IGMP snooping floods it exactly like
/// broadcast (and on WiFi it goes out at the lowest basic rate to every station), and firmware
/// cannot detect which kind of switch it is on. Unicast stays the portable default. E1.31 framing: CID stable per
/// device (from the MAC), source name `projectMM`, priority 100, one frame-level sequence per frame.
class NetworkSendDriver : public DriverBase {
public:
    /// DMX-over-network fixtures (ArtNet / E1.31 / DDP) are RGB by convention — the
    /// xLights/Falcon default — so a network sink references the "RGB" preset by default, unlike the
    /// WS2812 LED drivers that default to the strips' physical "GRB" order. The user can still
    /// pick any preset from the library; this only sets the sensible starting point per driver type.
    NetworkSendDriver() { setDefaultPresetName("RGB"); }

    /// Protocol names, index-aligned with the constants used in tick()'s switch (0 = ArtNet,
    /// 1 = E1.31, 2 = DDP). The destination port follows the protocol (6454 / 5568 / 4048).
    static constexpr const char* kProtocolOptions[] = {"ArtNet", "E1.31", "DDP",
                                                       "E1.31 multicast"};
    static constexpr uint8_t kProtocolCount = 4;
    static constexpr uint8_t kProtoE131Multicast = 3;

    /// sACN's own group for a universe: 239.255.{universe_hi}.{universe_lo} (E1.31 section
    /// 9.3.1). The universe is IN the address, which is what lets a switch with IGMP snooping
    /// filter per universe in hardware: each node then sees only the universes it joined.
    static void e131MulticastAddr(uint16_t universe, uint8_t out[4]) {
        out[0] = 239; out[1] = 255;
        out[2] = static_cast<uint8_t>(universe >> 8);
        out[3] = static_cast<uint8_t>(universe & 0xFF);
    }


    /// The receivers. A range (`192.168.1.70-74`, ends inclusive) or a list
    /// (`192.168.1.60,61,62,65`); both mix, and a further full address switches subnet. Blank = no
    /// destinations, so the driver idles (never falls back to broadcasting — see the class note).
    /// Capped at a wall of tubes, not a subnet scan.
    static constexpr uint8_t kMaxDestinations = 32;
    /// Receiver addresses, comma-separated (`192.168.1.10,192.168.1.11`). Each takes a contiguous
    /// run of the window per `lightsPerIp`, so the list order is the fan-out order. A broadcast
    /// address works but is not the default — see the unicast rationale above.
    char ips[64] = {};
    /// Lights per destination — the same idiom as an LED driver's `ledsPerPin`, and literally the
    /// same helper (`assignCounts`): blank = even split of the window, one number = that many each,
    /// a list = one per destination by position. Each gets a contiguous run, in order.
    char lightsPerIp[64] = {};
    /// Wire protocol (index into kProtocolOptions). Selects both the packet layout and the chunking:
    /// ArtNet / E1.31 split at 510 channels per universe (whole RGB lights, the xLights/Falcon
    /// convention; 170 lights/packet), consecutive universes from `universeStart`; DDP uses
    /// 1440-byte byte-offset chunks (480 lights/packet) and is the fast path — per-packet cost
    /// dominates wire time, so a 128×128 ArtNet frame drops to roughly a third on DDP. The ArtNet
    /// figure itself spans 38-155 ms across boards (performance.md § ArtNet send): the buffer pool
    /// dominates it, so quote a range rather than one number.
    uint8_t protocol = 0;
    /// First universe the slice maps onto (ArtNet / E1.31; DDP is byte-addressed). Emitted verbatim,
    /// no hidden 1-based adjust: buffer offset = `(universe − universeStart) × 510`. Strict sACN
    /// reserves universe 0, so set ≥ 1 on BOTH ends for it; our own receiver defaults to 0 so
    /// device↔device pairs align out of the box. Orthogonal to the DriverBase window (start/count),
    /// which picks WHICH buffer slice is sent — this picks which universe it lands on.
    uint16_t universeStart = 0;
    /// Send-rate ceiling (Hz); tick() rate-limits to this so a fast render tick doesn't flood the LAN.
    uint8_t fps = 50;

    /// This driver's own controls, added AFTER the per-driver correction block the base places at
    /// the top of every driver card: protocol, destination IP, universe offset, the shared window
    /// (start/count), then the rate cap.
    void defineDriverControls() override {
        controls_.addSelect("protocol", protocol, kProtocolOptions, kProtocolCount);
        controls_.addText("ips", ips, sizeof(ips));
        controls_.addText("lightsPerIp", lightsPerIp, sizeof(lightsPerIp));
        controls_.addControl("universe_start", universeStart);
        addWindowControls();   // start / count — the slice of the shared buffer this sink sends
        controls_.addControl("fps", fps, 1, 120);
    }

    /// A start/count change resizes the window this sink sends, and a Custom channel-count
    /// change grows outChannels; both route through the prepare sweep so resizeCorrected()
    /// re-sizes corrected_ — otherwise growing past the old buffer silently drops to passthrough.
    /// The rest of the correction controls (order/white/brightness) rebuild the LUT in place
    /// via DriverBase::onControlChanged, no prepare needed.
    bool affectsPrepare(const char* name) const override {
        // `ips` / `lightsPerIp` join the window+correction controls: both are PARSED in prepare()
        // (never in tick()), so a change to either must re-run the sweep to re-derive the
        // destination table, the per-destination counts, and the status.
        return std::strcmp(name, "ips") == 0 || std::strcmp(name, "lightsPerIp") == 0
               || isWindowControl(name) || isCorrectionControl(name);
    }


    /// One-time wiring only: derive the stable E1.31 component id (CID) from the MAC once — no
    /// UUID machinery needed for a deterministic, unique-enough id. The socket open (the acquire)
    /// lives in prepare(), the sole resource gate; enabled-independent here.
    void setup() override {
        std::memcpy(cid_, "projectMM\0", 10);
        platform::getMacAddress(cid_ + 10);
    }

    /// Close the socket on release, then chain to the base to clear any status this driver set.
    void release() override {
        socket_.close();
        nDest_ = 0;   // re-derived by prepare() on the next enable
        DriverBase::release();
    }

    /// Take the shared source buffer and (re)size the corrected_ buffer for it. Called from
    /// Drivers::passBufferToDrivers inside prepare (and once at setup); resizeCorrected() sizes
    /// corrected_ to the driver's own correction outChannels. All off the hot path.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        resizeCorrected();
    }

    /// Pure build (see MoonModule::prepare): open the UDP socket (idempotent), re-resolve the
    /// destination, and resize corrected_ off the hot path (tick() never allocates). No enabled()
    /// check — core's applyState() calls this only when effectively-enabled and routes to release()
    /// (socket closed, freed) otherwise, so a disabled sender (or one under a disabled parent) frees it.
    /// Resolve the destination list + each destination's slice of the window. All the parsing and
    /// the arithmetic happen HERE, off the hot path, so tick() is a bare walk of two small arrays —
    /// the same split an LED driver makes between `pins`/`ledsPerPin` (parsed in prepare) and the
    /// per-lane encode (which just reads laneCounts_).
    void prepare() override {
        socket_.open();          // idempotent: no-op if already open
        resizeCorrected();

        // Parse into LOCALS and publish only once EVERYTHING validates. `nDest_` is what tick() reads,
        // so writing it before the parse is fully checked would leave the driver sending to a partially
        // parsed list with stale per-destination counts — real packets to real hosts, despite an error
        // status on the card. On any error the driver idles instead (nDest_ = 0): wrong output is worse
        // than no output. Same all-or-nothing publish an LED driver's lane parse makes.
        uint8_t dest[kMaxDestinations][4] = {};
        uint8_t n = 0;
        const char* err = parseIpList(ips, dest, kMaxDestinations, n);
        if (err) { nDest_ = 0; setStatus(err, Severity::Error); return; }
        if (n == 0) {
            // Say WHY nothing is going out rather than idling silently — the driver looks broken
            // otherwise. Warning, not Error: an unset destination is an unfinished config, not a fault.
            nDest_ = 0;
            setStatus("set a destination ip", Severity::Warning);
            return;
        }

        // Split the window across the destinations — blank = even split, one number = that many
        // each, a list = per-destination by position. The identical rule (and the identical helper)
        // an LED driver uses to split its window across pins.
        nrOfLightsType winStart = 0, winLen = 0;
        if (sourceBuffer_) windowSlice(sourceBuffer_->count(), winStart, winLen);
        nrOfLightsType counts[kMaxDestinations] = {};
        const char* warn = nullptr;
        err = assignCounts(lightsPerIp, n, winLen, counts, 0, &warn);
        if (err) { nDest_ = 0; setStatus(err, Severity::Error); return; }

        // Everything validated — publish as one unit.
        std::memcpy(dest_, dest, sizeof(uint8_t) * 4 * n);
        std::memcpy(destCounts_, counts, sizeof(nrOfLightsType) * n);
        nDest_ = n;
        setStatus(warn, warn ? Severity::Warning : Severity::Status);
    }

    /// A preset toggle (RGB↔RGBW) changes correction_.outChannels without a structural rebuild;
    /// rebuildCorrection() calls this hook so corrected_ tracks the new channel count.
    void onCorrectionChanged() override {
        resizeCorrected();
    }

    /// Rate-limit to `fps`, apply this driver's correction into corrected_ (passthrough if it
    /// emits no channels), then chunk the window slice into protocol packets and send inline.
    void tick() MM_NONBLOCKING override {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;

        // No destination → idle. An unconfigured driver does nothing; it never falls back to
        // broadcasting, which would punish the whole LAN rather than this device (see `ips`).
        if (nDest_ == 0) return;

        // FPS limiting
        if (fps == 0) return;
        uint32_t now = platform::millis();
        uint32_t interval = 1000 / fps;
        if (now - lastSendTime_ < interval) return;
        lastSendTime_ = now;

        // Apply output correction (brightness / channel order / RGBW white) into the
        // pre-sized corrected_ buffer, then send that. Pure reader — sizing happens
        // in resizeCorrected() off the hot path (prepare / onCorrectionChanged
        // / setSourceBuffer / setCorrection). If correction isn't wired (e.g. a unit
        // test constructs the driver outside a Drivers parent) or its buffer doesn't
        // match the source size, fall back to passthrough — same degradation the
        // earlier in-loop allocate had if the allocation itself failed.
        const uint8_t* data;
        size_t totalBytes;
        // Send this sink's window slice [start, start+count) only (count 0 = the
        // whole buffer from start), so it covers just its lights — and a frame
        // isn't packed/sent for lights it doesn't own. winStart is the first light.
        nrOfLightsType winStart, nLights;
        windowSlice(sourceBuffer_->count(), winStart, nLights);
        // Three guards before applying correction: (a) correction produces channels
        // (outChannels != 0 — a Custom wiring with no roles placed emits nothing),
        // (b) corrected_ has the row count we need, (c) corrected_'s per-light stride
        // is at least outChannels — otherwise dst + i * outCh would overrun the
        // allocation. Falls back to passthrough when any guard fails (same degradation
        // the old in-loop allocate had on allocation failure). resizeCorrected() keeps
        // corrected_'s stride in sync with outChannels off the hot path, but the hot-path
        // check stays defensive — a stale corrected_ (e.g. a preset change without
        // onCorrectionChanged firing) should miss the apply, not corrupt memory.
        const uint8_t outCh = correction_.outChannels;
        if (outCh != 0 && corrected_.data()
            && corrected_.count() >= nLights
            && corrected_.channelsPerLight() >= outCh) {
            const uint8_t* src = sourceBuffer_->data();
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            uint8_t* dst = corrected_.data();
            for (nrOfLightsType i = 0; i < nLights; i++) {
                // Read the windowed light (slice starts at winStart); pack densely.
                // srcCh lets a wide light hand its motion channels through (pan/tilt/...).
                correction_.apply(src + (winStart + i) * srcCh, dst + i * outCh, srcCh);
            }
            data = dst;
            totalBytes = static_cast<size_t>(nLights) * outCh;
        } else {
            // Passthrough (no correction): honour the same window as the corrected
            // path — point at the slice start so a sliced sink sends only its lights.
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            data = sourceBuffer_->data() + static_cast<size_t>(winStart) * srcCh;
            totalBytes = static_cast<size_t>(nLights) * srcCh;
        }

        // Fan the frame out to each destination — each gets a CONTIGUOUS RUN of the window, in
        // order (tube 1 the first lights, tube 2 the next), and each is UNICAST only to itself: the
        // packet for a given universe goes once, to the one node that owns it. Art-Net 4 requires
        // exactly this ("ArtDmx packets must be unicast to subscribers of the specific universe"),
        // and it means N tubes cost the same total packets as one broadcast stream — while every
        // other host on the LAN sees none of them.
        //
        // Universes RESTART at universeStart for each destination: each tube is an independent node
        // addressing its own strip from its own first universe, which is what a per-node controller
        // expects and what one-driver-per-node would have produced.
        // DDP is byte-addressed, so it chunks by bytes. ArtNet and E1.31 carry DMX UNIVERSES, and
        // a fixture must not straddle two of them: an 11-channel moving head at 512 bytes per
        // universe would put fixture 47 half in one packet and half in the next, so it would read
        // a neighbour's channels as its own. Round the universe payload DOWN to whole fixtures.
        // (Harmless for a 3-channel strip, where the partial pixel is just a pixel; corrupting for
        // a fixture, which is why this only surfaced with moving heads.)
        size_t chunk = (protocol == 2) ? DDP_MAX_PAYLOAD : MAX_CHANNELS_PER_UNIVERSE;
        uint8_t packet[DDP_HEADER_SIZE + DDP_MAX_PAYLOAD];  // 1450 B covers all three
        const uint16_t port = protocolPort(protocol);
        const uint8_t bytesPerLight = (data == corrected_.data() && correction_.outChannels)
                                          ? correction_.outChannels
                                          : sourceBuffer_->channelsPerLight();
        if (protocol != 2 && bytesPerLight > 1) {
            const size_t whole = (chunk / bytesPerLight) * bytesPerLight;
            // A fixture WIDER than a universe cannot be served at all: keep the full universe
            // rather than sending zero bytes forever, so the failure is a visibly wrong fixture
            // instead of a silent dead output.
            if (whole > 0) chunk = whole;
        }

        size_t offset = 0;   // byte cursor into `data`, walking destination by destination
        for (uint8_t d = 0; d < nDest_ && offset < totalBytes; d++) {
            // This destination's run: its light count × the wire stride, clipped to what's left.
            size_t runBytes = static_cast<size_t>(destCounts_[d]) * bytesPerLight;
            if (offset + runBytes > totalBytes) runBytes = totalBytes - offset;
            if (runBytes == 0) continue;   // a zero-count destination is configured but idle

            uint16_t universe = universeStart;   // restart per destination
            size_t sent = 0;
            while (sent < runBytes) {
                const size_t n = std::min(runBytes - sent, chunk);
                const uint8_t* src = data + offset + sent;
                size_t packetLen;
                switch (protocol) {
                    case kProtoE131Multicast:   // same packet as E1.31, only the destination differs
                    case 1:
                        packetLen = buildE131Packet(packet, universe, sequence_, cid_,
                                                    src, static_cast<uint16_t>(n));
                        break;
                    case 2:
                        // DDP is byte-addressed: offsets are relative to THIS destination's strip,
                        // which starts at 0 on its own controller.
                        packetLen = buildDdpPacket(packet, static_cast<uint32_t>(sent),
                                                   /*push=*/sent + n >= runBytes,
                                                   src, static_cast<uint16_t>(n));
                        break;
                    default:
                        packetLen = buildArtDmxPacket(packet, universe, sequence_,
                                                      src, static_cast<uint16_t>(n));
                        break;
                }
                // Explicit per-packet address — one socket, N destinations. A send that fails (a
                // full tx buffer, an unreachable host) drops that packet and the loop continues:
                // one dark tube must not stall the others, and UDP gives us no delivery signal to
                // act on anyway. A never-answering host costs only lwIP's slow background ARP retry.
                // sACN addresses the UNIVERSE, not the node: the group carries the universe
                // number, so one send reaches every receiver that joined it and a switch with
                // IGMP snooping filters the rest out in hardware. Unicast E1.31 keeps using
                // the configured destination, which stays the portable default.
                uint8_t grp[4];
                const uint8_t* to = dest_[d];
                if (protocol == kProtoE131Multicast) { e131MulticastAddr(universe, grp); to = grp; }
                socket_.sendToAddr(to, port, packet, packetLen);
                sent += n;
                universe++;
            }
            offset += runBytes;
        }

        sequence_++;
    }

    // The packet builds, the constants, and the inverse parses live in
    // light/ArtNetPacket.h, light/E131Packet.h and light/DdpPacket.h, shared
    // with NetworkReceiveEffect — each wire format exists in exactly one place.

    /// Test-only accessor for the correction-applied buffer, letting the unit tests pin the
    /// no-allocation-in-loop contract (sized in prepare / onCorrectionChanged, never in the send
    /// loop). Not part of any runtime API.
    const Buffer& correctedBuffer() const { return corrected_; }

    /// The destination table prepare() derived: how many receivers, each one's address, and how many
    /// lights of the window it owns. Public for tests pinning the fan-out arithmetic (the same
    /// public-for-tests convention as correctedBuffer() / windowStart()); production reads them
    /// straight from the members in tick().
    uint8_t destinationCount() const { return nDest_; }
    const uint8_t* destinationAt(uint8_t i) const { return dest_[i]; }
    nrOfLightsType lightsAt(uint8_t i) const { return destCounts_[i]; }

private:
    /// The send socket: opened in prepare() (the sole resource gate), reused for every
    /// destination, and closed in release() so a disabled driver holds no socket.
    platform::UdpSocket socket_;
    /// The shared frame this driver reads its window from; borrowed, not owned.
    Buffer* sourceBuffer_ = nullptr;
    /// Owned: source bytes after brightness/order/white. Sized off the hot path (resizeCorrected).
    Buffer corrected_;
    /// Art-Net/E1.31 per-frame sequence counter; wraps at 255, which both protocols expect.
    uint8_t sequence_ = 0;
    /// millis() of the last frame sent — the `fps` rate limiter's reference point.
    uint32_t lastSendTime_ = 0;
    /// E1.31 component id, built once in setup() from the MAC so it is stable per device.
    uint8_t cid_[E131_CID_LENGTH] = {};
    /// Destination addresses, derived in prepare() (never in tick()).
    uint8_t dest_[kMaxDestinations][4] = {};
    /// Each destination's slice of the window, index-aligned with `dest_`.
    nrOfLightsType destCounts_[kMaxDestinations] = {};
    /// How many entries of `dest_` / `destCounts_` are live.
    uint8_t nDest_ = 0;

    /// The UDP port for a protocol index — each wire format has its own registered port.
    static uint16_t protocolPort(uint8_t p) {
        return (p == 1 || p == kProtoE131Multicast) ? E131_PORT
             : p == 2 ? DDP_PORT : ARTNET_PORT;
    }

    /// Size `corrected_` for the current source and this driver's correction. Called only off the
    /// hot path (prepare, onCorrectionChanged, setSourceBuffer) — that placement IS the
    /// no-allocation-in-the-send-loop contract. Skips when no source is wired yet, or when the
    /// existing allocation already fits.
    void resizeCorrected() {
        if (!sourceBuffer_) return;
        // Size for the window slice this sender actually transmits, not the whole
        // frame — a sink covering 64 of a 16K-light buffer reserves 64. The same
        // windowSlice() the send loop uses, so the buffers stay in lock-step.
        nrOfLightsType winStart, n;
        windowSlice(sourceBuffer_->count(), winStart, n);
        const uint8_t ch = correction_.outChannels;
        if (n == 0 || ch == 0) return;
        if (corrected_.count() >= n && corrected_.channelsPerLight() >= ch) return;
        corrected_.allocate(n, ch);
    }
};

} // namespace mm
