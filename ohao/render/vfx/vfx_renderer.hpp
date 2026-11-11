#pragma once
// The effects and transparent surfaces of a deferred frame (sub-project 12, "The frame:
// expansion, sorting, the forward pass"):
//   prepare      host: the RenderScene's effect records (VfxFrame) to batches (an emitter of a
//                record, its particles a range of slots), the blended draws (kDrawTransparent), and
//                every transparent object in back-to-front order (view distance of its centre,
//                ties by its id: integer order, the same every run)
//   expand       vfx.expand: each particle slot in closed form, lit at its centre
//   sort         vfx.sort: the particles of each alpha-blended batch (<= kVfxSortMax) back to front
//   transparent  one pass over the lit HDR image (depth read only, tested, not written):
//                the objects in order, each with its own blend state; coverage into an R8
//                transparency mask (MAX) that TAA lowers its history weight under
// Particles are drawn with the jittered projection the G-buffer used, soft against its depth;
// blended meshes reuse core/gbuffer.slang's vertexMain and are shaded per pixel by vfx/transparent_mesh.slang
// (the forward copy of the deferred light loop). Nothing here reads a clock, a frame counter or the
// frame-in-flight slot's history: the same records, camera and alpha give the same image.

#include "render/material/openpbr_tables_buffer.hpp"
#include "gpu/vulkan/gpu_allocator.hpp"
#include "render/graph/pass.hpp"
#include "render/frame/frame_resources.hpp"
#include "gpu/shared_layout.hpp"

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ohao::render { class RenderScene; }

namespace ohao {

class GBufferPass;

struct VfxFrameStats {
    uint32_t particles{0};      // particle slots expanded
    uint32_t batches{0};
    uint32_t sortedBatches{0};  // alpha batches vfx.sort ordered
    uint32_t particlesSortedMax{0};   // the most particles in one of those batches (<= kVfxSortMax)
    uint32_t meshes{0};         // blended draws
    uint32_t objects{0};        // objects the transparent pass drew, in its order
    uint32_t dropped{0};        // particles past the set's cap (caps.particles), not drawn
    uint32_t tracers{0};        // tracers drawn (six vertices each, host-built)
};

class VfxRenderer {
public:
    VfxRenderer() = default;
    ~VfxRenderer();
    VfxRenderer(const VfxRenderer&) = delete;
    VfxRenderer& operator=(const VfxRenderer&) = delete;

    // `textures`: the bindless set's layout; `gbufferScene`: GBufferPass's scene set layout (set 1 of
    // core/gbuffer.slang's vertexMain). False when a pipeline cannot be made (the frame then draws no effects).
    [[nodiscard]] bool initialize(VkDevice device, VkPhysicalDevice physicalDevice, GpuAllocator& allocator,
                                  VkDescriptorSetLayout textures, VkDescriptorSetLayout gbufferScene);
    void cleanup();
    // The frame's size and the images the pass draws into: the lit HDR image (RGBA16F), a copy of which glass
    // refracts, and the G-buffer's depth (D32). Rebuilds the mask and the refraction copy.
    [[nodiscard]] bool resize(uint32_t width, uint32_t height, gpu::Image& hdr, gpu::Image& depth);

    struct View {
        glm::mat4 view{1.0f};
        glm::mat4 proj{1.0f};          // the jittered projection the G-buffer was drawn with
        glm::vec3 cameraPos{0.0f};
        float nearPlane{0.1f};
    };
    // Host, the slot's fence waited: the scene's records and blended draws this frame, in order.
    void prepare(uint32_t slot, const render::RenderScene& scene, const View& view);
    [[nodiscard]] bool hasParticles() const noexcept { return m_stats.particles > 0; }
    [[nodiscard]] bool hasObjects() const noexcept { return !m_objects.empty(); }

    struct Lighting {
        GpuBuffer* lights{nullptr};            // LightGpu table (this slot's)
        uint32_t lightCount{0};
        GpuBuffer* clusters{nullptr};          // null: every light (no clusters)
        float sliceScale{0.0f}, sliceBias{0.0f};
        VkAccelerationStructureKHR tlas{VK_NULL_HANDLE};   // the scene's, for the sun's shadow ray (null: unshadowed)
        gpu::Image* giRadiance{nullptr};                    // RT GI's output (null: none)
        VkImageView envIrradiance{VK_NULL_HANDLE};          // null: no environment
        VkImageView envPrefiltered{VK_NULL_HANDLE};
        float envIntensity{1.0f};
    };
    // vfx.expand (after lighting) and vfx.sort; each records nothing without particles. Untimed, as
    // drawTransparent: the frame times them.
    void expand(const render::Frame& frame, const render::RenderScene& scene, const Lighting& lighting);
    void sort(const render::Frame& frame);
    // The transparent pass (after SSR, before bloom). `textures`: the bindless set; `gbuffer`: the pass whose
    // scene set the blended meshes draw with. Records nothing without objects.
    void drawTransparent(const render::Frame& frame, const render::RenderScene& scene, const Lighting& lighting,
                         VkDescriptorSet textures, GBufferPass& gbuffer);

    // The R8 coverage mask of this frame's pass, null when it drew nothing (TAA then reads no mask).
    [[nodiscard]] gpu::Image* transparencyMask() noexcept { return hasObjects() ? &m_mask : nullptr; }
    [[nodiscard]] bool ready() const noexcept { return m_tracerPipeline != VK_NULL_HANDLE; }
    [[nodiscard]] const VfxFrameStats& stats() const noexcept { return m_stats; }
    // Tests: the particle slots as the last expand wrote them (blocking: the device idles).
    [[nodiscard]] std::vector<gpu::shared::VfxParticleGpu> readParticles();
    // Tests: the transparent objects of the last prepare, in draw order (batch: 0, mesh: 1, tracer: 2; its id).
    struct Object { uint32_t kind; uint64_t id; float distance; uint32_t index; };
    [[nodiscard]] const std::vector<Object>& objects() const noexcept { return m_objects; }

