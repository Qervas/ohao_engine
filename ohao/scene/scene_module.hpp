#pragma once

/**
 * Umbrella for the scene subsystem (prefer specific headers in production TUs).
 * Art bar: C++20 string_view APIs, ComponentType concepts, MeshBufferInfo, Result loaders.
 */

#include "scene/actor/actor.hpp"
#include "scene/asset/model.hpp"
#include "scene/asset/model_loader.hpp"
#include "scene/asset/primitive_mesh_generator.hpp"
#include "scene/component/component.hpp"
#include "scene/component/component_factory.hpp"
#include "scene/component/component_pack.hpp"
#include "scene/component/light_component.hpp"
#include "scene/component/material_component.hpp"
#include "scene/component/mesh_component.hpp"
#include "scene/component/transform_component.hpp"
#include "scene/default_scene_factory.hpp"
#include "scene/scene.hpp"
#include "scene/scene_object.hpp"
#include "scene/transform.hpp"
