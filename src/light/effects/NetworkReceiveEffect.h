#pragma once

#include "light/effects/EffectBase.h"

#include "light/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/DdpPacket.h"      // shared DDP wire format
#include "light/E131Packet.h"     // shared E1.31/sACN wire format
#include "light/layers/Layer.h"    // Layer::bufferGen — is the held frame still ours? (see tick)
#include "platform/platform.h"    // platform::UdpSocket (the receive sockets — not a scratch buffer)

namespace mm {

// Lights-over-UDP receiver as an EFFECT: external light data is just another
// module that writes into the layer buffer, composable with modifiers and
// blending like any generated effect. The end-to-end pair with
// NetworkSendDriver — and the receive side for industry senders (Resolume,
// Madrix, xLights, LedFx, …).
//
// All three protocols are received AT ONCE: the effect binds the three
// well-known ports (ArtNet 6454, E1.31 5568, DDP 4048) and validates each
// packet against its port's wire format — WLED's multi-port pattern. There is
// deliberately no protocol control: whatever a sender speaks just works, and
// the status field shows what is being received.
//
// ArtNet discovery: controllers (Resolume's Advanced Output, Madrix, xLights)
// find nodes by broadcasting ArtPoll; this effect answers with ArtPollReply so
// the device appears in their node lists instead of needing manual IP entry.
//
// The layer clears its buffer at the start of every tick, so packets are
// drained into an owned STAGING buffer and staging is copied to the layer
// buffer each tick — hold-last-frame semantics; without it the lights would
// strobe black between frames. The drain is non-blocking and bounded per tick
// (network input is synchronous at the frame boundary — the architecture.md
// rule), so a packet flood can't wedge the render loop. Sequence fields (and
// DDP's push flag) are ignored: last write wins into staging.
//
// Prior art: MoonLight's D_NetworkIn (single node, three protocols), WLED's
// realtime UDP input (multi-port + per-packet validation, ArtPollReply), and
// projectMM v1's ArtNetInModule.
// Author: projectMM original (E1.31 / Art-Net receive)
/// Effect that paints the layer from received Art-Net/E1.31/DDP pixels.
class NetworkReceiveEffect : public EffectBase {
public:
    const char* tags() const override { return "📡🌙"; }  // network input · MoonLight / v1 lineage

    uint16_t universeStart = 0;        // mirrors the sender's universe_start (ArtNet/E1.31)
    // Bytes each universe maps to in the buffer. 510 = whole RGB lights per
    // universe (the xLights/Falcon convention and our sender's split); set 512
    // for senders that pack pixels across universe boundaries (Madrix-style).
    // Also clamps the copied payload, so a 512-channel frame from a 510-packed
    // source can't bleed its 2 padding bytes into the next universe's data.
    uint16_t channelsPerUniverse = static_cast<uint16_t>(MAX_CHANNELS_PER_UNIVERSE);

    void defineControls() override {
        controls_.addUint16("universe_start", universeStart);
        controls_.addUint16("channels_per_universe", channelsPerUniverse);
    }

    void release() override {
        // Close the sockets (this effect's own non-buffer state), then chain to the base — which
        // frees the registered staging_ ScratchBuffer (and recurses to children). Chaining is
        // required: without MoonModule::release() the staging buffer would leak on disable.
        artnetSocket_.close();
        e131Socket_.close();
        ddpSocket_.close();
        MoonModule::release();
        clearStatus();
    }

    /// Pure build (see MoonModule::prepare): open + bind the three receive sockets (each bind
    /// independent so one taken port can't stop the others) and size the staging buffer to the layer
    /// (one byte per channel byte, zeroed so a fresh grid starts dark). No enabled() check — core's
    /// applyState() calls this only when effectively-enabled and routes to release() (sockets closed,
    /// staging freed) otherwise, so a disabled effect (or one under a disabled parent) frees the ports.
    void prepare() override {
        const bool artnetOk = artnetSocket_.open() && artnetSocket_.bind(ARTNET_PORT);
        const bool e131Ok = e131Socket_.open() && e131Socket_.bind(E131_PORT);
        const bool ddpOk = ddpSocket_.open() && ddpSocket_.bind(DDP_PORT);
        if (artnetOk && e131Ok && ddpOk) {
            if (status() == kBindFailMsg) clearStatus();
        } else {
            setStatus(kBindFailMsg, Severity::Error);
        }
        // Size the staging buffer to the layer (one byte per channel byte). resize() reallocs
        // (zero-filled) only when the byte count changes, frees on 0, and keeps dynamicBytes current.
        staging_.resize(static_cast<size_t>(nrOfLights()) * channelsPerLight());
        dirty_ = true;   // a resize (or a re-enable) must repaint the layer from staging once
    }

