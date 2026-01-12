#pragma once
/**
 * unified_light.hpp - Type-Safe Unified Lighting System
 *
 * CORE PRINCIPLE: Light + Shadow = One Unit
 * A shadow-casting light MUST have its shadow data paired with it.
 * Invalid configurations won't compile.
 *
 * COMPILE-TIME SAFETY:
 * - Strong typed handles prevent mixing different ID types
 * - Deleted cross-type conversions catch errors at compile time
 * - static_assert validates struct layouts match GPU expectations
 * - checkedAccess() provides bounds validation with context
 */

#include <glm/glm.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "renderer/shader/shader_bindings.hpp"

namespace ohao {

// =============================================================================
// CONSTANTS (from shader_bindings.hpp for single source of truth)
// =============================================================================
constexpr int MAX_UNIFIED_LIGHTS = ShaderBindings::kMaxLights;
constexpr int MAX_SHADOW_MAPS = ShaderBindings::kMaxShadowMaps;
constexpr int MAX_CSM_CASCADES = ShaderBindings::kMaxCSMCascades;
constexpr int MAX_ATLAS_TILES = ShaderBindings::kMaxAtlasTiles;

// =============================================================================
// STRONG HANDLE TEMPLATE
// =============================================================================

/// @brief Tag types for strong handle differentiation
struct LightHandleTag {};
struct ShadowMapHandleTag {};
struct CascadeIndexTag {};
struct AtlasTileHandleTag {};

/**
 * @brief Type-safe handle template that prevents mixing different ID types
 *
 * Key safety features:
 * - Deleted cross-type constructors prevent implicit conversion
 * - Explicit construction only (no accident conversions from raw integers)
 * - Invalid state is explicitly representable
 *
 * @tparam Tag Type tag to differentiate handle types
 * @tparam T Underlying integer type (default: uint32_t)
 */
template<typename Tag, typename T = uint32_t>
struct StrongHandle {
    static constexpr T kInvalidValue = static_cast<T>(-1);

    T id{kInvalidValue};

    /// @brief Default constructor creates invalid handle
    constexpr StrongHandle() noexcept = default;

    /// @brief Explicit construction from value
    constexpr explicit StrongHandle(T value) noexcept : id(value) {}

    /// @brief Check if handle is valid
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return id != kInvalidValue;
    }

    /// @brief Factory for creating invalid handles
    [[nodiscard]] static constexpr StrongHandle invalid() noexcept {
        return StrongHandle{};
    }

    // Comparison operators
    constexpr bool operator==(const StrongHandle& other) const noexcept { return id == other.id; }
    constexpr bool operator!=(const StrongHandle& other) const noexcept { return id != other.id; }
    constexpr bool operator<(const StrongHandle& other) const noexcept { return id < other.id; }
    constexpr bool operator<=(const StrongHandle& other) const noexcept { return id <= other.id; }
    constexpr bool operator>(const StrongHandle& other) const noexcept { return id > other.id; }
    constexpr bool operator>=(const StrongHandle& other) const noexcept { return id >= other.id; }

    // COMPILE-TIME SAFETY: Delete conversion from other handle types
    template<typename OtherTag, typename = std::enable_if_t<!std::is_same_v<Tag, OtherTag>>>
    StrongHandle(const StrongHandle<OtherTag, T>&) = delete;

    template<typename OtherTag, typename = std::enable_if_t<!std::is_same_v<Tag, OtherTag>>>
    StrongHandle& operator=(const StrongHandle<OtherTag, T>&) = delete;
};

// =============================================================================
// CONCRETE HANDLE TYPES
// =============================================================================
using LightHandle     = StrongHandle<LightHandleTag>;
using ShadowMapHandle = StrongHandle<ShadowMapHandleTag>;
using CascadeIndex    = StrongHandle<CascadeIndexTag>;
using AtlasTileHandle = StrongHandle<AtlasTileHandleTag>;

// Compile-time verification that handle types cannot be confused
static_assert(!std::is_convertible_v<LightHandle, ShadowMapHandle>,
              "LightHandle must not convert to ShadowMapHandle");
static_assert(!std::is_convertible_v<ShadowMapHandle, LightHandle>,
              "ShadowMapHandle must not convert to LightHandle");
static_assert(!std::is_convertible_v<CascadeIndex, AtlasTileHandle>,
              "CascadeIndex must not convert to AtlasTileHandle");

// =============================================================================
// BOUNDS-CHECKED ACCESS
// =============================================================================

/**
 * @brief Provides bounds-checked container access with meaningful error context
 *
 * Throws std::out_of_range with context information if:
 * - Handle is invalid
 * - Handle ID exceeds container size
 *
 * @param container The container to access
 * @param handle The handle specifying the index
 * @param context Error context string for debugging
 * @return Reference to the element
 * @throws std::out_of_range on invalid access
 */
