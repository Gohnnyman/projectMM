#pragma once

// MQTT 3.1.1 wire framing — pure C++, no ESP-IDF or stdlib-network deps.
//
// Wire format (OASIS MQTT Version 3.1.1, §2 Fixed Header, §3 Control Packets):
//   [type<<4 | flags][remaining-length varint][variable header + payload]
//
// This header owns the framing + the handful of control packets a client light needs — CONNECT,
// PUBLISH (both directions), SUBSCRIBE, PINGREQ, DISCONNECT to send; CONNACK, SUBACK, PINGRESP,
// PUBLISH to receive. It is deliberately dependency-free so the packet math is host-unit-tested
// (test/unit/core/unit_MqttPacket.cpp) with golden byte vectors, exactly like ImprovFrame.h. The
// live socket lifecycle (connect/keepalive/reconnect) lives in MqttModule; this is only the bytes.
//
// Prior art: the protocol is the OASIS MQTT 3.1.1 standard (the same framing mosquitto, HiveMQ, and
// homebridge-mqttthing speak). Written fresh against our own buffers — no library.

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mm {

// --8<-- [start:mqtt-constants]
// Control packet types — the high nibble of the fixed header's first byte (§2.2.1). The low nibble
// carries per-type flags; for the packets we send it is 0 except SUBSCRIBE (0x2) and PUBLISH (QoS in
// bits 1-2 — we send QoS0, so 0).
enum class MqttPacketType : uint8_t {
    Connect     = 0x1,
    Connack     = 0x2,
    Publish     = 0x3,
    Suback      = 0x9,
    Subscribe   = 0x8,
    Pingreq     = 0xC,
    Pingresp    = 0xD,
    Disconnect  = 0xE,
};

// The protocol-name + level a 3.1.1 CONNECT carries in its variable header (§3.1.2.1).
inline constexpr char    kMqttProtocolName[4] = {'M','Q','T','T'};
inline constexpr uint8_t kMqttProtocolLevel   = 0x04;   // 4 == MQTT 3.1.1

// CONNECT flag bits (§3.1.2.3). We use clean-session + optionally username/password.
inline constexpr uint8_t kMqttConnectCleanSession = 0x02;
inline constexpr uint8_t kMqttConnectPasswordFlag = 0x40;
inline constexpr uint8_t kMqttConnectUsernameFlag = 0x80;
// --8<-- [end:mqtt-constants]

// --- Remaining-length varint (§2.2.3) ---
// 1-4 bytes, 7 data bits each, high bit = "more follows". The one genuinely fiddly bit of MQTT, so
// it's its own pair of pure functions, boundary-tested at 0 / 127 / 128 / 16383 / 16384.

// Encode `value` (0 .. 268435455) into `out` (needs ≤4 bytes). Returns bytes written, or 0 if the
// value exceeds the 4-byte maximum.
inline size_t encodeRemainingLength(uint32_t value, uint8_t* out) {
    if (value > 268435455u) return 0;   // 0xFFFFFFF — the 4-byte ceiling
    size_t n = 0;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value > 0) byte |= 0x80;    // more bytes follow
        out[n++] = byte;
    } while (value > 0);
    return n;
}

// Decode a remaining-length varint from `in` (up to `inLen` bytes). On success writes the value to
// `*value` and the varint's byte count to `*consumed`, returns true. Returns false if the field is
// truncated (need more bytes) or malformed (5th continuation byte — a protocol violation).
inline bool decodeRemainingLength(const uint8_t* in, size_t inLen,
                                  uint32_t* value, size_t* consumed) {
    uint32_t result = 0;
    uint32_t multiplier = 1;
    size_t i = 0;
    for (;;) {
        if (i >= inLen) return false;            // truncated — caller feeds more
        if (i >= 4) return false;                // >4 bytes → malformed varint
        const uint8_t byte = in[i++];
        result += static_cast<uint32_t>(byte & 0x7F) * multiplier;
        if ((byte & 0x80) == 0) break;           // high bit clear → last byte
        multiplier <<= 7;
    }
    *value = result;
    *consumed = i;
    return true;
}

