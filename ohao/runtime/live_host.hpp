#pragma once
// The window host (sub-project 4 spec, "Live host"): one loop for ohao_game (realtime, no
// protocol) and `ohao serve --window` (lockstep by default, a protocol server pumped once
// per frame). It owns the window, the renderer, the World and the Session, and is the
// session's api::Host. Per frame:
//
//   poll the window                     -> device events, kept for the next realtime step
//   pump requests                       -> the server runs what is queued on this thread
//                                          (waiting up to 16 ms while nothing steps)
//   the timeline's steps (realtime)     -> Session::step(device events, fixedDt), then
//                                          the fixedUpdate hook
//   the update hook                     -> per frame
//   render + present                    -> realtime: every frame; lockstep or paused: only
//                                          when the world may have changed
//                                          (Session::changeCount) or the window resized
//   flush replies                       -> after the frame they caused was presented
//
// While nothing steps (lockstep, paused) the window's device events are dropped, and the
// first step after stepping resumes starts with a focus loss (everything released): input
// reaches a lockstep world only through input.inject, and a key let go meanwhile is never
// left held. A capture in lockstep renders the world as a headless host does (the same
// procedure, runtime::captureFrame), in any mode, presenting each frame (a path-traced
// capture's sample passes converge on screen), then restores the window's live mode and
// settings; in realtime it returns the next presented frame, in the window's live mode.
#include "api/session.hpp"
#include "core/timeline.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/world.hpp"
#include "gpu/vulkan/renderer.hpp"
#include "runtime/click_capture.hpp"
#include "runtime/clock.hpp"
#include "runtime/combat_sounds.hpp"
#include "runtime/footsteps.hpp"
#include "runtime/window.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ohao::runtime {

class LiveHost;

// The game's entry points into the loop.
struct GameHooks {
    std::function<void(LiveHost&)> start;
    std::function<void(LiveHost&, float fixedDt)> fixedUpdate;           // after each step, its input as stepped
    std::function<void(LiveHost&, float frameDt, float alpha)> update;  // per frame
};

// A protocol server pumped by the loop (ohao serve --window): poll runs the queued
// requests, waiting up to `wait` for the first, false once its input ended; flush sends
// the replies it held.
struct RequestPump {
    std::function<bool(std::chrono::milliseconds wait)> poll;
    std::function<void()> flush;
};

struct LiveConfig {
    WindowConfig window;
    bool vsync{false};
    ClockConfig clock;
    TimeMode timeMode{TimeMode::realtime};   // a game steps from wall time; a server waits
    std::string root{"."};                   // the project root asset paths are relative to
    int frameLimit{0};                       // 0 = run until closed
    int resizeAtFrame{-1};                   // scripted resize, for the presentation gate
    VkExtent2D resizeTo{1280, 720};
    // What the window presents: deferred, or the path tracer (needs ray tracing; without it
    // the window stays deferred). rt_realtime presents every rendered frame; rt_offline its
    // accumulation, one sample pass a frame, and in lockstep or paused it keeps presenting
    // after a change until `spp` samples have accumulated, so a human watches it converge.
    RenderMode mode{RenderMode::Deferred};
    int spp{64};
    // Players' footsteps and landings, and combat's sounds, played through the default audio
    // device (runtime/footsteps.hpp, runtime/combat_sounds.hpp). ohao_game turns it on; a server's
    // window stays silent.
    bool footsteps{false};
};

struct RunReport {
    uint64_t frames{0};             // loop iterations
    uint64_t rendered{0};           // frames rendered and presented (captures included)
    uint64_t readbackCopies{0};     // copies beyond the host's requests: must stay 0
    uint64_t requestedReadbacks{0}; // copies made (each for a capture's request)
    uint64_t validationMessages{0};
    bool     validationEnabled{false};
    double   avgFrameMs{0.0};
    double   avgGpuMs{0.0};
    double   fps{0.0};
};

class LiveHost final : public api::Host {
public:
    LiveHost() = default;
    ~LiveHost() override { shutdown(); }
    LiveHost(const LiveHost&) = delete;
    LiveHost& operator=(const LiveHost&) = delete;

