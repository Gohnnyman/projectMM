// @module MqttPacket

// Pins the MQTT 3.1.1 wire framing (src/core/MqttPacket.h) with golden byte vectors + round-trips —
// the compatibility contract with any broker (mosquitto / homebridge-mqttthing), same rigor as the
// Improv frame golden vectors. Covers: the remaining-length varint at its boundaries; byte-exact
// CONNECT / PUBLISH / SUBSCRIBE / PINGREQ; CONNACK / SUBACK parse; and the byte-at-a-time inbound
// parser reassembling a PUBLISH across arbitrary read() boundaries (fragmentation).

#include "doctest.h"
#include "core/MqttPacket.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace mm;

// --- Remaining-length varint (§2.2.3) — the fiddly bit, boundary-tested ---
TEST_CASE("MqttPacket: remaining-length varint encodes/decodes at the boundaries") {
    struct Case { uint32_t value; std::vector<uint8_t> bytes; };
    const Case cases[] = {
        {0,      {0x00}},
        {127,    {0x7F}},
        {128,    {0x80, 0x01}},
        {16383,  {0xFF, 0x7F}},
        {16384,  {0x80, 0x80, 0x01}},
        {268435455, {0xFF, 0xFF, 0xFF, 0x7F}},   // the 4-byte maximum
    };
    for (const auto& c : cases) {
        uint8_t out[4] = {};
        const size_t n = encodeRemainingLength(c.value, out);
        REQUIRE(n == c.bytes.size());
        CHECK(std::memcmp(out, c.bytes.data(), n) == 0);

        uint32_t decoded = 0; size_t consumed = 0;
        REQUIRE(decodeRemainingLength(out, n, &decoded, &consumed));
        CHECK(decoded == c.value);
        CHECK(consumed == n);
    }
    // Over the 4-byte max → encode refuses.
    uint8_t out[4];
    CHECK(encodeRemainingLength(268435456u, out) == 0);
    // A 5th continuation byte → decode reports malformed.
    const uint8_t bad[] = {0x80, 0x80, 0x80, 0x80, 0x01};
    uint32_t v; size_t c;
    CHECK_FALSE(decodeRemainingLength(bad, sizeof(bad), &v, &c));
    // Truncated (high bit set, no more bytes) → decode needs more.
    const uint8_t trunc[] = {0x80};
    CHECK_FALSE(decodeRemainingLength(trunc, sizeof(trunc), &v, &c));
}

// --- CONNECT golden vector (§3.1) ---
TEST_CASE("MqttPacket: CONNECT is byte-exact (clean session, no auth)") {
    uint8_t out[64] = {};
    const size_t n = buildMqttConnect("mm", nullptr, nullptr, 60, out, sizeof(out));
    // Expected: fixed header 0x10, remaining-length 14, then:
    //   proto: 00 04 'M' 'Q' 'T' 'T'
    //   level: 04
    //   flags: 02 (clean session, no user/pass)
    //   keepalive: 00 3C (60)
    //   clientId: 00 02 'm' 'm'
    const uint8_t expected[] = {
        0x10, 0x0E,
        0x00, 0x04, 'M', 'Q', 'T', 'T',
        0x04,
        0x02,
        0x00, 0x3C,
        0x00, 0x02, 'm', 'm',
    };
    REQUIRE(n == sizeof(expected));
    CHECK(std::memcmp(out, expected, n) == 0);
}

TEST_CASE("MqttPacket: CONNECT with username + password sets the flags + payload") {
    uint8_t out[64] = {};
    const size_t n = buildMqttConnect("c", "u", "p", 15, out, sizeof(out));
    REQUIRE(n > 0);
    CHECK(out[0] == 0x10);                        // CONNECT
    // connect flags byte is at offset 9 (after fixed header[2] + proto[6] + level[1]).
    CHECK(out[9] == (kMqttConnectCleanSession | kMqttConnectUsernameFlag | kMqttConnectPasswordFlag));
    // The username "u" and password "p" appear length-prefixed after the clientId "c".
    // Tail: ...00 01 'c'  00 01 'u'  00 01 'p'
    const uint8_t tail[] = {0x00, 0x01, 'c', 0x00, 0x01, 'u', 0x00, 0x01, 'p'};
    CHECK(std::memcmp(out + n - sizeof(tail), tail, sizeof(tail)) == 0);
}

