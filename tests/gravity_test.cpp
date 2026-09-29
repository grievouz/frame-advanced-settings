#include "gravity.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

static int checks = 0;
static void check(bool ok, const char *name) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}
static bool close(double a, double b) {
    return std::abs(a - b) < 1e-9;
}
int main() {
    using namespace drag;
    Gravity g;
    Vec p{.3, 1, -.2};
    check(!g.step(p, .01, false) && p.y == 1, "gravity starts off");
    g.setEnabled(true);
    g.step(p, .1, false);
    check(close(p.y, .951), "fall follows one half g t squared from rest");
    check(p.x == .3 && p.z == -.2, "gravity preserves horizontal offsets");
    const auto beforeSuspend = p;
    check(!g.step(p, .1, true) && p.y == beforeSuspend.y && g.speed() == 0,
          "suspension stops motion and discards velocity");
    g.step(p, .1, false);
    check(close(beforeSuspend.y - p.y, .049), "resume starts from rest");
    g.setEnabled(false);
    check(!g.step(p, .1, false) && g.speed() == 0, "off stops falling");
    g.setEnabled(true);
    check(g.speed() == 0, "reenabling does not restore momentum");
    for (int i = 0; i < 200; ++i)
        g.step(p, .01, false);
    check(p.y == 0 && g.speed() == 0, "landing clamps to baseline without overshoot");
    check(!g.step(p, .1, false), "resting at floor does not request more writes");
    p.y = -.2;
    check(!g.step(p, .1, false) && p.y == -.2, "gravity never snaps a negative offset upward");
    p.y = 1;
    for (double dt : {0., -.01, .101, std::numeric_limits<double>::quiet_NaN(),
                      std::numeric_limits<double>::infinity()})
        check(!g.step(p, dt, false) && p.y == 1 && g.speed() == 0,
              "invalid or stalled timestep cannot advance gravity");
    p.x = std::numeric_limits<double>::quiet_NaN();
    check(!g.step(p, .01, false), "invalid offset cannot advance gravity");
    g.setStrength(-2);
    check(g.strength() == 1, "strength is positive and bounded below");
    g.setStrength(100);
    check(g.strength() == 20, "strength is bounded above");
    g.setStrength(std::numeric_limits<double>::quiet_NaN());
    check(g.strength() == 20, "invalid strength is ignored");
    Gravity fast, slow;
    fast.setEnabled(true);
    slow.setEnabled(true);
    Vec a{0, 2, 0}, b = a;
    for (int i = 0; i < 50; ++i)
        fast.step(a, .01, false);
    for (int i = 0; i < 5; ++i)
        slow.step(b, .1, false);
    check(close(a.y, b.y), "fall distance independent of tick rate including speed cap");
    check(fast.speed() == Gravity::maxSpeed && slow.speed() == Gravity::maxSpeed,
          "downward speed stays capped");

    g.setStrength(10);
    p = {};
    g.launch({0, 1, 0});
    check(g.step(p, .1, false) && close(p.y, .05) && close(g.speed(), 0),
          "an upward launch can leave the floor and reach its ballistic apex");
    g.step(p, .1, false);
    check(close(p.y, 0) && close(g.speed(), 0), "a throw lands without retaining velocity");
    g.launch({1, 0, 0});
    check(!g.step(p, .01, false) && close(p.x, 0) && g.speed() == 0,
          "horizontal momentum cannot slide along the floor");
    p = {.1, .1, -.1};
    g.launch({1, 1, -.5});
    g.step(p, .1, false);
    check(close(p.x, .2) && close(p.y, .15) && close(p.z, -.15),
          "XYZ release follows a ballistic arc with horizontal momentum");
    g.setEnabled(false);
    g.launch({0, 2, 0});
    check(g.speed() == 0 && !g.step(p, .1, false), "gravity off disables fling too");
    g.setEnabled(true);
    g.launch({0, 20, 0});
    check(g.speed() == Gravity::maxSpeed, "excessive upward release velocity is capped");
    g.launch({20, 20, 20});
    check(close(g.speed(), Gravity::maxSpeed), "diagonal launch uses a vector speed cap");
    g.launch({0, std::numeric_limits<double>::quiet_NaN(), 0});
    check(g.speed() == 0, "invalid launch cannot enter the integrator");
    g.launch({0, std::numeric_limits<double>::max(), 0});
    check(g.speed() == 0, "overflowing launch magnitude is rejected");
    fast.setStrength(10);
    slow.setStrength(10);
    fast.launch({.2, 2, -.1});
    slow.launch({.2, 2, -.1});
    a = b = {.1, .3, .1};
    for (int i = 0; i < 200; ++i)
        fast.step(a, .01, false);
    for (int i = 0; i < 20; ++i)
        slow.step(b, .1, false);
    check(length(a - b) < 1e-9 && a.y == 0 && fast.speed() == 0 && slow.speed() == 0,
          "XYZ throw including terminal speed and landing is independent of tick rate");
    p = {0, 1.99, 0};
    g.launch({0, 3, 0});
    g.step(p, .1, false);
    check(close(p.y, 2) && g.speed() == 0, "upward throw stops at the existing displacement limit");
    g.step(p, .1, false);
    check(p.y < 2, "gravity falls back after reaching the displacement limit");
    p = {.5, 1.93, -.1};
    g.launch({0, 3, 0});
    g.step(p, .1, false);
    check(close(length(p), 2) && p.x == .5 && p.z == -.1,
          "height-only throw limit preserves existing horizontal offsets");
    p = {1.99, .1, 0};
    g.launch({3, 0, 0});
    g.step(p, .1, false);
    check(close(length(p), 2) && g.speed() == 0,
          "horizontal throw respects the same displacement limit");
    g.launch({1, 1, 1});
    g.step(p, .01, true);
    check(g.speed() == 0, "suspending an XYZ throw clears every velocity component");
    DistanceLimit limit;
    limit.setMeters(5);
    p = {0, 4.99, 0};
    g.launch({0, 3, 0});
    g.step(p, .1, false, limit);
    check(close(p.y, 5) && g.speed() == 0,
          "fling respects the adjusted limit instead of the old two metre cap");
    g.step(p, .1, false, limit);
    check(p.y < 5 && p.y > 4.9, "gravity falls normally from an adjusted distance cap");
    limit.enabled = false;
    p = {0, 4.99, 0};
    g.launch({0, 3, 0});
    g.step(p, .1, false, limit);
    check(p.y > 5 && g.speed() > 0, "Off removes the distance cap from flight");
    limit.enabled = true;
    limit.setMeters(1);
    p = {0, 3, 0};
    g.stop();
    g.step(p, .1, false, limit);
    check(close(p.y, 2.95), "a reduced limit permits falling inward without snapping to it");
    std::printf("%d gravity checks passed\n", checks);
}
