/**
 * renderer_pipeline_tests.cpp - Comprehensive Tests for AAA Renderer Pipeline
 *
 * Tests all Phase 1-5 features:
 * - Phase 1: Core Deferred Pipeline (G-Buffer, Deferred Lighting)
 * - Phase 2: Integration (CSM Shadows, Light Culling)
 * - Phase 3: Advanced Effects (SSAO, SSR, Volumetrics, Motion Blur, DoF, Bloom, TAA)
 * - Phase 4: Performance (Render Graph, VMA, GPU-Driven, Async Compute)
 * - Phase 5: Material System (Bindless Textures, Material Instances, Advanced BRDF)
 */

#include <iostream>
#include <vector>
#include <memory>
#include <cstring>

// Vulkan
#include <vulkan/vulkan.h>

// Renderer includes
#include "renderer/passes/gbuffer_pass.hpp"
#include "renderer/passes/deferred_lighting_pass.hpp"
#include "renderer/passes/csm_pass.hpp"
#include "renderer/passes/bloom_pass.hpp"
#include "renderer/passes/taa_pass.hpp"
#include "renderer/passes/ssao_pass.hpp"
#include "renderer/passes/ssr_pass.hpp"
#include "renderer/passes/volumetric_pass.hpp"
#include "renderer/passes/motion_blur_pass.hpp"
#include "renderer/passes/dof_pass.hpp"
#include "renderer/passes/post_processing_pipeline.hpp"
#include "renderer/passes/indirect_draw_buffer.hpp"
#include "renderer/graph/render_graph.hpp"
#include "renderer/memory/gpu_allocator.hpp"
#include "renderer/material/material_instance.hpp"
#include "renderer/material/bindless_texture_manager.hpp"
#include "renderer/async/async_compute_queue.hpp"

namespace ohao::tests {

// =============================================================================
// TEST FRAMEWORK
// =============================================================================

static int testsRun = 0;
static int testsPassed = 0;
static int testsFailed = 0;
static int testsSkipped = 0;

#define TEST_BEGIN(name) \
    do { \
        testsRun++; \
        std::cout << "  TEST: " << name << "... " << std::flush; \
    } while(0)

#define TEST_PASS() \
    do { \
        testsPassed++; \
        std::cout << "\033[32mPASS\033[0m" << std::endl; \
    } while(0)

#define TEST_FAIL(msg) \
    do { \
        testsFailed++; \
        std::cout << "\033[31mFAIL: " << msg << "\033[0m" << std::endl; \
    } while(0)

#define TEST_SKIP(msg) \
    do { \
        testsSkipped++; \
        std::cout << "\033[33mSKIP: " << msg << "\033[0m" << std::endl; \
    } while(0)

#define EXPECT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            TEST_FAIL(#expr " was false"); \
            return false; \
        } \
    } while(0)

#define EXPECT_FALSE(expr) EXPECT_TRUE(!(expr))

#define EXPECT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            TEST_FAIL(#a " != " #b); \
            return false; \
        } \
    } while(0)

#define EXPECT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            TEST_FAIL(#a " == " #b); \
            return false; \
        } \
    } while(0)

#define EXPECT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == nullptr || (ptr) == VK_NULL_HANDLE) { \
            TEST_FAIL(#ptr " is null"); \
            return false; \
        } \
    } while(0)

// =============================================================================
// VULKAN CONTEXT FOR TESTING
// =============================================================================

class TestVulkanContext {
public:
    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    VkQueue computeQueue{VK_NULL_HANDLE};
    uint32_t graphicsQueueFamily{0};
    uint32_t computeQueueFamily{0};
    bool initialized{false};

    bool initialize() {
        // Create instance
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "OHAO Renderer Tests";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "OHAO Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        std::vector<const char*> extensions;
#ifdef __APPLE__
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
#ifdef __APPLE__
        createInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan instance" << std::endl;
            return false;
        }

        // Get physical device
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) {
            std::cerr << "No Vulkan devices found" << std::endl;
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
        physicalDevice = devices[0];

        // Get queue families
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphicsQueueFamily = i;
            }
            if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                computeQueueFamily = i;
            }
        }

        // Create logical device
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        float queuePriority = 1.0f;

        VkDeviceQueueCreateInfo graphicsQueueInfo{};
        graphicsQueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        graphicsQueueInfo.queueFamilyIndex = graphicsQueueFamily;
        graphicsQueueInfo.queueCount = 1;
        graphicsQueueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(graphicsQueueInfo);

        // Device extensions
        std::vector<const char*> deviceExtensions;
