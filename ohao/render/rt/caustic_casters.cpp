#include "render/rt/caustic_casters.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace ohao::render {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kMinRoughness = 0.01f;   // material/openpbr.slang's kMinSpecularRoughness
}

bool canBeSmooth(const MaterialSmoothness& m, float cut) {
    const float spec = std::max(m.specularRoughness * m.specularTexMin, kMinRoughness);
    if (spec < cut) return true;
    return m.coatWeight > 0.0f && std::max(m.coatRoughness * m.coatTexMin, kMinRoughness) < cut;
}

float textureChannelMin(std::span<const uint8_t> rgba, int width, int height, int channel) {
    const size_t n = static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0));
    if (n == 0 || rgba.size() < n * 4 || channel < 0 || channel > 3) return 1.0f;
    uint8_t lo = 255;
    for (size_t i = 0; i < n; ++i) lo = std::min(lo, rgba[i * 4 + static_cast<size_t>(channel)]);
    return static_cast<float>(lo) / 255.0f;
}

void causticBasis(glm::vec3 n, glm::vec3& t, glm::vec3& b) {
    const float s = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float c = n.x * n.y * a;
    t = glm::vec3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
    b = glm::vec3(c, s + n.y * n.y * a, -n.y);
}

CausticCasters buildCausticCasters(std::span<const CausticCasterInput> casters,
                                   std::span<const CausticLightInput> lights) {
    CausticCasters out;
    // One sphere per instance: its draws' bounds merged, in slot order (deterministic).
    std::map<uint32_t, std::pair<glm::vec3, glm::vec3>> bySlot;
    for (const CausticCasterInput& c : casters) {
        auto [it, fresh] = bySlot.try_emplace(c.slot, c.aabbMin, c.aabbMax);
        if (!fresh) {
            it->second.first = glm::min(it->second.first, c.aabbMin);
            it->second.second = glm::max(it->second.second, c.aabbMax);
        }
    }
    out.lightFirst.push_back(0u);
    double total = 0.0;
    std::vector<double> lightPower;
    for (uint32_t l = 0; l < lights.size(); ++l) {
        const CausticLightInput& li = lights[l];
        const float len = glm::length(li.direction);
        double area = 0.0;
        if (li.irradiance > 0.0f && len > 0.0f) {
            for (const auto& [slot, box] : bySlot) {
                const glm::vec3 center = 0.5f * (box.first + box.second);
                const float radius = 0.5f * glm::length(box.second - box.first);
                if (!(radius > 0.0f)) continue;
                out.disks.push_back({center, radius, l});
                area += double(kPi) * radius * radius;
                out.diskCdf.push_back(static_cast<float>(area));
            }
        }
        const uint32_t first = out.lightFirst.back();
        for (size_t d = first; d < out.disks.size(); ++d) out.diskCdf[d] = area > 0.0 ? static_cast<float>(out.diskCdf[d] / area) : 0.0f;
        if (out.disks.size() > first) out.diskCdf.back() = 1.0f;
        out.lightFirst.push_back(static_cast<uint32_t>(out.disks.size()));
        out.lightDirection.push_back(len > 0.0f ? li.direction / len : glm::vec3(0.0f, -1.0f, 0.0f));
        out.lightIrradiance.push_back(li.irradiance);
        out.lightRgb.push_back(li.rgb);
        out.lightArea.push_back(static_cast<float>(area));
        lightPower.push_back(double(std::max(li.irradiance, 0.0f)) * area);
        total += lightPower.back();
    }
    double run = 0.0;
    for (double p : lightPower) {
        run += p;
        out.lightCdf.push_back(total > 0.0 ? static_cast<float>(run / total) : 0.0f);
    }
    if (total > 0.0) {
        // The last light with power closes the CDF exactly.
        for (size_t l = out.lightCdf.size(); l-- > 0;)
            if (lightPower[l] > 0.0) {
                for (size_t k = l; k < out.lightCdf.size(); ++k) out.lightCdf[k] = 1.0f;
                break;
            }
    }
    out.totalPower = static_cast<float>(total);
    return out;
}

CausticOrigin sampleCausticOrigin(const CausticCasters& c, float u0, float u1, float u2, float u3) {
    uint32_t l = 0;
    while (l + 1 < c.lightCdf.size() && u0 >= c.lightCdf[l]) ++l;
    const uint32_t first = c.lightFirst[l], last = c.lightFirst[l + 1];
    uint32_t d = first;
    while (d + 1 < last && u1 >= c.diskCdf[d]) ++d;
    const CausticDisk& disk = c.disks[d];
    glm::vec3 t, b;
    causticBasis(c.lightDirection[l], t, b);
    const float r = disk.radius * std::sqrt(u2);
    const float phi = 2.0f * kPi * u3;
    return {l, disk.center + t * (r * std::cos(phi)) + b * (r * std::sin(phi))};
}

double causticOriginDensity(const CausticCasters& c, uint32_t light, glm::vec3 x) {
    if (light >= c.lightArea.size() || !(c.lightArea[light] > 0.0f)) return 0.0;
    const glm::vec3 dir = c.lightDirection[light];
    int covering = 0;
    for (uint32_t d = c.lightFirst[light]; d < c.lightFirst[light + 1]; ++d) {
        const glm::vec3 v = x - c.disks[d].center;
        const glm::vec3 perp = v - dir * glm::dot(v, dir);
        if (glm::dot(perp, perp) < c.disks[d].radius * c.disks[d].radius) ++covering;
    }
    const double pLight = c.lightCdf[light] - (light > 0 ? c.lightCdf[light - 1] : 0.0f);
    return pLight * covering / double(c.lightArea[light]);
}

} // namespace ohao::render
