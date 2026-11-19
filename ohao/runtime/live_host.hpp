#pragma once

#include "render/display/display.hpp"
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
// With the render thread (LiveConfig::renderThread, render/render_thread.hpp), "render + present" is the world's
// sync and the publish of its frame packet: the render thread records and presents it while the loop goes on to
// the next frame. Everything else the loop and the session ask of the renderer (a resize, a capture,
// render.settings, render.stats, a load) runs through RenderThread::run, after the frames published before it. A
// frame's stamps are kept until the render thread reports it submitted, then go into the frame log.
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
#include "render/render_thread.hpp"
#include "render/renderer/vulkan_renderer.hpp"
#include "runtime/audio_player.hpp"
#include "runtime/click_capture.hpp"
#include "runtime/clock.hpp"
#include "runtime/frame_log.hpp"
#include "runtime/window.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ohao::game {
struct LoadedModule;
}

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
    // How the window presents (immediate, mailbox, fifo = vsync), falling back down that list
    // when the surface lacks it (sub-project 9).
    PresentMode present{PresentMode::Mailbox};
    ClockConfig clock;
    TimeMode timeMode{TimeMode::realtime};   // a game steps from wall time; a server waits
    std::string root{"."};                   // the project root asset paths are relative to
    int frameLimit{0};                       // 0 = run until closed
    // The frame-rate cap while realtime (system design 4.7: an uncapped window ran a laptop GPU from 65 to 87 C in a
    // minute for frames nobody saw). -1: the display's refresh rate (60 if unknown); 0: none. Off under fifo, which
    // the display already paces.
    int maxFps{-1};
    int resizeAtFrame{-1};                   // scripted resize, for the presentation gate
    VkExtent2D resizeTo{1280, 720};
    // What the window presents: deferred, or the path tracer (needs ray tracing; without it
    // the window stays deferred). rt_realtime presents every rendered frame; rt_offline its
    // accumulation, one sample pass a frame, and in lockstep or paused it keeps presenting
    // after a change until `spp` samples have accumulated, so a human watches it converge.
    RenderMode mode{RenderMode::Deferred};
    int spp{64};
    // The world's voice schedule played through the default audio device (runtime/audio_player.hpp:
    // footsteps, combat, handling, the mix). ohao_game turns it on (--mute off: the schedule is still
    // built, no device is made); a server's window stays silent.
    bool sound{false};
    // The game installed after the engine (game/loader.hpp; ohao_game plays the slice), or none: an engine-only
    // world. `types` (initialize) holds its types.
    std::shared_ptr<const game::LoadedModule> game;
    // Draws the measured latency and frame time top left (ohao_game --latency; sub-project 9).
    bool latencyReadout{false};
    // The window's camera meters its deferred frames (render.settings autoExposure; frontier F2 T2).
    // Captures stay at a fixed EV100 either way.
    bool autoExposure{true};
    // Frames the frame log keeps (render.stats reads the newest; feel_bench keeps a whole run).
    std::size_t frameLogCapacity{FrameLog::kCapacity};
    // A render thread records and presents each frame while the loop steps the next (render/render_thread.hpp);
    // off, the default: the loop renders each frame itself, by the same path. A play window waits on the GPU, and
    // with the thread on its look latency was not lower beyond off's spread (feel_bench, immediate; step 4b-8).
    bool renderThread{false};
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
    std::string error;              // what escaped a frame and ended the loop; empty when it ended normally
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
    // Its own outside run() and between frames; inside run(), through renderThread().run(fn).
    [[nodiscard]] VulkanRenderer& renderer() { return *m_renderer; }
    [[nodiscard]] render::RenderThread& renderThread() { return *m_renderThread; }
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
    // Events a program supplies as a device would (feel_bench's scripted mouse and keys): they
    // join the next poll's events, stamped with that poll, and are measured as the window's own.
    void feedEvents(std::span<const InputEvent> events) { m_fed.insert(m_fed.end(), events.begin(), events.end()); }
    // The window's frame timing (sub-project 9): realtime frames only.
    [[nodiscard]] const FrameLog& frameLog() const noexcept { return m_frameLog; }

    // api::Host: render.capture, render.stats (the frames the window presented).
    [[nodiscard]] api::HostKind kind() const noexcept override { return api::HostKind::Window; }
    [[nodiscard]] api::RenderOutcome capture(const api::RenderRequest& request) override;
    [[nodiscard]] std::optional<api::RenderStats> renderStats(int frames) override;
    // render.settings: the window's renderer (applied between frames; the next frame redraws).
    [[nodiscard]] std::optional<api::RenderSettings> renderSettings() override;
    void setRenderSettings(const api::RenderSettings& settings) override;
    [[nodiscard]] std::optional<GpuInfo> gpuInfo() override;
    [[nodiscard]] std::optional<api::AudioDeviceState> audioDevice() override;
    void setAudioMix(const api::AudioMixOverrides& mix) override;