#ifdef __APPLE__
        deviceExtensions.push_back("VK_KHR_portability_subset");
#endif
        deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

        // Enable features
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
        timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
        timelineFeatures.timelineSemaphore = VK_TRUE;

        VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures{};
        indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        indexingFeatures.pNext = &timelineFeatures;
        indexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;
        indexingFeatures.runtimeDescriptorArray = VK_TRUE;
        indexingFeatures.descriptorBindingVariableDescriptorCount = VK_TRUE;
        indexingFeatures.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;

        VkPhysicalDeviceFeatures2 deviceFeatures2{};
        deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        deviceFeatures2.pNext = &indexingFeatures;

        VkDeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.pNext = &deviceFeatures2;
        deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS) {
            std::cerr << "Failed to create logical device" << std::endl;
            return false;
        }

        vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);

        initialized = true;
        return true;
    }

    void cleanup() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
        initialized = false;
    }

    ~TestVulkanContext() {
        cleanup();
    }
};

static TestVulkanContext* g_context = nullptr;

// =============================================================================
// PHASE 1: CORE DEFERRED PIPELINE TESTS
// =============================================================================

bool testGBufferPassCreation() {
    TEST_BEGIN("G-Buffer Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    GBufferPass gbuffer;
    bool result = gbuffer.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        gbuffer.onResize(1920, 1080);
        EXPECT_EQ(std::string(gbuffer.getName()), std::string("GBufferPass"));
        gbuffer.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("GBufferPass initialization failed");
    }

    return result;
}

bool testDeferredLightingPassCreation() {
    TEST_BEGIN("Deferred Lighting Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    DeferredLightingPass lighting;
    bool result = lighting.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        lighting.onResize(1920, 1080);
        EXPECT_EQ(std::string(lighting.getName()), std::string("DeferredLightingPass"));
        lighting.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("DeferredLightingPass initialization failed");
    }

    return result;
}

// =============================================================================
// PHASE 2: SHADOW SYSTEM TESTS
// =============================================================================

bool testCSMPassCreation() {
    TEST_BEGIN("Cascaded Shadow Map Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    CSMPass csm;
    bool result = csm.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        EXPECT_EQ(std::string(csm.getName()), std::string("CSMPass"));
        // Test cascade configuration (using constants)
        EXPECT_EQ(CSMPass::CASCADE_COUNT, 4u);
        EXPECT_EQ(CSMPass::SHADOW_MAP_SIZE, 2048u);
        csm.setSplitLambda(0.95f);
        csm.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("CSMPass initialization failed");
    }

    return result;
}

// =============================================================================
// PHASE 3: ADVANCED EFFECTS TESTS
// =============================================================================

bool testSSAOPassCreation() {
    TEST_BEGIN("SSAO Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    SSAOPass ssao;
    bool result = ssao.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        ssao.onResize(1920, 1080);
        EXPECT_EQ(std::string(ssao.getName()), std::string("SSAOPass"));
        ssao.setRadius(0.5f);
        ssao.setIntensity(1.0f);
        ssao.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("SSAOPass initialization failed");
    }

    return result;
}

bool testSSRPassCreation() {
    TEST_BEGIN("SSR Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    SSRPass ssr;
    bool result = ssr.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        ssr.onResize(1920, 1080);
        EXPECT_EQ(std::string(ssr.getName()), std::string("SSRPass"));
        ssr.setMaxDistance(100.0f);
        ssr.setThickness(0.5f);
        ssr.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("SSRPass initialization failed");
    }

    return result;
}

bool testVolumetricPassCreation() {
    TEST_BEGIN("Volumetric Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    VolumetricPass volumetric;
    bool result = volumetric.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        volumetric.onResize(1920, 1080);
        EXPECT_EQ(std::string(volumetric.getName()), std::string("VolumetricPass"));
        volumetric.setDensity(0.02f);
        volumetric.setScattering(0.8f);
        volumetric.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("VolumetricPass initialization failed");
    }

    return result;
}