    void tick() MM_NONBLOCKING override {
        if (!staging_) return;
        // Bounded non-blocking drain per socket: 128 packets ≈ one full ArtNet
        // frame for ~21k RGB lights; a flood costs at most 3×128 recvfrom calls
        // per tick, then the rest waits in the socket buffers for the next tick.
        uint16_t universe = 0, dataLen = 0;
        uint32_t byteOffset = 0;
        const uint8_t* data = nullptr;
        uint8_t srcIp[4];
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = artnetSocket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseArtDmxPacket(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                applyDmx(universe, data, dataLen);
                noteReceiving("Art-Net", srcIp);
            } else if (isArtPoll(pkt_, static_cast<size_t>(n))) {
                replyToPoll(srcIp);   // make the device show up in controller node lists
            }
        }
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = e131Socket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseE131Packet(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                applyDmx(universe, data, dataLen);
                noteReceiving("E1.31", srcIp);
            }
        }
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = ddpSocket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseDdpPacket(pkt_, static_cast<size_t>(n), byteOffset, data, dataLen)) {
                applyBytes(byteOffset, data, dataLen);
                noteReceiving("DDP", srcIp);
            }
        }
        // Staging → layer buffer. The Layer does not clear between frames, so when no packet
        // arrived AND nothing else touched the buffer, it still holds our last frame and the copy
        // would write identical bytes: 3.5 ms per tick at 12288 lights on an S3, for nothing.
        // Both halves are required. A sibling effect's tick, the collected fade (which the Layer
        // runs BEFORE the effect pass) or the live-modifier pass all leave the held frame altered,
        // and hold-last-frame is this module's contract — so the layer's write generation, not just
        // our own packet flag, decides. One uint32 compare, no scan.
        uint8_t* buf = buffer();
        if (!buf) return;
        const auto* layer = static_cast<const Layer*>(parent());
        const uint32_t gen = layer ? layer->bufferGen() : 0;
        if (!dirty_ && layer && gen == lastGen_) return;
        dirty_ = false;
        const size_t bufBytes = static_cast<size_t>(nrOfLights()) * channelsPerLight();
        std::memcpy(buf, staging_.data(), staging_.bytes() < bufBytes ? staging_.bytes() : bufBytes);
        // Record the generation our write produces: the Layer bumps it right after this tick
        // returns (the effect loop counts every effect), so the next tick compares against the
        // value that includes our own copy.
        lastGen_ = gen + 1;
    }

    // Place one universe's payload: byte offset (universe − universeStart) ×
    // channels_per_universe, payload clamped to one universe's stride so a
    // 512-channel frame can't bleed past its slot. Universes below the start or
    // beyond the buffer are ignored. Shared by ArtNet and E1.31; DDP skips the
    // universe math and calls applyBytes directly. Public for testability (the
    // buildArtDmxPacket precedent).
    void applyDmx(uint16_t universe, const uint8_t* data, uint16_t len) {
        if (universe < universeStart || channelsPerUniverse == 0) return;
        if (len > channelsPerUniverse) len = channelsPerUniverse;
        applyBytes(static_cast<size_t>(universe - universeStart) * channelsPerUniverse,
                   data, len);
    }

    // The one clamped write into staging (DDP's native addressing). The bound
    // check runs BEFORE any addition so a hostile 32-bit offset can't overflow
    // past it.
    void applyBytes(size_t offset, const uint8_t* data, uint16_t len) {
        if (!staging_ || offset >= staging_.bytes()) return;
        size_t n = len;
        if (offset + n > staging_.bytes()) n = staging_.bytes() - offset;
        std::memcpy(staging_.data() + offset, data, n);
        dirty_ = true;   // the one write into staging — tick() copies to the layer only after this
    }

    // Test-only accessors — let the unit tests pin the staging lifecycle
    // (sized off the hot path, never reallocated by loop, freed on release).
    const uint8_t* stagingData() const { return staging_.data(); }
    size_t stagingBytes() const { return staging_.bytes(); }

