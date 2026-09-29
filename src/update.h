#pragma once
#include <optional>
#include <string>
#include <tuple>

namespace updates {
struct Version {
    int major = 0, minor = 0, patch = 0, beta = 0;
    static std::optional<Version> parse(const std::string &text);
    bool operator<(const Version &other) const {
        const auto base = std::tie(major, minor, patch);
        const auto theirs = std::tie(other.major, other.minor, other.patch);
        if (base != theirs) return base < theirs;
        if (!beta || !other.beta) return beta && !other.beta;
        return beta < other.beta;
    }
};
struct Release { std::string version, archive; };
// Stable installations stay on stable; beta installations can receive newer betas.
std::optional<Release> select(const std::string &json, const std::string &installed);
} // namespace updates
