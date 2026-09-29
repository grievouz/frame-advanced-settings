#include "app_icons.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048
#include "stb_image.h"
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace appicons {
namespace fs = std::filesystem;
namespace {
std::string read(const fs::path &path, size_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return {};
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    auto size = file ? file.tellg() : std::streampos(-1);
    if (size <= 0 || size > static_cast<std::streamoff>(limit))
        return {};
    std::string data(static_cast<size_t>(size), '\0');
    file.seekg(0);
    file.read(data.data(), data.size());
    return file ? data : std::string{};
}
std::string trim(const std::string &s) {
    const auto begin = s.find_first_not_of(" \t\r");
    return begin == std::string::npos ? ""
                                      : s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
}
using Ini = std::map<std::string, std::map<std::string, std::string>>;
Ini ini(const std::string &text) {
    Ini result;
    std::istringstream lines(text);
    std::string section, line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const auto eq = line.find('=');
        if (eq != std::string::npos)
            result[section][trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return result;
}
std::vector<std::string> split(const std::string &text, char separator = ',') {
    std::vector<std::string> result;
    std::istringstream stream(text);
    std::string value;
    while (std::getline(stream, value, separator))
        if (!(value = trim(value)).empty())
            result.push_back(value);
    return result;
}
int number(const std::map<std::string, std::string> &section, const char *key, int fallback) {
    try {
        const int n = std::stoi(section.at(key));
        return n >= 0 && n <= 4096 ? n : fallback;
    } catch (...) {
        return fallback;
    }
}
bool safeRelative(const fs::path &path) {
    if (path.empty() || path.is_absolute() || path.has_root_name())
        return false;
    for (const auto &part : path)
        if (part == ".." || part == "." || part.string().find('\\') != std::string::npos)
            return false;
    return true;
}
bool name(const std::string &value) {
    return !value.empty() && value.size() <= 255 && safeRelative(fs::path(value)) &&
           fs::path(value).filename() == fs::path(value);
}
fs::path envPath(const char *key, fs::path fallback) {
    const char *value = std::getenv(key);
    return value && fs::path(value).is_absolute() ? fs::path(value) : std::move(fallback);
}
std::shared_ptr<const Bitmap> decode(const fs::path &path) {
    auto data = read(path, 2 * 1024 * 1024);
    if (data.empty())
        return {};
    auto bitmap = std::make_shared<Bitmap>();
    constexpr int side = Bitmap::size;
    bitmap->rgba.resize(side * side * 4);
    if (path.extension() == ".svg") {
        if (data.size() > 512 * 1024)
            return {};
        // Theme text icons inherit the panel's foreground color. NanoSVG does
        // not resolve CSS currentColor; it has no script or external URL loader.
        for (size_t at = 0; (at = data.find("currentColor", at)) != std::string::npos; at += 7)
            data.replace(at, 12, "#dee2e5");
        std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> svg(nsvgParse(data.data(), "px", 96),
                                                              nsvgDelete);
        if (!svg || !svg->shapes || !std::isfinite(svg->width) || !std::isfinite(svg->height) ||
            svg->width <= 0 || svg->height <= 0 || svg->width > 4096 || svg->height > 4096)
            return {};
        size_t shapes = 0, points = 0;
        for (auto shape = svg->shapes; shape; shape = shape->next) {
            if (++shapes > 2048)
                return {};
            for (auto path = shape->paths; path; path = path->next) {
                points += path->npts;
                if (points > 65536)
                    return {};
            }
        }
        std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> raster(
            nsvgCreateRasterizer(), nsvgDeleteRasterizer);
        if (!raster)
            return {};
        const float scale = side / std::max(svg->width, svg->height);
        nsvgRasterize(raster.get(), svg.get(), (side - svg->width * scale) / 2,
                      (side - svg->height * scale) / 2, scale, bitmap->rgba.data(), side, side,
                      side * 4);
    } else {
        int width = 0, height = 0, components = 0;
        const auto bytes = reinterpret_cast<const stbi_uc *>(data.data());
        if (!stbi_info_from_memory(bytes, static_cast<int>(data.size()), &width, &height,
                                   &components) ||
            width <= 0 || height <= 0 || width > 2048 || height > 2048)
            return {};
        std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
            stbi_load_from_memory(bytes, static_cast<int>(data.size()), &width, &height,
                                  &components, 4),
            stbi_image_free);
        if (!decoded)
            return {};
        const float scale = float(side) / std::max(width, height);
        const int w = std::max(1, int(std::lround(width * scale))),
                  h = std::max(1, int(std::lround(height * scale)));
        // Area sampling in premultiplied alpha avoids dark outlines around
        // transparent PNGs when reducing large application artwork to 48px.
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double left = double(x) * width / w, right = double(x + 1) * width / w;
                const double top = double(y) * height / h, bottom = double(y + 1) * height / h;
                double sum[4]{}, area = 0;
                for (int yy = int(top); yy < std::ceil(bottom); ++yy)
                    for (int xx = int(left); xx < std::ceil(right); ++xx) {
                        const double weight =
                            (std::min(right, double(xx + 1)) - std::max(left, double(xx))) *
                            (std::min(bottom, double(yy + 1)) - std::max(top, double(yy)));
                        const auto p =
                            decoded.get() +
                            (std::min(yy, height - 1) * width + std::min(xx, width - 1)) * 4;
                        area += weight;
                        sum[3] += p[3] * weight;
                        for (int c = 0; c < 3; ++c)
                            sum[c] += p[c] * p[3] * weight;
                    }
                const int at = ((y + (side - h) / 2) * side + x + (side - w) / 2) * 4;
                for (int c = 0; c < 3; ++c)
                    bitmap->rgba[at + c] = sum[3] ? static_cast<uint8_t>(std::clamp(
                                                        std::lround(sum[c] / sum[3]), 0l, 255l))
                                                  : 0;
                bitmap->rgba[at + 3] =
                    static_cast<uint8_t>(std::clamp(std::lround(sum[3] / area), 0l, 255l));
            }
    }
    bool visible = false;
    for (size_t i = 3; i < bitmap->rgba.size(); i += 4)
        visible |= bitmap->rgba[i] != 0;
    return visible ? bitmap : nullptr;
}
} // namespace
Options Options::fromEnvironment() {
    Options result;
    const auto home = envPath("HOME", {});
    const auto userData = envPath("XDG_DATA_HOME", home / ".local/share");
    result.roots = {userData / "icons", home / ".icons"};
    result.pixmaps = {userData / "pixmaps"};
    const auto envDirs = std::getenv("XDG_DATA_DIRS");
    auto dirs = split(envDirs && *envDirs ? envDirs : "/usr/local/share:/usr/share", ':');
    dirs.push_back((home / ".local/share/flatpak/exports/share").string());
    dirs.push_back("/var/lib/flatpak/exports/share");
    for (const auto &dir : dirs)
        if (fs::path(dir).is_absolute()) {
            result.roots.push_back(fs::path(dir) / "icons");
            result.pixmaps.push_back(fs::path(dir) / "pixmaps");
        }
    auto config =
        ini(read(envPath("XDG_CONFIG_HOME", home / ".config") / "kdeglobals", 256 * 1024));
    if (name(config["Icons"]["Theme"]))
        result.theme = config["Icons"]["Theme"];
    return result;
}
Loader::Loader(Options options) : options_(std::move(options)) {}
void Loader::clear() {
    cache_.clear();
    directories_.clear();
    indexed_ = false;
}
void Loader::indexThemes() {
    indexed_ = true;
    std::set<std::string> visited;
    std::function<void(const std::string &)> add = [&](const std::string &theme) {
        if (!name(theme) || visited.size() >= 16 || !visited.insert(theme).second)
            return;
        Ini info;
        for (const auto &root : options_.roots) {
            info = ini(read(root / theme / "index.theme", 256 * 1024));
            if (!info.empty())
                break;
        }
        auto subdirs = split(info["Icon Theme"]["Directories"]);
        const auto scaled = split(info["Icon Theme"]["ScaledDirectories"]);
        subdirs.insert(subdirs.end(), scaled.begin(), scaled.end());
        // Flatpak exports can contain hicolor sizes without their own index.
        if (subdirs.empty())
            subdirs = {"48x48/apps",   "64x64/apps",   "scalable/apps",
                       "128x128/apps", "256x256/apps", "32x32/apps"};
        std::vector<Directory> themeDirs;
        for (size_t i = 0; i < std::min(size_t(1024), subdirs.size()); ++i) {
            const auto &subdir = subdirs[i];
            if (!safeRelative(fs::path(subdir)))
                continue;
            const auto &section = info[subdir];
            int size = number(section, "Size", 48),
                scale = std::max(1, number(section, "Scale", 1));
            const auto type = section.find("Type");
            const auto kind = type == section.end() ? "Threshold" : type->second;
            int minimum = size, maximum = size;
            if (kind == "Scalable") {
                minimum = number(section, "MinSize", size);
                maximum = number(section, "MaxSize", size);
            } else if (kind == "Threshold") {
                const int threshold = number(section, "Threshold", 2);
                minimum -= threshold;
                maximum += threshold;
            }
            const int distance =
                std::max({0, minimum * scale - Bitmap::size, Bitmap::size - maximum * scale});
            for (const auto &root : options_.roots) {
                std::error_code error;
                const auto path = root / theme / subdir;
                if (fs::is_directory(path, error) && !error)
                    themeDirs.push_back({path, distance});
            }
        }
        std::stable_sort(
            themeDirs.begin(), themeDirs.end(),
            [](const Directory &a, const Directory &b) { return a.distance < b.distance; });
        directories_.insert(directories_.end(), themeDirs.begin(), themeDirs.end());
        for (const auto &parent : split(info["Icon Theme"]["Inherits"]))
            add(parent);
    };
    add(options_.theme);
    add("hicolor");
    for (const auto &root : options_.roots)
        directories_.push_back({root, 0});
    for (const auto &root : options_.pixmaps)
        directories_.push_back({root, 0});
}
std::shared_ptr<const Bitmap> Loader::load(const std::string &icon) {
    if (icon.empty() || icon.size() > 4096)
        return {};
    if (auto cached = cache_.find(icon); cached != cache_.end())
        return cached->second;
    std::shared_ptr<const Bitmap> bitmap;
    const fs::path requested(icon);
    if (requested.is_absolute())
        bitmap = decode(requested);
    else if (name(icon)) {
        if (!indexed_)
            indexThemes();
        for (const auto &directory : directories_) {
            for (const auto extension : {".png", ".svg", ".jpg"}) {
                const auto filename = requested.extension() == extension ? icon : icon + extension;
                bitmap = decode(directory.path / filename);
                if (bitmap)
                    break;
            }
            if (bitmap)
                break;
        }
    }
    if (cache_.size() < 512)
        cache_[icon] = bitmap;
    return bitmap;
}
} // namespace appicons