private:
    static constexpr int kMaxPacketsPerTick = 128;
    static constexpr const char* kBindFailMsg = "UDP bind failed — port in use?";
    // The receiving-status buffer: "receiving <protocol> from <ip>", the ONE writable status this effect
    // shows — the same field reports the protocol AND the sender IP, so it answers "who is driving me".
    // setStatus holds the pointer (doesn't copy), so the string must outlive the call, hence a member, not
    // a stack buffer. Sized for the longest form ("receiving Art-Net from 255.255.255.255" = 38 chars +
    // NUL). A bind error uses its own static literal. lastProto_/lastIp_ cache the last-formatted source so
    // noteReceiving can early-out on the common case (same sender + protocol) without even formatting.
    char recvStatus_[40] = "";
    const char* lastProto_ = nullptr;   // last protocol pointer the status was built from (literal, stable)
    uint8_t     lastIp_[4] = {};        // last sender IP the status was built from

    platform::UdpSocket artnetSocket_;
    platform::UdpSocket e131Socket_;
    platform::UdpSocket ddpSocket_;
    uint8_t pkt_[1500] = {};       // one datagram, any protocol (DDP max 1450)
    // Layer-buffer-sized staging (hold-last-frame). Self-sizing, self-freeing, self-reporting;
    // freed on disable via MoonModule::release() (the release() override chains to it).
    ScratchBuffer<uint8_t> staging_{*this};
    // Set by applyBytes, consumed by tick(): staging changed since the last copy to the layer.
    // Starts true so a fresh (zeroed) staging paints once, rather than leaving whatever the
    // previous effect left in the shared layer buffer.
    bool dirty_ = true;
    // The layer's write generation as of our last copy. Anything else writing the shared buffer
    // moves it, which is what makes skipping the copy safe (see tick).
    uint32_t lastGen_ = 0;

    // Update the "receiving <protocol> from <ip>" diagnostic, but never clobber a bind error (its status is
    // the kBindFailMsg literal, so status() != recvStatus_). The common case is the same sender + protocol
    // every packet, so short-circuit on the cached lastProto_/lastIp_ FIRST — no formatting at all — and
    // only snprintf + setStatus when the source actually changes. proto is always one of the same three
    // string literals, so a pointer compare identifies it. This runs on the receive hot path.
    void noteReceiving(const char* proto, const uint8_t ip[4]) {
        const char* s = status();
        if (s != nullptr && s != recvStatus_) return;   // a bind error (or foreign status) wins
        // Already showing this exact source? nothing to do (skip the format + the setStatus).
        if (s == recvStatus_ && proto == lastProto_ && std::memcmp(ip, lastIp_, 4) == 0) return;
        lastProto_ = proto;
        std::memcpy(lastIp_, ip, 4);
        std::snprintf(recvStatus_, sizeof(recvStatus_), "receiving %s from %u.%u.%u.%u", proto,
                      static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
                      static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));
        setStatus(recvStatus_, Severity::Status);
    }

    // Answer an ArtPoll with our IP/MAC/name so controllers list the device.
    // Runs at most once per poll (controllers poll every few seconds) — the
    // 239-byte reply lives on the stack, no allocation.
    void replyToPoll(const uint8_t pollerIp[4]) {
        uint8_t myIp[4];
        platform::ethGetIPv4(myIp);
        if (!myIp[0] && !myIp[1] && !myIp[2] && !myIp[3]) platform::wifiStaGetIPv4(myIp);
        // Desktop has no eth/wifi netif (both return 0.0.0.0); hostIp() reports the
        // host's LAN IP as a string, so parse that as the last resort.
        if (!myIp[0] && !myIp[1] && !myIp[2] && !myIp[3]) {
            if (!parseDottedQuad(platform::hostIp(), myIp)) return;  // no usable IP — stay silent
        }
        uint8_t mac[6];
        platform::getMacAddress(mac);
        uint8_t reply[ARTNET_POLL_REPLY_SIZE];
        buildArtPollReply(reply, myIp, mac, "projectMM", "projectMM NetworkReceive",
                          universeStart);
        artnetSocket_.sendToAddr(pollerIp, ARTNET_PORT, reply, sizeof(reply));
    }
};

} // namespace mm
