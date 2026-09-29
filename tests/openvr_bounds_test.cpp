#include "openvr_bounds.h"
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
struct Runtime {
    bool countOk = false, dataOk = true, nonFinite = false;
    uint32_t required = 4, returned = 4;
    int dataCalls = 0;
    bool operator()(vr::HmdQuad_t *quads, uint32_t *count) {
        if (!quads) {
            *count = required;
            return countOk;
        }
        ++dataCalls;
        const uint32_t capacity = *count;
        *count = returned;
        if (!dataOk)
            return false;
        for (uint32_t i = 0; i < std::min(capacity, returned); ++i)
            for (auto &corner : quads[i].vCorners) {
                corner.v[0] = static_cast<float>(i);
                corner.v[1] = nonFinite ? std::numeric_limits<float>::quiet_NaN() : 1.5f;
                corner.v[2] = -2.f;
            }
        return true;
    }
};
int main() {
    Runtime runtime;
    std::vector<std::array<drag::Vec, 4>> bounds;
    std::string problem;
    check(drag::readLiveBounds(runtime, bounds, problem) && bounds.size() == 4 &&
              bounds[3][2].x == 3 && bounds[0][0].y == 1.5 && bounds[0][0].z == -2 &&
              problem.empty(),
          "Frame false count query followed by successful four-quad read is accepted");
    runtime.countOk = true;
    check(drag::readLiveBounds(runtime, bounds, problem), "true count query also works");
    runtime.dataOk = false;
    check(!drag::readLiveBounds(runtime, bounds, problem) &&
              problem == "Live boundary data query returned false" && bounds.size() == 4,
          "a valid count never overrides a failed data read");
    for (uint32_t count : {0u, 513u}) {
        runtime = Runtime{};
        runtime.required = count;
        check(!drag::readLiveBounds(runtime, bounds, problem) && runtime.dataCalls == 0,
              "empty or excessive count blocks data read");
    }
    for (uint32_t count : {0u, 5u}) {
        runtime = Runtime{};
        runtime.returned = count;
        check(!drag::readLiveBounds(runtime, bounds, problem) && bounds.size() == 4,
              "empty result or growth beyond capacity rejected");
    }
    runtime = Runtime{};
    runtime.nonFinite = true;
    check(!drag::readLiveBounds(runtime, bounds, problem) &&
              problem == "Live boundary contains a non-finite coordinate" && bounds[0][0].y == 1.5,
          "non-finite data rejected without replacing the previous bounds");
    std::printf("%d OpenVR boundary read checks passed\n", checks);
}