// --- Builders (§3) ---
// Each writes a complete packet into a caller-owned buffer and returns the byte count, or 0 on
// overflow / bad input. No allocation. A 16-bit-length-prefixed UTF-8 string field (§1.5.3) is the
// repeating shape, so it gets a small helper.

// Append a 2-byte-big-endian-length-prefixed string. Returns the new write position, or 0 on
// overflow. `len == 0` writes just the 2-byte zero length (a valid empty string).
inline size_t mqttAppendString(uint8_t* out, size_t outLen, size_t pos,
                               const char* s, size_t len) {
    if (pos + 2 + len > outLen) return 0;
    out[pos++] = static_cast<uint8_t>((len >> 8) & 0xFF);
    out[pos++] = static_cast<uint8_t>(len & 0xFF);
    if (len > 0) { std::memcpy(out + pos, s, len); pos += len; }
    return pos;
}

// Assemble a fixed header (type/flags + remaining-length) followed by an already-built variable
// header+payload of `bodyLen` bytes sitting at `out + <fixed header size>`. Because the fixed
// header's size depends on the varint length, builders write the body into a scratch region first,
// then this prepends the header. To avoid a second buffer we compute the header, then memmove — but
// bodies here are small and known, so builders instead write the header first with a pre-computed
// length. This helper writes just the fixed header; callers pass the final bodyLen.
inline size_t mqttWriteFixedHeader(uint8_t* out, size_t outLen,
                                   MqttPacketType type, uint8_t flags, uint32_t bodyLen) {
    if (outLen < 1) return 0;
    out[0] = static_cast<uint8_t>((static_cast<uint8_t>(type) << 4) | (flags & 0x0F));
    const size_t rl = encodeRemainingLength(bodyLen, out + 1);
    if (rl == 0 || 1 + rl > outLen) return 0;
    return 1 + rl;   // total fixed-header size
}

// CONNECT (§3.1). clientId is required; username/password optional (nullptr → omitted). keepalive is
// in seconds. Clean session always set (we don't resume state). Returns total bytes, 0 on overflow.
inline size_t buildMqttConnect(const char* clientId,
                               const char* username, const char* password,
                               uint16_t keepaliveSec, uint8_t* out, size_t outLen) {
    if (!clientId) return 0;
    // Treat an empty-string username as "no username" (a UI text control left blank is "", not null),
    // and — MQTT-3.1.2-22 — the password flag MUST NOT be set without the username flag, so a password
    // with no username is dropped (a compliant broker would reject the whole CONNECT otherwise).
    if (username && !username[0]) username = nullptr;
    if (!username) password = nullptr;
    const size_t idLen   = std::strlen(clientId);
    const size_t userLen = username ? std::strlen(username) : 0;
    const size_t passLen = password ? std::strlen(password) : 0;

    // Variable header: protocol name (6) + level (1) + connect flags (1) + keepalive (2) = 10.
    // Payload: clientId, then username, then password — each length-prefixed.
    uint8_t connectFlags = kMqttConnectCleanSession;
    if (username) connectFlags |= kMqttConnectUsernameFlag;
    if (password) connectFlags |= kMqttConnectPasswordFlag;

    const uint32_t bodyLen = static_cast<uint32_t>(
        2 + 4 + 1 + 1 + 2 +                       // proto name(len+"MQTT") + level + flags + keepalive
        2 + idLen +
        (username ? 2 + userLen : 0) +
        (password ? 2 + passLen : 0));

    size_t pos = mqttWriteFixedHeader(out, outLen, MqttPacketType::Connect, 0, bodyLen);
    if (pos == 0) return 0;

    // Variable header
    pos = mqttAppendString(out, outLen, pos, kMqttProtocolName, 4);   // "MQTT"
    if (pos == 0) return 0;
    if (pos + 4 > outLen) return 0;
    out[pos++] = kMqttProtocolLevel;
    out[pos++] = connectFlags;
    out[pos++] = static_cast<uint8_t>((keepaliveSec >> 8) & 0xFF);
    out[pos++] = static_cast<uint8_t>(keepaliveSec & 0xFF);

    // Payload
    pos = mqttAppendString(out, outLen, pos, clientId, idLen);
    if (pos == 0) return 0;
    if (username) { pos = mqttAppendString(out, outLen, pos, username, userLen); if (pos == 0) return 0; }
    if (password) { pos = mqttAppendString(out, outLen, pos, password, passLen); if (pos == 0) return 0; }
    return pos;
}