template<typename HandleType, typename Container>
[[nodiscard]] auto& checkedAccess(Container& container, HandleType handle, const char* context) {
    if (!handle.isValid()) {
        throw std::out_of_range(std::string(context) + ": Invalid handle (id=" +
                                std::to_string(handle.id) + ")");
    }
    if (handle.id >= container.size()) {
        throw std::out_of_range(std::string(context) + ": Handle ID out of range (id=" +
                                std::to_string(handle.id) + ", size=" +
                                std::to_string(container.size()) + ")");
    }
    return container[handle.id];
}

/// @brief Const overload for read-only access
template<typename HandleType, typename Container>
[[nodiscard]] const auto& checkedAccess(const Container& container, HandleType handle, const char* context) {
    if (!handle.isValid()) {
        throw std::out_of_range(std::string(context) + ": Invalid handle (id=" +
                                std::to_string(handle.id) + ")");
    }
    if (handle.id >= container.size()) {
        throw std::out_of_range(std::string(context) + ": Handle ID out of range (id=" +
                                std::to_string(handle.id) + ", size=" +
                                std::to_string(container.size()) + ")");
    }
    return container[handle.id];
}

// =============================================================================
// SHADOW TYPE ENUMERATION
// =============================================================================

/**
 * @brief Type of shadow casting for a light
 *
 * Each shadow type has different resource requirements and shader paths.
 */
enum class ShadowType : uint8_t {
    None       = 0,  ///< No shadows cast by this light
    Simple     = 1,  ///< Single shadow map (legacy)
    Cascaded   = 2,  ///< CSM for directional lights (4 cascades)
    AtlasTile  = 3,  ///< Shadow atlas tile for point/spot lights
    Cubemap    = 4   ///< Omnidirectional shadows for point lights (future)
};

// =============================================================================
// LIGHT TYPE CONSTANTS
// =============================================================================
namespace UnifiedLightTypes {
    constexpr float Directional = 0.0f;
    constexpr float Point = 1.0f;
    constexpr float Spot = 2.0f;
}

// =============================================================================
// CSM CASCADE INFO (for per-cascade data)
// =============================================================================
struct CSMCascadeInfo {
    glm::mat4 viewProj;      ///< Light-space view-projection matrix for this cascade
    float splitDepth;         ///< View-space depth where this cascade ends
    float texelSize;          ///< Texel size for PCF filtering
    float padding[2];         ///< Alignment padding
};
static_assert(sizeof(CSMCascadeInfo) == 80, "CSMCascadeInfo must be 80 bytes for std140");

// =============================================================================
// ATLAS TILE INFO (for shadow atlas tiles)
// =============================================================================
struct AtlasTileInfo {
    glm::vec2 uvOffset;       ///< UV offset into atlas (0-1 range)
    glm::vec2 uvScale;        ///< UV scale (typically 0.25 for 4x4 tiles)
    glm::mat4 viewProj;       ///< Light-space view-projection matrix
};
static_assert(sizeof(AtlasTileInfo) == 80, "AtlasTileInfo must be 80 bytes for std140");

// =============================================================================
// UNIFIED LIGHT STRUCTURE
// =============================================================================

/**
 * @brief Unified light structure combining light and shadow data
 *
 * CRITICAL: This struct MUST be exactly 128 bytes and match GLSL layout exactly.
 * The struct uses std140 packing rules for GPU compatibility.
 *
 * Layout:
 * - Bytes 0-63: Core light properties
 * - Bytes 64-127: Shadow data (lightSpaceMatrix)
 */
struct UnifiedLight {
    // === Core light properties (64 bytes) ===
    glm::vec3 position;          ///< World-space position (point/spot lights)
    float type;                   ///< Light type (0=directional, 1=point, 2=spot)

    glm::vec3 color;             ///< Light color (linear RGB)
    float intensity;              ///< Light intensity multiplier

    glm::vec3 direction;         ///< Light direction (directional/spot lights)
    float range;                  ///< Attenuation range (point/spot lights)

    float innerCone;             ///< Inner cone angle in degrees (spot lights)
    float outerCone;             ///< Outer cone angle in degrees (spot lights)
    int32_t shadowMapIndex;      ///< Shadow map index (-1 = no shadow, >= 0 = shadow map index)
    float _padding;               ///< Alignment padding

    // === Shadow data (64 bytes) ===
    glm::mat4 lightSpaceMatrix;  ///< Transform from world space to light clip space

    // === Convenience methods ===
    [[nodiscard]] bool castsShadow() const noexcept { return shadowMapIndex >= 0; }
    [[nodiscard]] bool isDirectional() const noexcept { return type == UnifiedLightTypes::Directional; }
    [[nodiscard]] bool isPoint() const noexcept { return type == UnifiedLightTypes::Point; }
    [[nodiscard]] bool isSpot() const noexcept { return type == UnifiedLightTypes::Spot; }

