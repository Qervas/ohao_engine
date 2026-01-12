#pragma once
/**
 * ohao_vk_descriptor_builder.hpp - Type-Safe Descriptor Layout Builder
 *
 * CORE PRINCIPLE: Descriptor layouts defined at compile time
 * Wrong binding indices or descriptor types won't compile.
 *
 * COMPILE-TIME SAFETY:
 * - Template parameters encode binding, type, stages at compile time
 * - constexpr arrays of bindings constructed without runtime overhead
 * - static_assert validates binding uniqueness and completeness
 * - Uses constants from shader_bindings.hpp for single source of truth
 */

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include "renderer/shader/shader_bindings.hpp"

namespace ohao {

// =============================================================================
// COMPILE-TIME DESCRIPTOR BINDING DEFINITION
// =============================================================================

/**
 * @brief Type-safe descriptor binding template
 *
 * Encodes all binding properties at compile time. The toVkBinding() method
 * is constexpr and produces VkDescriptorSetLayoutBinding with zero runtime cost.
 *
 * @tparam BindingIndex The binding slot (from ShaderBindings::Set0)
 * @tparam Type VkDescriptorType for this binding
 * @tparam Stages VkShaderStageFlags indicating which stages use this binding
 * @tparam Count Number of descriptors (for arrays, default 1)
 */
template<uint32_t BindingIndex,
         VkDescriptorType Type,
         VkShaderStageFlags Stages,
         uint32_t Count = 1>
struct DescriptorBinding {
    /// @brief Binding index (compile-time constant)
    static constexpr uint32_t binding = BindingIndex;

    /// @brief Descriptor type (compile-time constant)
    static constexpr VkDescriptorType descriptorType = Type;

    /// @brief Shader stage flags (compile-time constant)
    static constexpr VkShaderStageFlags stageFlags = Stages;

    /// @brief Descriptor count (compile-time constant)
    static constexpr uint32_t descriptorCount = Count;

    /**
     * @brief Convert to Vulkan binding structure at compile time
     * @return VkDescriptorSetLayoutBinding configured from template parameters
     */
    [[nodiscard]] static constexpr VkDescriptorSetLayoutBinding toVkBinding() noexcept {
        return VkDescriptorSetLayoutBinding{
            .binding = BindingIndex,
            .descriptorType = Type,
            .descriptorCount = Count,
            .stageFlags = Stages,
            .pImmutableSamplers = nullptr
        };
    }
};

// =============================================================================
// MAIN DESCRIPTOR SET LAYOUT (Set 0)
// =============================================================================

/**
 * @brief Main descriptor set bindings using compile-time constants
 *
 * All binding indices come from ShaderBindings::Set0, ensuring consistency
 * with shader code. Any binding index mismatch will cause a compile error.
 */
namespace MainDescriptorSet {
    /// Common shader stages for vertex+fragment access
    constexpr VkShaderStageFlags kVertexFragment =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    /// Fragment-only shader stage
    constexpr VkShaderStageFlags kFragmentOnly = VK_SHADER_STAGE_FRAGMENT_BIT;

    // -------------------------------------------------------------------------
    // Binding definitions using ShaderBindings constants
    // -------------------------------------------------------------------------

    /// @brief Global UBO (LightingUBO) - binding 0
    using GlobalUBO = DescriptorBinding<
        ShaderBindings::Set0::kGlobalUBO,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        kVertexFragment,
        1
    >;

    /// @brief Shadow map array (legacy) - binding 1, array of MAX_SHADOW_MAPS
    using ShadowMapArray = DescriptorBinding<
        ShaderBindings::Set0::kShadowMapArray,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        kFragmentOnly,
        ShaderBindings::kMaxShadowMaps
    >;

    /// @brief Shadow atlas for local lights - binding 2
    using ShadowAtlas = DescriptorBinding<
        ShaderBindings::Set0::kShadowAtlas,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        kFragmentOnly,
        1
    >;

    /// @brief CSM cascade array - binding 3, array of MAX_CSM_CASCADES
    using CSMCascades = DescriptorBinding<
        ShaderBindings::Set0::kCSMCascades,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        kFragmentOnly,
        ShaderBindings::kMaxCSMCascades
    >;

    // -------------------------------------------------------------------------
    // Compile-time binding array
    // -------------------------------------------------------------------------

    /**
     * @brief All bindings for set 0 as a constexpr array
     *
     * This array is constructed at compile time and can be used directly
     * to create VkDescriptorSetLayout without any runtime initialization.
     */
    constexpr std::array<VkDescriptorSetLayoutBinding, ShaderBindings::Set0::kBindingCount> bindings = {
        GlobalUBO::toVkBinding(),
        ShadowMapArray::toVkBinding(),
        ShadowAtlas::toVkBinding(),
        CSMCascades::toVkBinding()
    };

    // -------------------------------------------------------------------------
    // Compile-time validation
    // -------------------------------------------------------------------------