// PUBLISH (§3.3), QoS0 (no packet identifier), not duplicate. `retain` sets the RETAIN flag (bit 0
// of the fixed-header flags nibble, §3.3.1.3) — a retained topic (the friendly `name`) is redelivered
// to a late-subscribing hub. Returns total bytes.
inline size_t buildMqttPublish(const char* topic, const uint8_t* payload, size_t payloadLen,
                               uint8_t* out, size_t outLen, bool retain = false) {
    if (!topic) return 0;
    const size_t topicLen = std::strlen(topic);
    const uint32_t bodyLen = static_cast<uint32_t>(2 + topicLen + payloadLen);   // topic (QoS0: no id)
    size_t pos = mqttWriteFixedHeader(out, outLen, MqttPacketType::Publish,
                                      retain ? 0x1 : 0x0, bodyLen);
    if (pos == 0) return 0;
    pos = mqttAppendString(out, outLen, pos, topic, topicLen);
    if (pos == 0) return 0;
    if (payloadLen > 0) {
        if (pos + payloadLen > outLen) return 0;
        std::memcpy(out + pos, payload, payloadLen);
        pos += payloadLen;
    }
    return pos;
}

// SUBSCRIBE (§3.8), one topic filter at QoS0. Flags nibble is 0x2 (reserved, mandated by spec).
// `packetId` must be non-zero. Returns total bytes.
inline size_t buildMqttSubscribe(uint16_t packetId, const char* topic, uint8_t* out, size_t outLen) {
    if (!topic || packetId == 0) return 0;
    const size_t topicLen = std::strlen(topic);
    const uint32_t bodyLen = static_cast<uint32_t>(2 + 2 + topicLen + 1);   // packetId + filter + QoS
    size_t pos = mqttWriteFixedHeader(out, outLen, MqttPacketType::Subscribe, 0x2, bodyLen);
    if (pos == 0) return 0;
    if (pos + 2 > outLen) return 0;
    out[pos++] = static_cast<uint8_t>((packetId >> 8) & 0xFF);
    out[pos++] = static_cast<uint8_t>(packetId & 0xFF);
    pos = mqttAppendString(out, outLen, pos, topic, topicLen);
    if (pos == 0) return 0;
    if (pos + 1 > outLen) return 0;
    out[pos++] = 0x00;   // requested QoS 0
    return pos;
}

// PINGREQ (§3.12) and DISCONNECT (§3.14) — 2-byte packets (type + zero remaining-length).
inline size_t buildMqttPingreq(uint8_t* out, size_t outLen) {
    return mqttWriteFixedHeader(out, outLen, MqttPacketType::Pingreq, 0, 0);
}
inline size_t buildMqttDisconnect(uint8_t* out, size_t outLen) {
    return mqttWriteFixedHeader(out, outLen, MqttPacketType::Disconnect, 0, 0);
}

// --- Inbound frame parser (byte-at-a-time state machine, ImprovFrameParser shape) ---
// The received side: the module feeds socket bytes here and inspects lastType() + the parsed body.
// A CONNACK's return code is read directly off the completed body (body()[1]); SUBACK/PINGRESP need
// no field beyond the type — so no standalone per-type parse helper is carried (the outbound build*
// helpers have callers; a receive-side raw-packet parser would not).
// Feeds off a non-blocking socket that hands over arbitrary byte runs; reassembles one control
// packet at a time so the receive path is host-testable with no socket. On a complete PUBLISH it
// exposes topic()/payload()/payloadLen(); for other types the caller reads lastType().

