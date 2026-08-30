// @module OscModule

#include "doctest.h"
#include "core/OscPacket.h"

#include <cstring>
#include <vector>

using namespace mm;

namespace {

// Build a message the way a real controller does, so the tests exercise the padding rather than a
// convenient hand-picked length: address, pad to 4, type tags, pad to 4, big-endian argument.
std::vector<uint8_t> build(const char* addr, const char* tags, const uint8_t* arg, size_t argLen) {
    std::vector<uint8_t> p;
    auto put = [&](const char* s) {
        const size_t n = std::strlen(s) + 1;
        for (size_t i = 0; i < n; i++) p.push_back(static_cast<uint8_t>(s[i]));
        while (p.size() % 4) p.push_back(0);
    };
    put(addr);
    put(tags);
    for (size_t i = 0; i < argLen; i++) p.push_back(arg[i]);
    return p;
}

std::vector<uint8_t> withFloat(const char* addr, float v) {
    uint8_t be[4];
    int32_t bits;
    std::memcpy(&bits, &v, 4);
    be[0] = static_cast<uint8_t>(bits >> 24); be[1] = static_cast<uint8_t>(bits >> 16);
    be[2] = static_cast<uint8_t>(bits >> 8);  be[3] = static_cast<uint8_t>(bits);
    return build(addr, ",f", be, 4);
}

std::vector<uint8_t> withInt(const char* addr, int32_t v) {
    const uint8_t be[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                           static_cast<uint8_t>(v >> 8),  static_cast<uint8_t>(v)};
    return build(addr, ",i", be, 4);
}

}  // namespace

// The exact bytes a controller puts on the wire. If this changes, every TouchOSC layout and
// Resolume patch built against projectMM breaks, so the golden vector is the alarm.
TEST_CASE("golden vector: the exact bytes of an OSC message with a float") {
    const auto p = withFloat("/mm/fader/1", 1.0f);

    // "/mm/fader/1" is 11 chars + NUL = 12, already a multiple of 4: no padding.
    // ",f" is 2 + NUL = 3, padded to 4. Then 4 bytes of big-endian 1.0f (0x3F800000).
    const uint8_t golden[] = {
        '/','m','m','/','f','a','d','e','r','/','1','\0',
        ',','f','\0','\0',
        0x3F, 0x80, 0x00, 0x00,
    };
    REQUIRE(p.size() == sizeof(golden));
    CHECK(std::memcmp(p.data(), golden, sizeof(golden)) == 0);

    osc::Message m;
    REQUIRE(osc::parse(p.data(), p.size(), m));
    CHECK(std::strcmp(m.address, "/mm/fader/1") == 0);
    CHECK(m.hasValue);
    CHECK(m.wasFloat);
    CHECK(m.f == doctest::Approx(1.0f));
}

// The pad rule is where a hand-rolled parser goes wrong: an address whose length is already a
// multiple of 4 gets a WHOLE extra word of padding, not none.
TEST_CASE("Addresses of every length parse, including the padding boundary") {
    for (const char* addr : {"/a", "/ab", "/abc", "/abcd", "/abcde", "/abcdef", "/abcdefg"}) {
        const auto p = withFloat(addr, 0.5f);
        osc::Message m;
        REQUIRE_MESSAGE(osc::parse(p.data(), p.size(), m), addr);
        CHECK(std::strcmp(m.address, addr) == 0);
        CHECK(m.f == doctest::Approx(0.5f));
    }
}

TEST_CASE("An int argument parses and reads back as both forms") {
    const auto p = withInt("/mm/fader/1", 200);
    osc::Message m;
    REQUIRE(osc::parse(p.data(), p.size(), m));
    CHECK(m.hasValue);
    CHECK_FALSE(m.wasFloat);
    CHECK(m.i == 200);
    CHECK(m.f == doctest::Approx(200.0f));
}

// Controllers disagree on the value range: TouchOSC and Resolume send 0..1 floats, hardware
// bridges send ints in the target's own range. Both must land on the same control value, and
// out-of-range must CLAMP: a controller sending 0..127 would otherwise look broken.
TEST_CASE("A float 0 to 1 and an int 0 to 255 mean the same control value") {
    osc::Message f{};
    f.hasValue = true; f.wasFloat = true;

    f.f = 0.0f;  CHECK(osc::toByte(f) == 0);
    f.f = 1.0f;  CHECK(osc::toByte(f) == 255);
    f.f = 0.5f;  CHECK(osc::toByte(f) == 128);      // rounded, not truncated to 127
    f.f = -1.0f; CHECK(osc::toByte(f) == 0);        // clamped, not wrapped
    f.f = 2.0f;  CHECK(osc::toByte(f) == 255);

    osc::Message i{};
    i.hasValue = true; i.wasFloat = false;
    i.i = 0;    CHECK(osc::toByte(i) == 0);
    i.i = 255;  CHECK(osc::toByte(i) == 255);
    i.i = 1000; CHECK(osc::toByte(i) == 255);       // clamped
    i.i = -5;   CHECK(osc::toByte(i) == 0);
}

