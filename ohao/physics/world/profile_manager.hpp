#pragma once

#include "simulation_profile.hpp"
#include <string>
#include <string_view>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ohao {
namespace physics {

/**
 * ProfileManager - Manages multiple simulation profiles
 *
 * Handles creation, deletion, and switching between simulation profiles.
 * Tracks the active profile and provides API for profile management UI.
 *
 * Workflow:
 * 1. Scene loads → no profile yet
 * 2. User clicks Play → creates Profile 1 automatically
 * 3. Simulation runs
 * 4. User clicks Stop → updates Profile 1 with current state
 * 5. User adjusts objects, clicks Play → creates Profile 2
 * 6. User can reset to any profile
 */
class ProfileManager {
public:
    ProfileManager();
    ~ProfileManager() = default;

    // === PROFILE CREATION/DELETION ===

    /**
     * Create a new profile with current physics state
     * @param name Profile name (e.g., "Gravity Test")
     * @param bodies Current rigid bodies to snapshot
     * @return Pointer to created profile, or nullptr on failure
     */
    SimulationProfile* createProfile(std::string_view name,
                                     const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies);

    /**
     * Delete a profile
     * @param name Profile name to delete
     * @return true if profile was deleted, false if not found
     */
    [[nodiscard]] bool deleteProfile(std::string_view name);

    /**
     * Rename an existing profile
     * @param oldName Current profile name
     * @param newName New profile name
     * @return true if renamed successfully, false if profile not found or new name exists
     */
    [[nodiscard]] bool renameProfile(std::string_view oldName, std::string_view newName);

    // === PROFILE SELECTION ===

    /**
     * Set the active profile
     * @param name Profile name to make active
     */
    void setActiveProfile(std::string_view name);

    /**
     * Get the currently active profile
     * @return Pointer to active profile, or nullptr if no profile is active
     */
    SimulationProfile* getActiveProfile() { return m_activeProfile; }
    const SimulationProfile* getActiveProfile() const { return m_activeProfile; }

    /**
     * Check if there's an active profile
     */
    bool hasActiveProfile() const { return m_activeProfile != nullptr; }

    /**
     * Get list of all profile names (for UI dropdown)
     */
    const std::vector<std::string>& getProfileNames() const { return m_profileNames; }

    // === STATE MANAGEMENT ===

    /**
     * Capture current state to active profile
     * @param bodies Current rigid bodies to snapshot
     */
    void captureToActive(const std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies);

    /**
     * Restore state from active profile
     * @param bodies Rigid bodies to restore state to
     */
    void restoreFromActive(std::vector<std::shared_ptr<dynamics::RigidBody>>& bodies);

    // === QUERIES ===

    /**
     * Check if any profiles exist
     */
    bool hasProfiles() const { return !m_profiles.empty(); }

    /**
     * Get number of profiles
     */
    size_t getProfileCount() const { return m_profiles.size(); }

    /**
     * Get a specific profile by name
     */
    [[nodiscard]] SimulationProfile* getProfile(std::string_view name);
    [[nodiscard]] const SimulationProfile* getProfile(std::string_view name) const;

    /**
     * Check if a profile with given name exists
     */
    [[nodiscard]] bool profileExists(std::string_view name) const;

    /**
     * Generate a unique profile name (e.g., "Profile 1", "Profile 2")
     */
    std::string generateUniqueName() const;

private:
    std::unordered_map<std::string, std::unique_ptr<SimulationProfile>> m_profiles;
    SimulationProfile* m_activeProfile = nullptr;  // Non-owning pointer to active profile
    std::vector<std::string> m_profileNames;       // Cached for UI dropdown (in creation order)

    /**
     * Rebuild the cached profile names list
     */
    void rebuildProfileNamesList();
};

} // namespace physics
} // namespace ohao
