#include "render/scene/light_table.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "core/log.hpp"

namespace ohao::render {

std::optional<LightEmitter> lightEmitter(const LightDesc& d, const std::optional<glm::vec3>& viewer) {
    constexpr float kPi = glm::pi<float>();
    const float r = std::max(d.radius, 0.01f);   // the path tracer's emitter radius
    // A light that gives no light has no mesh (a muzzle flash between rounds): drawn, it would be a
    // black shape in the air.
    if (!(d.intensity > 0.0f)) return std::nullopt;
    LightEmitter e;
    switch (d.kind) {
    case LightKind::Point:
    case LightKind::Spot:
        if (!(d.radius > 0.0f)) return std::nullopt;
        e.world = glm::scale(glm::translate(glm::mat4(1.0f), d.position), glm::vec3(r));
        e.radiance = d.color * (d.intensity / (kPi * r * r));
        if (d.kind == LightKind::Spot && viewer && glm::length(*viewer - d.position) > 0.0f) {
            // pt_raygen*'s spotFactor, toward the viewer.
            const float cosAngle = glm::dot(glm::normalize(*viewer - d.position), glm::normalize(d.direction));
            const float cosInner = std::cos(glm::radians(d.innerConeDeg));
            const float cosOuter = std::cos(glm::radians(d.outerConeDeg));
            e.radiance *= std::clamp((cosAngle - cosOuter) / (cosInner - cosOuter + 0.001f), 0.0f, 1.0f);
        }
        return e;
    case LightKind::AreaRect: {
        const glm::vec3 n = glm::cross(d.areaEdge1, d.areaEdge2);
        const float len = glm::length(n);
        if (!(len > 0.0f)) return std::nullopt;
        e.quad = true;
        e.world = glm::mat4(glm::vec4(d.areaEdge1, 0.0f), glm::vec4(d.areaEdge2, 0.0f), glm::vec4(n / len, 0.0f),
                            glm::vec4(d.position, 1.0f));
        e.radiance = d.color * (d.intensity / (4.0f * kPi * r * r));
        return e;
    }
    default:
        return std::nullopt;
    }
}

bool LightTable::initialize(GpuAllocator& allocator) {
    m_allocator = &allocator;
    m_rtBuffer = allocator.createBuffer(kHeaderBytes + VkDeviceSize{MAX_GPU_LIGHTS} * sizeof(GPULight),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        AllocationUsage::GpuOnly);
    if (!m_rtBuffer.isValid()) {
        OHAO_LOG(Error, render, "RenderScene: light buffer allocation failed");
        return false;
    }
    m_dirty = true;   // the first frame writes the header
    return true;
}

void LightTable::shutdown() {
    if (m_allocator) m_allocator->destroyBuffer(m_rtBuffer);
    *this = LightTable{};
}

void LightTable::set(std::span<const LightDesc> lights) {
    std::vector<LightDesc> sorted(lights.begin(), lights.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const LightDesc& a, const LightDesc& b) { return a.order < b.order; });
    m_descs = sorted;

