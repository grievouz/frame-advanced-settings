#pragma once
#include "playspace.h"

// Numeric geometry from PID 8059, build 7acc0e84cae6, 2026-09-29 12:29:39.
// The runtime replaced a 20-wall room with four walls in universe 424242.
// This is not an equivalent re-expression of the original physical boundary.
namespace fixture {
inline drag::Space frameSwitchBaseline() {
    const drag::Vec points[] = {{.1600617766, 0, .9866628051},   {.4574572742, 0, .8894883394},
                                {.7102686167, 0, .7051696777},   {.8937489986, 0, .4517492056},
                                {.9899378419, 0, .154033348},    {.9894197583, 0, -.1588350385},
                                {.8922452331, 0, -.4562306702},  {.707926631, 0, -.7090419531},
                                {.45450598, 0, -.8925223351},    {.1567903757, 0, -.9887111783},
                                {-.1560782641, 0, -.9881930351}, {-.4534736872, 0, -.8910185695},
                                {-.7062851191, 0, -.7066997886}, {-.889765501, 0, -.4532791674},
                                {-.9859541655, 0, -.1555637568}, {-.9854360819, 0, .157304883},
                                {-.8882615566, 0, .4547004998},  {-.7039427757, 0, .7075119615},
                                {-.4505225122, 0, .8909920454},  {-.1528066397, 0, .9871809483}};
    drag::Space s;
    s.universe = 5624895237226323188ULL;
    s.pose = {{{.9998907447, 0, .01477998309}, {0, 1, 0}, {-.01477998309, 0, .9998907447}},
              {.02059686556, -1.600437522, .1296200752}};
    for (size_t i = 0; i < 20; ++i) {
        const auto a = points[i], b = points[(i + 1) % 20];
        s.bounds.push_back(
            {a, drag::Vec{a.x, 2.430000067, a.z}, drag::Vec{b.x, 2.430000067, b.z}, b});
    }
    return s;
}
inline drag::Space frameSwitchExpected() {
    auto s = frameSwitchBaseline();
    s.pose.t = {-.5321386456, -2.005125284, .01140359696};
    return s;
}
inline drag::Space frameSwitchActual() {
    auto s = frameSwitchBaseline();
    s.universe = 424242;
    s.pose.t = {-.5321385264, -1.600576401, .01140359417};
    s.bounds.clear();
    const drag::Vec points[] = {{-.5, 0, -.4999999702},
                                {-.5, 0, .4999999702},
                                {.4999999404, 0, .4999999702},
                                {.4999999404, 0, -.4999999702}};
    for (size_t i = 0; i < 4; ++i) {
        const auto a = points[i], b = points[(i + 1) % 4];
        s.bounds.push_back(
            {a, drag::Vec{a.x, 2.430000067, a.z}, drag::Vec{b.x, 2.430000067, b.z}, b});
    }
    return s;
}
inline drag::Transform frameSwitchRuntimeOrigin() {
    auto t = frameSwitchActual().pose;
    t.t = {-.532138586, -1.600576401, .01140359603};
    return t;
}
} // namespace fixture
