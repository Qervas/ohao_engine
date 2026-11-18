// The visual module's registrations (visual/module.hpp), a part of types.cpp's: the camera, the environment and
// InstantiateError.
#include "visual/components.hpp"
#include "visual/module.hpp"
#include "ecs/scene_types.hpp"

namespace ohao::ecs {

void registerVisualTypesView(reflect::TypeRegistry& r) {
    r.type<Camera>("Camera").component().singleton().host(makeComponentOps<Camera>())
        .doc("The scene camera (one per world). Its pose is the entity's world transform: it looks along -Z with +Y "
             "up; roll is ignored.")
        .field("fovDeg", &Camera::fovDeg).doc("Vertical field of view").unit("deg").angle().min(1.0).max(179.0)
        .field("nearPlane", &Camera::nearPlane).doc("Near clip distance (raster passes)").unit("m").min(0.0001)
        .field("farPlane", &Camera::farPlane).doc("Far clip distance (raster passes); keep it above nearPlane")
            .unit("m").min(0.001)
        .field("active", &Camera::active).doc("false: the host keeps its own camera");
    r.type<Environment>("Environment").component().singleton().host(makeComponentOps<Environment>())
        .doc("What lights and backs the scene from afar (one per world): an equirectangular HDR image, else the "
             "physical sky (Hillaire) lit by the scene's first directional light. A world without one has the sky.")
        .field("hdri", &Environment::hdri).doc("Project-relative path of the .hdr image; null for the sky")
            .asset("environment image")
        .field("intensity", &Environment::intensity).doc("Multiplier on the environment's radiance").unitless().min(0.0)
            .uiRange(0.0, 10.0)
        .field("sky", &Environment::sky)
            .doc("Without an hdri: the physical sky for the scene's sun (none without a directional light); false: "
                 "no environment (black)");
    r.type<InstantiateError>("InstantiateError").component().runtime().host(makeComponentOps<InstantiateError>())
        .doc("Why instantiate could not build the entity's mesh. The entity then has no MeshRenderer.")
        .field("code", &InstantiateError::code).doc("E_ASSET_NOT_FOUND, E_ASSET_INVALID or E_MESH_INVALID")
        .field("message", &InstantiateError::message).doc("What went wrong, for people and agents");
}

} // namespace ohao::ecs
