#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace ohao {

namespace {
std::string toLower(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}
} // namespace

DenoiseMode parseDenoiseMode(const std::string& s) {
    const std::string lower = toLower(s);
    if (lower == "none") return DenoiseMode::None;
    if (lower == "oidn") return DenoiseMode::OIDN;
    std::cerr << "[Denoise] Unknown mode '" << s
              << "' — falling back to None\n";
    return DenoiseMode::None;
}

const char* denoiseModeName(DenoiseMode mode) {
    switch (mode) {
        case DenoiseMode::None: return "none";
        case DenoiseMode::OIDN: return "oidn";
    }
    return "unknown";
}

} // namespace ohao
