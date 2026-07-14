#pragma once

/**
 * Umbrella for the physics subsystem (prefer specific headers in production).
 * Art bar: C++20 handles/spans, ForceGeneratorLike / CollisionShapeLike concepts.
 * Jolt ABI and jolt_helpers remain frozen.
 */

#include "physics/backend/physics_backend.hpp"
#include "physics/collision/shapes/collision_shape.hpp"
#include "physics/common/physics_constants.hpp"
#include "physics/components/physics_component.hpp"
#include "physics/dynamics/rigid_body.hpp"
#include "physics/forces/force_generator.hpp"
#include "physics/forces/force_registry.hpp"
#include "physics/forces/forces.hpp"
#include "physics/material/physics_material.hpp"
#include "physics/utils/physics_math.hpp"
#include "physics/world/physics_world.hpp"