bool testMotionBlurPassCreation() {
    TEST_BEGIN("Motion Blur Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    MotionBlurPass motionBlur;
    bool result = motionBlur.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        motionBlur.onResize(1920, 1080);
        EXPECT_EQ(std::string(motionBlur.getName()), std::string("MotionBlurPass"));
        motionBlur.setIntensity(1.0f);
        motionBlur.setMaxSamples(16);
        motionBlur.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("MotionBlurPass initialization failed");
    }

    return result;
}

bool testDoFPassCreation() {
    TEST_BEGIN("Depth of Field Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    DoFPass dof;
    bool result = dof.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        dof.onResize(1920, 1080);
        EXPECT_EQ(std::string(dof.getName()), std::string("DoFPass"));
        dof.setFocusDistance(5.0f);
        dof.setAperture(2.8f);
        dof.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("DoFPass initialization failed");
    }

    return result;
}

bool testBloomPassCreation() {
    TEST_BEGIN("Bloom Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    BloomPass bloom;
    bool result = bloom.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        bloom.onResize(1920, 1080);
        EXPECT_EQ(std::string(bloom.getName()), std::string("BloomPass"));
        bloom.setThreshold(1.0f);
        bloom.setIntensity(0.5f);
        bloom.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("BloomPass initialization failed");
    }

    return result;
}

bool testTAAPassCreation() {
    TEST_BEGIN("TAA Pass Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    TAAPass taa;
    bool result = taa.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        taa.onResize(1920, 1080);
        EXPECT_EQ(std::string(taa.getName()), std::string("TAAPass"));
        taa.setBlendFactor(0.1f);
        // Test jitter sequence
        auto jitter = taa.getJitterOffset(0);
        taa.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("TAAPass initialization failed");
    }

    return result;
}

bool testPostProcessingPipeline() {
    TEST_BEGIN("Post-Processing Pipeline Creation");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    PostProcessingPipeline pp;
    bool result = pp.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        pp.onResize(1920, 1080);
        EXPECT_EQ(std::string(pp.getName()), std::string("PostProcessingPipeline"));

        // Test feature toggles
        pp.setBloomEnabled(true);
        pp.setTAAEnabled(true);
        pp.setSSAOEnabled(true);
        pp.setSSREnabled(true);
        pp.setVolumetricsEnabled(true);
        pp.setMotionBlurEnabled(true);
        pp.setDoFEnabled(true);

        // Test tonemapping config
        pp.setTonemapOperator(TonemapOperator::ACES);
        pp.setExposure(1.0f);
        pp.setGamma(2.2f);

        pp.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("PostProcessingPipeline initialization failed");
    }

    return result;
}

// =============================================================================
// PHASE 4: PERFORMANCE & ARCHITECTURE TESTS
// =============================================================================

bool testRenderGraphSystem() {
    TEST_BEGIN("Render Graph System");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    RenderGraph graph;
    bool result = graph.initialize(g_context->device, g_context->physicalDevice);

    if (result) {
        // Test import external texture
        auto imported = graph.importTexture(
            "TestTexture",
            VK_NULL_HANDLE,  // No actual image for this test
            VK_NULL_HANDLE,  // No actual view
            VK_FORMAT_R16G16B16A16_SFLOAT,
            1920, 1080,
            VK_IMAGE_LAYOUT_UNDEFINED
        );

        // Test adding a pass
        graph.addPass("TestPass",
            [](PassBuilder& builder) {
                // Pass setup
            },
            [](VkCommandBuffer cmd) {
                // Pass execution
            }
        );

        graph.reset();
        graph.shutdown();
        TEST_PASS();
    } else {
        TEST_FAIL("RenderGraph initialization failed");
    }

    return result;
}

