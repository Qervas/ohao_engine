#pragma once

#include "physics/dynamics/rigid_body.hpp"
#include "physics/forces/force_registry.hpp"
#include "physics/debug/force_debugger.hpp"
#include "physics/utils/physics_math.hpp"
#include "physics/world/profile_manager.hpp"
#include "physics/backend/physics_backend.hpp"

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

    // Body access (for profile system)
    const std::vector<std::shared_ptr<dynamics::RigidBody>>& getRigidBodies() const { return m_rigidBodies; }

    // Resolve a backend BodyHandle to the owning actor's name ("" if not found)
    std::string resolveHandleName(backend::BodyHandle handle) const;

    // Test-only: Add raw rigid body without component (for Python tests)
    void addRigidBodyForTesting(std::shared_ptr<dynamics::RigidBody> body);
    
    // Backward compatibility methods
    size_t getRigidBodyCount() const { return getBodyCount(); }
    void stepSimulation(float deltaTime) { step(deltaTime); }
    
    // Initialize overloads for backward compatibility
    template<typename T>
    bool initialize(const T& /*unused_settings*/) { initialize(); return true; }
    
    // === RAYCASTING & QUERIES ===
    bool castRay(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                 backend::RaycastHit& outHit, uint16_t layerMask = backend::CollisionLayer::ALL_MASK) const;
    std::vector<backend::RaycastHit> castRayAll(const glm::vec3& origin, const glm::vec3& direction,
                                                 float maxDistance, uint16_t layerMask = backend::CollisionLayer::ALL_MASK) const;
    bool castSphere(const glm::vec3& origin, const glm::vec3& direction, float radius,
                    float maxDistance, backend::ShapeCastResult& outHit,
                    uint16_t layerMask = backend::CollisionLayer::ALL_MASK) const;
    bool castBox(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& halfExtents,
                 const glm::quat& rotation, float maxDistance, backend::ShapeCastResult& outHit,
                 uint16_t layerMask = backend::CollisionLayer::ALL_MASK) const;
    std::vector<backend::BodyHandle> overlapSphere(const glm::vec3& center, float radius,
                                                    uint16_t layerMask = backend::CollisionLayer::ALL_MASK) const;
    std::vector<backend::BodyHandle> overlapBox(const glm::vec3& center, const glm::vec3& halfExtents,
                                                 const glm::quat& rotation,
                                                 uint16_t layerMask = backend::CollisionLayer::ALL_MASK) const;

    // === CONTACT CALLBACKS ===
    void setContactListener(backend::IContactListener* listener);
    std::vector<backend::ContactEvent> getContactEvents();

    // === COLLISION LAYERS ===
    void setLayerCollision(uint16_t layer1, uint16_t layer2, bool shouldCollide);
    // Assign a body to a layer (changes its collision layer in the backend)
    void setBodyLayer(backend::BodyHandle handle, uint16_t layer);

    // === CONSTRAINTS ===
    backend::ConstraintHandle createConstraint(const backend::ConstraintSettings& settings);
    void destroyConstraint(backend::ConstraintHandle handle);
    void setConstraintEnabled(backend::ConstraintHandle handle, bool enabled);
    void setConstraintMotorState(backend::ConstraintHandle handle, bool enabled, float speed, float maxForce);
    void setConstraintLimits(backend::ConstraintHandle handle, float min, float max);
    void setConstraintBreaking(backend::ConstraintHandle handle, float maxForce, float maxTorque);
    std::vector<backend::ConstraintHandle> getAndClearBrokenConstraints();

    // === CHARACTER CONTROLLER ===
    backend::CharacterHandle createCharacter(const backend::CharacterCreationInfo& info);
    void destroyCharacter(backend::CharacterHandle handle);
    backend::CharacterState getCharacterState(backend::CharacterHandle handle) const;
    void setCharacterPosition(backend::CharacterHandle handle, const glm::vec3& pos);
    void setCharacterRotation(backend::CharacterHandle handle, const glm::quat& rot);
    void setCharacterLinearVelocity(backend::CharacterHandle handle, const glm::vec3& vel);
    void updateCharacter(backend::CharacterHandle handle, float deltaTime,
                          const glm::vec3& gravity, const glm::vec3& movementInput);

    // Configuration
    void setConfig(const PhysicsWorldConfig& config);
    const PhysicsWorldConfig& getConfig() const { return m_config; }
    
    void setGravity(const glm::vec3& gravity);
    glm::vec3 getGravity() const { return m_config.gravity; }
    
    void setTimeStep(float timeStep);
    float getTimeStep() const { return m_config.timeStep; }
    
    // Force system access
    forces::ForceRegistry& getForceRegistry() { return m_forceRegistry; }
    const forces::ForceRegistry& getForceRegistry() const { return m_forceRegistry; }

    // Profile system access
    ProfileManager* getProfileManager() { return m_profileManager.get(); }
    const ProfileManager* getProfileManager() const { return m_profileManager.get(); }
    
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

    // === BACKEND (Plugin System) ===
    backend::IPhysicsBackend* getBackend() { return m_backend.get(); }
    const backend::IPhysicsBackend* getBackend() const { return m_backend.get(); }
    bool hasBackend() const { return m_backend && m_backend->isInitialized(); }
    const char* getBackendName() const { return m_backend ? m_backend->getName() : "none"; }

    // Register a body with the backend (called after shape is set)
    void registerBodyWithBackend(dynamics::RigidBody* body);

    // Eagerly push all pending (no-backend-body-yet) rigid bodies into the backend.
    // Call this after finish_sync() so get_actor_body_handle() returns valid handles
    // before physics stepping starts (needed for constraint creation).
    void flushPendingBodies() { syncPendingBodiesToBackend(); }

    // === FORCE UTILITIES ===
    // Radial impulse — applies an outward impulse to all dynamic bodies within radius.
    // falloff: 0=linear, 1=quadratic, 2=constant
    void applyRadialImpulse(const glm::vec3& center, float strength, float radius, int falloff = 0);

    // Global wind — persistent directional force on all dynamic bodies. turbulence 0..1.
    void setWind(const glm::vec3& direction, float strength, float turbulence = 0.1f);
    void clearWind();

    // Global buoyancy — water plane at liquidLevel Y. density: water=1000, oil=900.
    void setWater(float liquidLevel, float density = 1000.0f, float drag = 0.1f);
    void clearWater();

    // Force volumes — spatial AABB/sphere regions that apply a force to bodies inside.
    // Returns a handle (>0) usable with destroyForceVolume / setForceVolumeEnabled.
    int createForceVolumeBox(const glm::vec3& center, const glm::vec3& halfExtents,
                             const glm::vec3& force);
    int createForceVolumeSphere(const glm::vec3& center, float radius,
                                const glm::vec3& force);
    void destroyForceVolume(int handle);
    void setForceVolumeEnabled(int handle, bool enabled);

    // === SPRINGS ===
    // Body-to-body spring (both ends attached to dynamic bodies)
    int createSpring(dynamics::RigidBody* bodyA, dynamics::RigidBody* bodyB,
                     float stiffness, float restLength, float damping);
    // Anchored spring (one end fixed in world space)
    int createAnchorSpring(dynamics::RigidBody* body, const glm::vec3& anchor,
                           float stiffness, float restLength, float damping);
    void destroySpring(int handle);
    void setSpringEnabled(int handle, bool enabled);

