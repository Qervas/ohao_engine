#pragma once
#include <functional>
#include <memory>
#include <vector>
#include "gpu/vulkan/renderer.hpp"
#include "runtime/clock.hpp"
#include "runtime/input.hpp"
#include "runtime/window.hpp"

namespace ohao::runtime {

class Application;

// The game's entry points into the loop. Deliberately minimal: the ECS work
// replaces these with ordered systems.
struct GameHooks {
    std::function<void(Application&)> start;
    std::function<void(Application&, float fixedDt)> fixedUpdate;               // simulation
    std::function<void(Application&, float frameDt, float alpha)> update;       // per frame
};

struct AppConfig {
    WindowConfig window;
    bool vsync{false};
    ClockConfig clock;
    int frameLimit{0};                // 0 = run until closed
    int resizeAtFrame{-1};            // scripted resize, for the presentation gate
    VkExtent2D resizeTo{1280, 720};
};

struct RunReport {
    uint64_t frames{0};
    uint64_t readbackCopies{0};
    uint64_t validationMessages{0};
    bool     validationEnabled{false};
    double   avgFrameMs{0.0};
    double   avgGpuMs{0.0};
    double   fps{0.0};
};

// Owns the window, input and renderer and runs the loop:
//   poll -> input -> N x fixedUpdate (fixed step) -> update -> render/present
// Minimized windows keep simulating and skip rendering.
class Application {
public:
    Application() = default;
    ~Application() { shutdown(); }
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool initialize(const AppConfig& cfg);
    RunReport run(const GameHooks& hooks);
    void shutdown();

    [[nodiscard]] VulkanRenderer& renderer() { return *m_renderer; }
    [[nodiscard]] InputMap& input() { return m_input; }
    [[nodiscard]] Window& window() { return m_window; }
    void quit() { m_quit = true; }

private:
    AppConfig m_cfg;
    Window m_window;
    InputMap m_input;
    std::unique_ptr<VulkanRenderer> m_renderer;
    FixedStepClock m_clock;
    FrameStats m_stats;
    std::vector<InputEvent> m_events;
    bool m_quit{false};
};

} // namespace ohao::runtime
