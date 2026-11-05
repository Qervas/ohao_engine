#pragma once

// Caustics' casters (sub-project 24, docs/specs/2026-11-03-caustics-design.md, "The light pass"):
// the light pass traces rays from each directional light only toward what can hold a smooth lobe,
// since only a ray whose first hit has a smooth part can carry a term it owns. Each caster's world
// bounding sphere projects along the light to a disk on a plane perpendicular to it; a ray's origin is
// drawn on the disks (a light by its power, a disk by its area, a point uniformly on it), and its
// density per unit area of that plane is the sum over every disk that covers it, so overlapping disks
// keep the estimate exact. Pure functions of their inputs (no Vulkan, no scene): the pass builds the
// inputs from the RenderScene, and caustic_casters_test holds these to their sampling.

#include <glm/glm.hpp>
#include <algorithm>

#include <cstdint>
#include <span>
#include <vector>

namespace ohao::render {

// What decides whether a material can hold a smooth lobe: its specular roughness and coat as
// authored, and the minimum over each roughness texture's texels (1 without a texture; the shaders
// multiply the factor by the texture's G).
struct MaterialSmoothness {
    float specularRoughness{0.3f};
    float specularTexMin{1.0f};
    float coatWeight{0.0f};
    float coatRoughness{0.0f};
    float coatTexMin{1.0f};
    bool normalMap{false};   // caustics step 2: a normal-mapped material stays with the camera side (ruling M7)
};

// Whether any lobe can be under `cut` anywhere on the material (the module clamps roughness at 0.01;
// a coat widens the base's lobe, never narrows it, so the base's own roughness is a lower bound).
[[nodiscard]] bool canBeSmooth(const MaterialSmoothness& m, float cut);
// Each lobe on its own (caustics step 2: the manifold pass draws slopes for the lobes that can be smooth).
[[nodiscard]] bool baseCanBeSmooth(const MaterialSmoothness& m, float cut);
[[nodiscard]] bool coatCanBeSmooth(const MaterialSmoothness& m, float cut);

// The smallest value of one channel of an RGBA8 image, over 255.
[[nodiscard]] float textureChannelMin(std::span<const uint8_t> rgba, int width, int height, int channel);

// A draw whose material can be smooth: its world bounds and the instance it belongs to (a caster is
// an instance; its draws' bounds are merged).
struct CausticCasterInput {
    glm::vec3 aabbMin, aabbMax;
    uint32_t slot;
};

// A directional light: the direction it travels and its irradiance (luminance of colour x intensity),
// by which the lights share the rays.
struct CausticLightInput {
    glm::vec3 direction;
    float irradiance;
    glm::vec3 rgb{1.0f};   // the irradiance itself (colour x intensity)
};

struct CausticDisk {
    glm::vec3 center;   // the caster's bounding sphere's centre: the disk lies in the plane through it
    float radius;
    uint32_t light;
};

struct CausticCasters {
    std::vector<CausticDisk> disks;     // light l's are [lightFirst[l], lightFirst[l + 1])
    std::vector<float> diskCdf;         // per light, the cumulative share of its disks' area (1 at its last)
    std::vector<uint32_t> lightFirst;   // lights + 1 entries
    std::vector<glm::vec3> lightDirection;
    std::vector<float> lightIrradiance;
    std::vector<glm::vec3> lightRgb;
    std::vector<float> lightArea;       // the sum of the light's disks' areas
    std::vector<float> lightCdf;        // cumulative p(l), by irradiance x area
    float totalPower{0.0f};             // sum of irradiance x area; 0: nothing to trace
};

[[nodiscard]] CausticCasters buildCausticCasters(std::span<const CausticCasterInput> casters,
                                                 std::span<const CausticLightInput> lights);

// An origin as the shader draws it from four uniform numbers: the light, then a disk, then a point
// uniformly on it, in the plane through the disk's centre.
struct CausticOrigin {
    uint32_t light;
    glm::vec3 point;
};
[[nodiscard]] CausticOrigin sampleCausticOrigin(const CausticCasters& c, float u0, float u1, float u2, float u3);

// The density, per unit area of light `light`'s plane, of an origin whose line along the light passes
// through x: p(light) x (the disks covering it) / the light's disk area.
[[nodiscard]] double causticOriginDensity(const CausticCasters& c, uint32_t light, glm::vec3 x);

// The light pass's random stream for one path: rt/path_tracer.slang's causticTrace seeds its PCG walk with this.
[[nodiscard]] uint32_t causticPathSeed(uint32_t index, uint32_t frame);
// What that seed hashes (one PCG step, a permutation): two frames share their paths where these sets meet.
[[nodiscard]] uint32_t causticPathSeedInput(uint32_t index, uint32_t frame);

// An orthonormal pair perpendicular to n (Duff et al. 2017), the disks' plane's axes.
void causticBasis(glm::vec3 n, glm::vec3& t, glm::vec3& b);

// ---- caustics step 2: the manifold pass's owned draws (render/rt/manifold_casters.hpp) ----

// The bits of a material's word (the draws buffer, after its draws).
constexpr uint32_t kManifoldOwned = 1u;       // the pass owns its smooth reflections' cores
constexpr uint32_t kManifoldBaseSmooth = 2u;  // its base's specular can be smooth somewhere
constexpr uint32_t kManifoldCoatSmooth = 4u;  // its coat can be smooth somewhere

// One draw as the host sees it, for choosing the owned set (pure: manifold tests drive it without a GPU).
struct ManifoldDrawInput {
    uint32_t firstIndex{0}, triangles{0};
    int32_t vertexOffset{0};
    uint32_t slot{0}, material{0};
    bool skinned{false}, emitter{false}, viewModel{false};
    uint32_t firstMeshlet{0}, meshlets{0};
    bool hasMeshlets{true};   // the gather clusters a draw by its meshlets: one without stays with the camera side
};

struct ManifoldSelection {
    std::vector<uint32_t> draws;       // indices into the input, the gathered ones
    std::vector<uint32_t> materialBits;   // a word a material (0 to the largest material index used + 1)
};

// The owned materials and the draws to gather. `smoothness(material)` the host's record of it.
template <class Smoothness>
ManifoldSelection selectManifoldDraws(std::span<const ManifoldDrawInput> draws, Smoothness&& smoothness, float cut);

template <class Smoothness>
ManifoldSelection selectManifoldDraws(std::span<const ManifoldDrawInput> draws, Smoothness&& smoothness, float cut) {
    ManifoldSelection s;
    uint32_t materials = 0;
    for (const auto& d : draws) materials = std::max(materials, d.material + 1);
    s.materialBits.assign(materials, 0u);
    std::vector<uint8_t> blocked(materials, 0);
    for (const auto& d : draws) {
        const MaterialSmoothness m = smoothness(d.material);
        uint32_t bits = 0;
        if (baseCanBeSmooth(m, cut)) bits |= kManifoldBaseSmooth;
        if (coatCanBeSmooth(m, cut)) bits |= kManifoldCoatSmooth;
        s.materialBits[d.material] = bits;
        if (bits == 0 || m.normalMap || d.skinned || d.emitter || d.viewModel || !d.hasMeshlets) blocked[d.material] = 1;
    }
    for (uint32_t k = 0; k < materials; ++k)
        if (s.materialBits[k] != 0 && !blocked[k]) s.materialBits[k] |= kManifoldOwned;
    for (uint32_t i = 0; i < draws.size(); ++i)
        if ((s.materialBits[draws[i].material] & kManifoldOwned) != 0 && draws[i].triangles > 0) s.draws.push_back(i);
    return s;
}

} // namespace ohao::render
