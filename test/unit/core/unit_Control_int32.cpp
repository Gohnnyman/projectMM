// @module Control

// Int32 is the control type for a value that genuinely exceeds 16 bits. It exists because a
// MoonLive script scalar occupies a uniform 4-byte slot, so an `int` member has no narrower
// type that can hold it without wrapping — the failure Uint16/Int16 would produce silently.

#include "doctest.h"
#include "core/Control.h"
#include "core/JsonSink.h"

#include <cstdint>
#include <cstring>

TEST_CASE("an int32 control carries a value no 16-bit control could hold") {
    mm::ControlList controls;
    int32_t big = 0;
    controls.addControl("offset", big, -1000000, 1000000);

    const mm::ControlDescriptor& c = controls[0];
    CHECK(c.type == mm::ControlType::Int32);
    CHECK(std::strcmp(mm::controlTypeName(c.type), "int32") == 0);

    // 100000 wraps to -31072 in an int16 and is simply unrepresentable in a uint16.
    auto r = mm::applyControlValue(c, "{\"offset\":100000}", "offset", mm::ApplyPolicy::Clamp);
    CHECK(r == mm::ApplyResult::Ok);
    CHECK(big == 100000);

    char buf[64];
    mm::JsonSink sink(buf, sizeof(buf));
    mm::writeControlValue(sink, c);
    CHECK(std::strcmp(buf, "100000") == 0);
}

TEST_CASE("an int32 control round-trips a negative value") {
    mm::ControlList controls;
    int32_t v = 0;
    controls.addControl("delta", v, -2000000, 2000000);

    auto r = mm::applyControlValue(controls[0], "{\"delta\":-1500000}", "delta",
                                   mm::ApplyPolicy::Clamp);
    CHECK(r == mm::ApplyResult::Ok);
    CHECK(v == -1500000);

    char buf[64];
    mm::JsonSink sink(buf, sizeof(buf));
    mm::writeControlValue(sink, controls[0]);
    CHECK(std::strcmp(buf, "-1500000") == 0);
}

TEST_CASE("an int32 control clamps a write past its range and refuses it under Strict") {
    mm::ControlList controls;
    int32_t v = 0;
    controls.addControl("bounded", v, 0, 1000);

    CHECK(mm::applyControlValue(controls[0], "{\"bounded\":5000}", "bounded",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(v == 1000);

    v = 500;
    CHECK(mm::applyControlValue(controls[0], "{\"bounded\":5000}", "bounded",
                                mm::ApplyPolicy::Strict) == mm::ApplyResult::OutOfRange);
    CHECK(v == 500);   // a refused write leaves the value alone
}

TEST_CASE("an int32 control publishes its range to the UI") {
    mm::ControlList controls;
    int32_t v = 0;
    controls.addControl("span", v, -70000, 70000);

    char buf[128];
    mm::JsonSink sink(buf, sizeof(buf));
    mm::writeControlMetadata(sink, controls[0]);
    CHECK(std::strstr(buf, "\"min\":-70000") != nullptr);
    CHECK(std::strstr(buf, "\"max\":70000") != nullptr);
}

// --- addControl: one name, the widget from the type ---------------------------------------------

// A compiled module and a MoonLive script now spell a control the same way. The widget follows the
// VARIABLE'S TYPE, which the compiler already knows — so a call cannot disagree with the
// declaration, and there is one vocabulary to learn rather than five names carrying a width.
TEST_CASE("addControl binds the widget its member's type calls for") {
    mm::ControlList controls;
    uint8_t  u8  = 1;
    uint16_t u16 = 2;
    int16_t  i16 = 3;
    int32_t  i32 = 4;
    bool     b   = true;

    controls.addControl("u8",  u8);
    controls.addControl("u16", u16);
    controls.addControl("i16", i16);
    controls.addControl("i32", i32);
    controls.addControl("b",   b);

    REQUIRE(controls.count() == 5);
    CHECK(controls[0].type == mm::ControlType::Uint8);
    CHECK(controls[1].type == mm::ControlType::Uint16);
    CHECK(controls[2].type == mm::ControlType::Int16);
    CHECK(controls[3].type == mm::ControlType::Int32);
    CHECK(controls[4].type == mm::ControlType::Bool);
    // The wire names the UI keys off, so a widget change would be visible here too.
    CHECK(std::strcmp(mm::controlTypeName(controls[0].type), "uint8")  == 0);
    CHECK(std::strcmp(mm::controlTypeName(controls[3].type), "int32")  == 0);
    CHECK(std::strcmp(mm::controlTypeName(controls[4].type), "bool")   == 0);
}

// A call that omits min/max means "no UI constraint", and what that means DIFFERS per type: each
// overload defaults to its own type's full range. Unifying them would silently move the bounds of
// every control that relies on the default, which is why the overloads keep separate signatures.
TEST_CASE("addControl without a range gets its own type's full range") {
    mm::ControlList controls;
    uint8_t  u8  = 0;
    uint16_t u16 = 0;
    int16_t  i16 = 0;
    int32_t  i32 = 0;
    bool     b   = false;

    controls.addControl("u8",  u8);
    controls.addControl("u16", u16);
    controls.addControl("i16", i16);
    controls.addControl("i32", i32);
    controls.addControl("b",   b);

    CHECK(controls[0].min == 0);          CHECK(controls[0].max == 255);
    CHECK(controls[1].min == 0);          CHECK(controls[1].max == UINT16_MAX);
    CHECK(controls[2].min == INT16_MIN);  CHECK(controls[2].max == INT16_MAX);
    CHECK(controls[3].min == INT32_MIN);  CHECK(controls[3].max == INT32_MAX);
    CHECK(controls[4].min == 0);          CHECK(controls[4].max == 1);
}

// An explicit range is carried through unchanged, per type.
TEST_CASE("addControl carries an explicit range to the descriptor") {
    mm::ControlList controls;
    uint8_t  speed = 50;
    int32_t  span  = 0;
    controls.addControl("speed", speed, 1, 99);
    controls.addControl("span", span, -70000, 70000);
    CHECK(controls[0].min == 1);       CHECK(controls[0].max == 99);
    CHECK(controls[1].min == -70000);  CHECK(controls[1].max == 70000);
}

// NOT TESTABLE AT RUN TIME, stated here so the intent survives: `addControl(name, int8_t&)` is
// `= delete`d in Control.h. An int8_t is either a GPIO (addPin — PinsModule scans for
// ControlType::Pin to collect claimed pins) or telemetry (addReadOnlyInt, which needs a unit), and
// deducing one from the type would make any future small signed control register as a claimed
// GPIO. A compile failure cannot be a doctest case; the deleted overload IS the test.
