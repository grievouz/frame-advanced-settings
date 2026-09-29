#include "app_icons.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void put(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    check(bool(out), "Fixture write");
}
static std::string svg(const char *color) {
    return std::string("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"48\" height=\"48\"><rect "
                       "width=\"48\" height=\"48\" fill=\"") +
           color + "\"/></svg>";
}
static uint32_t center(const std::shared_ptr<const appicons::Bitmap> &bitmap) {
    check(bitmap && bitmap->rgba.size() == 48 * 48 * 4, "Decoded fixed-size thumbnail");
    const auto p = bitmap->rgba.data() + (24 * 48 + 24) * 4;
    return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
}
int main(int argc, char **argv) {
    const auto root =
        fs::absolute(fs::current_path() /
                     ("app-icons-fixture-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
            .lexically_normal();
    try {
        check(argc == 2 && fs::create_directory(root), "Create test fixture");
        appicons::Options options{{root / "user", root / "system"}, {root / "pixmaps"}, "theme"};
        const std::string index =
            "[Icon Theme]\nDirectories=48/apps,128/apps\nInherits=parent\n"
            "[48/apps]\nSize=48\nType=Fixed\n[128/apps]\nSize=128\nType=Fixed\n";
        put(root / "system/theme/index.theme", index);
        put(root / "system/parent/index.theme",
            "[Icon Theme]\nDirectories=48/apps\nInherits=theme\n[48/apps]\nSize=48\n");
        put(root / "system/hicolor/index.theme",
            "[Icon Theme]\nDirectories=48/apps\n[48/apps]\nSize=48\n");
        put(root / "system/theme/48/apps/app.svg", svg("#ff0000"));
        put(root / "user/theme/48/apps/app.svg", svg("#00ff00"));
        put(root / "system/parent/48/apps/inherited.svg", svg("#0000ff"));
        put(root / "system/theme/128/apps/large.svg", svg("#ff0000"));
        put(root / "system/parent/48/apps/large.svg", svg("#0000ff"));
        put(root / "system/hicolor/48/apps/flatpak.svg", svg("#00ff00"));
        put(root / "pixmaps/un themed.svg", svg("#ffff00"));
        appicons::Loader loader(options);
        check(center(loader.load("app")) == 0x00ff00, "User theme override wins");
        check(center(loader.load("inherited")) == 0x0000ff, "Inherited theme and cycle handling");
        check(center(loader.load("large")) == 0xff0000,
              "Current theme outranks closer-sized parent");
        check(center(loader.load("flatpak")) == 0x00ff00, "Hicolor fallback");
        check(center(loader.load("un themed")) == 0xffff00, "Unthemed pixmap with spaces");
        check(center(loader.load((root / "system/parent/48/apps/inherited.svg").string())) ==
                  0x0000ff,
              "Absolute SVG path");
        const auto first = loader.load("app");
        put(root / "user/theme/48/apps/app.svg", svg("#0000ff"));
        check(loader.load("app") == first && center(first) == 0x00ff00,
              "Cache stable between refreshes");
        loader.clear();
        check(center(loader.load("app")) == 0x0000ff, "Refresh reloads replaced icon");
        put(root / "invalid.png", "not a png");
        put(root / "oversized.svg",
            "<svg width=\"999999\" height=\"999999\"><rect width=\"1\" height=\"1\"/></svg>");
        check(!loader.load((root / "invalid.png").string()), "Corrupt PNG falls back");
        check(!loader.load((root / "oversized.svg").string()), "Oversized SVG falls back");
        check(!loader.load("missing") && !loader.load("../app") && !loader.load(""),
              "Missing or invalid name falls back");
        auto png = loader.load(fs::absolute(argv[1]).string());
        check(png && png->rgba[3] == 0, "PNG preserves transparent corner");
        const auto p = png->rgba.data() + (33 * 48 + 33) * 4;
        check(p[0] == 26 && p[1] == 159 && p[2] == 255 && p[3] == 255,
              "PNG resizing preserves icon color");
        check(root.parent_path() == fs::current_path().lexically_normal() &&
                  root.filename().string().rfind("app-icons-fixture-", 0) == 0,
              "Cleanup confined to generated fixture");
        fs::remove_all(root);
        std::cout << "Themed SVG/PNG lookup, inheritance, caching and fallback passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\nFixture retained at " << root << '\n';
        return 1;
    }
}