    // ---- Raster: every light, up to the table's capacity (clustered lighting) -----
    // RT shadow slots: the first kMaxShadowLights raster lights that cast shadows own one
    // visibility channel each (params.w), in light order; a casting light past them is lit
    // unshadowed (kShadowNever), and counted in shadowCastersWithoutChannel().
    std::vector<LightData> raster;
    m_shadowLights.clear();
    m_shadowCastersWithoutChannel = 0;
    for (const LightDesc& d : sorted) {
        if (d.kind == LightKind::EmissiveProxy) continue;
        if (raster.size() >= MAX_LIGHTS) break;

        LightData light{};
        // Map: Directional->0, Point->1, Spot->2, AreaRect->3
        const int deferredType = d.kind == LightKind::Point         ? 1
                               : d.kind == LightKind::Directional ? 0
                               : d.kind == LightKind::Spot        ? 2
                                                                  : 3;
        light.position = glm::vec4(d.position, static_cast<float>(deferredType));
        light.direction = glm::vec4(d.direction, d.range);
        light.color = glm::vec4(d.color, d.intensity);

        float shadowSlot = kShadowNever;
        if (d.castShadows) {
            if (m_shadowLights.size() < kMaxShadowLights) {
                shadowSlot = static_cast<float>(m_shadowLights.size());
                m_shadowLights.push_back(d);
            } else {
                ++m_shadowCastersWithoutChannel;
            }
        }
        // z: the sphere's radius for a point or spot light with an emitter mesh (lightEmitter's,
        // so its specular is the reflection of that mesh: evaluateSphere in includes/lighting/clustered_lights.slang); 0 a point.
        const float sphereRadius =
            (d.kind == LightKind::Point || d.kind == LightKind::Spot) && d.radius > 0.0f ? std::max(d.radius, 0.01f) : 0.0f;
        light.params = glm::vec4(
            glm::cos(glm::radians(d.innerConeDeg)),
            glm::cos(glm::radians(d.outerConeDeg)),
            sphereRadius,
            shadowSlot
        );
        if (d.kind == LightKind::AreaRect) {
            // The path tracer's rectangle (light type 3): centre, edges, and its radiance
            // I / (4 pi r^2) in color.w (clustered_lights.slang's evaluateRect).
            const float r = std::max(d.radius, 0.01f);
            light.position = glm::vec4(d.position + 0.5f * (d.areaEdge1 + d.areaEdge2), 3.0f);
            light.direction = glm::vec4(d.areaEdge1, d.range);
            light.color = glm::vec4(d.color, d.intensity / (4.0f * glm::pi<float>() * r * r));
            light.params = glm::vec4(d.areaEdge2, shadowSlot);
        }
        light.lightSpaceMatrix = glm::mat4(1.0f);
        raster.push_back(light);
    }
    // Emissive mesh lights after the regular ones (was m_cachedEmissiveLights).
    for (const LightDesc& d : sorted) {
        if (d.kind != LightKind::EmissiveProxy) continue;
        if (raster.size() >= MAX_LIGHTS) break;
        LightData light{};
        light.position = glm::vec4(d.position, 1.0f);  // type 1 = point light
        light.direction = glm::vec4(0, -1, 0, d.radius * 10.0f);
        light.color = glm::vec4(d.color, d.intensity);
        light.params = glm::vec4(0.0f, 0.0f, 0.0f, kShadowNever);   // inside its own mesh: never shadowed; a point
        light.lightSpaceMatrix = glm::mat4(1.0f);
        raster.push_back(light);
    }
    if (sorted.size() > MAX_LIGHTS && !m_capLogged) {
        OHAO_LOG(Warn, render, "RenderScene: {} lights: the raster table holds {}", sorted.size(), MAX_LIGHTS);
        m_capLogged = true;
    }
    if (raster.size() != m_raster.size() ||
        (!raster.empty() && std::memcmp(raster.data(), m_raster.data(), raster.size() * sizeof(LightData)) != 0))
        ++m_rasterVersion;
    m_raster = std::move(raster);

