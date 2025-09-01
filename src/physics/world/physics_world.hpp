#pragma once

#include "physics/dynamics/rigid_body.hpp"
#include "physics/dynamics/physics_integrator.hpp"
#include "physics/collision/collision_system.hpp"
#include "physics/constraints/constraint_solver.hpp"
#include "physics/forces/force_registry.hpp"
#include "physics/debug/force_debugger.hpp"
#include "physics/utils/physics_math.hpp"

#include <vector>
#include <memory>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <future>

namespace ohao {

// Forward declarations
class PhysicsComponent;

namespace physics {

// Physics world configuration
struct PhysicsWorldConfig {
    // Simulation parameters
    glm::vec3 gravity{0.0f, -9.81f, 0.0f};
    float timeStep{1.0f / 60.0f};
    int maxSubSteps{4};
    
    // Solver configuration
    constraints::ConstraintSolver::Config solverConfig;
    collision::CollisionSystem::Config collisionConfig;
    dynamics::PhysicsIntegrator::Config integratorConfig;
    
    // Performance settings
    bool enableMultithreading{true};
    int workerThreads{0}; // 0 = auto-detect
    bool enableSleeping{true};
    bool enableCCD{false}; // Continuous collision detection
    
    // Debug settings
    bool enableDebugVisualization{false};
    bool enableStatistics{true};
    bool enableProfiler{false};
    
    // Memory management
    size_t maxBodies{10000};
    size_t maxConstraints{50000};
    size_t initialBodyCapacity{100};
    
    // Default constructor with sensible defaults
    PhysicsWorldConfig();
};

// Simulation states
enum class SimulationState {
    STOPPED,
    RUNNING,
    PAUSED,
    STEPPING
};

// Enhanced physics world with multithreading and optimization
class PhysicsWorld {
public:
    explicit PhysicsWorld(const PhysicsWorldConfig& config = PhysicsWorldConfig{});
    ~PhysicsWorld();
    
    // Lifecycle
    void initialize();
    void shutdown();
    void reset();
    
    // Simulation control
    void step(float deltaTime);
    void stepOnce(); // Single timestep
    void pause();
    void resume();
    void stop();
    
    SimulationState getSimulationState() const { return m_state; }
    void setSimulationState(SimulationState state) { m_state = state; }
    
    // Body management
    std::shared_ptr<dynamics::RigidBody> createRigidBody(PhysicsComponent* component);
    void removeRigidBody(std::shared_ptr<dynamics::RigidBody> body);
    void removeRigidBody(dynamics::RigidBody* body);
    size_t getBodyCount() const { return m_rigidBodies.size(); }
    
    // Backward compatibility methods
    size_t getRigidBodyCount() const { return getBodyCount(); }
    void stepSimulation(float deltaTime) { step(deltaTime); }
    
    // Initialize overloads for backward compatibility
    template<typename T>
    bool initialize(const T& /*unused_settings*/) { initialize(); return true; }
    
    // Constraint management
    void addConstraint(std::unique_ptr<constraints::Constraint> constraint);
    void removeConstraint(constraints::Constraint* constraint);
    size_t getConstraintCount() const;
    
    // Configuration
    void setConfig(const PhysicsWorldConfig& config);
    const PhysicsWorldConfig& getConfig() const { return m_config; }
    
    void setGravity(const glm::vec3& gravity);
    glm::vec3 getGravity() const { return m_config.gravity; }
    
    void setTimeStep(float timeStep);
    float getTimeStep() const { return m_config.timeStep; }
    
    // Access to subsystems
    collision::CollisionSystem& getCollisionSystem() { return *m_collisionSystem; }
    const collision::CollisionSystem& getCollisionSystem() const { return *m_collisionSystem; }
    
    constraints::ConstraintManager& getConstraintManager() { return *m_constraintManager; }
    const constraints::ConstraintManager& getConstraintManager() const { return *m_constraintManager; }
    
