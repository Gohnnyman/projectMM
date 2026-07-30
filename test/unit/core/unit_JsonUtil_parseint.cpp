// @module JsonUtil

// Pins the string→int conversion the flat readers share (mm::json::parseIntStr, and parseInt
// through it). The interesting half is what a NON-number does: every caller treats the result as
// a value to clamp or compare, so "not a number" has to arrive as the fallback rather than as a
// plausible-looking integer.

#include "doctest.h"
#include "core/JsonUtil.h"

#include <climits>

using namespace mm;

TEST_CASE("a JSON integer value is read up to the character that ends it") {
    // Digits run until the JSON punctuation that follows them — the reader is handed a pointer
    // into the middle of a body, not a clean NUL-terminated number.
    CHECK(json::parseIntStr("7,\"next\":1") == 7);
    CHECK(json::parseIntStr("42}") == 42);
    CHECK(json::parseIntStr("-5,") == -5);
    CHECK(json::parseIntStr("0}") == 0);
}

TEST_CASE("text that does not start with a number reads as the fallback, not as zero-by-accident") {
    // The distinction atoi cannot make: it returns 0 for both "0" and "abc". A caller that treats
    // 0 as "absent" needs the two to be separable, so the fallback is explicit.
    CHECK(json::parseIntStr("abc") == 0);
    CHECK(json::parseIntStr("") == 0);
    CHECK(json::parseIntStr(nullptr) == 0);
    CHECK(json::parseIntStr("abc", -1) == -1);
    CHECK(json::parseIntStr("", -1) == -1);
    // A real zero still reads as zero — the fallback must not swallow the valid value.
    CHECK(json::parseIntStr("0", -1) == 0);
}

TEST_CASE("a value too large to represent reads as the fallback instead of wrapping") {
    // The case that made this worth sharing: atoi on an out-of-range value is undefined
    // behavior, and a caller narrowing the result would store a DIFFERENT valid number —
    // a Hue id of "65537" becoming light 1. Out-of-range must be visible, not silently wrapped.
    CHECK(json::parseIntStr("99999999999999999999") == 0);
    CHECK(json::parseIntStr("99999999999999999999", -1) == -1);
    CHECK(json::parseIntStr("-99999999999999999999", -1) == -1);
    // The boundaries themselves still convert.
    CHECK(json::parseIntStr("2147483647") == INT_MAX);
    CHECK(json::parseIntStr("-2147483648") == INT_MIN);
    // ONE past each boundary is the case that separates the two overflow checks, and the one that
    // differs by target: where `long` is 64-bit (desktop) these fit `long` and only the INT_MAX
    // compare rejects them; where it is 32-bit (ESP32, Windows) strtol saturates and sets ERANGE.
    // Both must reach the fallback, or a Hue id of "2147483648" would land as INT_MAX.
    CHECK(json::parseIntStr("2147483648", -1) == -1);
    CHECK(json::parseIntStr("-2147483649", -1) == -1);
}

TEST_CASE("parseInt reads a key's integer value, and absent keys read as zero") {
    CHECK(json::parseInt("{\"a\":1,\"b\":22}", "b") == 22);
    CHECK(json::parseInt("{\"a\": 7}", "a") == 7);        // space after the colon (json.dumps)
    CHECK(json::parseInt("{\"a\":1}", "missing") == 0);
    CHECK(json::parseInt(nullptr, "a") == 0);
    CHECK(json::parseInt("{\"a\":1}", nullptr) == 0);   // no key to look for = absent
    // A non-numeric value for a present key is not a number: 0, same as absent.
    CHECK(json::parseInt("{\"a\":\"text\"}", "a") == 0);
}
