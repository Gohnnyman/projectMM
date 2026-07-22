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
/// **Addressing: unicast to a LIST of receivers, and why that is the default.**
///
/// One driver feeds N receivers (`ips`), each taking a contiguous run of the window (`lightsPerIp`,
/// the same broadcasting idiom as an LED driver's `ledsPerPin`) and each addressed only by itself.
/// So a wall of tubes is one driver, not N — the driver-level twin of one LED driver fanning out to
/// N GPIO lanes.
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
/// **Liveness.** UDP is fire-and-forget, so a dead receiver is invisible to the sender: the send
/// loop simply tolerates it (a failed send drops that packet and moves on, so one dark tube cannot
/// stall the others) rather than pretending to detect it. Real detection is ArtPoll/ArtPollReply
/// discovery — the spec's own mechanism, and the next increment (backlog).
///
/// NOT multicast (no IGMP join yet — backlogged; it is sACN's native mode and the best answer on a
/// switch with IGMP snooping, but degrades to a flood without it). E1.31 framing: CID stable per
/// device (from the MAC), source name `projectMM`, priority 100, one frame-level sequence per frame.
///
/// **Synchronous send:** the whole frame goes out inline in tick() (~35 ms Ethernet / ~90 ms WiFi at
/// 128×128 ArtNet; DDP less). A decoupling send task is a PSRAM-gated backlog item. Added per board
/// via the catalog like the LED drivers; applies the same shared Correction, so network and wired
/// outputs show identical colours.
/// @card NetworkSendDriver.png
class NetworkSendDriver : public DriverBase {
public:
    /// DMX-over-network fixtures (ArtNet / E1.31 / DDP) are RGB by convention — the
    /// xLights/Falcon default — so a network sink references the "RGB" preset by default, unlike the
    /// WS2812 LED drivers that default to the strips' physical "GRB" order. The user can still
    /// pick any preset from the library; this only sets the sensible starting point per driver type.
    NetworkSendDriver() { setDefaultPresetName("RGB"); }

    /// Protocol names, index-aligned with the constants used in tick()'s switch (0 = ArtNet,
    /// 1 = E1.31, 2 = DDP). The destination port follows the protocol (6454 / 5568 / 4048).
    static constexpr const char* kProtocolOptions[] = {"ArtNet", "E1.31", "DDP"};
    static constexpr uint8_t kProtocolCount = 3;

    /// The receivers. A range (`192.168.1.70-74`, ends inclusive) or a list
    /// (`192.168.1.60,61,62,65`); both mix, and a further full address switches subnet. Blank = no
    /// destinations, so the driver idles (never falls back to broadcasting — see the class note).
    /// Capped at a wall of tubes, not a subnet scan.
    static constexpr uint8_t kMaxDestinations = 32;
    char ips[64] = {};
    /// Lights per destination — the same idiom as an LED driver's `ledsPerPin`, and literally the
    /// same helper (`assignCounts`): blank = even split of the window, one number = that many each,
    /// a list = one per destination by position. Each gets a contiguous run, in order.
    char lightsPerIp[64] = {};
    /// Wire protocol (index into kProtocolOptions). Selects both the packet layout and the chunking:
    /// ArtNet / E1.31 split at 510 channels per universe (whole RGB lights, the xLights/Falcon
    /// convention; 170 lights/packet), consecutive universes from `universeStart`; DDP uses
    /// 1440-byte byte-offset chunks (480 lights/packet) and is the fast path — per-packet cost
    /// dominates wire time, so a 128×128 WiFi frame drops from ~110 ms (ArtNet) to ~40 ms.
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
        controls_.addUint16("universe_start", universeStart);
        addWindowControls();   // start / count — the slice of the shared buffer this sink sends
        controls_.addUint8("fps", fps, 1, 120);
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
    void tick() override {
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
                correction_.apply(src + (winStart + i) * srcCh, dst + i * outCh);
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
        const size_t chunk = (protocol == 2) ? DDP_MAX_PAYLOAD : MAX_CHANNELS_PER_UNIVERSE;
        uint8_t packet[DDP_HEADER_SIZE + DDP_MAX_PAYLOAD];  // 1450 B covers all three
        const uint16_t port = protocolPort(protocol);
        const uint8_t bytesPerLight = (data == corrected_.data() && correction_.outChannels)
                                          ? correction_.outChannels
                                          : sourceBuffer_->channelsPerLight();

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
                socket_.sendToAddr(dest_[d], port, packet, packetLen);
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

    // Test-only accessor for the correction-applied buffer. Lets the unit
    // tests pin the no-allocation-in-loop contract (size set in prepare
    // / onCorrectionChanged, never in loop). Not part of any runtime API.
    const Buffer& correctedBuffer() const { return corrected_; }

    /// The destination table prepare() derived: how many receivers, each one's address, and how many
    /// lights of the window it owns. Public for tests pinning the fan-out arithmetic (the same
    /// public-for-tests convention as correctedBuffer() / windowStart()); production reads them
    /// straight from the members in tick().
    uint8_t destinationCount() const { return nDest_; }
    const uint8_t* destinationAt(uint8_t i) const { return dest_[i]; }
    nrOfLightsType lightsAt(uint8_t i) const { return destCounts_[i]; }

private:
    platform::UdpSocket socket_;
    Buffer* sourceBuffer_ = nullptr;
    Buffer corrected_;               // owned: source bytes after brightness/order/white
    uint8_t sequence_ = 0;
    uint32_t lastSendTime_ = 0;
    uint8_t cid_[E131_CID_LENGTH] = {};  // E1.31 component id, built once in setup()
    // Destinations + each one's slice of the window, both derived in prepare() (never in tick()).
    uint8_t dest_[kMaxDestinations][4] = {};
    nrOfLightsType destCounts_[kMaxDestinations] = {};
    uint8_t nDest_ = 0;

    static uint16_t protocolPort(uint8_t p) {
        return p == 1 ? E131_PORT : p == 2 ? DDP_PORT : ARTNET_PORT;
    }

    // Called off the hot path (prepare, onCorrectionChanged, setSourceBuffer) to make
    // sure corrected_ is sized for the current source + this driver's correction. Skips
    // when no source is wired yet, or when the existing allocation already fits.
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
