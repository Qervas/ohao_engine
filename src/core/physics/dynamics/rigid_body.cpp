#include "rigid_body.hpp"
#include "../../component/physics_component.hpp"
#include "../../component/transform_component.hpp"
#include "../../ui/components/console_widget.hpp"

namespace ohao {
namespace physics {
namespace dynamics {

RigidBody::RigidBody(PhysicsComponent* component) 
    : m_component(component) {
    // Initialize with default values
    // Transform will be synced when added to world
}

RigidBody::~RigidBody() {
    // Cleanup handled by PhysicsWorld
}

// === COMPONENT SYNC ===
void RigidBody::updateTransformComponent() {
    if (m_component) {
        auto transform = m_component->getTransformComponent();
        if (transform) {
            // TODO: Update transform component from physics state
            // transform->setPosition(m_position);
            // transform->setRotation(m_rotation);
        }
    }
}

} // namespace dynamics
} // namespace physics
} // namespace ohao