// Regression (reviewer #10 / MQTT-3.1.2-22): a password without a username must NOT set the password
// flag — a compliant broker rejects that CONNECT. The builder drops the password when username empty.
TEST_CASE("MqttPacket: CONNECT drops a password when there is no username") {
    uint8_t out[64] = {};
    const size_t n = buildMqttConnect("c", nullptr, "secret", 15, out, sizeof(out));
    REQUIRE(n > 0);
    // Flags byte at offset 9: clean-session only — neither username nor password flag set.
    CHECK(out[9] == kMqttConnectCleanSession);
    // The payload is just the clientId (no username/password fields appended).
    const uint8_t tail[] = {0x00, 0x01, 'c'};
    CHECK(std::memcmp(out + n - sizeof(tail), tail, sizeof(tail)) == 0);
    // Same when username is an empty string (not just nullptr).
    const size_t n2 = buildMqttConnect("c", "", "secret", 15, out, sizeof(out));
    REQUIRE(n2 > 0);
    CHECK(out[9] == kMqttConnectCleanSession);
}

// --- PUBLISH golden vector (§3.3), QoS0 ---
TEST_CASE("MqttPacket: PUBLISH is byte-exact (QoS0)") {
    uint8_t out[64] = {};
    const uint8_t payload[] = {'O', 'N'};
    const size_t n = buildMqttPublish("a/b", payload, sizeof(payload), out, sizeof(out));
    // 0x30 PUBLISH, remaining-length 7, topic 00 03 'a' '/' 'b', payload 'O' 'N'.
    const uint8_t expected[] = {
        0x30, 0x07,
        0x00, 0x03, 'a', '/', 'b',
        'O', 'N',
    };
    REQUIRE(n == sizeof(expected));
    CHECK(std::memcmp(out, expected, n) == 0);
}

// The retain flag (§3.3.1.3) sets bit 0 of the fixed-header type nibble — used for the friendly
// `name` topic so a late-subscribing hub still receives it.
TEST_CASE("MqttPacket: PUBLISH retain flag sets the fixed-header bit") {
    uint8_t out[32] = {};
    const uint8_t payload[] = {'x'};
    const size_t n = buildMqttPublish("t", payload, 1, out, sizeof(out), /*retain=*/true);
    REQUIRE(n > 0);
    CHECK(out[0] == 0x31);   // PUBLISH (0x3<<4) | retain (0x1)
    // Without retain, bit 0 is clear.
    const size_t n2 = buildMqttPublish("t", payload, 1, out, sizeof(out), /*retain=*/false);
    REQUIRE(n2 > 0);
    CHECK(out[0] == 0x30);
}

// --- SUBSCRIBE golden vector (§3.8) ---
TEST_CASE("MqttPacket: SUBSCRIBE is byte-exact (one filter, QoS0)") {
    uint8_t out[64] = {};
    const size_t n = buildMqttSubscribe(1, "a/b", out, sizeof(out));
    // 0x82 (SUBSCRIBE + reserved flag 0x2), remaining-length 8, packetId 00 01,
    // filter 00 03 'a' '/' 'b', requested QoS 00.
    const uint8_t expected[] = {
        0x82, 0x08,
        0x00, 0x01,
        0x00, 0x03, 'a', '/', 'b',
        0x00,
    };
    REQUIRE(n == sizeof(expected));
    CHECK(std::memcmp(out, expected, n) == 0);
    // packetId 0 is rejected (spec: must be non-zero).
    CHECK(buildMqttSubscribe(0, "a/b", out, sizeof(out)) == 0);
}

// --- PINGREQ / DISCONNECT ---
TEST_CASE("MqttPacket: PINGREQ and DISCONNECT are 2-byte packets") {
    uint8_t out[4] = {};
    REQUIRE(buildMqttPingreq(out, sizeof(out)) == 2);
    CHECK(out[0] == 0xC0);
    CHECK(out[1] == 0x00);
    REQUIRE(buildMqttDisconnect(out, sizeof(out)) == 2);
    CHECK(out[0] == 0xE0);
    CHECK(out[1] == 0x00);
}

