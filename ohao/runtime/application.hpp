#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "runtime/clock.hpp"
#include "runtime/input.hpp"
#include "runtime/window.hpp"

namespace ohao::runtime {

class Application;

// The game's entry points into the loop. Per-frame game logic can also live in
// systems added to Application::world().
struct GameHooks {
    std::function<void(Application&)> start;
    std::function<void(Application&, float fixedDt)> fixedUpdate;               // simulation
    std::function<void(Application&, float frameDt, float alpha)> update;       // per frame
};

struct AppConfig {
    WindowConfig window;
    bool vsync{false};
    ClockConfig clock;
    TimeMode timeMode{TimeMode::realtime};   // a game steps from wall time
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

// Owns the window, input, renderer and the game World, and runs the loop:
//   poll -> input -> the timeline's steps, each: fixedUpdate, world.simulate(fixedDt)
//        -> update -> world.sync -> render/present
// The timeline (core/timeline.hpp) decides the steps: realtime, whole fixed steps from
// wall time; paused or lockstep, none (a host steps the world on request). The World's
// render extract feeds the renderer's RenderScene. Minimized windows keep simulating and
// skip rendering.
class Application {
public:
    Application() = default;
    ~Application() { shutdown(); }
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool initialize(const AppConfig& cfg);
    RunReport run(const GameHooks& hooks);
    // Tears down renderer then window. Returns the validation messages seen over the
    // whole run *including teardown* -- leaked objects are only reported when the
    // device is destroyed, after run()'s report was taken. 0 when validation is off.
    uint64_t shutdown();

    [[nodiscard]] VulkanRenderer& renderer() { return *m_renderer; }
    // The game world; valid between initialize() and shutdown(). Its render extract
    // is installed on renderer().renderScene().
    [[nodiscard]] ecs::World& world() { return *m_world; }
    [[nodiscard]] InputMap& input() { return m_input; }
    [[nodiscard]] Window& window() { return m_window; }
    // The frame counter, mode and pause state.
    [[nodiscard]] Timeline& timeline() { return m_time; }
    void quit() { m_quit = true; }

private:
    AppConfig m_cfg;
    Window m_window;
    InputMap m_input;
    std::unique_ptr<VulkanRenderer> m_renderer;
    std::unique_ptr<ecs::World> m_world;   // destroyed before the renderer (it holds RenderScene slots)
    Timeline m_time;
    FrameStats m_stats;
    std::vector<InputEvent> m_events;
    bool m_quit{false};
    uint64_t m_validationAtShutdown{0};
};

} // namespace ohao::runtime
