#include "physics/backend/physics_backend.hpp"
#include "core/log.hpp"
#include <exception>
#include <string_view>

#if OHAO_HAS_JOLT
#include "physics/backend/jolt/jolt_backend.hpp"
#endif

namespace ohao {
namespace physics {
namespace backend {

std::unique_ptr<IPhysicsBackend> createPhysicsBackend(std::string_view preferred) {
#if OHAO_HAS_JOLT
    if (preferred == "jolt" || preferred == "auto" || preferred.empty()) {
        try {
            auto jolt = std::make_unique<JoltPhysicsBackend>();
            OHAO_LOG(Info, physics, "Physics: created Jolt backend");
            return jolt;
        } catch (const std::exception& e) {
            OHAO_LOG(Error, physics, "Physics: failed to create Jolt backend: {}, falling back to null",
                     e.what());
        } catch (...) {
            OHAO_LOG(Error, physics, "Physics: failed to create Jolt backend (unknown error), falling back to null");
        }
    }
#else
    if (preferred == "jolt") {
        OHAO_LOG(Warn, physics, "Physics: Jolt backend requested but not compiled in, using null backend");
    }
#endif

    // Null backend - always available, zero dependencies
    OHAO_LOG(Info, physics, "Physics: using null backend (physics disabled)");
    return std::make_unique<NullPhysicsBackend>();
}

} // namespace backend
} // namespace physics
} // namespace ohao
