#pragma once

// shaders/sampling/vmf.slang on the CPU (tools/sampling/vmf_kernel.slang, compiled to C++ by
// slangc): what tests/renderer/vmf_test calls to check the path guiding's lobes and states.

#include <array>
#include <cstdint>

namespace ohao::sampling {

using Vec3 = std::array<float, 3>;

struct GuideStateCpu {
    float w = 0.0f;
    Vec3 y{0.0f, 0.0f, 0.0f};
    float r = 0.0f;
    uint32_t nt = 0;
    uint32_t check = 0;
};

float vmfPdf(const Vec3& dir, const Vec3& mu, float kappa);
Vec3 vmfSample(const Vec3& mu, float kappa, float u0, float u1);
float vmfKappa(float meanCosine);
float vmfMeanCosine(float kappa);
GuideStateCpu guideUpdate(const GuideStateCpu& s, const Vec3& x, const Vec3& target, float f, uint32_t frame);
// The lobe seen from x; false when the state is empty.
bool guideLobe(const GuideStateCpu& s, const Vec3& x, Vec3& mu, float& kappa);

} // namespace ohao::sampling
