// @module ActiveInstance

#include "doctest.h"
#include "core/ActiveInstance.h"

using namespace mm;

namespace {

// A minimal participant type. ActiveInstance<Widget> gives it a per-type static seat; each Widget
// holds its own ActiveInstance member tied to itself.
struct Widget {
    ActiveInstance<Widget> seat{*this};
    int id;
    explicit Widget(int i) : id(i) {}
};

} // namespace

// A fresh seat is empty until someone claims it.
TEST_CASE("ActiveInstance: seat starts empty") {
    Widget w{1};
    CHECK(ActiveInstance<Widget>::active() == nullptr);
    CHECK_FALSE(w.seat.seated());
}

// First claim wins; a second claimant does NOT displace the holder (claim-if-empty).
TEST_CASE("ActiveInstance: first claim wins, a second does not displace") {
    Widget a{1}, b{2};
    a.seat.claim();
    CHECK(ActiveInstance<Widget>::active() == &a);
    CHECK(a.seat.seated());
    CHECK_FALSE(b.seat.seated());

    b.seat.claim();   // seat is held → no-op
    CHECK(ActiveInstance<Widget>::active() == &a);   // a still wins
    CHECK_FALSE(b.seat.seated());
}

// vacate() only releases if this instance holds the seat — it never yanks another's.
TEST_CASE("ActiveInstance: vacate releases only the holder's seat") {
    Widget a{1}, b{2};
    a.seat.claim();
    b.seat.vacate();   // b doesn't hold it → no-op
    CHECK(ActiveInstance<Widget>::active() == &a);
    a.seat.vacate();   // a holds it → releases
    CHECK(ActiveInstance<Widget>::active() == nullptr);
}

// After the holder vacates, a surviving instance reclaims the empty seat with the SAME claim() call
// (the idempotent-claim = survivor-reclaim contract that AudioService's tick relies on).
TEST_CASE("ActiveInstance: a survivor reclaims an emptied seat via claim()") {
    Widget a{1}, b{2};
    a.seat.claim();   // a wins
    b.seat.claim();   // b captured but not seated
    CHECK(ActiveInstance<Widget>::active() == &a);

    a.seat.vacate();  // a releases (e.g. the active module is removed)
    CHECK(ActiveInstance<Widget>::active() == nullptr);

    b.seat.claim();   // b's next "tick" reclaim takes the now-empty seat
    CHECK(ActiveInstance<Widget>::active() == &b);
    b.seat.vacate();
}

// The destructor vacates a held seat — the dangling-static guard. Without it, active() would point
// at freed memory after the holder is destroyed.
TEST_CASE("ActiveInstance: destructor vacates the seat (no dangling static)") {
    {
        Widget a{1};
        a.seat.claim();
        CHECK(ActiveInstance<Widget>::active() == &a);
    }   // a destructs here — its ActiveInstance member's dtor vacates
    CHECK(ActiveInstance<Widget>::active() == nullptr);
}

// Destroying a NON-holder leaves the holder's seat intact (vacate is guarded on "if mine").
TEST_CASE("ActiveInstance: destroying a non-holder leaves the seat intact") {
    Widget a{1};
    a.seat.claim();
    {
        Widget b{2};
        b.seat.claim();   // captured, not seated
        CHECK(ActiveInstance<Widget>::active() == &a);
    }   // b destructs — must NOT clear a's seat
    CHECK(ActiveInstance<Widget>::active() == &a);
    a.seat.vacate();
}

// The seat is PER-TYPE: two different participant types have independent seats.
TEST_CASE("ActiveInstance: the seat is per type") {
    struct Other { ActiveInstance<Other> seat{*this}; };
    Widget w{1};
    Other  o;
    w.seat.claim();
    o.seat.claim();
    CHECK(ActiveInstance<Widget>::active() == &w);
    CHECK(ActiveInstance<Other>::active()  == &o);
    w.seat.vacate();
    CHECK(ActiveInstance<Widget>::active() == nullptr);
    CHECK(ActiveInstance<Other>::active()  == &o);   // independent
    o.seat.vacate();
}
