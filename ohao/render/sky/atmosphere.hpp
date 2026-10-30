#pragma once

// The physical sky (frontier F2 T3): Hillaire, EGSR 2020, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique" (Unreal's SkyAtmosphere), on the CPU. Earth's atmosphere (Rayleigh,
// Mie and ozone, Hillaire's coefficients, in RGB) lit by one sun:
//   - a transmittance table T(r, mu) (256 x 64);
//   - Hillaire's multiple scattering table Psi_ms(r, mu_s) (32 x 32): every order past the first at
//     once, as the geometric series L2 / (1 - f_ms) of an isotropic second order;
//   - the sky's radiance along each direction from the camera's altitude, ray marched with single
//     scattering from the sun plus Psi_ms, and the ground (a Lambertian albedo) where the ray meets it.
// The result is an equirectangular map with row 0 the zenith (the environment's convention), which
// the renderer uploads exactly as an HDRI: the CDF, the IBL prefilter, RT GI's and the path tracer's
// misses read it unchanged. The sun's disc is not in it: the directional light is the sun.
//
// Units: radiance for a sun of illuminance `sunIlluminance` outside the atmosphere, in the engine's
// light units (a directional light of intensity I is irradiance I).

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace ohao::render {

struct AtmosphereParams {
    float bottomRadius{6360e3f};                                   // m
    float topRadius{6460e3f};
    glm::vec3 rayleighScattering{5.802e-6f, 13.558e-6f, 33.1e-6f};   // 1/m at sea level
    float rayleighScaleHeight{8000.0f};
    float mieScattering{3.996e-6f};
    float mieExtinction{4.440e-6f};
    float mieScaleHeight{1200.0f};
    float mieG{0.8f};
    glm::vec3 ozoneAbsorption{0.650e-6f, 1.881e-6f, 0.085e-6f};   // peak, a tent 30 km wide at 25 km
    glm::vec3 groundAlbedo{0.3f};
    float cameraAltitude{200.0f};                                  // m above the ground
};

class Atmosphere {
public:
    explicit Atmosphere(const AtmosphereParams& params = {});

    // Transmittance from a point at radius r toward direction cosine mu (to the top of the
    // atmosphere; 0 when the ray meets the ground).
    [[nodiscard]] glm::vec3 transmittance(float r, float mu) const;
    // The multiple scattering term Psi_ms at radius r for a sun of zenith cosine muS (per unit
    // sun illuminance).
    [[nodiscard]] glm::vec3 multipleScattering(float r, float muS) const;
    // The sky's radiance toward `dir` (unit, y up) from the camera, for a sun toward `sunDir` of
    // illuminance `sunIlluminance` (RGB) outside the atmosphere.
    [[nodiscard]] glm::vec3 radiance(glm::vec3 dir, glm::vec3 sunDir, glm::vec3 sunIlluminance) const;

    // The sky as an equirectangular RGBA float map (w x h, row 0 the zenith: u = atan2(z, x) / 2pi
    // + 0.5, v = 0.5 - asin(y) / pi, the environment's lookup), alpha 1.
    [[nodiscard]] std::vector<float> equirect(uint32_t width, uint32_t height, glm::vec3 sunDir,
                                              glm::vec3 sunIlluminance) const;

    [[nodiscard]] const AtmosphereParams& params() const noexcept { return m_p; }

private:
    struct Medium {
        glm::vec3 scatteringRayleigh;
        float scatteringMie;
        glm::vec3 extinction;
    };
    [[nodiscard]] Medium mediumAt(float altitude) const;
    [[nodiscard]] glm::vec3 transmittanceIntegral(float r, float mu) const;   // to build the table
    void buildTransmittance();
    void buildMultipleScattering();

    AtmosphereParams m_p;
    static constexpr int kTW = 256, kTH = 64, kMS = 32;
    std::vector<glm::vec3> m_transmittance;   // kTW x kTH
    std::vector<glm::vec3> m_multiple;        // kMS x kMS
};

}  // namespace ohao::render
