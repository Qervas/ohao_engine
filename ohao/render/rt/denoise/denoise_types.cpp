#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include "core/log.hpp"

namespace ohao {

namespace {
std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
} // namespace

DenoiseMode parseDenoiseMode(std::string_view s) {
    const std::string lower = toLower(s);
    if (lower == "none")  return DenoiseMode::None;
    if (lower == "oidn")  return DenoiseMode::OIDN;
    if (lower == "optix") {
        OHAO_LOG(Warn, render, "Denoise: --denoise=optix is no longer supported — falling back to OIDN");
        return DenoiseMode::OIDN;
    }
    if (lower == "nrd") {
#ifdef OHAO_NRD_ENABLED
        return DenoiseMode::NRD;
#else
        OHAO_LOG(Warn, render, "Denoise: --denoise=nrd requested but OHAO_NRD=OFF at build time — falling back to None");
        return DenoiseMode::None;
#endif
    }
    if (lower == "dlssrr" || lower == "dlss" || lower == "dlssd") {
#ifdef OHAO_DLSS_ENABLED
        return DenoiseMode::DLSSRR;
#else
        OHAO_LOG(Warn, render, "Denoise: --denoise={} requested but OHAO_DLSS=OFF at build time — falling back to None",
                 lower);
        return DenoiseMode::None;
#endif
    }
    OHAO_LOG(Warn, render, "Denoise: unknown mode '{}' — falling back to None", s);
    return DenoiseMode::None;
}

const char* denoiseModeName(DenoiseMode mode) {
    switch (mode) {
        case DenoiseMode::None:   return "none";
        case DenoiseMode::OIDN:   return "oidn";
        case DenoiseMode::NRD:    return "nrd";
        case DenoiseMode::DLSSRR: return "dlssrr";
    }
    return "unknown";
}

} // namespace ohao
