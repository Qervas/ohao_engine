#include "forces.hpp"

namespace ohao {
namespace physics {
namespace forces {

void ForcePresets::setupEarthEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies) {
    // Standard Earth gravity
    auto gravity = ForceFactory::createGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    registry.registerForce(std::move(gravity), "earth_gravity", bodies);
    
    // Light air resistance
    auto airDrag = ForceFactory::createAirDrag(1.0f);
    registry.registerForce(std::move(airDrag), "air_resistance", bodies);
}

void ForcePresets::setupSpaceEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies) {
    // No forces added - objects continue moving indefinitely
}

void ForcePresets::setupUnderwaterEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies) {
    // Reduced gravity effect underwater
    auto gravity = ForceFactory::createGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    registry.registerForce(std::move(gravity), "underwater_gravity", bodies);
    
    // Buoyancy force
    auto buoyancy = ForceFactory::createBuoyancy(1000.0f, 0.0f);
    registry.registerForce(std::move(buoyancy), "water_buoyancy", bodies);
    
    // Strong water drag
    auto waterDrag = ForceFactory::createWaterDrag(2.0f);
    registry.registerForce(std::move(waterDrag), "water_resistance", bodies);
}

void ForcePresets::setupWindyEnvironment(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies, 
                                        const glm::vec3& windDirection) {
    setupEarthEnvironment(registry, bodies);
    
    // Add wind force with turbulence
    auto wind = ForceFactory::createWind(windDirection, 15.0f, 0.2f);
    registry.registerForce(std::move(wind), "environmental_wind", bodies);
}

void ForcePresets::setupGamePhysics(ForceRegistry& registry, const std::vector<dynamics::RigidBody*>& bodies) {
    // Slightly reduced gravity for more floaty feel
    auto gravity = ForceFactory::createGravity(glm::vec3(0.0f, -7.0f, 0.0f));
    registry.registerForce(std::move(gravity), "game_gravity", bodies);
    
    // Light drag to prevent objects from sliding forever
    auto drag = ForceFactory::createLinearDrag(0.05f);
    registry.registerForce(std::move(drag), "game_drag", bodies);
}

} // namespace forces
} // namespace physics
} // namespace ohao