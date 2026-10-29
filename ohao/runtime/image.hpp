#pragma once
// RGBA8 images for captures: crop, scale to a preview, encode PNG or JPEG in memory, write
// a PNG file. CPU-only. PNG is OHAO's own one-pass encoder (filter Up, one fixed-Huffman
// deflate block with runs as distance-1 matches): lossless and fast; JPEG is stb_image_write.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ohao::runtime {

struct Rgba8Image {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> pixels;   // width * height * 4, rows top to bottom
};

// The [x, y, w, h] part of a width x height RGBA8 frame; the caller checks it lies inside.
[[nodiscard]] Rgba8Image crop(std::span<const uint8_t> frame, uint32_t width, uint32_t height,
                              const std::array<int32_t, 4>& region);
// The image scaled down, aspect kept, so its long side is at most maxSide (area average:
// every source pixel weighs by the part of it an output pixel covers). An image that fits
// is returned as is.
[[nodiscard]] Rgba8Image fitWithin(Rgba8Image image, uint32_t maxSide);
// Encoded files, in memory. JPEG drops alpha.
[[nodiscard]] std::string encodePng(const Rgba8Image& image);
[[nodiscard]] std::string encodeJpeg(const Rgba8Image& image, int quality);
// False when the file cannot be written.
[[nodiscard]] bool writePng(const std::string& path, std::span<const uint8_t> pixels, uint32_t width, uint32_t height);
// Linear RGB as a Portable Float Map (PF, little-endian, rows bottom to top as the format has
// them) from RGBA32F rows top to bottom; alpha is dropped. False when the file cannot be written.
[[nodiscard]] bool writePfm(const std::string& path, std::span<const float> rgba, uint32_t width, uint32_t height);

} // namespace ohao::runtime