    // === Factory methods ===
    [[nodiscard]] static UnifiedLight createDirectional(
        const glm::vec3& dir,
        const glm::vec3& col = glm::vec3(1.0f),
        float inten = 1.0f
    ) {
        UnifiedLight light{};
        light.type = UnifiedLightTypes::Directional;
        light.direction = glm::normalize(dir);
        light.color = col;
        light.intensity = inten;
        light.shadowMapIndex = -1;
        light.lightSpaceMatrix = glm::mat4(1.0f);
        return light;
    }

    [[nodiscard]] static UnifiedLight createPoint(
        const glm::vec3& pos,
        const glm::vec3& col = glm::vec3(1.0f),
        float inten = 1.0f,
        float rng = 10.0f
    ) {
        UnifiedLight light{};
        light.type = UnifiedLightTypes::Point;
        light.position = pos;
        light.color = col;
        light.intensity = inten;
        light.range = rng;
        light.shadowMapIndex = -1;
        light.lightSpaceMatrix = glm::mat4(1.0f);
        return light;
    }

    [[nodiscard]] static UnifiedLight createSpot(
        const glm::vec3& pos,
        const glm::vec3& dir,
        float innerAngle,
        float outerAngle,
        const glm::vec3& col = glm::vec3(1.0f),
        float inten = 1.0f,
        float rng = 10.0f
    ) {
        UnifiedLight light{};
        light.type = UnifiedLightTypes::Spot;
        light.position = pos;
        light.direction = glm::normalize(dir);
        light.innerCone = innerAngle;
        light.outerCone = outerAngle;
        light.color = col;
        light.intensity = inten;
        light.range = rng;
        light.shadowMapIndex = -1;
        light.lightSpaceMatrix = glm::mat4(1.0f);
        return light;
    }
};

// =============================================================================
// COMPILE-TIME LAYOUT VALIDATION
// =============================================================================
static_assert(sizeof(UnifiedLight) == 128,
    "UnifiedLight must be exactly 128 bytes for GPU alignment");

static_assert(offsetof(UnifiedLight, position) == 0, "position offset mismatch");
static_assert(offsetof(UnifiedLight, type) == 12, "type offset mismatch");
static_assert(offsetof(UnifiedLight, color) == 16, "color offset mismatch");
static_assert(offsetof(UnifiedLight, intensity) == 28, "intensity offset mismatch");
static_assert(offsetof(UnifiedLight, direction) == 32, "direction offset mismatch");
static_assert(offsetof(UnifiedLight, range) == 44, "range offset mismatch");
static_assert(offsetof(UnifiedLight, innerCone) == 48, "innerCone offset mismatch");
static_assert(offsetof(UnifiedLight, outerCone) == 52, "outerCone offset mismatch");
static_assert(offsetof(UnifiedLight, shadowMapIndex) == 56, "shadowMapIndex offset mismatch");
static_assert(offsetof(UnifiedLight, _padding) == 60, "_padding offset mismatch");
static_assert(offsetof(UnifiedLight, lightSpaceMatrix) == 64, "lightSpaceMatrix offset mismatch");

// =============================================================================
// LIGHTING UBO STRUCTURE
// =============================================================================
struct LightingUBO {
    UnifiedLight lights[MAX_UNIFIED_LIGHTS];  ///< Array of unified lights (1024 bytes)
    int32_t numLights;                         ///< Number of active lights
    float shadowBias;                          ///< Global shadow bias
    float shadowStrength;                      ///< Global shadow strength (0-1)
    float _padding;                            ///< Alignment padding
};

static_assert(sizeof(LightingUBO) == 1040, "LightingUBO size mismatch");

// =============================================================================
// CSM UBO STRUCTURE
// =============================================================================
struct CSMUBO {
    CSMCascadeInfo cascades[MAX_CSM_CASCADES];  ///< Per-cascade data (320 bytes)
    glm::mat4 invView;                           ///< Inverse view matrix (64 bytes)
    float cascadeSplitDepths[4];                 ///< View-space split depths (16 bytes)
    int32_t numCascades;                         ///< Number of active cascades
    float shadowBias;                            ///< Shadow bias
    float normalBias;                            ///< Normal offset bias
    float _padding;                              ///< Alignment padding
};

static_assert(sizeof(CSMUBO) == 416, "CSMUBO size mismatch");

// =============================================================================
// LIGHT CONFIGURATION
// =============================================================================
struct LightConfig {
    float type = UnifiedLightTypes::Point;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    float innerCone = 30.0f;
    float outerCone = 45.0f;
    ShadowType shadowType = ShadowType::None;
};

} // namespace ohao
