#pragma once

#include "render_pass_base.hpp"
#include <glm/glm.hpp>
#include <vector>

namespace ohao {

// RipplePass — GPU ping-pong wave equation simulation (step 4.63 in render order).
//
// Maintains two R16_SFLOAT 256×256 images that simulate expanding ring waves.
// Physics objects hitting the water surface inject impulses via addRipple().
// The output height map (getRippleMapView()) is consumed by WaterPass to
// perturb the water surface normals with circular ripple displacement.
//
// Implementation: discrete wave equation with damping and up to 4 impulse
// sources per frame.  Ping-pong: A is src→dst one frame, B the next.
//
// Usage:
//   ripple.setTerrainSize(1024.0f);
//   ripple.setEnabled(true);
//   ripple.addRipple({5.0f, 3.0f}, 0.8f, 4.0f);  // from physics contact
//   ripple.setDeltaTime(dt);
//   ripple.execute(cmd, frameIndex);
//   waterPass.setRippleMap(ripple.getRippleMapView());
class RipplePass : public RenderPassBase {
public:
    static constexpr uint32_t MAP_SIZE    = 256;
    static constexpr uint32_t MAX_SOURCES = 12;

    struct RippleSource {
        glm::vec2 pos;        // world XZ position
        float     strength;   // displacement amplitude [0, 1]
        float     radius;     // metres
    };

    RipplePass()  = default;
    ~RipplePass() override;

    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice) override;
    void cleanup() override;
    void execute(VkCommandBuffer cmd, uint32_t frameIndex) override;
    void onResize(uint32_t, uint32_t) override {}  // not screen-resolution dependent
    const char* getName() const override { return "RipplePass"; }
    bool reloadShader(const std::string& spvPath) override;

    // Current ripple height map view (consumed by WaterPass)
    VkImageView getRippleMapView() const;

    // --- Parameters ---
    void setEnabled(bool v)       { m_enabled     = v; }
    bool getEnabled() const       { return m_enabled; }
    void setTerrainSize(float v)  { m_terrainSize = glm::max(v, 1.0f); }
    void setDeltaTime(float dt)   { m_dt          = dt; }
    void setDamping(float v)      { m_damping     = glm::clamp(v, 0.0f, 0.1f); }
    void setWaveSpeed(float v)    { m_waveSpeed   = glm::clamp(v, 0.5f, 20.0f); }

    // Add a ripple impulse (up to MAX_SOURCES per frame; excess are silently dropped)
    void addRipple(glm::vec2 worldPosXZ, float strength, float radius = 4.0f);
    void clearRipples();

private:
    // Push constants — 224 bytes, matching water_ripple.comp (MAX_SOURCES=12)
    struct RipplePC {
        float terrainSize;       //  4
        float damping;           //  4
        float waveSpeed;         //  4
        float dt;                //  4 — 16
        uint32_t rippleCount;    //  4
        uint32_t mapSize;        //  4
        float    pad0;           //  4
        float    pad1;           //  4 — 32
        // RippleSource[12] = 12 * (vec2 + float + float) = 12 * 16 = 192
        float sourceData[48];    // 192 — 224 total
    };
    static_assert(sizeof(RipplePC) == 224, "RipplePC must be 224 bytes");

    bool createImages();
    bool createDescriptors();
    bool createPipeline();
    void updateDescriptors();

    // ---- Vulkan resources (owned) ----
    VkPipeline            m_pipeline{VK_NULL_HANDLE};
    VkPipelineLayout      m_pipelineLayout{VK_NULL_HANDLE};
    VkDescriptorSetLayout m_descLayout{VK_NULL_HANDLE};
    VkDescriptorPool      m_descPool{VK_NULL_HANDLE};
    VkDescriptorSet       m_descSetAB{VK_NULL_HANDLE};  // A=src, B=dst
    VkDescriptorSet       m_descSetBA{VK_NULL_HANDLE};  // B=src, A=dst

    // Ping-pong images (R16_SFLOAT 256×256)
    VkImage        m_imageA{VK_NULL_HANDLE};
    VkImage        m_imageB{VK_NULL_HANDLE};
    VkDeviceMemory m_memA{VK_NULL_HANDLE};
    VkDeviceMemory m_memB{VK_NULL_HANDLE};
    VkImageView    m_viewA{VK_NULL_HANDLE};
    VkImageView    m_viewB{VK_NULL_HANDLE};

    // ---- State ----
    bool     m_pingPong{false};   // false: A=src/output, true: B=src/output
    bool     m_enabled{false};
    float    m_terrainSize{1024.0f};
    float    m_dt{0.016f};
    float    m_damping{0.005f};
    float    m_waveSpeed{8.0f};
    bool     m_imagesInitialized{false};
    bool     m_descDirty{true};

    std::vector<RippleSource> m_pendingSources;  // injected this frame, max MAX_SOURCES
};

} // namespace ohao
