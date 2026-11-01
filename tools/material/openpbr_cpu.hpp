#pragma once

// shaders/material/openpbr.slang on the CPU (tools/material/openpbr_kernel.slang, compiled to C++
// by slangc): what the energy-table generator and the module's tests call. The material and the
// directions are the kernel's; see its header for the ops.

#include <array>
#include <cstdint>
#include <span>

namespace ohao::material {

enum class Op : uint32_t {
    Eval = 0, Sample = 1, Pdf = 2, Albedo = 3, Tables = 4, Fresnel = 5, Lobes = 6, Aniso = 7, Glass = 8, Film = 9, Ior = 10,
    VanDeHulst = 11, LtcDensity = 12, LtcQuad = 13
};

using Float3 = std::array<float, 3>;
using Float4 = std::array<float, 4>;

// OpenPBR's parameters (the module's MaterialInputs), OpenPBR 1.1.1's defaults.
struct Material {
    Float3 baseColor{0.8f, 0.8f, 0.8f};
    float baseWeight{1.0f};
    float baseMetalness{0.0f};
    float baseDiffuseRoughness{0.0f};
    float specularWeight{1.0f};
    Float3 specularColor{1.0f, 1.0f, 1.0f};
    float specularRoughness{0.3f};
    float specularIor{1.5f};
    float specularRoughnessAnisotropy{0.0f};
    float coatWeight{0.0f};
    Float3 coatColor{1.0f, 1.0f, 1.0f};
    float coatRoughness{0.0f};
    float coatRoughnessAnisotropy{0.0f};
    float coatIor{1.6f};
    float coatDarkening{1.0f};
    float fuzzWeight{0.0f};
    Float3 fuzzColor{1.0f, 1.0f, 1.0f};
    float fuzzRoughness{0.5f};
    float transmissionWeight{0.0f};
    Float3 transmissionColor{1.0f, 1.0f, 1.0f};
    bool thinWalled{false};
    float thinFilmWeight{0.0f};
    float thinFilmThickness{0.5f};   // micrometres
    float thinFilmIor{1.4f};
    float subsurfaceWeight{0.0f};
    Float3 subsurfaceColor{0.8f, 0.8f, 0.8f};
    bool subsurfaceWalked{false};   // lobe 5 (the path tracer's walk) instead of the local lobe
};

// One call of the kernel: an op on a material at (wo, v), v being wi or the random numbers.
struct Item {
    Op op{Op::Eval};
    Material material;
    Float3 wo{0.0f, 0.0f, 1.0f};
    Float3 v{0.0f, 0.0f, 1.0f};
    Float4 r0{};   // the result
    Float4 r1{};
};

// Runs every item through the kernel; `tables` is the energy tables (kOpenPBRTables), or empty
// for the ops that read none (Tables, Fresnel).
void run(std::span<Item> items, std::span<const float> tables);

} // namespace ohao::material
