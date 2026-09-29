#include "update.h"
#include "json.hpp"
#include <regex>
#include <stdexcept>

namespace updates {
std::optional<Version> Version::parse(const std::string &text) {
    static const std::regex pattern(
        R"(^(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})\.(0|[1-9][0-9]{0,8})(-beta\.([1-9][0-9]{0,8}))?$)");
    std::smatch match;
    if (!std::regex_match(text, match, pattern)) return {};
    return Version{std::stoi(match[1]), std::stoi(match[2]), std::stoi(match[3]),
                   match[5].matched ? std::stoi(match[5]) : 0};
}
std::optional<Release> select(const std::string &text, const std::string &installed) {
    auto current = Version::parse(installed);
    if (!current) throw std::runtime_error("Invalid installed version");
    auto best = *current;
    const bool allowBeta = current->beta != 0;
    const auto releases = nlohmann::json::parse(text);
    if (!releases.is_array()) throw std::runtime_error("Expected GitHub releases array");
    std::optional<Release> result;
    for (const auto &item : releases) {
        if (!item.is_object() || !item.contains("draft") || !item["draft"].is_boolean() ||
            item["draft"].get<bool>() || !item.contains("prerelease") ||
            !item["prerelease"].is_boolean() || !item.contains("tag_name") ||
            !item["tag_name"].is_string()) continue;
        const auto tag = item["tag_name"].get<std::string>();
        if (tag.empty() || tag.front() != 'v') continue;
        const auto version = Version::parse(tag.substr(1));
        if (!version || (version->beta && !allowBeta) ||
            item["prerelease"].get<bool>() != bool(version->beta) || !(best < *version)) continue;
        const auto archive = "frame-advanced-settings-" + tag.substr(1) + "-linux-aarch64.tar.gz";
        const auto prefix = "https://github.com/grievouz/frame-advanced-settings/releases/download/" + tag + "/";
        bool package = false, checksum = false;
        if (!item.contains("assets") || !item["assets"].is_array()) continue;
        for (const auto &asset : item["assets"]) {
            if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string() ||
                !asset.contains("browser_download_url") || !asset["browser_download_url"].is_string() ||
                !asset.contains("size") || !asset["size"].is_number_unsigned()) continue;
            const auto name = asset["name"].get<std::string>();
            if (asset["browser_download_url"].get<std::string>() != prefix + name) continue;
            const auto size = asset["size"].get<uint64_t>();
            if (name == archive && size > 0 && size <= 32 * 1024 * 1024) package = true;
            if (name == archive + ".sha256" && size > 0 && size <= 1024) checksum = true;
        }
        if (package && checksum) {
            best = *version;
            result = Release{tag.substr(1), archive};
        }
    }
    return result;
}
} // namespace updates
