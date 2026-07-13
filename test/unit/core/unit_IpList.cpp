// @module IpList

// Pins parseIpList — the destination-list parser a NetworkSendDriver uses to fan one buffer out to N
// receivers (Art-Net 4 requires ArtDmx to be unicast to the node owning each universe, so driving a
// row of tubes means a list of destinations, not one). Two syntaxes, both typed by hand, so the
// parser is where a typo must be caught rather than turned into packets aimed at the wrong host.

#include "doctest.h"
#include "core/IpList.h"

namespace {
// Compact assertion helper: "is destination i equal to a.b.c.d?"
bool is(const uint8_t q[4], int a, int b, int c, int d) {
    return q[0] == a && q[1] == b && q[2] == c && q[3] == d;
}
}  // namespace

TEST_CASE("parseIpList: a range expands over the last octet") {
    uint8_t ips[32][4];
    uint8_t n = 0;
    REQUIRE(mm::parseIpList("192.168.1.60-63", ips, 32, n) == nullptr);
    CHECK(n == 4);                            // 60, 61, 62, 63 — inclusive at both ends
    CHECK(is(ips[0], 192, 168, 1, 60));
    CHECK(is(ips[3], 192, 168, 1, 63));
}

TEST_CASE("parseIpList: a list of bare host numbers continues the same subnet") {
    uint8_t ips[32][4];
    uint8_t n = 0;
    // The shorthand people actually type: full quad once, then just the host numbers.
    REQUIRE(mm::parseIpList("192.168.1.60,61,62,65", ips, 32, n) == nullptr);
    CHECK(n == 4);
    CHECK(is(ips[0], 192, 168, 1, 60));
    CHECK(is(ips[1], 192, 168, 1, 61));
    CHECK(is(ips[2], 192, 168, 1, 62));
    CHECK(is(ips[3], 192, 168, 1, 65));       // gaps are fine — it's a list, not a range
}

TEST_CASE("parseIpList: full quads may switch subnet, and ranges/lists mix") {
    uint8_t ips[32][4];
    uint8_t n = 0;
    REQUIRE(mm::parseIpList("192.168.1.60-61, 10.0.0.5, 6", ips, 32, n) == nullptr);
    CHECK(n == 4);
    CHECK(is(ips[0], 192, 168, 1, 60));
    CHECK(is(ips[1], 192, 168, 1, 61));
    CHECK(is(ips[2], 10, 0, 0, 5));           // an explicit quad switches subnet
    CHECK(is(ips[3], 10, 0, 0, 6));           // and a bare number then continues THAT subnet
}

TEST_CASE("parseIpList: blank is not an error — it means no destinations (idle)") {
    uint8_t ips[32][4];
    uint8_t n = 7;
    CHECK(mm::parseIpList("", ips, 32, n) == nullptr);      // an unconfigured driver must idle,
    CHECK(n == 0);                                          // not error and not broadcast
    CHECK(mm::parseIpList("   ", ips, 32, n) == nullptr);
    CHECK(n == 0);
    CHECK(mm::parseIpList(nullptr, ips, 32, n) == nullptr);
    CHECK(n == 0);
}

TEST_CASE("parseIpList: malformed input is rejected, never guessed at") {
    uint8_t ips[32][4];
    uint8_t n = 0;
    CHECK(mm::parseIpList("192.168.1.999", ips, 32, n) != nullptr);   // octet > 255
    CHECK(mm::parseIpList("192.168.1.60,,61", ips, 32, n) != nullptr); // empty token
    CHECK(mm::parseIpList("192.168.1.60-", ips, 32, n) != nullptr);    // range with no end
    CHECK(mm::parseIpList("192.168.1.70-60", ips, 32, n) != nullptr);  // backwards range
    CHECK(mm::parseIpList("60,61", ips, 32, n) != nullptr);            // bare number, no subnet yet
    CHECK(mm::parseIpList("hello", ips, 32, n) != nullptr);
}

TEST_CASE("parseIpList: the destination cap is enforced, not silently truncated") {
    uint8_t ips[4][4];
    uint8_t n = 0;
    // A range that would overflow the caller's array must ERROR — silently dropping tubes would
    // leave a wall half-lit with no explanation.
    CHECK(mm::parseIpList("192.168.1.1-100", ips, 4, n) != nullptr);
}
