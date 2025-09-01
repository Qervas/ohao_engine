#include "force_debugger.hpp"
#include "physics/forces/force_registry.hpp"
#include "ui/components/console_widget.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

namespace ohao {
namespace physics {
namespace debug {

ForceDebugger::ForceDebugger() {
    initializeDefaultColors();
}

void ForceDebugger::startFrame() {
    if (m_frameActive) return;
    
    m_frameActive = true;
    m_frameStartTime = std::chrono::high_resolution_clock::now();
    
    // Clear previous frame data
    m_forceVectors.clear();
    m_torqueVectors.clear();
    m_bodyStats.clear();
    m_frameStats = FrameStats{};
}

void ForceDebugger::endFrame() {
    if (!m_frameActive) return;
    
    updateFrameStatistics();
    filterVectorsByMode();
    
    if (m_profilingEnabled) {
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - m_frameStartTime);
        m_profilingData.collectionTimeMs = duration.count() / 1000.0f;
    }
    
    m_frameActive = false;
}

void ForceDebugger::recordForceApplication(dynamics::RigidBody* body, 
                                         const glm::vec3& force, 
                                         const glm::vec3& applicationPoint,
                                         const std::string& sourceId) {
    if (!m_frameActive || !body) return;
    
    float magnitude = glm::length(force);
    if (magnitude < m_minMagnitude) return;
    
    ForceVector forceVec;
    forceVec.origin = applicationPoint;
    forceVec.direction = glm::normalize(force);
    forceVec.magnitude = magnitude;
    forceVec.color = getColorForForceType(sourceId);
    forceVec.sourceId = sourceId;
    forceVec.bodyName = getBodyName(body);
    
    m_forceVectors.push_back(forceVec);
    
    // Update body statistics
    auto bodyIt = std::find_if(m_bodyStats.begin(), m_bodyStats.end(),
                              [body](const BodyForceStats& stats) {
                                  return stats.bodyId == reinterpret_cast<size_t>(body);
                              });
    
    if (bodyIt == m_bodyStats.end()) {
        BodyForceStats newStats;
        newStats.bodyId = reinterpret_cast<size_t>(body);
        newStats.bodyName = getBodyName(body);
        newStats.netForce = force;
        newStats.totalForceApplied = magnitude;
        newStats.forceApplicationCount = 1;
        newStats.activeForces.push_back(sourceId);
        m_bodyStats.push_back(newStats);
    } else {
        bodyIt->netForce += force;
        bodyIt->totalForceApplied += magnitude;
        bodyIt->forceApplicationCount++;
        
        // Add source if not already present
        if (std::find(bodyIt->activeForces.begin(), bodyIt->activeForces.end(), sourceId) == 
            bodyIt->activeForces.end()) {
            bodyIt->activeForces.push_back(sourceId);
        }
    }
}

void ForceDebugger::recordTorqueApplication(dynamics::RigidBody* body,
                                          const glm::vec3& torque,
                                          const std::string& sourceId) {
    if (!m_frameActive || !body || !m_showTorques) return;
    
    float magnitude = glm::length(torque);
    if (magnitude < m_minMagnitude) return;
    
    TorqueVector torqueVec;
    torqueVec.center = body->getPosition();
    torqueVec.axis = glm::normalize(torque);
    torqueVec.magnitude = magnitude;
    torqueVec.color = getColorForForceType(sourceId);
    torqueVec.sourceId = sourceId;
    torqueVec.bodyName = getBodyName(body);
    
    m_torqueVectors.push_back(torqueVec);
    
    // Update body statistics
    auto bodyIt = std::find_if(m_bodyStats.begin(), m_bodyStats.end(),
                              [body](const BodyForceStats& stats) {
                                  return stats.bodyId == reinterpret_cast<size_t>(body);
                              });
    
    if (bodyIt != m_bodyStats.end()) {
        bodyIt->netTorque += torque;
    }
}

void ForceDebugger::analyzeForceRegistry(const forces::ForceRegistry& registry,
                                       const std::vector<dynamics::RigidBody*>& bodies) {
    if (!m_frameActive) return;
    
    auto analysisStart = std::chrono::high_resolution_clock::now();
    
    // Collect information about active force generators
    m_frameStats.activeForceGenerators = registry.getForceCount();
    m_frameStats.activeBodies = bodies.size();
    
    // Analyze each body's force state
    for (auto* body : bodies) {
        if (!body) continue;
        
        const auto& forceStats = body->getForceStats();
        
        // Record net forces if significant
        float netForceMagnitude = glm::length(forceStats.totalForceApplied);
        if (netForceMagnitude >= m_minMagnitude) {
            recordForceApplication(body, forceStats.totalForceApplied, 
                                 body->getPosition(), "net_force");
        }
        
        // Record net torques if significant
        float netTorqueMagnitude = glm::length(forceStats.totalTorqueApplied);
        if (netTorqueMagnitude >= m_minMagnitude) {
            recordTorqueApplication(body, forceStats.totalTorqueApplied, "net_torque");
        }
    }
    
    if (m_profilingEnabled) {
        auto analysisEnd = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(analysisEnd - analysisStart);
        m_profilingData.analysisTimeMs = duration.count() / 1000.0f;
    }
}

void ForceDebugger::resetStats() {
    m_frameStats = FrameStats{};
    m_profilingData = ProfilingData{};
}

void ForceDebugger::setForceTypeColor(const std::string& forceType, const glm::vec3& color) {
    m_forceTypeColors[forceType] = color;
}

glm::vec3 ForceDebugger::getForceTypeColor(const std::string& forceType) const {
    auto it = m_forceTypeColors.find(forceType);
    return (it != m_forceTypeColors.end()) ? it->second : glm::vec3(1.0f, 1.0f, 1.0f);
}

void ForceDebugger::logForceStatistics() const {
    std::stringstream log;
    log << "=== Force Debug Statistics ===\n";
    log << "Total forces applied: " << m_frameStats.totalForcesApplied << "\n";
    log << "Total torques applied: " << m_frameStats.totalTorquesApplied << "\n";
    log << "Max force magnitude: " << std::fixed << std::setprecision(2) << m_frameStats.maxForceMagnitude << "\n";
    log << "Max torque magnitude: " << std::fixed << std::setprecision(2) << m_frameStats.maxTorqueMagnitude << "\n";
    log << "Average force magnitude: " << std::fixed << std::setprecision(2) << m_frameStats.averageForceMagnitude << "\n";
    log << "Active bodies: " << m_frameStats.activeBodies << "\n";
    log << "Active force generators: " << m_frameStats.activeForceGenerators << "\n";
    
    if (m_profilingEnabled) {
        log << "\n=== Performance ===\n";
        log << "Collection time: " << std::fixed << std::setprecision(3) << m_profilingData.collectionTimeMs << " ms\n";
        log << "Analysis time: " << std::fixed << std::setprecision(3) << m_profilingData.analysisTimeMs << " ms\n";
        log << "Visualization time: " << std::fixed << std::setprecision(3) << m_profilingData.visualizationTimeMs << " ms\n";
    }
    
    OHAO_LOG_INFO(log.str());
}

void ForceDebugger::logBodyForceBreakdown() const {
    for (const auto& bodyStats : m_bodyStats) {
        std::stringstream log;
        log << "Body: " << bodyStats.bodyName << "\n";
        log << "  Net Force: (" << bodyStats.netForce.x << ", " << bodyStats.netForce.y << ", " << bodyStats.netForce.z << ")\n";
        log << "  Net Torque: (" << bodyStats.netTorque.x << ", " << bodyStats.netTorque.y << ", " << bodyStats.netTorque.z << ")\n";
        log << "  Total Force Applied: " << bodyStats.totalForceApplied << "\n";
        log << "  Force Applications: " << bodyStats.forceApplicationCount << "\n";
        log << "  Active Forces: ";
        for (size_t i = 0; i < bodyStats.activeForces.size(); ++i) {
            if (i > 0) log << ", ";
            log << bodyStats.activeForces[i];
        }
        log << "\n";
        
        OHAO_LOG_DEBUG(log.str());
    }
}

std::string ForceDebugger::generateForceReport() const {
    std::stringstream report;
    
    report << "OHAO Physics Engine - Force Debug Report\n";
    report << "========================================\n\n";
    
    // Summary statistics
    report << "SUMMARY STATISTICS\n";
    report << "------------------\n";
    report << "Total Forces Applied: " << m_frameStats.totalForcesApplied << "\n";
    report << "Total Torques Applied: " << m_frameStats.totalTorquesApplied << "\n";
    report << "Max Force Magnitude: " << m_frameStats.maxForceMagnitude << " N\n";
    report << "Max Torque Magnitude: " << m_frameStats.maxTorqueMagnitude << " N⋅m\n";
    report << "Average Force Magnitude: " << m_frameStats.averageForceMagnitude << " N\n";
    report << "Active Bodies: " << m_frameStats.activeBodies << "\n";
    report << "Active Force Generators: " << m_frameStats.activeForceGenerators << "\n\n";
    
    // Body breakdown
    report << "BODY FORCE BREAKDOWN\n";
    report << "--------------------\n";
    for (const auto& bodyStats : m_bodyStats) {
        report << "Body: " << bodyStats.bodyName << "\n";
        report << "  Net Force: [" << bodyStats.netForce.x << ", " << bodyStats.netForce.y << ", " << bodyStats.netForce.z << "] N\n";
        report << "  Net Torque: [" << bodyStats.netTorque.x << ", " << bodyStats.netTorque.y << ", " << bodyStats.netTorque.z << "] N⋅m\n";
        report << "  Total Force Applied: " << bodyStats.totalForceApplied << " N\n";
        report << "  Force Applications: " << bodyStats.forceApplicationCount << "\n";
        report << "  Active Forces: ";
        for (size_t i = 0; i < bodyStats.activeForces.size(); ++i) {
            if (i > 0) report << ", ";
            report << bodyStats.activeForces[i];
        }
        report << "\n\n";
    }
    
    // Force vector details
    report << "FORCE VECTORS\n";
    report << "-------------\n";
    for (const auto& force : m_forceVectors) {
        report << force.sourceId << " -> " << force.bodyName << ":\n";
        report << "  Origin: [" << force.origin.x << ", " << force.origin.y << ", " << force.origin.z << "]\n";
        report << "  Direction: [" << force.direction.x << ", " << force.direction.y << ", " << force.direction.z << "]\n";
        report << "  Magnitude: " << force.magnitude << " N\n\n";
    }
    
    // Performance data
    if (m_profilingEnabled) {
        report << "PERFORMANCE DATA\n";
        report << "----------------\n";
        report << "Collection Time: " << m_profilingData.collectionTimeMs << " ms\n";
        report << "Analysis Time: " << m_profilingData.analysisTimeMs << " ms\n";
        report << "Visualization Time: " << m_profilingData.visualizationTimeMs << " ms\n";
    }
    
    return report.str();
}

void ForceDebugger::saveForceReport(const std::string& filename) const {
    std::ofstream file(filename);
    if (file.is_open()) {
        file << generateForceReport();
        file.close();
        OHAO_LOG_INFO("Force report saved to: " + filename);
    } else {
        OHAO_LOG_ERROR("Failed to save force report to: " + filename);
    }
}

void ForceDebugger::initializeDefaultColors() {
    // Standard force type colors
    m_forceTypeColors["gravity"] = glm::vec3(1.0f, 1.0f, 0.0f);          // Yellow
    m_forceTypeColors["drag"] = glm::vec3(1.0f, 0.5f, 0.0f);             // Orange
    m_forceTypeColors["spring"] = glm::vec3(0.0f, 1.0f, 0.0f);           // Green
    m_forceTypeColors["explosion"] = glm::vec3(1.0f, 0.0f, 0.0f);        // Red
    m_forceTypeColors["wind"] = glm::vec3(0.0f, 0.8f, 1.0f);             // Light Blue
    m_forceTypeColors["buoyancy"] = glm::vec3(0.0f, 0.4f, 1.0f);         // Blue
    m_forceTypeColors["magnetic"] = glm::vec3(1.0f, 0.0f, 1.0f);         // Magenta
    m_forceTypeColors["vortex"] = glm::vec3(0.5f, 0.0f, 1.0f);           // Purple
    m_forceTypeColors["net_force"] = glm::vec3(1.0f, 1.0f, 1.0f);        // White
    m_forceTypeColors["net_torque"] = glm::vec3(0.8f, 0.8f, 0.8f);       // Light Gray
}

glm::vec3 ForceDebugger::getColorForForceType(const std::string& forceType) const {
    auto it = m_forceTypeColors.find(forceType);
    if (it != m_forceTypeColors.end()) {
        return it->second;
    }
    
    // Generate a color based on string hash for unknown types
    std::hash<std::string> hasher;
    size_t hash = hasher(forceType);
    float r = ((hash >> 16) & 0xFF) / 255.0f;
    float g = ((hash >> 8) & 0xFF) / 255.0f;
    float b = (hash & 0xFF) / 255.0f;
    return glm::vec3(r, g, b);
}

std::string ForceDebugger::getBodyName(dynamics::RigidBody* body) const {
    if (!body) return "Unknown";
    
    // Try to get name from component
    auto* component = body->getComponent();
    if (component) {
        // In a real implementation, this would get the entity/component name
        return "Body_" + std::to_string(reinterpret_cast<size_t>(component));
    }
    
    return "Body_" + std::to_string(reinterpret_cast<size_t>(body));
}

void ForceDebugger::updateFrameStatistics() {
    m_frameStats.totalForcesApplied = m_forceVectors.size();
    m_frameStats.totalTorquesApplied = m_torqueVectors.size();
    
    // Calculate max and average force magnitudes
    float totalMagnitude = 0.0f;
    for (const auto& force : m_forceVectors) {
        m_frameStats.maxForceMagnitude = std::max(m_frameStats.maxForceMagnitude, force.magnitude);
        totalMagnitude += force.magnitude;
    }
    
    if (!m_forceVectors.empty()) {
        m_frameStats.averageForceMagnitude = totalMagnitude / m_forceVectors.size();
    }
    
    // Calculate max torque magnitude
    for (const auto& torque : m_torqueVectors) {
        m_frameStats.maxTorqueMagnitude = std::max(m_frameStats.maxTorqueMagnitude, torque.magnitude);
    }
}

void ForceDebugger::filterVectorsByMode() {
    switch (m_vizMode) {
        case VisualizationMode::NET_FORCES_ONLY:
            // Remove individual forces, keep only net forces
            m_forceVectors.erase(
                std::remove_if(m_forceVectors.begin(), m_forceVectors.end(),
                              [](const ForceVector& force) {
                                  return force.sourceId != "net_force";
                              }),
                m_forceVectors.end());
            break;
            
        case VisualizationMode::ABOVE_THRESHOLD:
            // Keep only forces above threshold (already filtered during recording)
            break;
            
        case VisualizationMode::BY_TYPE:
            // Sort by force type for easier visualization
            std::sort(m_forceVectors.begin(), m_forceVectors.end(),
                     [](const ForceVector& a, const ForceVector& b) {
                         return a.sourceId < b.sourceId;
                     });
            break;
            
        case VisualizationMode::ALL_FORCES:
        default:
            // No filtering needed
            break;
    }
}

} // namespace debug
} // namespace physics
} // namespace ohao