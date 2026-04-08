#pragma once

#include "physics/dynamics/rigid_body.hpp"
#include "physics/forces/force_registry.hpp"
#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <glm/glm.hpp>

namespace ohao {
namespace physics {
namespace debug {

/**
 * Force Debugger for visualizing and analyzing forces in the physics world
 */
class ForceDebugger {
public:
    struct ForceVector {
        glm::vec3 origin;           // Point where force is applied
        glm::vec3 direction;        // Force direction (normalized)
        float magnitude;            // Force magnitude
        glm::vec3 color;           // Color for visualization
        std::string sourceId;       // ID of force generator
        std::string bodyName;       // Name/ID of affected body
    };
    
    struct TorqueVector {
        glm::vec3 center;           // Center of rotation
        glm::vec3 axis;             // Torque axis
        float magnitude;            // Torque magnitude
        glm::vec3 color;           // Color for visualization
        std::string sourceId;       // ID of force generator
        std::string bodyName;       // Name/ID of affected body
    };
    
    struct BodyForceStats {
        size_t bodyId;
        std::string bodyName;
        glm::vec3 netForce{0.0f};
        glm::vec3 netTorque{0.0f};
        float totalForceApplied{0.0f};
        size_t forceApplicationCount{0};
        std::vector<std::string> activeForces;
    };
    
    enum class VisualizationMode {
        ALL_FORCES,         // Show all force vectors
        NET_FORCES_ONLY,    // Show only net forces
        BY_TYPE,           // Group by force type
        ABOVE_THRESHOLD    // Only forces above magnitude threshold
    };
    
public:
    ForceDebugger();
    ~ForceDebugger() = default;
    
    // === VISUALIZATION CONTROL ===
    void setVisualizationMode(VisualizationMode mode) { m_vizMode = mode; }
    VisualizationMode getVisualizationMode() const { return m_vizMode; }
    
    void setForceScale(float scale) { m_forceScale = scale; }
    float getForceScale() const { return m_forceScale; }
    
    void setMinimumMagnitudeThreshold(float threshold) { m_minMagnitude = threshold; }
    float getMinimumMagnitudeThreshold() const { return m_minMagnitude; }
    
    void setShowTorques(bool show) { m_showTorques = show; }
    bool getShowTorques() const { return m_showTorques; }
    
    void setShowForceLabels(bool show) { m_showLabels = show; }
    bool getShowForceLabels() const { return m_showLabels; }
    
    // === DATA COLLECTION ===
    void startFrame();
    void endFrame();
    
    void recordForceApplication(dynamics::RigidBody* body, 
                              const glm::vec3& force, 
                              const glm::vec3& applicationPoint,
                              const std::string& sourceId);
    
    void recordTorqueApplication(dynamics::RigidBody* body,
                               const glm::vec3& torque,
                               const std::string& sourceId);
    
    void analyzeForceRegistry(const forces::ForceRegistry& registry,
                            const std::vector<dynamics::RigidBody*>& bodies);
    
    // === VISUALIZATION DATA ===
    const std::vector<ForceVector>& getForceVectors() const { return m_forceVectors; }
    const std::vector<TorqueVector>& getTorqueVectors() const { return m_torqueVectors; }
    const std::vector<BodyForceStats>& getBodyStats() const { return m_bodyStats; }
    
    // === STATISTICS ===
    struct FrameStats {
        size_t totalForcesApplied{0};
        size_t totalTorquesApplied{0};
        float maxForceMagnitude{0.0f};
        float maxTorqueMagnitude{0.0f};
        float averageForceMagnitude{0.0f};
        size_t activeBodies{0};
        size_t activeForceGenerators{0};
    };
    
    const FrameStats& getFrameStats() const { return m_frameStats; }
    void resetStats();
    
    // === FORCE TYPE COLORS ===
    void setForceTypeColor(const std::string& forceType, const glm::vec3& color);
    glm::vec3 getForceTypeColor(const std::string& forceType) const;
    
    // === DEBUGGING UTILITIES ===
    void logForceStatistics() const;
    void logBodyForceBreakdown() const;
    
    std::string generateForceReport() const;
    void saveForceReport(const std::string& filename) const;
    
    // === PERFORMANCE PROFILING ===
    void setProfilingEnabled(bool enabled) { m_profilingEnabled = enabled; }
    bool isProfilingEnabled() const { return m_profilingEnabled; }
    
    struct ProfilingData {
        float collectionTimeMs{0.0f};
        float analysisTimeMs{0.0f};
        float visualizationTimeMs{0.0f};
    };
    
    const ProfilingData& getProfilingData() const { return m_profilingData; }

private:
    // Visualization settings
    VisualizationMode m_vizMode{VisualizationMode::ALL_FORCES};
    float m_forceScale{0.1f};           // Scale factor for force vector visualization
    float m_minMagnitude{0.1f};         // Minimum force magnitude to display
    bool m_showTorques{true};
    bool m_showLabels{true};
    
    // Data storage
    std::vector<ForceVector> m_forceVectors;
    std::vector<TorqueVector> m_torqueVectors;
    std::vector<BodyForceStats> m_bodyStats;
    
    // Statistics
    FrameStats m_frameStats;
    bool m_profilingEnabled{false};
    ProfilingData m_profilingData;
    
    // Color mapping for force types
    std::unordered_map<std::string, glm::vec3> m_forceTypeColors;
    
    // Frame tracking
    bool m_frameActive{false};
    std::chrono::high_resolution_clock::time_point m_frameStartTime;
    
    // Helper methods
    void initializeDefaultColors();
    glm::vec3 getColorForForceType(const std::string& forceType) const;
    std::string getBodyName(dynamics::RigidBody* body) const;
    void updateFrameStatistics();
    void filterVectorsByMode();
};

/**
 * RAII helper for frame-based force debugging
 */
class ForceDebugFrame {
public:
    explicit ForceDebugFrame(ForceDebugger& debugger) : m_debugger(debugger) {
        m_debugger.startFrame();
    }
    
    ~ForceDebugFrame() {
        m_debugger.endFrame();
    }
    
private:
    ForceDebugger& m_debugger;
};

#define DEBUG_FORCE_FRAME(debugger) ForceDebugFrame _forceFrame(debugger)

} // namespace debug
} // namespace physics
} // namespace ohao