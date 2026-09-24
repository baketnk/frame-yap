#include "gestures.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
using namespace frameyap;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #x << '\n'; std::exit(1); } } while (false)
int main() {
    using Clock = DoubleTap::Clock;
    const auto t = Clock::time_point{};
    DoubleTap left;
    auto l = [&](bool active, bool down, bool enabled, int ms) {
        return left.update(active, down, enabled, t + std::chrono::milliseconds(ms));
    };
    CHECK(!l(true, true, true, 0)); // held on reconnect must rearm
    CHECK(!l(true, false, true, 10));
    CHECK(!l(true, true, true, 20));
    CHECK(!l(true, false, true, 270)); // inclusive 250ms
    CHECK(!l(true, true, true, 620)); // inclusive 350ms
    CHECK(l(true, false, true, 870));
    CHECK(!l(true, true, true, 880));
    CHECK(!l(true, false, true, 890));
    CHECK(!l(true, true, true, 1241)); // late second press
    CHECK(!l(true, false, true, 1250));
    CHECK(!l(true, true, true, 1260));
    CHECK(!l(true, false, true, 1511)); // long second press: no Enter
    CHECK(!l(false, false, true, 1520));
    CHECK(!l(true, true, true, 1530));
    CHECK(!l(true, false, true, 1540));
    CHECK(!l(true, true, false, 1550));
    CHECK(!l(true, false, true, 1560));

    using R = GripRecord::Change;
    GripRecord right;
    auto r = [&](bool active, bool down, int ms) {
        return right.update(active, down, t + std::chrono::milliseconds(ms));
    };
    CHECK(r(true, true, 0) == R::None);
    CHECK(r(true, false, 10) == R::None);
    CHECK(r(true, true, 20) == R::None);
    CHECK(r(true, false, 270) == R::None);
    CHECK(r(true, true, 620) == R::Begin);
    CHECK(r(true, true, 99999) == R::None); // no accidental Enter or stop on long hold
    CHECK(r(true, false, 100000) == R::End);
    CHECK(r(true, true, 100010) == R::None);
    CHECK(r(true, false, 100011) == R::None);
    CHECK(r(true, true, 100362) == R::None);
    CHECK(r(true, false, 100370) == R::None);
    CHECK(r(true, true, 100380) == R::Begin);
    CHECK(r(false, true, 100381) == R::Cancel); // lost pose/activity while held
    CHECK(r(true, true, 100382) == R::None); // reconnect held cannot finish/start
    CHECK(r(true, false, 100383) == R::None);
    CHECK(r(true, true, 100390) == R::None);
    CHECK(r(true, false, 100641) == R::None); // first squeeze too long
    CHECK(r(true, true, 100650) == R::None);
    CHECK(r(true, false, 100660) == R::None);
    CHECK(r(true, true, 101010) == R::Begin);
    CHECK(right.reset() == R::Cancel); // overlay focus lost
    CHECK(right.reset() == R::None);
    CHECK(r(true, true, 101020) == R::None);
    CHECK(r(true, false, 101030) == R::None);

    NeutralEdge e;
    CHECK(e.update(true, true) == NeutralEdge::Change::None);
    CHECK(e.update(true, false) == NeutralEdge::Change::None);
    CHECK(e.update(true, true) == NeutralEdge::Change::Down);
    CHECK(e.reset()); // PTT activity/tracking/focus loss cancels capture
    CHECK(e.update(true, true) == NeutralEdge::Change::None);
    CHECK(e.update(true, false) == NeutralEdge::Change::None);
    CHECK(e.update(true, true) == NeutralEdge::Change::Down);
    CHECK(e.update(true, false) == NeutralEdge::Change::Up);
    CHECK(!e.reset());
}