bool testVMAIntegration() {
    TEST_BEGIN("VMA (GPU Allocator) Integration");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    GpuAllocator allocator;
    bool result = allocator.initialize(g_context->instance, g_context->physicalDevice, g_context->device);

    if (result) {
        // Test buffer allocation
        GpuBuffer buffer = allocator.createBuffer(
            1024 * 1024,  // 1MB
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            AllocationUsage::GpuOnly
        );
        EXPECT_TRUE(buffer.isValid());
        EXPECT_NOT_NULL(buffer.buffer);

        allocator.destroyBuffer(buffer);

        // Test image allocation
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {1024, 1024, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        GpuImage image = allocator.createImage(imageInfo, AllocationUsage::GpuOnly);
        EXPECT_TRUE(image.isValid());
        EXPECT_NOT_NULL(image.image);

        allocator.destroyImage(image);
        allocator.shutdown();
        TEST_PASS();
    } else {
        TEST_FAIL("GpuAllocator initialization failed");
    }

    return result;
}

bool testGPUDrivenRendering() {
    TEST_BEGIN("GPU-Driven Rendering (Indirect Draw Buffer)");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    IndirectDrawBuffer drawBuffer;
    bool result = drawBuffer.initialize(g_context->device, g_context->physicalDevice, 1024);

    if (result) {
        // Test adding draw commands
        IndirectDrawCommand cmd{};
        cmd.indexCount = 36;
        cmd.instanceCount = 1;
        cmd.firstIndex = 0;
        cmd.vertexOffset = 0;
        cmd.firstInstance = 0;

        DrawInstance instance{};
        instance.modelMatrix = glm::mat4(1.0f);
        instance.normalMatrix = glm::mat4(1.0f);
        instance.materialIndex = 0;
        instance.meshIndex = 0;
        instance.flags = 0;

        drawBuffer.addDraw(cmd, instance);
        EXPECT_EQ(drawBuffer.getDrawCount(), 1u);

        drawBuffer.reset();
        EXPECT_EQ(drawBuffer.getDrawCount(), 0u);

        drawBuffer.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("IndirectDrawBuffer initialization failed");
    }

    return result;
}

bool testAsyncComputeQueue() {
    TEST_BEGIN("Async Compute Queue");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    AsyncComputeQueue asyncQueue;
    bool result = asyncQueue.initialize(
        g_context->device,
        g_context->physicalDevice,
        g_context->graphicsQueueFamily,  // Use graphics queue if no dedicated compute
        0
    );

    if (result) {
        EXPECT_NOT_NULL(asyncQueue.getTimelineSemaphore());
        EXPECT_EQ(asyncQueue.getPendingTaskCount(), 0u);

        asyncQueue.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("AsyncComputeQueue initialization failed");
    }

    return result;
}

// =============================================================================
// PHASE 5: MATERIAL SYSTEM TESTS
// =============================================================================

bool testMaterialSystem() {
    TEST_BEGIN("Material Instance System");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    // Create texture manager first (required for material system)
    BindlessTextureManager texManager;
    bool texResult = texManager.initialize(
        g_context->device, g_context->physicalDevice, nullptr, 256,
        g_context->graphicsQueueFamily, g_context->graphicsQueue);

    if (!texResult) {
        TEST_FAIL("BindlessTextureManager initialization failed");
        return false;
    }

    MaterialManager matManager;
    bool result = matManager.initialize(g_context->device, g_context->physicalDevice, &texManager, 64);

    if (result) {
        // Test template creation
        auto* metalTemplate = matManager.createTemplate("Metal");
        EXPECT_NOT_NULL(metalTemplate);
        metalTemplate->defaultParams.metallic = 1.0f;
        metalTemplate->defaultParams.roughness = 0.3f;

        // Test instance creation
        auto* instance = matManager.createInstance(metalTemplate);
        EXPECT_NOT_NULL(instance);

        instance->setAlbedoColor(glm::vec3(0.9f, 0.9f, 0.9f));
        instance->setRoughness(0.2f);
        EXPECT_TRUE(instance->isDirty());

        // Test advanced material features
        instance->setClearCoat(0.8f, 0.1f);
        instance->setAnisotropy(0.5f, 0.0f);

        // Update GPU
        matManager.updateGPU();
        EXPECT_FALSE(instance->isDirty());

        matManager.cleanup();
        texManager.cleanup();
        TEST_PASS();
    } else {
        texManager.cleanup();
        TEST_FAIL("MaterialManager initialization failed");
    }

    return result;
}

bool testBindlessTexturing() {
    TEST_BEGIN("Bindless Texture Manager");

    if (!g_context->initialized) {
        TEST_SKIP("Vulkan not initialized");
        return true;
    }

    BindlessTextureManager texManager;
    bool result = texManager.initialize(
        g_context->device, g_context->physicalDevice, nullptr, 256,
        g_context->graphicsQueueFamily, g_context->graphicsQueue);

    if (result) {
        EXPECT_NOT_NULL(texManager.getDescriptorSetLayout());
        EXPECT_NOT_NULL(texManager.getDescriptorSet());

        // Check default textures
        EXPECT_TRUE(texManager.getDefaultWhiteTexture().valid());
        EXPECT_TRUE(texManager.getDefaultBlackTexture().valid());
        EXPECT_TRUE(texManager.getDefaultNormalTexture().valid());

        EXPECT_EQ(texManager.getMaxTextures(), 256u);
        EXPECT_TRUE(texManager.getLoadedTextureCount() >= 3);  // Default textures

        texManager.cleanup();
        TEST_PASS();
    } else {
        TEST_FAIL("BindlessTextureManager initialization failed");
    }

    return result;
}

bool testMaterialParams() {
    TEST_BEGIN("PBR Material Parameters Structure");

    // Test structure alignment (must be 16-byte aligned for GPU)
    EXPECT_EQ(sizeof(PBRMaterialParams) % 16, 0u);

    // Test default values
    PBRMaterialParams params{};
    EXPECT_EQ(params.roughness, 0.5f);
    EXPECT_EQ(params.metallic, 0.0f);
    EXPECT_EQ(params.ao, 1.0f);
    EXPECT_EQ(params.normalStrength, 1.0f);
    EXPECT_EQ(params.ior, 1.5f);

    // Test texture indices (should be invalid by default)
    EXPECT_EQ(params.albedoTexIndex, UINT32_MAX);
    EXPECT_EQ(params.normalTexIndex, UINT32_MAX);

    TEST_PASS();
    return true;
}

// =============================================================================
// TEST RUNNER
// =============================================================================

void runAllTests() {
    std::cout << "\n\033[1m=== OHAO AAA Renderer Pipeline Tests ===\033[0m" << std::endl;

    // Initialize Vulkan context
    g_context = new TestVulkanContext();
    if (!g_context->initialize()) {
        std::cerr << "\033[31mFailed to initialize Vulkan test context\033[0m" << std::endl;
        std::cerr << "Some tests will be skipped." << std::endl;
    } else {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(g_context->physicalDevice, &props);
        std::cout << "Testing on: " << props.deviceName << std::endl;
    }

    // Phase 1: Core Deferred Pipeline
    std::cout << "\n\033[1m--- Phase 1: Core Deferred Pipeline ---\033[0m" << std::endl;
    testGBufferPassCreation();
    testDeferredLightingPassCreation();

    // Phase 2: Shadow System
    std::cout << "\n\033[1m--- Phase 2: Shadow System ---\033[0m" << std::endl;
    testCSMPassCreation();

    // Phase 3: Advanced Effects
    std::cout << "\n\033[1m--- Phase 3: Advanced Effects ---\033[0m" << std::endl;
    testSSAOPassCreation();
    testSSRPassCreation();
    testVolumetricPassCreation();
    testMotionBlurPassCreation();
    testDoFPassCreation();
    testBloomPassCreation();
    testTAAPassCreation();
    testPostProcessingPipeline();

    // Phase 4: Performance & Architecture
    std::cout << "\n\033[1m--- Phase 4: Performance & Architecture ---\033[0m" << std::endl;
    testRenderGraphSystem();
    testVMAIntegration();
    testGPUDrivenRendering();
    testAsyncComputeQueue();

    // Phase 5: Material System
    std::cout << "\n\033[1m--- Phase 5: Material System ---\033[0m" << std::endl;
    testMaterialParams();
    testBindlessTexturing();
    testMaterialSystem();

    // Cleanup
    delete g_context;
    g_context = nullptr;

    // Results
    std::cout << "\n\033[1m=== Test Results ===\033[0m" << std::endl;
    std::cout << "Tests run:    " << testsRun << std::endl;
    std::cout << "\033[32mPassed:       " << testsPassed << "\033[0m" << std::endl;
    if (testsFailed > 0) {
        std::cout << "\033[31mFailed:       " << testsFailed << "\033[0m" << std::endl;
    }
    if (testsSkipped > 0) {
        std::cout << "\033[33mSkipped:      " << testsSkipped << "\033[0m" << std::endl;
    }

    if (testsFailed > 0) {
        std::cout << "\n\033[31m*** SOME TESTS FAILED ***\033[0m" << std::endl;
    } else {
        std::cout << "\n\033[32m*** ALL TESTS PASSED ***\033[0m" << std::endl;
    }
}

} // namespace ohao::tests

int main() {
    ohao::tests::runAllTests();
    return ohao::tests::testsFailed > 0 ? 1 : 0;
}