    // render.settings vfx (presentation switches, all on by default): the lit particles, their soft
    // fade and the tracers. Off, prepare builds none of them; the rest of the frame is the same.
    void setParticlesEnabled(bool on) noexcept { m_particlesEnabled = on; }
    void setSoftEnabled(bool on) noexcept { m_softEnabled = on; }
    void setTracersEnabled(bool on) noexcept { m_tracersEnabled = on; }
    [[nodiscard]] bool particlesEnabled() const noexcept { return m_particlesEnabled; }
    [[nodiscard]] bool softEnabled() const noexcept { return m_softEnabled; }
    [[nodiscard]] bool tracersEnabled() const noexcept { return m_tracersEnabled; }

private:
    struct Batch {
        uint32_t emitter, seed, first, count;
        glm::mat4 pose;
        float age;
        uint32_t blend;
        bool sorted;
    };
    struct Slot {
        GpuBuffer emitters, batches, tracerVerts;
        VkDeviceSize emitterBytes{0}, batchBytes{0}, tracerBytes{0};
        VkDescriptorSet expandSet{VK_NULL_HANDLE}, sortSet{VK_NULL_HANDLE}, drawSet{VK_NULL_HANDLE}, forwardSet{VK_NULL_HANDLE};
    };

    [[nodiscard]] bool createLayouts(VkDescriptorSetLayout textures, VkDescriptorSetLayout gbufferScene);
    [[nodiscard]] bool createPipelines();
    [[nodiscard]] bool ensureHostBuffer(GpuBuffer& buf, VkDeviceSize& cap, VkDeviceSize bytes,
                                        VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    void writeExpandSet(uint32_t slot, const Lighting& lighting);
    // `refracted`: this frame's copy of the lit image is bound (else the zero stand-in).
    void writeForwardSet(uint32_t slot, const Lighting& lighting, const render::RenderScene& scene, bool refracted);
    // The lit frame as it is before the transparent pass, into the refraction copy. False: nothing copied.
    bool copyRefraction(const render::Frame& frame);
    // The stand-in's one texel, zeroed once.
    void zeroDummy(const render::Frame& frame);
    void destroySizeResources();

    VkDevice m_device{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};
    uint32_t m_width{0}, m_height{0};
    gpu::Image* m_hdr{nullptr};     // the lit image
    gpu::Image* m_depth{nullptr};   // the G-buffer's depth
    // OpenPBR step 3: the lit frame before the transparent pass (RGBA16F, the frame's size), copied
    // when a blended mesh draws: what glass refracts (forward set binding 8).
    gpu::Image m_refraction;

    bool m_particlesEnabled{true}, m_softEnabled{true}, m_tracersEnabled{true};   // render.settings vfx
    // kVfx caps: one set of slots, fenced by queue order. Host-visible: readParticles reads them.
    GpuBuffer m_particles, m_order;
    uint32_t m_particleCapacity{0};
    gpu::Image m_mask;                      // R8_UNORM, the frame's size, cleared by each pass that draws
    gpu::Image m_dummy;                     // 1x1 RGBA16F zero: missing GI / environment views
    GpuBuffer m_dummyBuffer;                // missing lights / clusters
    render::OpenPBRTablesBuffer m_openpbrTables;   // the transparent pass's set 2, binding 7
    VkSampler m_sampler{VK_NULL_HANDLE};

    VkDescriptorSetLayout m_expandLayout{VK_NULL_HANDLE}, m_sortLayout{VK_NULL_HANDLE}, m_drawLayout{VK_NULL_HANDLE},
        m_forwardLayout{VK_NULL_HANDLE};
    VkDescriptorPool m_pool{VK_NULL_HANDLE};
    VkPipelineLayout m_expandPipelineLayout{VK_NULL_HANDLE}, m_sortPipelineLayout{VK_NULL_HANDLE},
        m_drawPipelineLayout{VK_NULL_HANDLE}, m_meshPipelineLayout{VK_NULL_HANDLE}, m_tracerPipelineLayout{VK_NULL_HANDLE};
    VkPipeline m_expandPipeline{VK_NULL_HANDLE}, m_sortPipeline{VK_NULL_HANDLE}, m_tracerPipeline{VK_NULL_HANDLE};
    std::array<VkPipeline, 3> m_particlePipelines{};   // by kVfxBlend*
    std::array<VkPipeline, 2> m_meshPipelines{};       // [0] one-sided (back faces culled), [1] double-sided
    std::array<Slot, MAX_FRAMES_IN_FLIGHT> m_slots{};

    // prepare's output
    std::vector<gpu::shared::VfxEmitterGpu> m_emitterTable;
    std::vector<uint32_t> m_emitterBase;   // the set's effect -> its first emitter in the table
    std::vector<Batch> m_batches;
    std::vector<Object> m_objects;
    std::vector<uint32_t> m_meshDraws;      // the blended draws (draw-table indices), by object index
    std::vector<gpu::shared::TracerVertexGpu> m_tracerVerts;   // six a tracer, by object index (kind 2)
    View m_view;
    VfxFrameStats m_stats;
};

} // namespace ohao
