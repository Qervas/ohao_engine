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
    if (lower == "none")  return DenoiseMode::None;
    if (lower == "oidn")  return DenoiseMode::OIDN;
    if (lower == "optix") {
        std::cerr << "[Denoise] --denoise=optix is no longer supported — falling back to OIDN\n";
        return DenoiseMode::OIDN;
    }
    if (lower == "nrd") {
#ifdef OHAO_NRD_ENABLED
        return DenoiseMode::NRD;
#else
        std::cerr << "[Denoise] --denoise=nrd requested but OHAO_NRD=OFF at build time — falling back to None\n";
        return DenoiseMode::None;
#endif
    }
    if (lower == "atrous") return DenoiseMode::Atrous;
    if (lower == "dlssrr" || lower == "dlss" || lower == "dlssd") {
#ifdef OHAO_DLSS_ENABLED
        return DenoiseMode::DLSSRR;
#else
        std::cerr << "[Denoise] --denoise=" << lower
                  << " requested but OHAO_DLSS=OFF at build time — falling back to None\n";
        return DenoiseMode::None;
#endif
    }
    std::cerr << "[Denoise] Unknown mode '" << s
              << "' — falling back to None\n";
    return DenoiseMode::None;
}

const char* denoiseModeName(DenoiseMode mode) {
    switch (mode) {
        case DenoiseMode::None:   return "none";
        case DenoiseMode::OIDN:   return "oidn";
        case DenoiseMode::NRD:    return "nrd";
        case DenoiseMode::Atrous: return "atrous";
        case DenoiseMode::DLSSRR: return "dlssrr";
    }
    return "unknown";
}

} // namespace ohao