    // ---- RT (ported from VulkanRenderer::uploadLightBuffer) --------------------
    std::vector<GPULight> gpuLights;
    for (const LightDesc& d : sorted) {
        if (d.kind == LightKind::EmissiveProxy) continue;
        // LightKind Point/Directional/Spot/AreaRect = GPULight type 0/1/2/3
        GPULight gl{};
        gl.positionAndType = glm::vec4(d.position, static_cast<float>(static_cast<uint32_t>(d.kind)));
        gl.colorAndIntensity = glm::vec4(d.color, d.intensity);
        float dirParam = d.radius;
        if (d.kind == LightKind::Spot) {
            dirParam = d.innerConeDeg;
        }
        gl.dirAndParam = glm::vec4(d.direction, dirParam);

        if (d.kind == LightKind::AreaRect) {
            glm::vec3 e1 = d.areaEdge1, e2 = d.areaEdge2;
            float area = glm::length(glm::cross(e1, e2));
            gl.extra = glm::vec4(e1, 0.0f);
            gl.extra2 = glm::vec4(e2, area);
        } else {
            gl.extra = glm::vec4(0.0f, 0.0f, 0.0f, d.outerConeDeg);
            // Range and radius: the path tracer windows a point or spot light to zero at its
            // range exactly as the raster table's direction.w does, and samples a spot's
            // emitter at its radius (dirAndParam.w holds the inner cone there).
            gl.extra2 = glm::vec4(d.range, d.radius, 0.0f, 0.0f);
        }
        gpuLights.push_back(gl);
    }
    // Emissive proxies stand for emissive surfaces in the raster frame only: the path tracer
    // meets the surfaces themselves, and a proxy light beside them would count their light twice.
    if (gpuLights.size() > MAX_GPU_LIGHTS) {
        static bool logged = false;
        if (!logged) {
            OHAO_LOG(Warn, render, "RenderScene: {} RT lights: only the first {} are used",
                     gpuLights.size(), MAX_GPU_LIGHTS);
            logged = true;
        }
        gpuLights.resize(MAX_GPU_LIGHTS);
    }

    const bool same = gpuLights.size() == m_rt.size() &&
        (gpuLights.empty() ||
         std::memcmp(gpuLights.data(), m_rt.data(), gpuLights.size() * sizeof(GPULight)) == 0);
    if (!same) {
        m_rt = std::move(gpuLights);
        m_dirty = true;
    }
}

void LightTable::setEnvMapTextureIndex(uint32_t texIdx) {
    if (texIdx == m_envMapTexIdx) return;
    m_envMapTexIdx = texIdx;
    m_dirty = true;
}

void LightTable::setEmitterLights(std::vector<uint32_t> rtIndices) {
    if (rtIndices == m_emitterLights) return;
    m_emitterLights = std::move(rtIndices);
    m_dirty = true;
}

void LightTable::setAlphaRays(bool on) {
    if (on == m_alphaRays) return;
    m_alphaRays = on;
    m_dirty = true;
}

void LightTable::setEnvIntensity(float scale) {
    if (std::bit_cast<uint32_t>(scale) == std::bit_cast<uint32_t>(m_envIntensity)) return;
    m_envIntensity = scale;
    m_dirty = true;
}

VkDeviceSize LightTable::pendingBytes() const noexcept {
    return m_dirty ? kHeaderBytes + m_rt.size() * sizeof(GPULight) : 0;
}

std::size_t LightTable::recordUploads(detail::StagingBatch& batch) {
    if (!m_dirty) return 0;
    // Header: count, envMapTexIdx, env intensity scale, alpha rays (1: an instance has an alpha
    // material, so the path tracer's rays let pt_anyhit decide; 0: they force opacity).
    std::vector<std::byte> bytes(static_cast<std::size_t>(pendingBytes()));
    const uint32_t header[4] = {static_cast<uint32_t>(m_rt.size()), m_envMapTexIdx,
                                std::bit_cast<uint32_t>(m_envIntensity), m_alphaRays ? 1u : 0u};
    std::memcpy(bytes.data(), header, sizeof(header));
    if (!m_rt.empty()) {
        std::vector<GPULight> rt = m_rt;
        for (const uint32_t i : m_emitterLights) {
            if (i >= rt.size()) continue;
            if (rt[i].positionAndType.w > 2.5f) rt[i].extra.w = 1.0f;   // a rectangle
            else rt[i].extra2.z = 1.0f;                                  // a point or spot's sphere
        }
        std::memcpy(bytes.data() + kHeaderBytes, rt.data(), rt.size() * sizeof(GPULight));
    }
    batch.upload(m_rtBuffer.buffer, 0, bytes.data(), bytes.size());
    m_dirty = false;
    return bytes.size();
}

} // namespace ohao::render
