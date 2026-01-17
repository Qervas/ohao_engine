#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

// Renderer includes
#include "renderer/passes/render_pass_base.hpp"
#include "renderer/passes/gbuffer_pass.hpp"
#include "renderer/passes/deferred_lighting_pass.hpp"
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

namespace py = pybind11;
using namespace ohao;

// Test helper - checks if Vulkan is available
bool checkVulkanAvailable() {
    // Simple check - try to enumerate instance extensions
    uint32_t extensionCount = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    return result == VK_SUCCESS && extensionCount > 0;
}

// Renderer capability report
struct RendererCapabilities {
    bool vulkanAvailable{false};
    bool bindlessSupported{false};
    bool timelineSemaphoresSupported{false};
    bool asyncComputeSupported{false};
    uint32_t maxTextures{0};
    uint32_t maxMaterials{0};
    std::string deviceName;
    std::string vulkanVersion;

    std::string toString() const {
        std::string result = "Renderer Capabilities:\n";
        result += "  Vulkan Available: " + std::string(vulkanAvailable ? "Yes" : "No") + "\n";
        result += "  Device: " + deviceName + "\n";
        result += "  Vulkan Version: " + vulkanVersion + "\n";
        result += "  Bindless Texturing: " + std::string(bindlessSupported ? "Yes" : "No") + "\n";
        result += "  Timeline Semaphores: " + std::string(timelineSemaphoresSupported ? "Yes" : "No") + "\n";
        result += "  Async Compute: " + std::string(asyncComputeSupported ? "Yes" : "No") + "\n";
        result += "  Max Textures: " + std::to_string(maxTextures) + "\n";
        result += "  Max Materials: " + std::to_string(maxMaterials) + "\n";
        return result;
    }
};

// Query renderer capabilities
RendererCapabilities queryCapabilities() {
    RendererCapabilities caps;
    caps.vulkanAvailable = checkVulkanAvailable();

    if (!caps.vulkanAvailable) {
        return caps;
    }

    // Create temporary Vulkan instance to query device info
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "OHAO Renderer Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "OHAO Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

#ifdef __APPLE__
    createInfo.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    const char* extensions[] = {
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
    };
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = extensions;
#endif

    VkInstance instance;
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        return caps;
    }

    // Get physical device
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount > 0) {
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        VkPhysicalDevice physicalDevice = devices[0];

        // Get device properties
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        caps.deviceName = props.deviceName;
        caps.vulkanVersion = std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
                             std::to_string(VK_VERSION_MINOR(props.apiVersion)) + "." +
                             std::to_string(VK_VERSION_PATCH(props.apiVersion));

        // Check for descriptor indexing (bindless)
        VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures{};
        indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &indexingFeatures;

        vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

        caps.bindlessSupported = indexingFeatures.descriptorBindingPartiallyBound &&
                                  indexingFeatures.runtimeDescriptorArray;

        // Check for timeline semaphores
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
        timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

        features2.pNext = &timelineFeatures;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);

        caps.timelineSemaphoresSupported = timelineFeatures.timelineSemaphore;

        // Check for async compute (separate compute queue family)
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

        for (const auto& qf : queueFamilies) {
            if ((qf.queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf.queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                caps.asyncComputeSupported = true;
                break;
            }
        }

        // Default capacities
        caps.maxTextures = 4096;
        caps.maxMaterials = 1024;
    }

    vkDestroyInstance(instance, nullptr);
    return caps;
}

// Render pass info for inspection
struct RenderPassInfo {
    std::string name;
    bool enabled;
    uint32_t width;
    uint32_t height;
};

// Material parameter struct for Python
struct PyMaterialParams {
    float albedoR{0.8f}, albedoG{0.8f}, albedoB{0.8f}, albedoA{1.0f};
    float roughness{0.5f};
    float metallic{0.0f};
    float ao{1.0f};
    float normalStrength{1.0f};
    float clearCoatIntensity{0.0f};
    float clearCoatRoughness{0.0f};
    float subsurfaceIntensity{0.0f};
    float anisotropy{0.0f};
    float sheenIntensity{0.0f};
    float transmission{0.0f};
    float ior{1.5f};

    std::string toString() const {
        return "Material(albedo=(" + std::to_string(albedoR) + "," +
               std::to_string(albedoG) + "," + std::to_string(albedoB) + "), " +
               "roughness=" + std::to_string(roughness) + ", " +
               "metallic=" + std::to_string(metallic) + ")";
    }
};

