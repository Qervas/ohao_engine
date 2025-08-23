#pragma once

namespace ohao {
namespace physics {

enum class SimulationState {
    STOPPED = 0,
    RUNNING = 1,
    PAUSED = 2,
    STEPPING = 3  // Single step mode
};

} // namespace physics
} // namespace ohao