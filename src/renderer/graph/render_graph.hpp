#pragma once

#include "resource_handle.hpp"
#include "render_pass.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace ohao {

class GpuAllocator;

/**
 * Physical resource backing for a texture handle
 */
struct PhysicalTexture {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    uint32_t width{0};
    uint32_t height{0};
    VkImageLayout currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    bool ownsMemory{true}; // False for external/aliased resources
};

/**
 * Physical resource backing for a buffer handle
 */
struct PhysicalBuffer {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkDeviceSize size{0};
    void* mapped{nullptr};
    bool ownsMemory{true};
};

/**
 * Barrier to insert between passes
 */
struct ResourceBarrier {
    TextureHandle texture;
    BufferHandle buffer;
    VkPipelineStageFlags srcStage{0};
    VkPipelineStageFlags dstStage{0};
    VkAccessFlags srcAccess{0};
    VkAccessFlags dstAccess{0};
    VkImageLayout oldLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkImageLayout newLayout{VK_IMAGE_LAYOUT_UNDEFINED};
};

/**
 * Compiled pass ready for execution
 */
struct CompiledPass {
    uint32_t passIndex{0};
    std::vector<ResourceBarrier> barriers;
};

/**
 * Render Graph - Frame Graph style rendering abstraction
 *
 * The render graph allows declarative definition of render passes and their
 * resource dependencies. The graph is compiled each frame to:
 * - Deduce resource lifetimes
 * - Alias transient resource memory
 * - Generate optimal barriers
 * - Sort passes in correct execution order
 *
 * Usage:
 *   RenderGraph graph(device, allocator);
 *
 *   // Setup phase - declare passes and resources
 *   graph.addPass("ShadowPass", [](PassBuilder& builder) {
 *       auto shadowMap = builder.createDepthAttachment("ShadowMap", 2048, 2048);
 *   }, [](VkCommandBuffer cmd) {
 *       // Render shadow casters
 *   });
 *
 *   graph.addPass("MainPass", [](PassBuilder& builder) {
 *       builder.sampleTexture(shadowMap);
 *       auto color = builder.createColorAttachment("Color", 1920, 1080);
 *   }, [](VkCommandBuffer cmd) {
 *       // Render scene
 *   });
 *
 *   // Compile and execute
 *   graph.compile();
 *   graph.execute(commandBuffer);
 *   graph.reset(); // Prepare for next frame
 */
class RenderGraph {
public:
    RenderGraph() = default;
    ~RenderGraph();

    // Non-copyable
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    /**
     * Initialize the render graph
     *
     * @param device Vulkan device
     * @param physicalDevice Physical device for memory queries
     * @param allocator Optional GPU allocator (uses VMA if provided)
     */
    bool initialize(VkDevice device, VkPhysicalDevice physicalDevice,
                    GpuAllocator* allocator = nullptr);

    /**
     * Shutdown and cleanup all resources
     */
    void shutdown();

    /**
     * Add a render pass to the graph
     *
     * @param name Pass name for debugging
     * @param setup Setup function to declare resources
     * @param execute Execute function to record commands
     */
    void addPass(const std::string& name,
                 std::function<void(PassBuilder&)> setup,
                 std::function<void(VkCommandBuffer)> execute);

    /**
     * Add a compute pass to the graph
     */
    void addComputePass(const std::string& name,
                        std::function<void(PassBuilder&)> setup,
                        std::function<void(VkCommandBuffer)> execute);

    /**
     * Import an external texture (e.g., swapchain image)
     *
     * @param name Resource name
     * @param image Existing Vulkan image
     * @param view Existing image view
     * @param format Image format
     * @param width Image width
     * @param height Image height
     * @param currentLayout Current image layout
     * @return Handle to the imported texture
     */
    TextureHandle importTexture(const std::string& name, VkImage image, VkImageView view,
                                 VkFormat format, uint32_t width, uint32_t height,
                                 VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED);

    /**
     * Import an external buffer
     */
    BufferHandle importBuffer(const std::string& name, VkBuffer buffer,
                               VkDeviceSize size, void* mapped = nullptr);

    /**
     * Set the final output texture (for presentation or readback)
     */
    void setOutput(TextureHandle handle);

    /**
     * Compile the graph for execution
     * - Validates dependencies
     * - Computes execution order
     * - Allocates resources
     * - Generates barriers
     */
    bool compile();

    /**
     * Execute all passes in order
     *
     * @param cmd Command buffer to record into
     */
    void execute(VkCommandBuffer cmd);

    /**
     * Reset the graph for next frame
     * Clears pass list but keeps allocated resources
     */
    void reset();

    /**
     * Get the physical texture for a handle (valid after compile)
     */
    PhysicalTexture* getPhysicalTexture(TextureHandle handle);

    /**
     * Get the physical buffer for a handle (valid after compile)
     */
    PhysicalBuffer* getPhysicalBuffer(BufferHandle handle);

    /**
     * Check if initialized
     */
    bool isInitialized() const { return m_device != VK_NULL_HANDLE; }

private:
    friend class PassBuilder;

    // Internal resource creation (called by PassBuilder)
    TextureHandle createTexture(const TextureDesc& desc);
    BufferHandle createBuffer(const BufferDesc& desc);

    // Add resource access to a pass
    void addPassRead(uint32_t passIndex, const ResourceAccess& access);
    void addPassWrite(uint32_t passIndex, const ResourceAccess& access);

    // Compilation steps
    void buildDependencyGraph();
    void topologicalSort();
    void allocateResources();
    void computeBarriers();
    void createRenderPasses();
    void createFramebuffers();

    // Resource allocation helpers
    bool allocateTexture(TextureHandle handle);
    bool allocateBuffer(BufferHandle handle);
    void freeTexture(TextureHandle handle);
    void freeBuffer(BufferHandle handle);

    // Helper to find memory type
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    // Convert usage flags to Vulkan equivalents
    VkImageUsageFlags toVkImageUsage(TextureUsage usage);
    VkBufferUsageFlags toVkBufferUsage(BufferUsage usage);
    VkImageLayout getOptimalLayout(TextureUsage usage);

    VkDevice m_device{VK_NULL_HANDLE};
    VkPhysicalDevice m_physicalDevice{VK_NULL_HANDLE};
    GpuAllocator* m_allocator{nullptr};

    // Resource descriptions (indexed by handle)
    std::vector<TextureDesc> m_textureDescs;
    std::vector<BufferDesc> m_bufferDescs;

    // Physical resources (indexed by handle)
    std::vector<PhysicalTexture> m_physicalTextures;
    std::vector<PhysicalBuffer> m_physicalBuffers;

    // Pass definitions
    std::vector<RenderPassDef> m_passes;

    // Compiled execution order
    std::vector<CompiledPass> m_compiledPasses;

    // Output handle
    TextureHandle m_outputHandle{TextureHandle::invalid()};

    // Compilation state
    bool m_compiled{false};

    // Name to handle mapping for debugging
    std::unordered_map<std::string, TextureHandle> m_textureNameMap;
    std::unordered_map<std::string, BufferHandle> m_bufferNameMap;
};

} // namespace ohao
