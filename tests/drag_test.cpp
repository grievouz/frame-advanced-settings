#include "drag.h"
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
int main() {
    using namespace drag;
    Transform base;
    base.t = {4, 2, -1};
    Engine e;
    check(!e.sample(true, true, {0, 1, 0}, base, .01), "held input at launch cannot move");
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {0, 1, 0}, base, .01);
    check(e.sample(true, true, {0, .9, 0}, base, .01), "drag movement");
    check(std::abs(e.offset.y - .1) < 1e-8, "downward hand raises user");
    auto shifted = withOffset(base, e.offset);
    check(std::abs(shifted.t.y - 1.9) < 1e-8, "standing-to-raw sign");
    for (int i = 0; i < 500; i++)
        e.sample(true, true, {0, .9, 0}, base, .01);
    check(std::abs(e.offset.y - .1) < 1e-8, "stationary hand cannot accumulate drift");
    e.sample(true, false, {0, .7, 0}, base, .01);
    e.sample(true, true, {0, .6, 0}, base, .01);
    check(std::abs(e.offset.y - .1) < 1e-8 && !e.held(), "tracking return requires release");
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {0, .6, 0}, base, .01);
    e.sample(true, true, {0, 3, 0}, base, .01);
    check(!e.held() && std::abs(e.offset.y - .1) < 1e-8, "tracking discontinuity rejected");
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {0, 1, 0}, base, .01);
    e.sample(true, true, {0, .9, 0}, base, .5);
    check(!e.held(), "stalled frame cannot move playspace");
    e.reset();
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {0, 1, 0}, base, .01);
    e.sample(true, true, {.1, .9, .1}, base, .01);
    check(e.offset.x == 0 && e.offset.z == 0, "vertical lock");
    e.reset();
    e.xyz = true;
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {}, base, .01);
    for (int i = 1; i <= 200; i++)
        e.sample(true, true, {i * .1, 0, 0}, base, .01);
    check(std::abs(length(e.offset) - 2) < 1e-8, "total offset bounded");
    e.reset();
    e.distanceLimit.setMeters(5);
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {}, base, .01);
    for (int i = 1; i <= 80; ++i)
        e.sample(true, true, {i * .1, 0, 0}, base, .01);
    check(std::abs(length(e.offset) - 5) < 1e-8,
          "adjusted drag limit allows movement past two metres and stops at five");
    e.reset();
    e.distanceLimit.enabled = false;
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {}, base, .01);
    for (int i = 1; i <= 80; ++i)
        e.sample(true, true, {i * .1, 0, 0}, base, .01);
    check(std::abs(length(e.offset) - 8) < 1e-8, "Off removes the distance cap from dragging");
    e.reset();
    e.xyz = false;
    e.distanceLimit.enabled = true;
    e.distanceLimit.setMeters(1.5);
    e.offset = {1, .5, -.2};
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {}, base, .01);
    for (int i = 1; i <= 20; ++i)
        e.sample(true, true, {0, -.1 * i, 0}, base, .01);
    check(std::abs(length(e.offset) - 1.5) < 1e-8 && e.offset.x == 1 && e.offset.z == -.2,
          "height-only drag reaches the selected radius without changing locked axes");
    e.reset();
    e.xyz = true;
    e.offset = {4, 0, 0};
    e.distanceLimit.setMeters(1);
    check(e.offset.x == 4, "lowering the selected cap does not move the current offset");
    e.sample(false, true, {}, base, .01);
    e.sample(true, true, {}, base, .01);
    e.sample(true, true, {-.1, 0, 0}, base, .01);
    check(e.offset.x == 4, "reduced limit blocks further outward drag without snapping inward");
    e.sample(true, true, {.1, 0, 0}, base, .01);
    check(std::abs(e.offset.x - 3.9) < 1e-8, "reduced limit permits a gradual drag back inward");
    e.distanceLimit.setMeters(-1);
    check(e.distanceLimit.meters() == .5, "minimum selectable distance is half a metre");
    e.distanceLimit.setMeters(1000);
    check(e.distanceLimit.meters() == 100, "numeric distance is capped at one hundred metres");
    for (double invalid :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        e.distanceLimit.setMeters(invalid);
        check(e.distanceLimit.meters() == 100, "non-finite distance settings are ignored");
    }
    Transform rotated;
    rotated.r[0][0] = 0;
    rotated.r[0][2] = 1;
    rotated.r[2][0] = -1;
    rotated.r[2][2] = 0;
    rotated.t = {3, 1, 2};
    check(rotated.valid(), "valid rotated playspace");
    check(near(rotated.inverse().inverse(), rotated), "inverse round trip with rotated origin");
    check(length(rotated.inverse().point(rotated.point({.3, .7, -.2})) - Vec{.3, .7, -.2}) < 1e-8,
          "inverse converts raw coordinates back to standing");
    Transform moved = withOffset(rotated, {.3, .8, -.2});
    for (Vec v : {Vec{1, 0, 1}, Vec{-2, 2, -1}, Vec{0, 1, 0}}) {
        auto nv = keepBoundaryFixed(v, rotated, moved);
        check(length(rotated.point(v) - moved.point(nv)) < 1e-8,
              "boundary fixed in physical space");
    }
    Transform invalid = base;
    invalid.r[0][0] = 2;
    check(!invalid.valid(), "reject nonrigid pose");
    invalid = base;
    invalid.t.y = std::numeric_limits<double>::quiet_NaN();
    check(!invalid.valid() && !near(base, invalid), "reject NaN");
    check(!near(base, shifted), "detect external origin change");

    Engine thrower;
    thrower.sample(false, true, {}, base, .01);
    thrower.sample(true, true, {0, 1, 0}, base, .01);
    for (int i = 1; i <= 10; ++i)
        thrower.sample(true, true, {.005 * i, 1 - .01 * i, .003 * i}, base, .01);
    thrower.sample(false, true, {.05, .9, .03}, base, .01);
    check(length(thrower.releasedVelocity() - Vec{0, 1, 0}) < 1e-8,
          "height-only release carries inverse hand velocity without horizontal motion");
    thrower.sample(false, true, {}, base, .01);
    check(length(thrower.releasedVelocity()) == 0, "release velocity is consumed once");
    thrower.sample(true, true, {}, base, .01);
    for (int i = 1; i <= 5; ++i)
        thrower.sample(true, true, {0, -.01 * i, 0}, base, .01);
    for (int i = 0; i < 10; ++i)
        thrower.sample(true, true, {0, -.05, 0}, base, .01);
    thrower.sample(false, true, {0, -.05, 0}, base, .01);
    check(length(thrower.releasedVelocity()) == 0,
          "holding still before release discards old throw velocity");
    for (double invalidDt : {0., .5, std::numeric_limits<double>::quiet_NaN()}) {
        thrower.sample(true, true, {}, base, .01);
        for (int i = 1; i <= 5; ++i)
            thrower.sample(true, true, {0, -.01 * i, 0}, base, .01);
        thrower.sample(false, true, {}, base, invalidDt);
        check(!thrower.held() && length(thrower.releasedVelocity()) == 0,
              "invalid or stalled release cannot launch a stale throw");
        thrower.sample(false, true, {}, base, .01);
    }
    thrower.xyz = true;
    thrower.gain = 2;
    thrower.sample(true, true, {}, rotated, .01);
    for (int i = 1; i <= 10; ++i)
        thrower.sample(true, true, {.005 * i, -.01 * i, 0}, rotated, .01);
    thrower.sample(false, true, {.05, -.1, 0}, rotated, .01);
    check(length(thrower.releasedVelocity() - Vec{0, 2, -1}) < 1e-8,
          "XYZ throw follows standing axes and drag gain under a rotated origin");
    thrower.sample(true, true, {}, base, .01);
    for (int i = 1; i <= 5; ++i)
        thrower.sample(true, true, {0, -.01 * i, 0}, base, .01);
    thrower.release();
    thrower.sample(false, true, {}, base, .01);
    check(length(thrower.releasedVelocity()) == 0,
          "forced release for reset or suspension clears the throw estimate");
    thrower.sample(true, true, {}, base, .01);
    for (int i = 1; i <= 5; ++i)
        thrower.sample(true, true, {0, -.01 * i, 0}, base, .01);
    thrower.sample(false, false, {}, base, .01);
    check(length(thrower.releasedVelocity()) == 0, "lost tracking on release cannot throw");
    std::printf("%d checks passed\n", checks);
}
