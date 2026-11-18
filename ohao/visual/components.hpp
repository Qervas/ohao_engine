#pragma once
// The visual module's components (visual/module.hpp): what an entity looks like (its mesh's source, its material, a
// light) and where the world is seen from (the camera, the environment). Data only: the renderer is world_render's.
#include "reflect/asset_ref.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include "ecs/storage.hpp"   // OHAO_EXTERN_STORAGE: the storages below are compiled once

namespace ohao::ecs {

using AssetRef = reflect::AssetRef;   // as ecs/components.hpp names it

enum class LightType : uint8_t { Point, Directional, Spot, AreaRect };

// Position and direction come from WorldTransform: direction is the entity's -Z.
struct Light {
    LightType type{LightType::Point};
    glm::vec3 color{1.0f};
    float intensity{1.0f};
    float range{10.0f};
    float radius{0.1f};
    float innerConeDeg{30.0f};
    float outerConeDeg{45.0f};
    glm::vec3 areaEdge1{1.0f, 0.0f, 0.0f};   // AreaRect, local space
    glm::vec3 areaEdge2{0.0f, 0.0f, 1.0f};
    bool castShadows{true};
};

// ---- meshes from data (instantiate, world_render/instantiate.hpp) ------------------------------------

struct ModelAsset   { AssetRef asset; std::string mesh; };   // mesh: one glTF mesh (name or #index); empty: the file
struct SpherePrim   { float radius{0.5f}; int32_t sectors{32}; int32_t stacks{16}; };
struct CubePrim     { float size{1.0f}; };
struct PlatformPrim { float width{2.0f}; float height{0.4f}; float depth{2.0f}; };
struct CylinderPrim { float radius{0.5f}; float height{1.0f}; int32_t segments{16}; };
struct ConePrim     { float radius{0.5f}; float height{1.0f}; int32_t segments{16}; };
// The examples' builders, made reusable (vertex color = color, uv 0, tangent (1,0,0,1)).
// Defaults: a unit quad facing +Z, a unit box around the origin.
struct QuadPrim {
    glm::vec3 a{-0.5f, -0.5f, 0.0f};
    glm::vec3 b{0.5f, -0.5f, 0.0f};
    glm::vec3 c{0.5f, 0.5f, 0.0f};
    glm::vec3 d{-0.5f, 0.5f, 0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec3 color{1.0f};
};
struct BoxPrim { glm::vec3 lo{-0.5f}; glm::vec3 hi{0.5f}; glm::vec3 color{1.0f}; };

// Where an entity's mesh comes from. Instantiate builds MeshRenderer from it; meshes are shared per
// distinct value.
struct MeshSource {
    std::variant<ModelAsset, SpherePrim, CubePrim, PlatformPrim, CylinderPrim, ConePrim, QuadPrim, BoxPrim> source;
};

// A material's alpha mode (render::AlphaMode): opaque, alpha-tested (Mask: cut below 0.5),
// or blended (Blend: drawn by the sorted forward pass).
enum class MaterialAlpha : uint8_t { Opaque, Mask, Blend };

// The material of a mesh without its own materials (primitives); for a model with
// materials it only feeds the legacy RT GI word. OpenPBR Surface 1.1.1's base
// (docs/specs/2026-10-29-openpbr-materials-design.md): the fields are saved under OpenPBR's
// names (base_color, specular_roughness, ...) with its defaults.
struct Material {
    glm::vec3 baseColor{0.8f};          // base_color
    float baseWeight{1.0f};             // base_weight
    float baseMetalness{0.0f};          // base_metalness
    float baseDiffuseRoughness{0.0f};   // base_diffuse_roughness
    float specularWeight{1.0f};         // specular_weight
    glm::vec3 specularColor{1.0f};      // specular_color
    float specularRoughness{0.3f};      // specular_roughness
    float specularIor{1.5f};            // specular_ior
    float emissionLuminance{0.0f};      // emission_luminance (the engine's radiance units, ruling O4)
    glm::vec3 emissionColor{1.0f};      // emission_color
    float subsurfaceWeight{0.0f};       // subsurface_weight: the dielectric's diffuse slot that scatters under the surface
    MaterialAlpha alphaMode{MaterialAlpha::Opaque};   // alpha_mode
    float geometryOpacity{1.0f};        // geometry_opacity: coverage (Blend's factor; Mask cuts below 0.5)
    // OpenPBR step 2: the coat, the fuzz and specular anisotropy.
    float specularRoughnessAnisotropy{0.0f};   // specular_roughness_anisotropy
    float coatWeight{0.0f};             // coat_weight
    glm::vec3 coatColor{1.0f};          // coat_color
    float coatRoughness{0.0f};          // coat_roughness
    float coatRoughnessAnisotropy{0.0f};   // coat_roughness_anisotropy
    float coatIor{1.6f};                // coat_ior
    float coatDarkening{1.0f};          // coat_darkening
    float fuzzWeight{0.0f};             // fuzz_weight
    glm::vec3 fuzzColor{1.0f};          // fuzz_color
    float fuzzRoughness{0.5f};          // fuzz_roughness
    float geometryTangentRotation{0.0f};   // geometry_tangent_rotation (turns; ruling O9)
    // OpenPBR step 3: transmission, its volume and dispersion, thin-walled, the thin film.
    float transmissionWeight{0.0f};     // transmission_weight
    glm::vec3 transmissionColor{1.0f};  // transmission_color
    float transmissionDepth{0.0f};      // transmission_depth (m; 0: the colour tints the surface)
    float transmissionDispersionScale{0.0f};        // transmission_dispersion_scale
    float transmissionDispersionAbbeNumber{20.0f};  // transmission_dispersion_abbe_number
    bool geometryThinWalled{false};     // geometry_thin_walled
    float thinFilmWeight{0.0f};         // thin_film_weight
    float thinFilmThickness{0.5f};      // thin_film_thickness (micrometres)
    float thinFilmIor{1.4f};            // thin_film_ior
    // OpenPBR step 4: subsurface scattering (subsurface_weight above).
    glm::vec3 subsurfaceColor{0.8f};    // subsurface_color
    float subsurfaceRadius{1.0f};       // subsurface_radius (m; ruling O15)
    glm::vec3 subsurfaceRadiusScale{1.0f, 0.5f, 0.25f};   // subsurface_radius_scale
    float subsurfaceScatterAnisotropy{0.0f};             // subsurface_scatter_anisotropy
};

// Why instantiate (or a level) could not build an entity's mesh (no MeshRenderer then).
struct InstantiateError {
    std::string code;      // E_ASSET_NOT_FOUND, E_ASSET_INVALID, E_MESH_INVALID
    std::string message;
};

// ---- the view (visual/scene_view.hpp; applied to a renderer by the host) ---------------------

// The scene camera, one per world. Its pose is the entity's world transform: the view
// looks along -Z with +Y up (roll is ignored; the renderer camera is pitch/yaw).
struct Camera {
    float fovDeg{45.0f};       // vertical field of view
    float nearPlane{0.1f};
    float farPlane{100.0f};
    bool active{true};         // false: the host keeps its own camera
};

// Image-based lighting, one per world. No hdri: the renderer's default sky/black.
struct Environment {
    std::optional<AssetRef> hdri;
    float intensity{1.0f};
    bool sky{true};   // no hdri: the physical sky, lit by the scene's sun (render/sky/atmosphere.hpp)
};

// Runtime, render only: the entity and its subtree are drawn first person (a gun in the view): in no shadow cascade
// and seen by camera rays only (render::DrawDesc::viewModel). A game adds it (the slice: to every ViewModel). Never
// saved or hashed.
struct FirstPerson {};

// Runtime, render only (sub-project 10 step 3): the field of view a live frame draws a Camera with,
// in place of Camera.fovDeg: aiming down the sights narrows it (the `view_model` sync system writes it
// on the view's camera in realtime frames only, and removes it otherwise, so lockstep frames and every
// capture keep Camera.fovDeg). Never saved or hashed.
struct DrawnFov {
    float deg{45.0f};
};

} // namespace ohao::ecs

// The storages of these components, compiled once (visual/storage.cpp).
OHAO_EXTERN_STORAGE(ohao::ecs::Camera);
OHAO_EXTERN_STORAGE(ohao::ecs::DrawnFov);
OHAO_EXTERN_STORAGE(ohao::ecs::Environment);
OHAO_EXTERN_STORAGE(ohao::ecs::FirstPerson);
OHAO_EXTERN_STORAGE(ohao::ecs::InstantiateError);
OHAO_EXTERN_STORAGE(ohao::ecs::Light);
OHAO_EXTERN_STORAGE(ohao::ecs::Material);
OHAO_EXTERN_STORAGE(ohao::ecs::MeshSource);
