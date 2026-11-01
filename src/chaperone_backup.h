#pragma once
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace chaperonebackup {
namespace fs = std::filesystem;
constexpr size_t maxFileBytes = 2 * 1024 * 1024;
inline std::optional<std::string> read(const fs::path &path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size > maxFileBytes)
        return {};
    std::ifstream input(path, std::ios::binary);
    std::string data(static_cast<size_t>(size), '\0');
    if (!input.read(data.data(), static_cast<std::streamsize>(data.size())))
        return {};
    // Refuse a changing file instead of backing up a truncated snapshot.
    if (input.peek() != std::char_traits<char>::eof())
        return {};
    return data;
}
struct Backup {
    fs::path source, copy;
    std::string contents;
};
inline std::optional<Backup> save(const fs::path &configRoot, const fs::path &stateRoot) {
    try {
        // Resolve the config directory OpenVR is registered to use. Do not
        // commit after backing up a guessed path belonging to another runtime.
        const auto registry = read(configRoot / "openvr/openvrpaths.vrpath");
        if (!registry)
            return {};
        const auto paths = nlohmann::json::parse(*registry).at("config");
        if (!paths.is_array() || paths.empty() || !paths[0].is_string())
            return {};
        const fs::path config = paths[0].get<std::string>();
        if (!config.is_absolute())
            return {};
        const auto source = config / "chaperone_info.vrchap";
        const auto content = read(source);
        if (!content)
            return {};
        const auto parsed = nlohmann::json::parse(*content);
        if (!parsed.is_object() || !parsed.contains("universes") ||
            !parsed["universes"].is_array() || parsed["universes"].empty())
            return {};
        const auto directory = stateRoot / "chaperone-backups";
        fs::create_directories(directory);
        std::vector<fs::path> previous;
        for (const auto &entry : fs::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (!entry.is_regular_file() || entry.is_symlink() ||
                name.rfind("chaperone-", 0) != 0 || entry.path().extension() != ".vrchap")
                continue;
            if (read(entry.path()) == content)
                return Backup{source, entry.path(), *content};
            previous.push_back(entry.path());
        }
        const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const auto destination = directory / ("chaperone-" + std::to_string(stamp) + ".vrchap");
        if (fs::exists(destination))
            return {};
        // A failed write leaves at most one bounded temporary file.
        const auto temporary = directory / "chaperone-pending.tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(content->data(), static_cast<std::streamsize>(content->size()));
            output.close();
            if (!output)
                return {};
        }
        if (read(temporary) != content)
            return {};
        fs::rename(temporary, destination);
        // Keep at most five verified configurations (10 MiB maximum).
        std::sort(previous.begin(), previous.end());
        while (previous.size() > 4) {
            fs::remove(previous.front());
            previous.erase(previous.begin());
        }
        return Backup{source, destination, *content};
    } catch (const std::exception &) {
        return {};
    }
}
} // namespace chaperonebackup