    // Verify binding indices match expected values
    static_assert(GlobalUBO::binding == 0, "GlobalUBO must be at binding 0");
    static_assert(ShadowMapArray::binding == 1, "ShadowMapArray must be at binding 1");
    static_assert(ShadowAtlas::binding == 2, "ShadowAtlas must be at binding 2");
    static_assert(CSMCascades::binding == 3, "CSMCascades must be at binding 3");

    // Verify array has correct number of bindings
    static_assert(bindings.size() == ShaderBindings::Set0::kBindingCount,
                  "Binding count mismatch with ShaderBindings::Set0::kBindingCount");

    // Verify descriptor counts for arrays
    static_assert(ShadowMapArray::descriptorCount == ShaderBindings::kMaxShadowMaps,
                  "ShadowMapArray count must match kMaxShadowMaps");
    static_assert(CSMCascades::descriptorCount == ShaderBindings::kMaxCSMCascades,
                  "CSMCascades count must match kMaxCSMCascades");

} // namespace MainDescriptorSet

// =============================================================================
// MINIMAL DESCRIPTOR SET (for shadow pass - UBO only)
// =============================================================================

/**
 * @brief Minimal descriptor set for shadow depth pass
 *
 * Shadow pass only needs the global UBO for light space matrix.
 * No shadow map samplers needed since we're writing to the shadow map.
 */
namespace ShadowPassDescriptorSet {
    /// @brief Global UBO only for shadow pass
    using GlobalUBO = DescriptorBinding<
        ShaderBindings::Set0::kGlobalUBO,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_SHADER_STAGE_VERTEX_BIT,  // Only vertex shader needs it
        1
    >;

    constexpr std::array<VkDescriptorSetLayoutBinding, 1> bindings = {
        GlobalUBO::toVkBinding()
    };
}

// =============================================================================
// DESCRIPTOR POOL SIZE CALCULATOR
// =============================================================================

/**
 * @brief Calculate pool sizes needed for a descriptor set layout
 *
 * Counts how many of each descriptor type are needed, useful for
 * creating VkDescriptorPoolCreateInfo.
 */
struct DescriptorPoolSizeCalculator {
    uint32_t uniformBuffers = 0;
    uint32_t combinedImageSamplers = 0;
    uint32_t storageBuffers = 0;
    uint32_t storageImages = 0;

    /**
     * @brief Add requirements from a binding
     */
    template<uint32_t Binding, VkDescriptorType Type, VkShaderStageFlags Stages, uint32_t Count>
    constexpr void addBinding(const DescriptorBinding<Binding, Type, Stages, Count>&) {
        if constexpr (Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            uniformBuffers += Count;
        } else if constexpr (Type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            combinedImageSamplers += Count;
        } else if constexpr (Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            storageBuffers += Count;
        } else if constexpr (Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            storageImages += Count;
        }
    }

    /**
     * @brief Calculate pool sizes for main descriptor set
     * @param maxSets Maximum number of descriptor sets to allocate
     * @return DescriptorPoolSizeCalculator with counts multiplied by maxSets
     */
    [[nodiscard]] static constexpr DescriptorPoolSizeCalculator forMainSet(uint32_t maxSets = 1) {
        DescriptorPoolSizeCalculator calc;
        // Main set: 1 UBO + 4 shadow maps + 1 atlas + 4 CSM cascades = 1 UBO + 9 samplers
        calc.uniformBuffers = 1 * maxSets;
        calc.combinedImageSamplers = (ShaderBindings::kMaxShadowMaps +
                                      1 +  // atlas
                                      ShaderBindings::kMaxCSMCascades) * maxSets;
        return calc;
    }
};

// =============================================================================
// HELPER: GET BINDING BY INDEX (compile-time)
// =============================================================================

/**
 * @brief Get binding information by index at compile time
 *
 * Useful for static_assert validation or template metaprogramming.
 */
template<uint32_t Index>
struct GetMainSetBinding;

template<>
struct GetMainSetBinding<0> { using type = MainDescriptorSet::GlobalUBO; };

template<>
struct GetMainSetBinding<1> { using type = MainDescriptorSet::ShadowMapArray; };

template<>
struct GetMainSetBinding<2> { using type = MainDescriptorSet::ShadowAtlas; };

template<>
struct GetMainSetBinding<3> { using type = MainDescriptorSet::CSMCascades; };

// Convenience alias
template<uint32_t Index>
using MainSetBindingType = typename GetMainSetBinding<Index>::type;

// =============================================================================
// GLOBAL VALIDATION
// =============================================================================

// Verify all bindings in main set are sequential (0, 1, 2, 3)
static_assert(MainSetBindingType<0>::binding == 0, "Binding 0 must have index 0");
static_assert(MainSetBindingType<1>::binding == 1, "Binding 1 must have index 1");
static_assert(MainSetBindingType<2>::binding == 2, "Binding 2 must have index 2");
static_assert(MainSetBindingType<3>::binding == 3, "Binding 3 must have index 3");

} // namespace ohao
