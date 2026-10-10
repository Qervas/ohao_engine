#include "render/scene/light_table.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>

namespace ohao::render {

glm::mat4 lightSpaceMatrix(const LightData& light) {
    // For directional lights, use orthographic projection
    int lightType = static_cast<int>(light.position.w);

    if (lightType == 0) {  // Directional light
        glm::vec3 lightDir = glm::normalize(glm::vec3(light.direction));

        // Calculate scene bounds - use larger frustum to encompass typical scenes
        float orthoSize = 50.0f;  // Covers -50 to 50 units (increased from 25)
        float nearPlane = 0.1f;   // Closer near plane
        float farPlane = 200.0f;  // Further far plane

        // Light view matrix: looking from "infinity" in light direction toward scene center
        glm::vec3 sceneCenter = glm::vec3(0.0f, 0.0f, 0.0f);
        glm::vec3 lightPos = sceneCenter - lightDir * 100.0f;  // Position light further away

        // Calculate up vector (avoid parallel with light direction)
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        glm::mat4 lightView = glm::lookAt(lightPos, sceneCenter, up);
        glm::mat4 lightProj = glm::ortho(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);

        // Flip Y for Vulkan coordinate system (do this BEFORE combining)
        lightProj[1][1] *= -1.0f;

        // Combine view and projection
        glm::mat4 lightSpaceMatrix = lightProj * lightView;

        // Convert Z from OpenGL NDC [-1,1] to Vulkan [0,1] range
        // Formula: z_vulkan = z_ndc * 0.5 + 0.5
        // This scales the Z output and adds a bias
        // In GLM column-major: matrix[col][row], so row 2 is the Z output
        for (int i = 0; i < 4; i++) {
            lightSpaceMatrix[i][2] *= 0.5f;  // Scale Z coefficients
        }
        lightSpaceMatrix[3][2] += 0.5f;  // Add bias (multiplied by w=1)

        return lightSpaceMatrix;
    }
    else if (lightType == 2) {  // Spot light - use perspective projection
        glm::vec3 lightPos = glm::vec3(light.position);
        glm::vec3 lightDir = glm::normalize(glm::vec3(light.direction));

        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(lightDir, up)) > 0.99f) {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        float outerCone = glm::acos(light.params.y);  // Convert from cosine to angle
        float fov = outerCone * 2.0f;
        float range = light.direction.w;

        glm::mat4 lightView = glm::lookAt(lightPos, lightPos + lightDir, up);
        float near = 0.1f;
        float far = range;
        glm::mat4 lightProj = glm::perspective(fov, 1.0f, near, far);

        // Flip Y for Vulkan coordinate system
        lightProj[1][1] *= -1.0f;

        // Combine view and projection
        glm::mat4 lightSpaceMatrix = lightProj * lightView;

        // Convert Z from OpenGL NDC [-1,1] to Vulkan [0,1] range
        for (int i = 0; i < 4; i++) {
            lightSpaceMatrix[i][2] *= 0.5f;  // Scale Z coefficients
        }
        lightSpaceMatrix[3][2] += 0.5f;  // Add bias (multiplied by w)

        return lightSpaceMatrix;
    }

    // Point lights don't use a single matrix (need cube maps)
    return glm::mat4(1.0f);
}

bool LightTable::initialize(GpuAllocator& allocator) {
    m_allocator = &allocator;
    m_rtBuffer = allocator.createBuffer(kHeaderBytes + VkDeviceSize{MAX_GPU_LIGHTS} * sizeof(GPULight),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        AllocationUsage::GpuOnly);
    if (!m_rtBuffer.isValid()) {
        std::cerr << "[RenderScene] light buffer allocation failed\n";
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

    // ---- Raster (ported from VulkanRenderer::updateLightBuffer) ----------------
    std::vector<LightData> raster;
    int shadowCasterIndex = -1;  // Index of first shadow-casting light
    for (const LightDesc& d : sorted) {
        if (d.kind == LightKind::EmissiveProxy) continue;
        if (raster.size() >= MAX_LIGHTS) break;

        LightData light{};
        // Map: Point->1, Directional->0, Spot/AreaRect->2
        const int deferredType = (d.kind == LightKind::Point) ? 1 : (d.kind == LightKind::Directional) ? 0 : 2;
        light.position = glm::vec4(d.position, static_cast<float>(deferredType));
        light.direction = glm::vec4(d.direction, d.range);
        light.color = glm::vec4(d.color, d.intensity);

        float shadowMapIndex = -1.0f;
        if (m_shadowsEnabled && shadowCasterIndex < 0) {
            // The legacy rule, kept as is: old LightType 0/2 = Sphere(point)/Spot.
            if (d.kind == LightKind::Point || d.kind == LightKind::Spot) {
                shadowMapIndex = 0.0f;
                shadowCasterIndex = static_cast<int>(raster.size());
            }
        }

        light.params = glm::vec4(
            glm::cos(glm::radians(d.innerConeDeg)),
            glm::cos(glm::radians(d.outerConeDeg)),
            shadowMapIndex,
            0.0f
        );

        if (shadowMapIndex >= 0.0f) {
            light.lightSpaceMatrix = lightSpaceMatrix(light);
        } else {
            light.lightSpaceMatrix = glm::mat4(1.0f);
        }
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
        light.params = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
        light.lightSpaceMatrix = glm::mat4(1.0f);
        raster.push_back(light);
    }
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
            gl.extra2 = glm::vec4(0.0f);
        }
        gpuLights.push_back(gl);
    }
    for (const LightDesc& d : sorted) {
        if (d.kind != LightKind::EmissiveProxy) continue;
        GPULight gl{};
        gl.positionAndType = glm::vec4(d.position, 0.0f);  // sphere type
        gl.colorAndIntensity = glm::vec4(d.color, d.intensity);
        gl.dirAndParam = glm::vec4(0, -1, 0, d.radius);
        gl.extra = glm::vec4(0);
        gl.extra2 = glm::vec4(0);
        gpuLights.push_back(gl);
    }
    if (gpuLights.size() > MAX_GPU_LIGHTS) {
        static bool logged = false;
        if (!logged) {
            std::cerr << "[RenderScene] " << gpuLights.size() << " RT lights: only the first "
                      << MAX_GPU_LIGHTS << " are used\n";
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
    // Header as uploadLightBuffer wrote it: count, envMapTexIdx, env intensity
    // scale, 0xFFFFFFFF.
    std::vector<std::byte> bytes(static_cast<std::size_t>(pendingBytes()));
    const uint32_t header[4] = {static_cast<uint32_t>(m_rt.size()), m_envMapTexIdx,
                                std::bit_cast<uint32_t>(m_envIntensity), 0xFFFFFFFFu};
    std::memcpy(bytes.data(), header, sizeof(header));
    if (!m_rt.empty())
        std::memcpy(bytes.data() + kHeaderBytes, m_rt.data(), m_rt.size() * sizeof(GPULight));
    batch.upload(m_rtBuffer.buffer, 0, bytes.data(), bytes.size());
    m_dirty = false;
    return bytes.size();
}

} // namespace ohao::render
