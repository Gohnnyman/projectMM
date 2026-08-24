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
    controls.addInt32("offset", big, -1000000, 1000000);

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
    controls.addInt32("delta", v, -2000000, 2000000);

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
    controls.addInt32("bounded", v, 0, 1000);

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
    controls.addInt32("span", v, -70000, 70000);

    char buf[128];
    mm::JsonSink sink(buf, sizeof(buf));
    mm::writeControlMetadata(sink, controls[0]);
    CHECK(std::strstr(buf, "\"min\":-70000") != nullptr);
    CHECK(std::strstr(buf, "\"max\":70000") != nullptr);
}
