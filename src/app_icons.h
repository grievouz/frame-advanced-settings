#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace appicons {
struct Bitmap {
    static constexpr int size = 48;
    std::vector<uint8_t> rgba;
};
struct Options {
    std::vector<std::filesystem::path> roots, pixmaps;
    std::string theme = "breeze";
    static Options fromEnvironment();
};
// Worker-thread only. Decoded thumbnails and failed lookups are cached until
// Refresh. No image parsing, directory scans or filesystem I/O in panel.render.
class Loader {
  public:
    explicit Loader(Options options);
    std::shared_ptr<const Bitmap> load(const std::string &icon);
    void clear();

  private:
    struct Directory {
        std::filesystem::path path;
        int distance = 0;
    };
    Options options_;
    std::vector<Directory> directories_;
    std::map<std::string, std::shared_ptr<const Bitmap>> cache_;
    bool indexed_ = false;
    void indexThemes();
};
} // namespace appicons
