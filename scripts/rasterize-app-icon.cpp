// Offline SVG rasterizer using the already pinned NanoSVG dependency.
// Writes 512x512 straight-alpha RGBA; PNG encoding is performed by Pillow.
// g++ -std=c++17 -O2 -Ivendor/nanosvg scripts/rasterize-app-icon.cpp -o build/rasterize-app-icon
// build/rasterize-app-icon assets/icons/frame-advanced-settings.svg build/app-icon.rgba
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: rasterize-app-icon INPUT.svg OUTPUT.rgba\n";
        return 1;
    }
    std::unique_ptr<NSVGimage, decltype(&nsvgDelete)> image(
        nsvgParseFromFile(argv[1], "px", 96), nsvgDelete);
    std::unique_ptr<NSVGrasterizer, decltype(&nsvgDeleteRasterizer)> rasterizer(
        nsvgCreateRasterizer(), nsvgDeleteRasterizer);
    if (!image || !image->shapes || image->width != 128 || image->height != 128 || !rasterizer) {
        std::cerr << "Expected a valid 128x128 SVG icon.\n";
        return 1;
    }
    constexpr int size = 512;
    std::vector<unsigned char> rgba(size * size * 4);
    nsvgRasterize(rasterizer.get(), image.get(), 0, 0, size / image->width,
                  rgba.data(), size, size, size * 4);
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char *>(rgba.data()), rgba.size());
    return output ? 0 : 1;
}
