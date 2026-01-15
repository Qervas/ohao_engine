#include "profile_manager.hpp"
#include "ui/components/console_widget.hpp"

namespace ohao {
namespace physics {

ProfileManager::ProfileManager() {
}

SimulationProfile* ProfileManager::createProfile(const std::string& name,
                                                  const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies) {
    // Check if profile with this name already exists
    if (profileExists(name)) {
        OHAO_LOG("Error: Profile '" + name + "' already exists");
        return nullptr;
    }

    // Create new profile
    auto profile = std::make_unique<SimulationProfile>(name);
    profile->capture(bodies);

    // Store profile and update active pointer
    auto* profilePtr = profile.get();
    m_profiles[name] = std::move(profile);
    m_activeProfile = profilePtr;

    // Rebuild cached name list
    rebuildProfileNamesList();

    OHAO_LOG("Created profile: " + name);
    return profilePtr;
}

bool ProfileManager::deleteProfile(const std::string& name) {
    auto it = m_profiles.find(name);
    if (it == m_profiles.end()) {
        OHAO_LOG("Warning: Cannot delete non-existent profile '" + name + "'");
        return false;
    }

    // If deleting active profile, clear active pointer
    if (m_activeProfile && m_activeProfile->getName() == name) {
        m_activeProfile = nullptr;
    }

    m_profiles.erase(it);
    rebuildProfileNamesList();

    OHAO_LOG("Deleted profile: " + name);
    return true;
}

bool ProfileManager::renameProfile(const std::string& oldName, const std::string& newName) {
    // Check if old profile exists
    auto it = m_profiles.find(oldName);
    if (it == m_profiles.end()) {
        OHAO_LOG("Error: Profile '" + oldName + "' not found");
        return false;
    }

    // Check if new name already exists
    if (profileExists(newName)) {
        OHAO_LOG("Error: Profile '" + newName + "' already exists");
        return false;
    }

    // Update profile name
    it->second->setName(newName);

    // Move to new key in map
    auto profile = std::move(it->second);
    m_profiles.erase(it);
    m_profiles[newName] = std::move(profile);

    // Rebuild cached name list
    rebuildProfileNamesList();

    OHAO_LOG("Renamed profile: '" + oldName + "' -> '" + newName + "'");
    return true;
}

void ProfileManager::setActiveProfile(const std::string& name) {
    auto it = m_profiles.find(name);
    if (it == m_profiles.end()) {
        OHAO_LOG("Warning: Cannot set active profile to non-existent '" + name + "'");
        m_activeProfile = nullptr;
        return;
    }

    m_activeProfile = it->second.get();
    OHAO_LOG("Active profile set to: " + name);
}

void ProfileManager::captureToActive(const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies) {
    if (!m_activeProfile) {
        OHAO_LOG("Warning: No active profile to capture to");
        return;
    }

    m_activeProfile->capture(bodies);
}

void ProfileManager::restoreFromActive(std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies) {
    if (!m_activeProfile) {
        OHAO_LOG("Warning: No active profile to restore from");
        return;
    }

    m_activeProfile->restore(bodies);
}

SimulationProfile* ProfileManager::getProfile(const std::string& name) {
    auto it = m_profiles.find(name);
    if (it == m_profiles.end()) {
        return nullptr;
    }
    return it->second.get();
}

const SimulationProfile* ProfileManager::getProfile(const std::string& name) const {
    auto it = m_profiles.find(name);
    if (it == m_profiles.end()) {
        return nullptr;
    }
    return it->second.get();
}

bool ProfileManager::profileExists(const std::string& name) const {
    return m_profiles.find(name) != m_profiles.end();
}

std::string ProfileManager::generateUniqueName() const {
    int profileNumber = 1;
    std::string name;

    do {
        name = "Profile " + std::to_string(profileNumber);
        profileNumber++;
    } while (profileExists(name));

    return name;
}

void ProfileManager::rebuildProfileNamesList() {
    m_profileNames.clear();
    m_profileNames.reserve(m_profiles.size());

    for (const auto& [name, profile] : m_profiles) {
        m_profileNames.push_back(name);
    }

    // Sort alphabetically for consistent UI ordering
    std::sort(m_profileNames.begin(), m_profileNames.end());
}

} // namespace physics
} // namespace ohao
