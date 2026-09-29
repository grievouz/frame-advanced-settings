#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace voiceclip {
// 48 kHz mono signed 16-bit PCM, held only in memory. The short warm-up lets
// SteamOS activate the same filters that are used by other capture apps.
inline constexpr size_t bytesPerSecond = 48000 * 2;
inline constexpr size_t clipBytes = 6 * bytesPerSecond;
inline constexpr size_t warmupBytes = 3 * bytesPerSecond / 10;
using Audio = std::vector<uint8_t>;
class Capture {
    size_t skip_ = warmupBytes;
    Audio samples_;

  public:
    Capture() { samples_.reserve(clipBytes); }
    void append(const uint8_t *bytes, size_t size) {
        const auto skipped = std::min(skip_, size);
        skip_ -= skipped;
        bytes += skipped;
        size -= skipped;
        const auto count = std::min(size, clipBytes - samples_.size());
        samples_.insert(samples_.end(), bytes, bytes + count);
    }
    size_t size() const { return samples_.size(); }
    bool full() const { return size() == clipBytes; }
    int secondsLeft() const {
        return int((clipBytes - size() + bytesPerSecond - 1) / bytesPerSecond);
    }
    Audio take() { return std::move(samples_); }
};
} // namespace voiceclip