// --- Inbound parser: CONNACK return code read off the completed body (how the module reads it) ---
TEST_CASE("MqttPacket: inbound parser exposes a CONNACK's return code in its body") {
    const uint8_t accepted[] = {0x20, 0x02, 0x00, 0x00};   // session-present 0, return 0 (accepted)
    MqttInboundParser p;
    MqttFeedResult result = MqttFeedResult::NeedMore;
    for (uint8_t b : accepted) result = p.feed(b);
    REQUIRE(result == MqttFeedResult::PacketReady);
    CHECK(p.lastType() == static_cast<uint8_t>(MqttPacketType::Connack));
    REQUIRE(p.bodyLen() == 2);
    CHECK(p.body()[1] == 0);   // return code — the module checks this byte for accept/refuse
}

// --- Inbound parser: round-trip a PUBLISH built by our own builder ---
TEST_CASE("MqttPacket: inbound parser reassembles a PUBLISH (build→parse round-trip)") {
    uint8_t pkt[64] = {};
    const uint8_t payload[] = {'h', 'i'};
    const size_t n = buildMqttPublish("proj/on/set", payload, sizeof(payload), pkt, sizeof(pkt));
    REQUIRE(n > 0);

    MqttInboundParser parser;
    MqttFeedResult result = MqttFeedResult::NeedMore;
    for (size_t i = 0; i < n; i++) result = parser.feed(pkt[i]);
    REQUIRE(result == MqttFeedResult::PacketReady);
    CHECK(parser.lastType() == static_cast<uint8_t>(MqttPacketType::Publish));

    const char* topic = nullptr; const uint8_t* pl = nullptr; size_t plLen = 0;
    REQUIRE(parser.publish(&topic, &pl, &plLen));
    CHECK(std::strcmp(topic, "proj/on/set") == 0);
    REQUIRE(plLen == 2);
    CHECK(pl[0] == 'h');
    CHECK(pl[1] == 'i');
}

// --- Inbound parser: fragmentation — a packet split across feeds still reassembles ---
TEST_CASE("MqttPacket: inbound parser reassembles across read() boundaries") {
    uint8_t pkt[64] = {};
    const uint8_t payload[] = {'x'};
    const size_t n = buildMqttPublish("t", payload, sizeof(payload), pkt, sizeof(pkt));
    REQUIRE(n > 0);

    // Feed one byte at a time — every byte but the last returns NeedMore, proving the state machine
    // holds partial state across calls (the socket hands over arbitrary runs).
    MqttInboundParser parser;
    for (size_t i = 0; i + 1 < n; i++) {
        CHECK(parser.feed(pkt[i]) == MqttFeedResult::NeedMore);
    }
    CHECK(parser.feed(pkt[n - 1]) == MqttFeedResult::PacketReady);
    const char* topic = nullptr;
    REQUIRE(parser.publish(&topic, nullptr, nullptr));
    CHECK(std::strcmp(topic, "t") == 0);
}

// --- Inbound parser: a multi-byte remaining-length (128+ body) is handled ---
TEST_CASE("MqttPacket: inbound parser handles a 2-byte remaining-length") {
    // A PUBLISH whose body is 130 bytes forces a 2-byte remaining-length varint.
    uint8_t big[200] = {};
    uint8_t payload[126];
    for (auto& b : payload) b = 'z';
    const size_t n = buildMqttPublish("tt", payload, sizeof(payload), big, sizeof(big));
    REQUIRE(n > 0);
    CHECK((big[1] & 0x80) != 0);   // first remaining-length byte has continuation bit → multi-byte

    MqttInboundParser parser;
    MqttFeedResult result = MqttFeedResult::NeedMore;
    for (size_t i = 0; i < n; i++) result = parser.feed(big[i]);
    CHECK(result == MqttFeedResult::PacketReady);
    const char* topic = nullptr; const uint8_t* pl = nullptr; size_t plLen = 0;
    REQUIRE(parser.publish(&topic, &pl, &plLen));
    CHECK(std::strcmp(topic, "tt") == 0);
    CHECK(plLen == sizeof(payload));
}