// The parse reads an unauthenticated datagram off the LAN, so a malformed one must be refused
// rather than read past. Every case here would be an out-of-bounds read in a naive parser.
TEST_CASE("A malformed datagram is refused, never read past its end") {
    osc::Message m;

    CHECK_FALSE(osc::parse(nullptr, 16, m));                       // null buffer
    const uint8_t tiny[] = {'/', 'a', 0, 0};
    CHECK_FALSE(osc::parse(tiny, sizeof(tiny), m));                // too short to hold tags

    const uint8_t noSlash[] = {'x','y','z','\0', ',','f','\0','\0', 0,0,0,0};
    CHECK_FALSE(osc::parse(noSlash, sizeof(noSlash), m));          // address must start with '/'

    const uint8_t bundle[] = {'#','b','u','n','d','l','e','\0', 0,0,0,0,0,0,0,0};
    CHECK_FALSE(osc::parse(bundle, sizeof(bundle), m));            // bundles are out of scope

    const uint8_t noTags[] = {'/','a','\0','\0', 'f','\0','\0','\0', 0,0,0,0};
    CHECK_FALSE(osc::parse(noTags, sizeof(noTags), m));            // type tags must start with ','

    // An address with no NUL anywhere: the classic overrun.
    uint8_t unterminated[16];
    std::memset(unterminated, 'a', sizeof(unterminated));
    unterminated[0] = '/';
    CHECK_FALSE(osc::parse(unterminated, sizeof(unterminated), m));

    // Tags promise a float, but the argument was truncated off the end.
    const uint8_t shortArg[] = {'/','a','\0','\0', ',','f','\0','\0', 0x3F, 0x80};
    CHECK_FALSE(osc::parse(shortArg, sizeof(shortArg), m));
}

// A message with no arguments is legal, and is how a pad press arrives.
TEST_CASE("A message with no arguments parses, carrying no value") {
    const auto p = build("/mm/pad/3", ",", nullptr, 0);
    osc::Message m;
    REQUIRE(osc::parse(p.data(), p.size(), m));
    CHECK(std::strcmp(m.address, "/mm/pad/3") == 0);
    CHECK_FALSE(m.hasValue);
}

// A controller that labels its messages must not make them unusable: the string is stepped over
// and the number after it is still found.
TEST_CASE("A leading string argument is skipped to reach the number") {
    std::vector<uint8_t> p;
    auto put = [&](const char* s) {
        const size_t n = std::strlen(s) + 1;
        for (size_t i = 0; i < n; i++) p.push_back(static_cast<uint8_t>(s[i]));
        while (p.size() % 4) p.push_back(0);
    };
    put("/mm/fader/2");
    put(",sf");
    put("label");
    const uint8_t be[4] = {0x3F, 0x80, 0x00, 0x00};   // 1.0f
    for (uint8_t b : be) p.push_back(b);

    osc::Message m;
    REQUIRE(osc::parse(p.data(), p.size(), m));
    CHECK(m.hasValue);
    CHECK(m.f == doctest::Approx(1.0f));
}

// The address contract, tested as a whole: these strings are what a TouchOSC layout or a
// TouchDesigner patch is built against, so a change here breaks someone's file. Routing is
// checked through the parse + toByte pair the module uses, without needing a live socket.
TEST_CASE("The address contract: what a controller can send") {
    struct Case { const char* addr; float value; int expectByte; };
    const Case cases[] = {
        {"/mm/fader/1", 1.0f, 255},
        {"/mm/fader/8", 0.0f, 0},
        {"/mm/enc/3", 0.5f, 128},
        {"/mm/control/Drivers/brightness", 1.0f, 255},
    };
    for (const auto& c : cases) {
        const auto p = withFloat(c.addr, c.value);
        osc::Message m;
        REQUIRE_MESSAGE(osc::parse(p.data(), p.size(), m), c.addr);
        CHECK(std::strcmp(m.address, c.addr) == 0);
        CHECK(static_cast<int>(osc::toByte(m)) == c.expectByte);
    }
}

// A string argument whose NUL lands 1-3 bytes short of a 4-byte boundary AT THE END of the
// datagram. stringLen returns the PADDED length, which then exceeds the bytes actually available:
// subtracting it from a size_t wrapped argAvail to a huge number, every later `argAvail < 4` guard
// passed, and the next numeric argument was read off the end of the buffer. One unauthenticated
// UDP packet, reproduced under AddressSanitizer as a heap-buffer-overflow in beFloat32.
//
// Every other fixture in this file pads its arguments, which is exactly why none of them could
// catch it: the bug lives in the UNpadded tail a real attacker controls.
TEST_CASE("A string argument with an unpadded tail is rejected, not read past") {
    // "/a\0\0"  ",sf\0"  "xy\0"  -- 11 bytes, and the last element is 3 bytes where 4 are implied.
    const std::vector<uint8_t> pkt = {
        '/', 'a', 0, 0,
        ',', 's', 'f', 0,
        'x', 'y', 0,
    };
    osc::Message m;
    CHECK_FALSE(osc::parse(pkt.data(), pkt.size(), m));   // refused rather than overread
    CHECK_FALSE(m.hasValue);

    // The same message with the string PROPERLY padded is still short of its float, and must also
    // be refused: the fix must not turn an overread into a bogus value.
    const std::vector<uint8_t> padded = {
        '/', 'a', 0, 0,
        ',', 's', 'f', 0,
        'x', 'y', 0, 0,
    };
    osc::Message m2;
    CHECK_FALSE(osc::parse(padded.data(), padded.size(), m2));
    CHECK_FALSE(m2.hasValue);
}