private:
    // A frame published and not yet reported (render/render_thread.hpp): what the loop stamped, its frame time and
    // whether it was a realtime frame (only those go into the frame log).
    struct PendingFrame {
        uint64_t seq{0};
        bool realtime{false};
        double frameMs{0.0};
        FrameStamps stamps;
    };
    void renderFrame(PendingFrame frame);   // world.sync, the world's view, the frame packet published
    // The frames the render thread reported, and the present stamps the renderer resolved, into the frame log.
    void collectPresents();
    void resolve(const render::RenderThread::FrameReport& report);
    // The world has a look-led player in control (a play window): a frame's input is polled late.
    [[nodiscard]] bool pollsLate() const;
    // `t` moved to the nearest frame start on the display's refreshes (unchanged until one is known).
    [[nodiscard]] FrameTime onRefreshGrid(FrameTime t) const;
    // capture(), holding the renderer.
    [[nodiscard]] api::RenderOutcome captureHeld(const api::RenderRequest& request);
    // ohao_game --latency: the measured numbers as HUD text, top left.
    void drawLatencyReadout(render::HudList& hud) const;
    // Readback copies the renderer made beyond the host's own requests (a bug if not 0).
    [[nodiscard]] uint64_t unrequestedReadbacks() const noexcept;

    LiveConfig m_cfg;
    Window m_window;
    std::unique_ptr<VulkanRenderer> m_renderer;
    std::unique_ptr<render::RenderThread> m_renderThread;   // destroyed before the renderer, after the world
    std::unique_ptr<ecs::FileAssetResolver> m_assets;
    std::unique_ptr<ecs::World> m_world;       // destroyed before the renderer (it holds RenderScene slots)
    std::unique_ptr<api::Session> m_session;   // destroyed before the world
    std::unique_ptr<AudioSystem> m_audio;       // LiveConfig::sound: the device
    std::unique_ptr<AudioPlayer> m_player;      // LiveConfig::sound: the schedule onto it
    ClickToCapture m_clickCapture;
    bool m_clickToCapture{false};
    bool m_captureClicked{false};
    Timeline m_time;
    FrameStats m_stats;
    std::vector<InputEvent> m_events;   // polled, not yet consumed by a step
    std::vector<InputEvent> m_fed;      // feedEvents, for the next poll
    // Frame timing (sub-project 9). A look movement (a poll whose events held mouse motion, or an
    // input.inject call's mouse event) is shown in full by a frame and consumed by a step, in
    // either order: with the look lead the frame after its poll shows it, before any step; without
    // it, the first frame whose interpolation starts at the step that consumed it.
    struct LookInFlight {
        FrameTime poll{};
        FrameTime consume{};
        bool injected{false};
        bool consumed{false};
        uint64_t consumedAtStep{0};   // the session's frame right after the step that applied it
        uint64_t shownSeq{0};         // the published frame that first shows it in full, until reported (0: none)
        uint64_t shownFrame{0};       // that frame, submitted (0: none yet)
    };
    static constexpr std::size_t kMaxLooksInFlight = 4096;
    void trackLook(FrameTime poll, bool injected);
    void lookConsumed(LookInFlight& l, FrameTime at);
    FrameLog m_frameLog;
    std::vector<LookInFlight> m_looks;
    std::deque<PendingFrame> m_pending;            // published, not yet reported
    std::vector<PresentStamp> m_presents;          // resolved, for frames not yet reported
    uint64_t m_reportedFrame{0};                   // the newest submitted-frame number a report gave
    uint32_t m_viewWidth{0}, m_viewHeight{0};      // the renderer's size after the newest reported frame
    FrameTime m_lastFrameStart{};
    // Frames at the display's rate start on its refreshes, not on a timer that drifts across them: the
    // compositor takes a frame at one point of each refresh, so a start that drifts shows up to a refresh
    // later (docs/notes/platform.md, "Latency on this laptop").
    bool m_refreshLocked{false};   // the cap is the display's refresh rate, and presents are confirmed
    FrameTime m_refreshAt{};       // a refresh, as the present stamps place it (each stamp moves it a tenth)
    // A frame starts this long before a stamp's place in the refresh: measured under KWin, starts from
    // 0.6 ms after it show a refresh later.
    static constexpr std::chrono::microseconds kRefreshLead{450};
    std::chrono::nanoseconds m_framePeriod{0};   // the frame-rate cap's period (0: none)
    int m_maxFps{0};
    FrameTime m_frameView{};   // this frame's view resolve (renderFrame)
    bool m_releaseOnResume{false};      // stepping stopped: the next step starts with a focus loss
    bool m_needRender{true};            // a resize, a capture, the first frame
    uint64_t m_renderedChange{~0ull};   // the session's changeCount at the last render
    uint64_t m_rendered{0};
    uint64_t m_readbackRequests{0};     // readbacks the host asked for (captures)
    float m_defaultEv100{render::kDefaultEv100};   // the renderer's own, for captures that name none
    render::DisplayCurve m_defaultCurve{render::DisplayCurve::AgX};
    RenderMode m_liveMode{RenderMode::Deferred};   // what the window presents between captures
    int m_liveSamples{0};               // rt_offline: sample passes presented since the last change
    bool m_quit{false};
    bool m_shutDown{false};
};

} // namespace ohao::runtime
