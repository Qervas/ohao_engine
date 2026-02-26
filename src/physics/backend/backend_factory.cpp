#include "physics/backend/physics_backend.hpp"
#include <iostream>

#if OHAO_HAS_JOLT
#include "physics/backend/jolt/jolt_backend.hpp"
#endif

namespace ohao {
namespace physics {
namespace backend {

std::unique_ptr<IPhysicsBackend> createPhysicsBackend(const std::string& preferred) {
#if OHAO_HAS_JOLT
    if (preferred == "jolt" || preferred == "auto" || preferred.empty()) {
        try {
            auto jolt = std::make_unique<JoltPhysicsBackend>();
            std::cout << "[Physics] Created Jolt backend" << std::endl;
            return jolt;
        } catch (const std::exception& e) {
            std::cerr << "[Physics] Failed to create Jolt backend: " << e.what()
                      << ", falling back to null" << std::endl;
        } catch (...) {
            std::cerr << "[Physics] Failed to create Jolt backend (unknown error)"
                      << ", falling back to null" << std::endl;
        }
    }
#else
    if (preferred == "jolt") {
        std::cerr << "[Physics] Jolt backend requested but not compiled in"
                  << ", using null backend" << std::endl;
    }
#endif

    // Null backend - always available, zero dependencies
    std::cout << "[Physics] Using null backend (physics disabled)" << std::endl;
    return std::make_unique<NullPhysicsBackend>();
}

} // namespace backend
} // namespace physics
} // namespace ohao