    dynamics::PhysicsIntegrator& getIntegrator() { return *m_integrator; }
    const dynamics::PhysicsIntegrator& getIntegrator() const { return *m_integrator; }
    
    // Force system access
    forces::ForceRegistry& getForceRegistry() { return m_forceRegistry; }
    const forces::ForceRegistry& getForceRegistry() const { return m_forceRegistry; }
    
    // Convenience methods for force management
    size_t registerForce(std::unique_ptr<forces::ForceGenerator> generator, 
                        const std::string& name = "",
                        const std::vector<dynamics::RigidBody*>& targetBodies = {});
    bool unregisterForce(size_t forceId);
    void clearAllForces();
    
    // Setup common force configurations
    void setupEarthEnvironment();
    void setupSpaceEnvironment();
    void setupUnderwaterEnvironment();
    void setupGamePhysics();
    
    // Queries
    collision::CollisionQueries* getCollisionQueries() { return m_collisionQueries.get(); }
    
    // Statistics and profiling
    struct PhysicsStats {
        // Timing
        float totalTimeMs{0.0f};
        float collisionTimeMs{0.0f};
        float constraintTimeMs{0.0f};
        float integrationTimeMs{0.0f};
        float synchronizationTimeMs{0.0f};
        
        // Object counts
        size_t totalBodies{0};
        size_t activeBodies{0};
        size_t sleepingBodies{0};
        size_t totalConstraints{0};
        size_t activeConstraints{0};
        
        // Performance metrics
        size_t broadPhasePairs{0};
        size_t narrowPhasePairs{0};
        size_t contactManifolds{0};
        int solverIterations{0};
        
        // Memory usage
        size_t memoryUsageMB{0};
        
        // Threading
        int activeThreads{0};
    };
    
    const PhysicsStats& getStats() const { return m_stats; }
    void resetStats();
    
    // Debug visualization
    struct DebugVisualization {
        std::vector<math::AABB> bodyAABBs;
        std::vector<glm::vec3> contactPoints;
        std::vector<std::pair<glm::vec3, glm::vec3>> contactNormals;
        std::vector<std::pair<glm::vec3, glm::vec3>> constraintLines;
        std::vector<glm::vec3> centerOfMass;
        std::vector<std::pair<glm::vec3, glm::vec3>> velocityVectors;
    };
    
    const DebugVisualization& getDebugVisualization() const { return m_debugViz; }
    void enableDebugVisualization(bool enable);
    bool isDebugVisualizationEnabled() const { return m_config.enableDebugVisualization; }
    
    // Force debugging
    void enableForceDebugging(bool enable);
    bool isForceDebuggingEnabled() const;
    debug::ForceDebugger* getForceDebugger() { return m_forceDebugger.get(); }
    const debug::ForceDebugger* getForceDebugger() const { return m_forceDebugger.get(); }
    
    // Thread safety
    void lockBodies() { m_bodiesMutex.lock(); }
    void unlockBodies() { m_bodiesMutex.unlock(); }
    
    // Memory management
    void compactMemory(); // Defragment and optimize memory layout
    size_t getMemoryUsage() const; // Returns usage in bytes

private:
    PhysicsWorldConfig m_config;
    SimulationState m_state{SimulationState::STOPPED};
    
    // Core subsystems
    std::unique_ptr<collision::CollisionSystem> m_collisionSystem;
    std::unique_ptr<constraints::ConstraintManager> m_constraintManager;
    std::unique_ptr<dynamics::PhysicsIntegrator> m_integrator;
    std::unique_ptr<collision::CollisionQueries> m_collisionQueries;
    
    // Force system
    forces::ForceRegistry m_forceRegistry;
    
    // Body management
    std::vector<std::shared_ptr<dynamics::RigidBody>> m_rigidBodies;
    std::vector<dynamics::RigidBody*> m_activeBodyPointers; // Cache for performance
    std::unordered_map<PhysicsComponent*, std::shared_ptr<dynamics::RigidBody>> m_componentToBody;
    
