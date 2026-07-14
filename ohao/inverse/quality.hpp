#pragma once

// Dual-budget quality presets for inverse_fit (Phase A plan).

#include <cstdint>
#include <string_view>

namespace ohao::inverse {

struct RenderBudget {
    uint32_t width{640};
    uint32_t height{360};
    int spp{128};
};

struct QualityPreset {
    const char* name;
    RenderBudget fit;
    RenderBudget show;
};

// high = product default (clean 1080p stills)
inline constexpr QualityPreset kQualityDraft{
    "draft", {384, 216, 32}, {1280, 720, 256}};
inline constexpr QualityPreset kQualityHigh{
    "high", {640, 360, 128}, {1920, 1080, 1024}};
inline constexpr QualityPreset kQualityUltra{
    "ultra", {960, 540, 256}, {1920, 1080, 2048}};
inline constexpr QualityPreset kQualityCinema{
    "cinema", {960, 540, 256}, {3840, 2160, 2048}};

[[nodiscard]] inline QualityPreset qualityFromName(std::string_view s) {
    if (s == "draft") return kQualityDraft;
    if (s == "ultra") return kQualityUltra;
    if (s == "cinema") return kQualityCinema;
    return kQualityHigh; // default
}

} // namespace ohao::inverse