PYBIND11_MODULE(ohao_renderer, m) {
    m.doc() = "OHAO AAA Renderer - Python Bindings for Testing";

    // ===== Renderer Capabilities =====
    py::class_<RendererCapabilities>(m, "RendererCapabilities")
        .def(py::init<>())
        .def_readonly("vulkan_available", &RendererCapabilities::vulkanAvailable)
        .def_readonly("bindless_supported", &RendererCapabilities::bindlessSupported)
        .def_readonly("timeline_semaphores_supported", &RendererCapabilities::timelineSemaphoresSupported)
        .def_readonly("async_compute_supported", &RendererCapabilities::asyncComputeSupported)
        .def_readonly("max_textures", &RendererCapabilities::maxTextures)
        .def_readonly("max_materials", &RendererCapabilities::maxMaterials)
        .def_readonly("device_name", &RendererCapabilities::deviceName)
        .def_readonly("vulkan_version", &RendererCapabilities::vulkanVersion)
        .def("__str__", &RendererCapabilities::toString)
        .def("__repr__", &RendererCapabilities::toString);

    m.def("query_capabilities", &queryCapabilities,
          "Query renderer capabilities and Vulkan device info");

    m.def("check_vulkan_available", &checkVulkanAvailable,
          "Check if Vulkan is available on this system");

    // ===== Blend Mode =====
    py::enum_<BlendMode>(m, "BlendMode")
        .value("OPAQUE", BlendMode::Opaque)
        .value("ALPHA_BLEND", BlendMode::AlphaBlend)
        .value("ADDITIVE", BlendMode::Additive)
        .value("MULTIPLY", BlendMode::Multiply)
        .export_values();

    // ===== Render Queue =====
    py::enum_<RenderQueue>(m, "RenderQueue")
        .value("BACKGROUND", RenderQueue::Background)
        .value("GEOMETRY", RenderQueue::Geometry)
        .value("ALPHA_TEST", RenderQueue::AlphaTest)
        .value("TRANSPARENT", RenderQueue::Transparent)
        .value("OVERLAY", RenderQueue::Overlay)
        .export_values();

    // ===== Tonemap Operator =====
    py::enum_<TonemapOperator>(m, "TonemapOperator")
        .value("ACES", TonemapOperator::ACES)
        .value("REINHARD", TonemapOperator::Reinhard)
        .value("UNCHARTED2", TonemapOperator::Uncharted2)
        .value("NEUTRAL", TonemapOperator::Neutral)
        .export_values();

    // ===== Material Parameters =====
    py::class_<PyMaterialParams>(m, "MaterialParams")
        .def(py::init<>())
        .def_readwrite("albedo_r", &PyMaterialParams::albedoR)
        .def_readwrite("albedo_g", &PyMaterialParams::albedoG)
        .def_readwrite("albedo_b", &PyMaterialParams::albedoB)
        .def_readwrite("albedo_a", &PyMaterialParams::albedoA)
        .def_readwrite("roughness", &PyMaterialParams::roughness)
        .def_readwrite("metallic", &PyMaterialParams::metallic)
        .def_readwrite("ao", &PyMaterialParams::ao)
        .def_readwrite("normal_strength", &PyMaterialParams::normalStrength)
        .def_readwrite("clear_coat_intensity", &PyMaterialParams::clearCoatIntensity)
        .def_readwrite("clear_coat_roughness", &PyMaterialParams::clearCoatRoughness)
        .def_readwrite("subsurface_intensity", &PyMaterialParams::subsurfaceIntensity)
        .def_readwrite("anisotropy", &PyMaterialParams::anisotropy)
        .def_readwrite("sheen_intensity", &PyMaterialParams::sheenIntensity)
        .def_readwrite("transmission", &PyMaterialParams::transmission)
        .def_readwrite("ior", &PyMaterialParams::ior)
        .def("__str__", &PyMaterialParams::toString)
        .def("__repr__", &PyMaterialParams::toString);

    // ===== Render Pass Info =====
    py::class_<RenderPassInfo>(m, "RenderPassInfo")
        .def(py::init<>())
        .def_readwrite("name", &RenderPassInfo::name)
        .def_readwrite("enabled", &RenderPassInfo::enabled)
        .def_readwrite("width", &RenderPassInfo::width)
        .def_readwrite("height", &RenderPassInfo::height);

    // ===== Feature list =====
    m.def("get_supported_features", []() {
        std::vector<std::string> features = {
            "Deferred Rendering",
            "G-Buffer (Position, Normal, Albedo, Motion Vectors)",
            "Tile-Based Light Culling",
            "Cascaded Shadow Maps (4 cascades)",
            "PCSS Soft Shadows",
            "Screen-Space Ambient Occlusion (SSAO)",
            "Screen-Space Reflections (SSR)",
            "Volumetric Lighting/Fog",
            "Temporal Anti-Aliasing (TAA)",
            "Bloom (HDR)",
            "Motion Blur",
            "Depth of Field (Bokeh)",
            "Tonemapping (ACES, Reinhard, Uncharted2, Neutral)",
            "GPU-Driven Rendering (Indirect Draw)",
            "GPU Frustum Culling",
            "Async Compute Queue",
            "Render Graph System",
            "VMA Integration",
            "Bindless Texturing (4096 textures)",
            "Material Instance System",
            "Clear Coat",
            "Subsurface Scattering",
            "Anisotropic Reflections",
            "Sheen (Fabric)",
            "Transmission (Glass)"
        };
        return features;
    }, "Get list of supported renderer features");

    // ===== Post-processing configuration =====
    py::class_<PostProcessingPipeline>(m, "PostProcessingPipeline")
        .def("set_bloom_enabled", &PostProcessingPipeline::setBloomEnabled)
        .def("set_taa_enabled", &PostProcessingPipeline::setTAAEnabled)
        .def("set_ssao_enabled", &PostProcessingPipeline::setSSAOEnabled)
        .def("set_ssr_enabled", &PostProcessingPipeline::setSSREnabled)
        .def("set_volumetrics_enabled", &PostProcessingPipeline::setVolumetricsEnabled)
        .def("set_motion_blur_enabled", &PostProcessingPipeline::setMotionBlurEnabled)
        .def("set_dof_enabled", &PostProcessingPipeline::setDoFEnabled)
        .def("set_tonemapping_enabled", &PostProcessingPipeline::setTonemappingEnabled)
        .def("set_tonemap_operator", &PostProcessingPipeline::setTonemapOperator)
        .def("set_exposure", &PostProcessingPipeline::setExposure)
        .def("set_gamma", &PostProcessingPipeline::setGamma)
        .def("set_bloom_threshold", &PostProcessingPipeline::setBloomThreshold)
        .def("set_bloom_intensity", &PostProcessingPipeline::setBloomIntensity)
        .def("get_name", &PostProcessingPipeline::getName);

    // ===== Version info =====
    m.attr("__version__") = "1.0.0";
    m.attr("RENDERER_NAME") = "OHAO AAA Renderer";
    m.attr("VULKAN_API_VERSION") = "1.2";
}
