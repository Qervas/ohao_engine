#include "simulation_profile.hpp"
#include "core/log.hpp"
#include <sstream>
#include <iomanip>

namespace ohao {
namespace physics {

SimulationProfile::SimulationProfile(std::string_view name)
    : m_name(name),
      m_creationTime(std::chrono::system_clock::now()) {
}

void SimulationProfile::capture(const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies) {
    m_bodySnapshots.clear();
    m_bodyIdToIndex.clear();

    for (const auto& body : bodies) {
        if (!body) continue;

        BodySnapshot snapshot;
        snapshot.bodyId = body->getUniqueId();
        snapshot.position = body->getPosition();
        snapshot.rotation = body->getRotation();
        snapshot.linearVelocity = body->getLinearVelocity();
        snapshot.angularVelocity = body->getAngularVelocity();
        snapshot.accumulatedForce = body->getAccumulatedForce();
        snapshot.accumulatedTorque = body->getAccumulatedTorque();
        snapshot.isAwake = body->isAwake();

        m_bodyIdToIndex[snapshot.bodyId] = m_bodySnapshots.size();
        m_bodySnapshots.push_back(snapshot);
    }

    m_creationTime = std::chrono::system_clock::now();
    OHAO_LOG(Info, physics, "Profile '{}' captured {} bodies", m_name, m_bodySnapshots.size());
}

void SimulationProfile::restore(std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies) {
    int restoredCount = 0;

    for (auto& body : bodies) {
        if (!body) continue;

        uint32_t bodyId = body->getUniqueId();
        auto it = m_bodyIdToIndex.find(bodyId);
        if (it == m_bodyIdToIndex.end()) {
            OHAO_LOG(Warn, physics, "Body {} not found in profile '{}'", bodyId, m_name);
            continue;
        }

        const BodySnapshot& snapshot = m_bodySnapshots[it->second];

        // Restore all state
        body->setPosition(snapshot.position);
        body->setRotation(snapshot.rotation);
        body->setLinearVelocity(snapshot.linearVelocity);
        body->setAngularVelocity(snapshot.angularVelocity);

        // Clear forces first, then apply snapshot forces
        body->clearForces();
        body->applyForce(snapshot.accumulatedForce);
        body->applyTorque(snapshot.accumulatedTorque);

        body->setAwake(snapshot.isAwake);

        restoredCount++;
    }

    OHAO_LOG(Info, physics, "Profile '{}' restored {} bodies", m_name, restoredCount);
}

std::string SimulationProfile::getCreationTimeString() const {
    auto time_t = std::chrono::system_clock::to_time_t(m_creationTime);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

} // namespace physics
} // namespace ohao
