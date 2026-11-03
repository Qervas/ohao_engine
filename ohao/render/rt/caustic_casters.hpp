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
};

// Whether any lobe can be under `cut` anywhere on the material (the module clamps roughness at 0.01;
// a coat widens the base's lobe, never narrows it, so the base's own roughness is a lower bound).
[[nodiscard]] bool canBeSmooth(const MaterialSmoothness& m, float cut);

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

// An orthonormal pair perpendicular to n (Duff et al. 2017), the disks' plane's axes.
void causticBasis(glm::vec3 n, glm::vec3& t, glm::vec3& b);

} // namespace ohao::render
