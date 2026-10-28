#pragma once

// The environment's sun: found in an equirectangular HDRI and painted out, so that a scene's
// directional light, which stands for it, is the only sun that lights the scene.
//
// Why: an outdoor HDRI's sun is a few texels far brighter than the sky (env_outdoor.hdr: 7 texels
// over 1,000, peaking at 60,450 against a median sky of 0.215, carrying 49% of the map's radiant
// power over 0.003% of the sphere). RT GI's cosine rays find it about once in 10^4 rays and bring
// back tens of thousands: fireflies its 2 rays a pixel and its denoiser cannot average away.
// When the scene also has a directional light, that light already is the sun (with RT shadows),
// so the environment's sun is also counted twice. Painting it out of the lighting environment
// fixes both: the sun arrives once, from the directional light, and GI's rays see only sky.
//
// What counts as the sun, relative to the sky's median luminance m (a texel median, which the sun
// cannot move):
//   - the map has one only if its brightest texel is at least kSunPeakOverMedian x m (a studio's
//     softboxes, a few hundred times their sky, are lights the environment keeps);
//   - it is the brightest texel and every texel 8-connected to it (across the u seam too) over
//     kSunTexelOverMedian x m: GI's estimate averages about 64 samples (2 rays over the 32 frames
//     it accumulates), so one sample over 64 m adds more than the sky's own radiance to it;
//   - it covers at most kSunMaxSolidAngle steradians (a sun with its glare, not a bright sky).
// Its texels are set to the mean of the texels bordering it: the sky around the sun.
//
// Directions use the engine's lookup (gi_trace.slang, deferred_lighting.frag, pt_miss.rmiss):
// u = atan2(z, x) / 2pi + 0.5, v = asin(y) / pi + 0.5, row 0 at v = 0.

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace ohao::render {

inline constexpr float kSunPeakOverMedian = 1.0e4f;
inline constexpr float kSunTexelOverMedian = 64.0f;
inline constexpr float kSunMaxSolidAngle = 0.02f;   // sr: a cone of about 4.6 degrees half-angle

struct EnvSun {
    bool found{false};
    uint32_t texels{0};
    float solidAngle{0.0f};          // sr, the painted-out texels'
    glm::vec3 direction{0, 1, 0};    // toward the sun: the removed radiance's weighted mean direction
    glm::vec3 irradiance{0.0f};      // the removed radiance over its solid angle (sum of (L - fill) dOmega)
    float powerShare{0.0f};          // of the map's luminance over the sphere
    float peak{0.0f};                // the brightest texel's luminance
    float median{0.0f};              // the sky's median luminance
};

// Finds the sun in `rgba` (width x height RGBA floats, row-major, row 0 at v = 0) and paints it
// out in place. Without one (see above) the map is untouched and `found` is false.
EnvSun separateEnvSun(std::span<float> rgba, int width, int height);

}  // namespace ohao::render