private:
    PhysicsWorldConfig m_config;
    SimulationState m_state{SimulationState::STOPPED};

    // Fixed timestep accumulator
    float m_timestepAccumulator{0.0f};
    float m_fixedTimestep{1.0f / 60.0f};  // 60 Hz physics

    // Force system
    forces::ForceRegistry m_forceRegistry;
    size_t m_windForceId{0};    // 0 = not registered
    size_t m_waterForceId{0};   // 0 = not registered
    std::unordered_map<int, size_t> m_forceVolumeMap;  // user handle -> registry id
    int m_nextForceVolumeHandle{1};
    std::unordered_map<int, size_t> m_springMap;        // user handle -> registry id
    int m_nextSpringHandle{1};

    // Profile system
    std::unique_ptr<ProfileManager> m_profileManager;

    // Body management
    std::vector<std::shared_ptr<dynamics::RigidBody>> m_rigidBodies;
    std::vector<dynamics::RigidBody*> m_activeBodyPointers; // Cache for performance
    std::unordered_map<PhysicsComponent*, std::shared_ptr<dynamics::RigidBody>> m_componentToBody;
    
    // Threading
    std::mutex m_bodiesMutex;
    
    // Statistics and profiling
    PhysicsStats m_stats;
    DebugVisualization m_debugViz;
    std::chrono::high_resolution_clock::time_point m_stepStartTime;
    
    // Force debugging
    std::unique_ptr<debug::ForceDebugger> m_forceDebugger;
    bool m_forceDebuggingEnabled{false};

    // Physics backend (Jolt, null, etc.)
    std::unique_ptr<backend::IPhysicsBackend> m_backend;

    // Internal methods
    void updateActiveBodyPointers();
    void updateDebugVisualization();
    void updateStatistics();

    // Fixed timestep physics tick (delegates to backend)
    void stepFixed(float fixedDt);

    // Backend sync helpers
    void syncPendingBodiesToBackend();
    void syncBodiesFromBackend();
    backend::BodyCreationInfo buildCreationInfo(const dynamics::RigidBody* body) const;
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