    // Threading
    std::vector<std::thread> m_workerThreads;
    std::mutex m_bodiesMutex;
    std::mutex m_constraintsMutex;
    bool m_shutdownRequested{false};
    
    // Statistics and profiling
    PhysicsStats m_stats;
    DebugVisualization m_debugViz;
    std::chrono::high_resolution_clock::time_point m_stepStartTime;
    
    // Force debugging
    std::unique_ptr<debug::ForceDebugger> m_forceDebugger;
    bool m_forceDebuggingEnabled{false};
    
    // Internal methods
    void initializeSubsystems();
    void updateActiveBodyPointers();
    void updateDebugVisualization();
    void updateStatistics();
    
    // Multithreaded simulation pipeline
    void stepMultithreaded(float deltaTime);
    void stepSinglethreaded(float deltaTime);
    
    // Worker thread functions
    void collisionWorker(const std::vector<dynamics::RigidBody*>& bodies, float deltaTime);
    void integrationWorker(const std::vector<dynamics::RigidBody*>& bodies, float deltaTime);
    
    // Performance optimization
    void optimizeMemoryLayout();
    void cullInactiveBodies();
    void updateSpatialAcceleration();
    
    // Validation and debugging
    void validatePhysicsState();
    void checkForNanValues();
    void detectInfiniteLoops();
};

// Physics world factory with preset configurations
class PhysicsWorldFactory {
public:
    // Preset configurations
    static PhysicsWorldConfig createGameWorld();           // Optimized for real-time games
    static PhysicsWorldConfig createSimulationWorld();     // High precision for simulations
    static PhysicsWorldConfig createMobileWorld();         // Battery/performance optimized
    static PhysicsWorldConfig createVRWorld();            // Low latency for VR
    static PhysicsWorldConfig createDebugWorld();         // Full debugging enabled
    
    // Create world with preset
    static std::unique_ptr<PhysicsWorld> create(const PhysicsWorldConfig& config);
    
    // Auto-configuration based on system capabilities
    static PhysicsWorldConfig autoConfigureForSystem();
};

// Global physics world instance management
class PhysicsManager {
public:
    static PhysicsManager& getInstance();
    
    void initialize(const PhysicsWorldConfig& config = PhysicsWorldConfig{});
    void shutdown();
    
    PhysicsWorld* getWorld() { return m_world.get(); }
    const PhysicsWorld* getWorld() const { return m_world.get(); }
    
    bool isInitialized() const { return m_world != nullptr; }

private:
    PhysicsManager() = default;
    ~PhysicsManager() = default;
    
    std::unique_ptr<PhysicsWorld> m_world;
    
    // Non-copyable
    PhysicsManager(const PhysicsManager&) = delete;
    PhysicsManager& operator=(const PhysicsManager&) = delete;
};

// Physics profiler for performance analysis
class PhysicsProfiler {
public:
    struct ProfileSection {
        std::string name;
        float timeMs{0.0f};
        size_t callCount{0};
        float minTime{std::numeric_limits<float>::max()};
        float maxTime{0.0f};
        float avgTime{0.0f};
    };
    
    static void beginSection(const std::string& name);
    static void endSection(const std::string& name);
    static void reset();
    
    static const std::vector<ProfileSection>& getSections();
    static void logProfile();

private:
    static std::unordered_map<std::string, ProfileSection> s_sections;
    static std::unordered_map<std::string, std::chrono::high_resolution_clock::time_point> s_startTimes;
};

// RAII profiler helper
class ProfileScope {
public:
    explicit ProfileScope(const std::string& name) : m_name(name) {
        PhysicsProfiler::beginSection(m_name);
    }
    
    ~ProfileScope() {
        PhysicsProfiler::endSection(m_name);
    }

private:
    std::string m_name;
};

#define PROFILE_PHYSICS(name) ProfileScope _prof(name)

} // namespace physics
} // namespace ohao