    // The window, a deferred renderer presenting into it, the World with instantiate and
    // the render extract installed, and a Session on the World sharing this host's
    // timeline (the session's host is this). `types`: the engine registry; it must outlive
    // the host.
    bool initialize(const LiveConfig& cfg, const reflect::TypeRegistry& types);
    // Runs until the window closes, quit(), the frame limit, or the pump's input ends.
    RunReport run(const GameHooks& hooks, const RequestPump* pump = nullptr);
    // Tears down session, world, renderer, then window, and logs the totals. Returns the
    // validation messages over the whole run *including teardown* (leaked objects are
    // reported only when the device is destroyed). 0 when validation is off.
    uint64_t shutdown();

    [[nodiscard]] api::Session& session() { return *m_session; }
    [[nodiscard]] ecs::World& world() { return *m_world; }
    [[nodiscard]] VulkanRenderer& renderer() { return *m_renderer; }
    [[nodiscard]] Window& window() { return m_window; }
    [[nodiscard]] Timeline& timeline() { return m_time; }
    // The world's input map, stepped once per simulation step.
    [[nodiscard]] InputMap& input() { return m_world->input(); }
    void quit() { m_quit = true; }
    // A play window (runtime/click_capture.hpp): a mouse press on a free cursor only asks for the
    // cursor, never reaching the world's input (the click that captures it fires no round).
    void setClickToCapture(bool on) noexcept { m_clickToCapture = on; }
    // True once after such a click: the game captures the cursor.
    [[nodiscard]] bool takeCaptureClick() noexcept { return std::exchange(m_captureClicked, false); }

    // api::Host: render.capture, render.stats (the frames the window presented).
    [[nodiscard]] api::HostKind kind() const noexcept override { return api::HostKind::Window; }
    [[nodiscard]] api::RenderOutcome capture(const api::RenderRequest& request) override;
    [[nodiscard]] std::optional<api::RenderStats> renderStats(int frames) override;
    // render.settings: the window's renderer (applied between frames; the next frame redraws).
    [[nodiscard]] std::optional<api::RenderSettings> renderSettings() override;
    void setRenderSettings(const api::RenderSettings& settings) override;
    [[nodiscard]] std::optional<GpuInfo> gpuInfo() override;

private:
    void renderFrame();   // world.sync, the world's view, render + present
    // Readback copies the renderer made beyond the host's own requests (a bug if not 0).
    [[nodiscard]] uint64_t unrequestedReadbacks() const noexcept;

    LiveConfig m_cfg;
    Window m_window;
    std::unique_ptr<VulkanRenderer> m_renderer;
    std::unique_ptr<ecs::FileAssetResolver> m_assets;
    std::unique_ptr<ecs::World> m_world;       // destroyed before the renderer (it holds RenderScene slots)
    std::unique_ptr<api::Session> m_session;   // destroyed before the world
    std::unique_ptr<Footsteps> m_footsteps;     // LiveConfig::footsteps
    std::unique_ptr<CombatSounds> m_combatSounds;   // LiveConfig::footsteps too
    ClickToCapture m_clickCapture;
    bool m_clickToCapture{false};
    bool m_captureClicked{false};
    Timeline m_time;
    FrameStats m_stats;
    std::vector<InputEvent> m_events;   // polled, not yet consumed by a step
    bool m_releaseOnResume{false};      // stepping stopped: the next step starts with a focus loss
    bool m_needRender{true};            // a resize, a capture, the first frame
    uint64_t m_renderedChange{~0ull};   // the session's changeCount at the last render
    uint64_t m_rendered{0};
    uint64_t m_readbackRequests{0};     // readbacks the host asked for (captures)
    float m_defaultExposure{0.6f};      // the renderer's own, for captures that name none
    RenderMode m_liveMode{RenderMode::Deferred};   // what the window presents between captures
    int m_liveSamples{0};               // rt_offline: sample passes presented since the last change
    bool m_quit{false};
    bool m_shutDown{false};
};

} // namespace ohao::runtime
