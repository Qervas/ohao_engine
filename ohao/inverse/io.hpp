#pragma once

// PNG / θ-JSON I/O + target highlight diagnostics for inverse_fit.

#include "inverse/image_loss.hpp"

#include "stb_image.h"
#include "stb_image_write.h"

#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ohao::inverse {

/// Specular proxy for target image (floor/wall crop).
/// Combines bright-pixel fraction with max/mean luma contrast — mirror floors under
/// soft studio HDRI often lack crushed whites but still show high peak contrast.
[[nodiscard]] inline double targetHighlightScore(const ImageRGBA8& img, double xMaxFrac, double yMinFrac) {
    if (img.empty()) return 0.0;
    const uint32_t xLim = (xMaxFrac >= 1.0)
                              ? img.width
                              : static_cast<uint32_t>(std::ceil(xMaxFrac * img.width));
    const uint32_t y0 = (yMinFrac <= 0.0)
                            ? 0u
                            : static_cast<uint32_t>(std::floor(yMinFrac * img.height));
    size_t bright = 0, count = 0;
    double sumL = 0.0, maxL = 0.0, sumMax = 0.0;
    for (uint32_t y = y0; y < img.height; ++y) {
        for (uint32_t x = 0; x < xLim; ++x) {
            const size_t o = (static_cast<size_t>(y) * img.width + x) * 4;
            const double r = img.rgba[o] / 255.0;
            const double g = img.rgba[o + 1] / 255.0;
            const double b = img.rgba[o + 2] / 255.0;
            const double mx = std::max({r, g, b});
            const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            if (mx > 0.55 || luma > 0.50) ++bright;
            sumL += luma;
            sumMax += mx;
            maxL = std::max(maxL, luma);
            ++count;
        }
    }
    if (count == 0) return 0.0;
    const double frac = static_cast<double>(bright) / static_cast<double>(count);
    const double meanL = sumL / static_cast<double>(count);
    const double meanMax = sumMax / static_cast<double>(count);
    const double contrast = maxL / (meanL + 1e-3);
    // Soft blend: bright fraction + normalized peak contrast + mean-max lift.
    const double cScore = std::clamp((contrast - 1.4) / 2.5, 0.0, 1.0);
    const double mScore = std::clamp((meanMax - 0.25) / 0.55, 0.0, 1.0);
    return std::clamp(0.40 * frac + 0.35 * cScore + 0.25 * mScore, 0.0, 1.0);
}

inline ImageRGBA8 loadPNG(const std::filesystem::path& path) {
    ImageRGBA8 img;
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!data || w <= 0 || h <= 0) {
        if (data) stbi_image_free(data);
        return img;
    }
    img.width = static_cast<uint32_t>(w);
    img.height = static_cast<uint32_t>(h);
    img.rgba.assign(data, data + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    stbi_image_free(data);
    return img;
}

/// Load C1 θ prior JSON: `{"theta":[...]}` or bare `[...]`.
/// Prefer the array after `"theta"` so names/metadata arrays are ignored.
inline bool loadThetaInit(const std::filesystem::path& path, std::vector<double>& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string s = buf.str();
    size_t lb = std::string::npos;
    const auto key = s.find("\"theta\"");
    if (key != std::string::npos) {
        lb = s.find('[', key);
    }
    if (lb == std::string::npos) {
        lb = s.find('['); // bare array fallback
    }
    if (lb == std::string::npos) return false;
    // Match the bracket for this array only (not rfind of whole file).
    int depth = 0;
    size_t rb = std::string::npos;
    for (size_t i = lb; i < s.size(); ++i) {
        if (s[i] == '[') ++depth;
        else if (s[i] == ']') {
            --depth;
            if (depth == 0) {
                rb = i;
                break;
            }
        }
    }
    if (rb == std::string::npos || rb <= lb) return false;
    out.clear();
    std::stringstream arr(s.substr(lb + 1, rb - lb - 1));
    std::string tok;
    while (std::getline(arr, tok, ',')) {
        size_t a = 0, b = tok.size();
        while (a < b && std::isspace(static_cast<unsigned char>(tok[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1]))) --b;
        if (a >= b) continue;
        // Skip non-numeric tokens (e.g. leftover strings)
        const char c0 = tok[a];
        if (!(c0 == '-' || c0 == '+' || c0 == '.' || (c0 >= '0' && c0 <= '9'))) continue;
        try {
            out.push_back(std::stod(tok.substr(a, b - a)));
        } catch (...) {
            return false;
        }
    }
    return !out.empty();
}

inline bool savePNG(const ImageRGBA8& img, const std::filesystem::path& path) {
    if (img.empty()) return false;
    std::filesystem::create_directories(path.parent_path());
    return stbi_write_png(path.string().c_str(), static_cast<int>(img.width),
                          static_cast<int>(img.height), 4, img.rgba.data(),
                          static_cast<int>(img.width * 4)) != 0;
}


} // namespace ohao::inverse
