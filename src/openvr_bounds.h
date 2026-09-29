#pragma once
#include "drag.h"
#include "openvr.h"
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace drag {
// The callback has GetLiveCollisionBoundsInfo's signature. Keeping the read
// separate allows regression testing with the actual Frame query sequence.
template <typename Query>
bool readLiveBounds(Query &&query, std::vector<std::array<Vec, 4>> &bounds, std::string &problem) {
    problem.clear();
    auto fail = [&](const std::string &reason) {
        problem = reason;
        return false;
    };
    uint32_t count = 0;
    // On Frame, a null-buffer call returns false while still providing the
    // required count. Trust only the bounded count here, then require a
    // successful data read; false on the data call remains an error.
    query(nullptr, &count);
    if (count == 0 || count > 512)
        return fail("Live boundary count rejected: " + std::to_string(count));
    std::vector<vr::HmdQuad_t> quads(count);
    if (!query(quads.data(), &count))
        return fail("Live boundary data query returned false");
    if (count == 0 || count > quads.size())
        return fail("Live boundary count changed during read: " + std::to_string(count));
    std::vector<std::array<Vec, 4>> result(count);
    for (size_t i = 0; i < count; ++i)
        for (size_t j = 0; j < 4; ++j) {
            const auto &v = quads[i].vCorners[j].v;
            result[i][j] = {v[0], v[1], v[2]};
            if (!finite(result[i][j]))
                return fail("Live boundary contains a non-finite coordinate");
        }
    bounds = std::move(result);
    return true;
}
} // namespace drag