inline constexpr size_t kMqttMaxPacket = 512;   // control packets a light exchanges are tiny

enum class MqttFeedResult : uint8_t {
    NeedMore,       // mid-packet
    PacketReady,    // a complete packet is available via lastType() + (for PUBLISH) topic()/payload()
    Malformed,      // bad varint / oversize packet — parser resynced
};

class MqttInboundParser {
public:
    // Feed one received byte. Returns NeedMore until a full control packet has been read.
    MqttFeedResult feed(uint8_t byte) {
        switch (state_) {
            case State::FixedHeader:
                type_ = static_cast<uint8_t>(byte >> 4);
                flags_ = static_cast<uint8_t>(byte & 0x0F);
                rlValue_ = 0;
                rlMultiplier_ = 1;
                rlBytes_ = 0;
                state_ = State::RemainingLength;
                return MqttFeedResult::NeedMore;
            case State::RemainingLength: {
                rlValue_ += static_cast<uint32_t>(byte & 0x7F) * rlMultiplier_;
                rlMultiplier_ <<= 7;
                if (++rlBytes_ > 4) { reset(); return MqttFeedResult::Malformed; }  // >4 → malformed
                if ((byte & 0x80) == 0) {                                           // last length byte
                    if (rlValue_ > kMqttMaxPacket) { reset(); return MqttFeedResult::Malformed; }
                    bodyLen_ = rlValue_;
                    bodyPos_ = 0;
                    if (bodyLen_ == 0) { state_ = State::FixedHeader; return MqttFeedResult::PacketReady; }
                    state_ = State::Body;
                }
                return MqttFeedResult::NeedMore;
            }
            case State::Body:
                body_[bodyPos_++] = byte;
                if (bodyPos_ >= bodyLen_) {
                    state_ = State::FixedHeader;
                    return MqttFeedResult::PacketReady;
                }
                return MqttFeedResult::NeedMore;
        }
        return MqttFeedResult::NeedMore;   // unreachable
    }

    uint8_t lastType() const { return type_; }
    uint8_t lastFlags() const { return flags_; }
    const uint8_t* body() const { return body_; }
    size_t bodyLen() const { return bodyLen_; }

    // For a completed PUBLISH: the topic (NUL-terminated in topicBuf_) and the payload slice. Returns
    // false if the last packet wasn't a well-formed PUBLISH. QoS0 assumed (no packet-id in the body).
    bool publish(const char** topic, const uint8_t** payload, size_t* payloadLen) {
        if (type_ != static_cast<uint8_t>(MqttPacketType::Publish)) return false;
        if (bodyLen_ < 2) return false;
        const size_t tLen = static_cast<size_t>((body_[0] << 8) | body_[1]);
        if (2 + tLen > bodyLen_) return false;
        if (tLen >= sizeof(topicBuf_)) return false;
        std::memcpy(topicBuf_, body_ + 2, tLen);
        topicBuf_[tLen] = '\0';
        if (topic) *topic = topicBuf_;
        if (payload) *payload = body_ + 2 + tLen;
        if (payloadLen) *payloadLen = bodyLen_ - 2 - tLen;
        return true;
    }

private:
    void reset() { state_ = State::FixedHeader; }
    enum class State : uint8_t { FixedHeader, RemainingLength, Body };
    State    state_ = State::FixedHeader;
    uint8_t  type_ = 0;
    uint8_t  flags_ = 0;
    uint32_t rlValue_ = 0;
    uint32_t rlMultiplier_ = 1;
    uint8_t  rlBytes_ = 0;
    size_t   bodyLen_ = 0;
    size_t   bodyPos_ = 0;
    uint8_t  body_[kMqttMaxPacket] = {};
    char     topicBuf_[128] = {};
};

} // namespace mm
