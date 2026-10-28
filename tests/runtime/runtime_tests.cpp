// Runtime host tests. No GPU, no window: every input here is synthetic.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/audio_bank.hpp"
#include "ecs/animation.hpp"
#include "ecs/asset_resolver.hpp"
#include "ecs/audio_schedule.hpp"
#include "ecs/combat.hpp"
#include "ecs/physics_scene.hpp"
#include "ecs/scene_io.hpp"
#include "ecs/scene_types.hpp"
#include "ecs/snapshot.hpp"
#include "level/level_system.hpp"
#include "reflect/registry.hpp"
#include "runtime/click_capture.hpp"
#include "runtime/audio_player.hpp"
#include "runtime/clock.hpp"
#include "runtime/combat_fx.hpp"
#include "runtime/frame_capture.hpp"
#include "runtime/frame_log.hpp"
#include "runtime/image.hpp"
#include "core/input.hpp"
#include "gpu/vulkan/swapchain.hpp"

#include "common/harness.hpp"   // CHECK, RUN, the JSON summary line

// The reference decoder for the capture PNG writer (this file's own copy).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace ohao;
using namespace ohao::runtime;
using namespace std::chrono_literals;

// ---------------------------------------------------------------- clock

static void clock_sixty_frames_of_one_sixtieth_is_sixty_steps() {
    FixedStepClock c;
    int steps = 0;
    for (int i = 0; i < 60; ++i) steps += c.advance(16'666'667ns).steps;
    CHECK(steps == 60, "60 frames at exactly the fixed step must run exactly 60 steps");
    CHECK(c.totalSteps() == 60, "totalSteps must agree");
}

static void clock_half_step_frames_alternate() {
    FixedStepClock c;
    const Nanos half{8'333'334};
    Tick a = c.advance(half);
    CHECK(a.steps == 0, "first half step runs nothing");
    CHECK(a.alpha > 0.49f && a.alpha < 0.51f, "alpha ~0.5 after half a step");
    Tick b = c.advance(half);
    CHECK(b.steps == 1, "second half step completes one step");
    CHECK(b.alpha >= 0.0f && b.alpha < 0.01f, "alpha ~0 right after a step");
}

static void clock_clamps_a_long_stall() {
    FixedStepClock c;                       // maxSteps = 5
    Tick t = c.advance(2'000'000'000ns);    // a 2 s debugger pause
    CHECK(t.steps == 5, "a stall runs at most maxSteps");
    CHECK(t.discarded > 1'900'000'000ns, "the rest is discarded, not carried");
    CHECK(t.alpha >= 0.0f && t.alpha < 1.0f, "alpha stays in [0,1)");
    Tick next = c.advance(16'666'667ns);
    CHECK(next.steps == 1, "the frame after a stall is normal again, no spiral");
}

static void clock_rejects_negative_and_zero_dt() {
    FixedStepClock c;
    Tick z = c.advance(0ns);
    CHECK(z.steps == 0 && z.alpha == 0.0f, "zero dt does nothing");
    Tick n = c.advance(Nanos{-5'000'000});
    CHECK(n.steps == 0 && n.frameDt == 0ns, "negative dt (clock went backwards) is treated as zero");
}

static void clock_alpha_always_in_unit_interval() {
    FixedStepClock c;
    const Nanos seq[] = {3'000'000ns, 41'000'000ns, 16'666'667ns, 1ns, 99'999'999ns, 7'777'777ns};
    for (int r = 0; r < 50; ++r)
        for (Nanos d : seq) {
            Tick t = c.advance(d);
            CHECK(t.alpha >= 0.0f && t.alpha < 1.0f, "alpha must stay in [0,1) for any dt");
        }
}

static void frame_stats_average_and_fps() {
    FrameStats s(4);
    CHECK(s.fps() == 0.0, "no samples, no fps");
    s.record(10.0, 5.0);
    s.record(20.0, 7.0);
    CHECK(std::abs(s.avgCpuMs() - 15.0) < 1e-9, "average of 10 and 20");
    CHECK(std::abs(s.avgGpuMs() - 6.0) < 1e-9, "gpu average separate from cpu");
    CHECK(std::abs(s.fps() - 1000.0 / 15.0) < 1e-9, "fps from cpu frame time");
    for (int i = 0; i < 4; ++i) s.record(4.0, 1.0);   // window of 4 drops the old samples
    CHECK(std::abs(s.avgCpuMs() - 4.0) < 1e-9, "rolling window discards old frames");
    CHECK(s.frames() == 6, "frames counts every record");
}

// ---------------------------------------------------------------- timeline (sub-project 4, test 11)

static void sp4_t11_lockstep_ignores_wall_time() {
    Timeline t;   // lockstep by default
    for (int i = 0; i < 120; ++i) CHECK(t.advance(16'666'667ns).steps == 0, "lockstep: wall time runs no step");
    t.stepped(7);
    CHECK(t.frame() == 7 && !t.running(), "steps on request count the frame");
}

static void sp4_t11_realtime_counts_are_exact() {
    Timeline t(TimeMode::realtime);
    uint64_t steps = 0;
    for (int i = 0; i < 600; ++i) {
        const int n = t.advance(16'666'667ns).steps;
        steps += static_cast<uint64_t>(n);
        t.stepped(static_cast<uint64_t>(n));
    }
    CHECK(steps == 600 && t.frame() == 600, "600 frames of exactly one fixed step: 600 steps");
}

static void sp4_t11_pause_and_mode_switches_neither_run_nor_drop_a_step() {
    Timeline t(TimeMode::realtime);
    t.stepped(static_cast<uint64_t>(t.advance(8'000'000ns).steps));   // half a step pending
    t.setPaused(true);
    CHECK(t.advance(10'000'000'000ns).steps == 0 && t.frame() == 0, "paused: ten seconds run nothing");
    t.stepped(3);   // world.step while paused
    t.setPaused(false);
    CHECK(t.advance(10'000'000ns).steps == 0, "resuming does not catch up on the pause (nor on the half step)");
    CHECK(t.advance(7'000'000ns).steps == 1, "the first step after resuming comes a whole fixed step later");
    t.stepped(1);
    t.setMode(TimeMode::lockstep);
    CHECK(t.advance(1'000'000'000ns).steps == 0 && t.frame() == 4, "lockstep again: the frame is kept, no step");
    t.setFixedStep(50'000'000ns);
    CHECK(std::abs(t.fixedDt() - 0.05f) < 1e-9f, "the fixed step can change");
    Timeline d;
    CHECK(d.fixedDt() == 1.0f / 60.0f, "the default fixed step is 1/60 s exactly, as a float");
}

// ---------------------------------------------------------------- input

constexpr int KEY_W = 87, KEY_S = 83, KEY_SPACE = 32, KEY_UP = 265;   // GLFW codes
constexpr int MOUSE_LEFT = 0;

static InputEvent down(int k) { return {InputEvent::Type::Down, InputDevice::Key, k}; }
static InputEvent up(int k)   { return {InputEvent::Type::Up,   InputDevice::Key, k}; }

static void press_hold_release_edges() {
    InputMap m;
    m.bindAction("jump", {InputDevice::Key, KEY_SPACE});
    std::vector<InputEvent> f1{down(KEY_SPACE)};
    m.beginStep(f1);
    CHECK(m.pressed("jump") && m.held("jump") && !m.released("jump"), "frame of the press");
    m.beginStep({});
    CHECK(!m.pressed("jump") && m.held("jump"), "next frame: held, not pressed again");
    std::vector<InputEvent> f3{up(KEY_SPACE)};
    m.beginStep(f3);
    CHECK(!m.held("jump") && m.released("jump"), "frame of the release");
    m.beginStep({});
    CHECK(!m.released("jump"), "released is a one-frame edge");
}

static void tap_within_one_frame() {
    InputMap m;
    m.bindAction("fire", {InputDevice::MouseButton, MOUSE_LEFT});
    std::vector<InputEvent> f{{InputEvent::Type::Down, InputDevice::MouseButton, MOUSE_LEFT},
                              {InputEvent::Type::Up,   InputDevice::MouseButton, MOUSE_LEFT}};
    m.beginStep(f);
    CHECK(m.pressed("fire"), "a tap shorter than a frame must still register as pressed");
    CHECK(m.released("fire"), "...and as released");
    CHECK(!m.held("fire"), "...but not as held");
}

static void repress_within_one_frame() {
    // A held key let go and hit again before the next frame (mashing fire at low
    // fps) is a new press, and the let-go is a release.
    InputMap m;
    m.bindAction("fire", {InputDevice::Key, KEY_W});
    std::vector<InputEvent> a{down(KEY_W)};
    m.beginStep(a);
    std::vector<InputEvent> b{up(KEY_W), down(KEY_W)};
    m.beginStep(b);
    CHECK(m.pressed("fire"), "a re-press inside one frame must register as pressed");
    CHECK(m.released("fire"), "...and the let-go as released");
    CHECK(m.held("fire"), "...and the key is held at frame end");
}

static void press_after_focus_loss_in_one_frame() {
    // Alt-tab away and back, then press, all before the next frame: the press is real.
    InputMap m;
    m.bindAction("jump", {InputDevice::Key, KEY_W});
    std::vector<InputEvent> a{{InputEvent::Type::FocusLost}, down(KEY_W)};
    m.beginStep(a);
    CHECK(m.held("jump") && m.pressed("jump"), "a Down after FocusLost in the same frame survives");
}

static void two_bindings_one_action() {
    InputMap m;
    m.bindAction("forward", {InputDevice::Key, KEY_W});
    m.bindAction("forward", {InputDevice::Key, KEY_UP});
    std::vector<InputEvent> a{down(KEY_W), down(KEY_UP)};
    m.beginStep(a);
    std::vector<InputEvent> b{up(KEY_W)};
    m.beginStep(b);
    CHECK(m.held("forward"), "releasing one binding keeps the action held by the other");
    CHECK(!m.released("forward"), "and is not a release of the action");
}

static void rebind_replaces() {
    InputMap m;
    m.bindAction("jump", {InputDevice::Key, KEY_SPACE});
    m.clearAction("jump");
    m.bindAction("jump", {InputDevice::Key, KEY_W});
    std::vector<InputEvent> f{down(KEY_SPACE)};
    m.beginStep(f);
    CHECK(!m.held("jump"), "the old key no longer triggers the action");
    std::vector<InputEvent> g{down(KEY_W)};
    m.beginStep(g);
    CHECK(m.held("jump"), "the new key does");
}

static void focus_loss_releases_everything() {
    InputMap m;
    m.bindAction("sprint", {InputDevice::Key, KEY_W});
    m.bindAxis("move", {InputDevice::Key, KEY_W}, {InputDevice::Key, KEY_S});
    std::vector<InputEvent> a{down(KEY_W)};
    m.beginStep(a);
    std::vector<InputEvent> b{{InputEvent::Type::FocusLost}};
    m.beginStep(b);
    CHECK(!m.held("sprint") && m.released("sprint"), "alt-tab must release held actions");
    CHECK(m.axis("move") == 0.0f, "and zero every axis");
}

static void axis_from_key_pair() {
    InputMap m;
    m.bindAxis("move", {InputDevice::Key, KEY_W}, {InputDevice::Key, KEY_S});
    std::vector<InputEvent> a{down(KEY_W)};
    m.beginStep(a);
    CHECK(m.axis("move") == 1.0f, "positive key");
    std::vector<InputEvent> b{down(KEY_S)};
    m.beginStep(b);
    CHECK(m.axis("move") == 0.0f, "both keys cancel");
    std::vector<InputEvent> c{up(KEY_W)};
    m.beginStep(c);
    CHECK(m.axis("move") == -1.0f, "negative key");
}

static void mouse_delta_no_first_jump() {
    InputMap m;
    std::vector<InputEvent> a{{InputEvent::Type::MouseMove, InputDevice::Key, 0, 500.0, 300.0}};
    m.beginStep(a);
    CHECK(m.mouseDelta() == glm::vec2(0.0f), "first move establishes position, no jump");
    std::vector<InputEvent> b{{InputEvent::Type::MouseMove, InputDevice::Key, 0, 510.0, 295.0},
                              {InputEvent::Type::MouseMove, InputDevice::Key, 0, 512.0, 290.0}};
    m.beginStep(b);
    CHECK(m.mouseDelta() == glm::vec2(12.0f, -10.0f), "deltas sum within a frame");
    m.beginStep({});
    CHECK(m.mouseDelta() == glm::vec2(0.0f), "no motion, no delta");
}

static void unknown_names_are_inert() {
    InputMap m;
    m.beginStep({});
    CHECK(!m.pressed("nope") && !m.held("nope") && !m.released("nope"), "unbound action is false");
    CHECK(m.axis("nope") == 0.0f, "unbound axis is zero");
}

// ---------------------------------------------------------------- swapchain choice

// The deferred output is already gamma-encoded by the tonemap shader, so the
// swapchain must be UNORM: an sRGB swapchain would encode it a second time.
static void choose_format_prefers_unorm_bgra() {
    VkSurfaceFormatKHR fs[] = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                               {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                               {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    CHECK(ohao::Swapchain::chooseFormat(fs).format == VK_FORMAT_B8G8R8A8_UNORM,
          "prefer BGRA8 UNORM: the image is already display-encoded");
    VkSurfaceFormatKHR srgbOnly[] = {{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                     {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    CHECK(ohao::Swapchain::chooseFormat(srgbOnly).format == VK_FORMAT_R8G8B8A8_UNORM,
          "any UNORM 8-bit format beats an sRGB one");
    VkSurfaceFormatKHR only[] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    CHECK(ohao::Swapchain::chooseFormat(only).format == VK_FORMAT_R8G8B8A8_UNORM, "else first offered");
}

// ---------------------------------------------------------------- input as data (sub-project 4, test 13)

static void sp4_t13_key_names_round_trip() {
    int n = 0;
    for (const KeyName& k : keyNames()) {
        CHECK(keyFromName(k.name) == k.key && keyName(k.key) == k.name, "name -> key -> name, for every key");
        ++n;
    }
    CHECK(n > 80, "letters, digits, F-keys, modifiers, arrows, mouse buttons");
    CHECK(keyBinding(Key::W) == Binding(InputDevice::Key, 87) && keyBinding(Key::MouseRight) == Binding(InputDevice::MouseButton, 1),
          "keys are GLFW codes; mouse buttons are buttons");
    CHECK(!keyBinding(Key::None) && !keyFromName("Spcae") && keyName(static_cast<Key>(12345)).empty(), "None binds nothing");
}

static InputEvent virt(InputEvent::Type t, const char* name, float v = 0.0f) {
    InputEvent e{t};
    e.name = name;
    e.value = v;
    return e;
}

static void sp4_t13_virtual_device_acts_like_a_device() {
    InputMap m;
    m.bindAction("fire", {InputDevice::Key, KEY_SPACE});
    std::vector<InputEvent> a{virt(InputEvent::Type::ActionDown, "fire")};
    m.beginStep(a);
    CHECK(m.pressed("fire") && m.held("fire"), "an injected press is a press");
    std::vector<InputEvent> b{down(KEY_SPACE)};
    m.beginStep(b);
    CHECK(!m.pressed("fire") && m.held("fire"), "a real key on top changes nothing");
    std::vector<InputEvent> c{virt(InputEvent::Type::ActionUp, "fire")};
    m.beginStep(c);
    CHECK(m.held("fire") && !m.released("fire"), "the key still holds it");
    std::vector<InputEvent> d{up(KEY_SPACE)};
    m.beginStep(d);
    CHECK(m.released("fire") && !m.held("fire"), "both let go: released");
    std::vector<InputEvent> e{virt(InputEvent::Type::ActionDown, "dash")};
    m.beginStep(e);
    CHECK(m.pressed("dash") && m.held("dash"), "an action with no binding exists while injected");
    std::vector<InputEvent> f{virt(InputEvent::Type::ActionUp, "dash")};
    m.beginStep(f);
    CHECK(m.released("dash") && !m.held("dash"), "and is released");
    InputEvent md{InputEvent::Type::MouseDelta};
    md.x = 3;
    md.y = -2;
    std::vector<InputEvent> g{md, md};
    m.beginStep(g);
    CHECK(m.mouseDelta() == glm::vec2(6, -4), "mouse deltas add up within a step");
    m.beginStep({});
    CHECK(m.mouseDelta() == glm::vec2(0, 0), "and do not carry over");
}

static void sp4_t13_a_rebind_counts_at_the_next_step_without_an_event() {
    InputMap m;
    m.bindAction("fire", {InputDevice::Key, KEY_SPACE});
    std::vector<InputEvent> a{down(KEY_SPACE)};
    m.beginStep(a);
    CHECK(m.held("fire"), "held");
    m.clearBindings();
    m.bindAction("fire", {InputDevice::Key, KEY_W});
    m.beginStep({});
    CHECK(!m.held("fire") && m.released("fire"), "rebound away from the held key: released at the next step");
    m.bindAction("fire", {InputDevice::Key, KEY_SPACE});
    m.beginStep({});
    CHECK(m.held("fire") && m.pressed("fire"), "bound to a key already down: pressed at the next step");
}

// Sub-project 9, test 20: the present mode asked for, else the next one down (immediate, then
// mailbox, then fifo, which every surface offers).
static void sp9_t20_present_mode_choice() {
    using M = ohao::PresentMode;
    const auto pick = [](std::span<const VkPresentModeKHR> offered, M want) {
        return ohao::Swapchain::choosePresentMode(offered, want);
    };
    const VkPresentModeKHR all[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    const VkPresentModeKHR noImmediate[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
    const VkPresentModeKHR fifoOnly[] = {VK_PRESENT_MODE_FIFO_KHR};
    CHECK(pick(all, M::Immediate) == VK_PRESENT_MODE_IMMEDIATE_KHR, "immediate when offered");
    CHECK(pick(all, M::Mailbox) == VK_PRESENT_MODE_MAILBOX_KHR, "mailbox when asked and offered");
    CHECK(pick(all, M::Fifo) == VK_PRESENT_MODE_FIFO_KHR, "fifo when asked, whatever else is offered");
    CHECK(pick(noImmediate, M::Immediate) == VK_PRESENT_MODE_MAILBOX_KHR, "immediate falls back to mailbox");
    CHECK(pick(fifoOnly, M::Immediate) == VK_PRESENT_MODE_FIFO_KHR, "then to fifo");
    CHECK(pick(fifoOnly, M::Mailbox) == VK_PRESENT_MODE_FIFO_KHR, "mailbox falls back to fifo");
    const VkPresentModeKHR immediateOnly[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    CHECK(pick(immediateOnly, M::Mailbox) == VK_PRESENT_MODE_FIFO_KHR, "mailbox never tears: fifo, not immediate");
    CHECK(ohao::presentModeFromName("immediate") == M::Immediate && ohao::presentModeFromName("mailbox") == M::Mailbox &&
              ohao::presentModeFromName("fifo") == M::Fifo && !ohao::presentModeFromName("vsync"),
          "the names --present takes");
    CHECK(ohao::presentModeName(M::Immediate) == "immediate", "and back");
}

// ---- sub-project 9, step 1: frame timing (runtime/frame_log.hpp) -----------------------------

using FrameTime = std::chrono::steady_clock::time_point;

static FrameStamps stampedFrame(uint64_t frame, FrameTime start, double frameMs) {
    FrameStamps f;
    f.frame = frame;
    f.start = start;
    f.poll = start + 100us;
    f.view = start + 300us;
    f.submit = start + 1500us;
    f.frameMs = frameMs;
    return f;
}

// Test 19: a mouse movement's latency is poll to present of the first frame showing it in full;
// frames without look carry no sample, and a frame whose present is not known yet counts once it is.
static void sp9_t19_latency_stages_ordered() {
    CHECK(percentile({}, 0.5) == 0.0, "no values: 0");
    CHECK(percentile({3.0}, 0.95) == 3.0, "one value: itself");
    CHECK(std::abs(percentile({1.0, 2.0, 3.0, 4.0}, 0.5) - 2.5) < 1e-12, "the median of an even count: the mean of the middle two");

    FrameLog log;
    CHECK(!log.latency(60) && !log.pacing(60), "nothing recorded: no stats");
    const FrameTime t0{};
    // Ten frames with look: frame i shows a movement polled (5 + i) ms before its present, consumed
    // by a step 2 ms after the poll. Ten frames without look between them.
    for (int i = 0; i < 10; ++i) {
        FrameStamps look = stampedFrame(100 + 2 * i, t0 + std::chrono::milliseconds(20 * i), 6.9);
        const FrameTime present = look.submit + 3ms;
        look.looks.push_back({present - std::chrono::microseconds(5000 + 1000 * i),
                              present - std::chrono::microseconds(3000 + 1000 * i)});
        log.record(look);
        CHECK(log.present(look.frame, present, true), "the present of a recorded frame is kept");
        log.record(stampedFrame(101 + 2 * i, t0 + std::chrono::milliseconds(20 * i + 10), 6.9));
    }
    // A frame with look whose present never arrived: no sample.
    FrameStamps unpresented = stampedFrame(200, t0 + 300ms, 6.9);
    unpresented.looks.push_back({unpresented.poll, unpresented.poll + 1ms});
    log.record(unpresented);
    CHECK(!log.present(999, t0, true), "a frame never recorded: nothing to stamp");

    const auto lat = log.latency(1024);
    CHECK(lat && lat->samples == 10 && lat->frames == 10, "ten samples from ten presented frames with look");
    CHECK(lat && std::abs(lat->medianMs - 9.5) < 1e-6, "median of 5..14 ms: 9.5");
    CHECK(lat && std::abs(lat->p95Ms - 13.55) < 1e-6, "p95 by linear interpolation between ranks: 13.55");
    CHECK(lat && lat->presentWait, "every sample's present came from present wait");
    CHECK(lat && std::abs(lat->pollToConsumeMs - 2.0) < 1e-6, "poll to the consuming step: 2 ms");
    CHECK(lat && std::abs(lat->viewToSubmitMs - 1.2) < 1e-6 && std::abs(lat->submitToPresentMs - 3.0) < 1e-6,
          "view to submit 1.2 ms, submit to present 3 ms");
    CHECK(lat && std::abs(lat->pollToViewMs - (9.5 - 4.2)) < 1e-6, "poll to view: the latency less view to present");

    // The newest frames only: the last 5 records hold 2 presented frames with look (14 and 13 ms).
    const auto newest = log.latency(5);
    CHECK(newest && newest->samples == 2 && std::abs(newest->medianMs - 13.5) < 1e-6, "frames counts the newest records");

    // A present known only from the present call (no present wait) makes the whole set unconfirmed.
    FrameStamps late = stampedFrame(300, t0 + 400ms, 6.9);
    late.looks.push_back({late.poll, late.poll + 1ms});
    log.record(late);
    CHECK(log.present(300, late.submit + 1ms, false), "a present-call stamp");
    const auto mixed = log.latency(1024);
    CHECK(mixed && mixed->samples == 11 && !mixed->presentWait, "one sample without present wait: presentWait false");

    // The ring keeps the newest kCapacity frames.
    for (uint64_t i = 0; i < FrameLog::kCapacity + 10; ++i) log.record(stampedFrame(1000 + i, t0 + 1s, 6.9));
    CHECK(log.size() == FrameLog::kCapacity && !log.latency(1024), "older frames fall out of the ring");
}

// Test 20: late input sampling. Before a frame polls its input, the frame `framesAhead + 1` back
// must have been presented, so at most `framesAhead` earlier frames are submitted and not presented.
static void sp9_t20_frames_ahead_bounded() {
    CHECK(presentToAwait(10, 1) == 9, "one frame ahead: frame 11 polls once frame 9 is presented");
    CHECK(presentToAwait(10, 0) == 10, "none ahead: once the previous frame is presented");
    CHECK(presentToAwait(10, 2) == 8, "two ahead");
    CHECK(presentToAwait(1, 1) == 0 && presentToAwait(0, 0) == 0, "nothing to wait for before enough frames exist");
    CHECK(presentToAwait(10, -1) == 0, "unbounded (a negative count): never waits");
    // Simulated: a frame is presented 3 submits after it is made unless the host waits.
    for (int ahead = 0; ahead <= 2; ++ahead) {
        uint64_t submitted = 0, presented = 0;
        int worst = 0;
        for (int frame = 0; frame < 50; ++frame) {
            const uint64_t need = presentToAwait(submitted, ahead);
            if (presented < need) presented = need;   // the host waits for it
            worst = std::max(worst, static_cast<int>(submitted - presented));
            ++submitted;
            if (submitted >= 3 && presented < submitted - 3) presented = submitted - 3;   // the queue drains
        }
        CHECK(worst <= ahead, "at most framesAhead frames unpresented when a frame polls");
    }
}

// Test 20: frame pacing over the newest frames: the frame time's percentiles, steps per frame, and
// the steps the clock's cap discarded.
static void sp9_t20_pacing_stats() {
    FrameLog log;
    const FrameTime t0{};
    for (int i = 0; i < 1010; ++i) {
        FrameStamps f = stampedFrame(static_cast<uint64_t>(i + 1), t0 + std::chrono::milliseconds(7 * i),
                                     i < 980 ? 6.9 : 20.0);
        f.steps = i < 980 ? (i % 2) : 2 + (i % 3 == 0 ? 1 : 0);
        f.discardedSteps = i == 1009 ? 3 : 0;
        log.record(f);
    }
    const auto p = log.pacing(1024);
    CHECK(p && p->frames == 1010, "every recorded frame up to the ring");
    CHECK(p && std::abs(p->medianMs - 6.9) < 1e-9 && std::abs(p->p95Ms - 6.9) < 1e-9, "median and p95 6.9 ms");
    CHECK(p && std::abs(p->p99Ms - 20.0) < 1e-9, "p99 20 ms: the 30 slow frames are the top 3 %");
    CHECK(p && p->steps[0] == 490 && p->steps[1] == 490 && p->steps[2] == 20 && p->steps[3] == 10,
          "frames that ran 0, 1, 2 and 3+ steps");
    CHECK(p && p->discardedSteps == 3, "steps the cap discarded");
    const auto last = log.pacing(30);
    CHECK(last && last->frames == 30 && last->medianMs == 20.0, "the newest 30 frames only");
}

static void choose_image_count_unlimited_max() {
    VkSurfaceCapabilitiesKHR c{};
    c.minImageCount = 2;
    c.maxImageCount = 0;   // 0 = no limit
    CHECK(ohao::Swapchain::chooseImageCount(c) == 3, "min+1 when max is unlimited");
    c.maxImageCount = 2;
    CHECK(ohao::Swapchain::chooseImageCount(c) == 2, "clamped to max");
}

static void choose_extent_uses_desired_when_surface_defers() {
    VkSurfaceCapabilitiesKHR c{};
    c.currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu};
    c.minImageExtent = {1, 1};
    c.maxImageExtent = {4096, 4096};
    auto e = ohao::Swapchain::chooseExtent(c, {1920, 1080});
    CHECK(e.width == 1920 && e.height == 1080, "surface defers: use the window size");
    auto big = ohao::Swapchain::chooseExtent(c, {9000, 9000});
    CHECK(big.width == 4096 && big.height == 4096, "clamped to maxImageExtent");
    c.currentExtent = {800, 600};
    auto fixed = ohao::Swapchain::chooseExtent(c, {1920, 1080});
    CHECK(fixed.width == 800 && fixed.height == 600, "surface dictates: obey it");
}

static void zero_extent_means_minimized() {
    VkSurfaceCapabilitiesKHR c{};
    c.currentExtent = {0, 0};
    c.minImageExtent = {0, 0};
    c.maxImageExtent = {0, 0};
    auto e = ohao::Swapchain::chooseExtent(c, {1920, 1080});
    CHECK(e.width == 0 && e.height == 0, "a minimized window reports 0x0 and must not be inflated");
}

// ---------------------------------------------------------------- capture images (step 9)

// A 1280x720 frame with what renders produce: flat walls, gradients, noise.
static ohao::runtime::Rgba8Image syntheticFrame() {
    ohao::runtime::Rgba8Image im{1280, 720, std::vector<uint8_t>(1280u * 720u * 4u)};
    uint32_t seed = 12345;
    for (uint32_t y = 0; y < im.height; ++y)
        for (uint32_t x = 0; x < im.width; ++x) {
            uint8_t* p = im.pixels.data() + (static_cast<size_t>(y) * im.width + x) * 4;
            seed = seed * 1664525u + 1013904223u;
            if (x < 400) { p[0] = 140; p[1] = 120; p[2] = 100; }                       // a flat wall
            else if (x < 800) { p[0] = static_cast<uint8_t>(x); p[1] = static_cast<uint8_t>(y); p[2] = 60; }
            else { p[0] = static_cast<uint8_t>(seed >> 24); p[1] = static_cast<uint8_t>(seed >> 16); p[2] = static_cast<uint8_t>(seed >> 8); }
            p[3] = 255;
        }
    return im;
}

template <class F>
static double medianMs(F&& f, int runs = 7) {
    std::vector<double> t;
    for (int i = 0; i < runs; ++i) {
        const auto s = std::chrono::steady_clock::now();
        f();
        t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count());
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

// The capture PNG writer (OHAO's own deflate) is lossless: stb_image decodes exactly the
// pixels it was given, for a frame, an all-noise frame (a 1-frame RT GI render is mostly
// noise) and odd sizes.
static void sp4_t16_capture_png_round_trips_losslessly() {
    using namespace ohao::runtime;
    const Rgba8Image frame = syntheticFrame();
    std::string png;
    const double ms = medianMs([&] { png = encodePng(frame); });
    Rgba8Image noise{1280, 720, std::vector<uint8_t>(1280u * 720u * 4u)};
    uint32_t seed = 99;
    for (auto& b : noise.pixels) b = static_cast<uint8_t>((seed = seed * 1664525u + 1013904223u) >> 24);
    std::string noisy;
    const double noiseMs = medianMs([&] { noisy = encodePng(noise); });
    int w = 0, h = 0, n = 0;
    unsigned char* back = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(png.data()),
                                                static_cast<int>(png.size()), &w, &h, &n, 4);
    CHECK(back && w == 1280 && h == 720, "the PNG decodes at its size");
    CHECK(back && std::equal(frame.pixels.begin(), frame.pixels.end(), back), "to exactly the pixels encoded");
    stbi_image_free(back);
    back = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(noisy.data()), static_cast<int>(noisy.size()),
                                 &w, &h, &n, 4);
    CHECK(back && std::equal(noise.pixels.begin(), noise.pixels.end(), back), "an all-noise frame round trips too");
    stbi_image_free(back);
    for (const auto [ow, oh] : {std::pair{1u, 1u}, std::pair{3u, 7u}, std::pair{257u, 5u}}) {
        Rgba8Image odd{ow, oh, std::vector<uint8_t>(static_cast<size_t>(ow) * oh * 4)};
        for (size_t i = 0; i < odd.pixels.size(); ++i) odd.pixels[i] = static_cast<uint8_t>(i * 37 % 251);
        const std::string p = encodePng(odd);
        unsigned char* b = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(p.data()),
                                                 static_cast<int>(p.size()), &w, &h, &n, 4);
        CHECK(b && w == static_cast<int>(ow) && h == static_cast<int>(oh) &&
                  std::equal(odd.pixels.begin(), odd.pixels.end(), b),
              "odd sizes round trip too");
        stbi_image_free(b);
    }
    std::printf("\n      1280x720 PNG: %zu bytes (raw %u), encode median %.2f ms; all noise %zu bytes, %.2f ms",
                png.size(), 1280u * 720u * 4u, ms, noisy.size(), noiseMs);
}

// Crop, the area downscale and the JPEG preview.
static void sp4_t16_crop_scale_and_jpeg_preview() {
    using namespace ohao::runtime;
    const Rgba8Image frame = syntheticFrame();
    const Rgba8Image part = crop(frame.pixels, frame.width, frame.height, {10, 20, 300, 200});
    CHECK(part.width == 300 && part.height == 200 &&
              std::equal(part.pixels.begin(), part.pixels.begin() + 4, frame.pixels.begin() + (20 * 1280 + 10) * 4),
          "crop keeps the region's pixels");
    // 2x2 blocks of known values average exactly at half size.
    Rgba8Image blocks{4, 2, {}};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 4; ++c) blocks.pixels.push_back(static_cast<uint8_t>(x < 2 ? (y ? 100 : 50) : 200));
    const Rgba8Image half = fitWithin(blocks, 2);
    CHECK(half.width == 2 && half.height == 1 && half.pixels[0] == 75 && half.pixels[4] == 200,
          "the downscale averages the area each output pixel covers");
    Rgba8Image preview;
    const double scaleMs = medianMs([&] { preview = fitWithin(frame, 768); });
    CHECK(preview.width == 768 && preview.height == 432, "a 1280x720 frame previews at 768x432");
    std::string jpeg;
    const double jpegMs = medianMs([&] { jpeg = encodeJpeg(preview, 85); });
    int w = 0, h = 0, n = 0;
    unsigned char* back = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(jpeg.data()),
                                                static_cast<int>(jpeg.size()), &w, &h, &n, 3);
    CHECK(back && w == 768 && h == 432, "the JPEG decodes at its size");
    stbi_image_free(back);
    std::printf("\n      preview: downscale median %.2f ms, JPEG %zu bytes median %.2f ms", scaleMs, jpeg.size(), jpegMs);
}

// render.stats' summary (2b test 14): medians of the frame and of each pass over the frames
// that ran it, passes in registry (frame) order whatever order they were timed in.
static void sp2b_t14_stats_are_medians_per_pass_in_registry_order() {
    const auto timing = [](uint64_t frame, float gpu, float cpu,
                           std::initializer_list<std::pair<std::string_view, float>> passes) {
        render::FrameTiming f;
        f.frame = frame;
        f.gpuMs = gpu;
        f.cpuMs = cpu;
        for (const auto& [name, ms] : passes) {
            f.pass[f.count] = static_cast<uint8_t>(render::passIndex(name));
            f.ms[f.count++] = ms;
        }
        return f;
    };
    CHECK(!summarizeFrames({}), "no frames: no stats");
    // bloom runs in two frames of four; "lighting" is listed before "gbuffer.late" in frame 3.
    const std::vector<render::FrameTiming> frames{
        timing(10, 4.0f, 0.5f, {{"gbuffer.late", 1.0f}, {"lighting", 2.0f}, {"post.bloom", 0.5f}}),
        timing(11, 6.0f, 0.7f, {{"gbuffer.late", 3.0f}, {"lighting", 2.0f}}),
        timing(12, 5.0f, 0.6f, {{"lighting", 4.0f}, {"gbuffer.late", 2.0f}, {"post.bloom", 0.7f}}),
        timing(13, 9.0f, 0.9f, {{"gbuffer.late", 2.0f}, {"lighting", 1.0f}}),
    };
    const auto st = summarizeFrames(frames);
    CHECK(st && st->frame == 13 && st->frames == 4, "the newest frame and the count");
    CHECK(st && st->gpuMs == 5.5 && std::abs(st->cpuMs - 0.65) < 1e-6, "even counts take the mean of the middle two");
    const std::vector<std::pair<std::string, double>> want{{"gbuffer.late", 2.0}, {"lighting", 2.0}, {"post.bloom", 0.6}};
    CHECK(st && st->passes.size() == 3 && st->passes[0].first == "gbuffer.late" && st->passes[1].first == "lighting" &&
              st->passes[2].first == "post.bloom",
          "passes in registry order");
    bool close = st.has_value();
    for (std::size_t i = 0; close && i < want.size(); ++i) close = std::abs(st->passes[i].second - want[i].second) < 1e-6;
    CHECK(close, "each pass's median over the frames that ran it (bloom: two frames)");
}

// ---- sub-project 9, step 3: particles from combat (runtime/combat_fx.hpp) -------------------------

static void sp9_t21_particles_from_events() {
    using K = ecs::CombatEvent::Kind;
    constexpr uint64_t player = 0xb1a7e2, enemy = 0xe1;
    ecs::CombatEvent shot;
    shot.kind = K::Shot;
    shot.sourceId = player;
    shot.point = {0.0f, 1.62f, 0.0f};    // the eye
    shot.normal = {0.0f, 0.0f, -1.0f};   // the round's direction
    const glm::vec3 muzzle(0.1f, 1.5f, -0.2f);
    auto out = particlesFor(shot, player, muzzle);
    CHECK(out.size() == 1 && out[0].type == ParticleType::MUZZLE_FLASH && out[0].position == muzzle &&
              out[0].direction == shot.normal,
          "the player's round: a muzzle burst at its drawn muzzle, along the round");
    shot.sourceId = enemy;
    out = particlesFor(shot, player, muzzle);
    CHECK(out.size() == 1 && out[0].position == shot.point, "an enemy's: at its eye");
    ecs::CombatEvent hit;
    hit.kind = K::Hit;
    hit.point = {1.0f, 1.0f, -5.0f};
    hit.normal = {0.0f, 0.0f, 1.0f};
    std::vector<glm::vec4> dustColours;
    for (const char* surface : {"concrete", "metal", "wood", ""}) {
        hit.surface = surface;
        out = particlesFor(hit, player, muzzle);
        CHECK(out.size() == 2 && out[0].type == ParticleType::IMPACT_SPARK && out[0].position == hit.point &&
                  out[0].direction == hit.normal,
              std::string("a hit on '") + surface + "': sparks along the normal");
        if (out.size() == 2) dustColours.push_back(out[1].colorStart);
    }
    CHECK(dustColours.size() == 4 && dustColours[0] != dustColours[2] && dustColours[0] != dustColours[3] &&
              dustColours[1] != dustColours[0],
          "and a puff by its surface (concrete, metal, wood, default each their own)");
    for (const K quiet : {K::Damage, K::Death, K::Reload, K::Reloaded, K::RoundStart, K::Restart, K::Win}) {
        ecs::CombatEvent e;
        e.kind = quiet;
        CHECK(particlesFor(e, player, muzzle).empty(), "nothing else emits");
    }

    // A real fight: emits only while realtime; the player's burst at its muzzle light.
    ecs::World w;
    ecs::FileAssetResolver files(OHAO_SOURCE_DIR);
    w.seedIds(5);
    ecs::installPhysics(w, files);
    ecs::installCombat(w);
    const entt::entity p = w.createWithId(player, "Player");
    w.registry().emplace<ecs::PlayerController>(p);
    w.registry().emplace<ecs::Health>(p);
    ecs::Weapon steady;
    steady.spreadDeg = 0.0f;
    steady.recoilPitchDeg = 0.0f;
    steady.recoilYawDeg = 0.0f;
    w.registry().emplace<ecs::Weapon>(p, steady);
    const entt::entity head = w.create("Head", ecs::Transform{{0.0f, 1.62f, 0.0f}});
    w.setParent(head, p);
    w.registry().emplace<ecs::Camera>(head, ecs::Camera{75.0f, 0.05f, 100.0f, true});
    const entt::entity m = w.create("Muzzle", ecs::Transform{{0.2f, -0.2f, -0.5f}});
    w.setParent(m, head);
    w.registry().emplace<ecs::Light>(m);
    w.registry().emplace<ecs::Muzzle>(m);
    const entt::entity wall = w.create("Wall", ecs::Transform{{0.0f, 1.0f, -6.0f}});
    w.registry().emplace<ecs::Collider>(wall, ecs::Collider{ecs::ColliderShape::Box, {5.0f, 3.0f, 0.2f}, glm::vec3(0.0f), false, "metal"});
    w.sync();
    CombatFx fx;
    (void)fx.collect(w, true);   // starts at the log's end
    const auto fire = [&](bool down) {
        InputEvent e;
        e.type = down ? InputEvent::Type::ActionDown : InputEvent::Type::ActionUp;
        e.name = "fire";
        const std::vector<InputEvent> events{e};
        w.stepInput(events);
        w.update(1.0f / 60.0f);
    };
    fire(true);
    fire(false);
    CHECK(fx.collect(w, false).empty(), "lockstep (not realtime): nothing emitted");
    for (int i = 0; i < 12; ++i) fire(i % 2 == 0);   // the next round, past the cooldown
    const auto live = fx.collect(w, true);
    const glm::vec3 muzzleAt(w.registry().get<ecs::WorldTransform>(m).world[3]);
    bool burst = false, sparks = false;
    for (const auto& c : live) {
        burst = burst || (c.type == ParticleType::MUZZLE_FLASH && glm::length(c.position - muzzleAt) < 1e-5f);
        sparks = sparks || c.type == ParticleType::IMPACT_SPARK;
    }
    std::printf("\n      realtime: %zu emits", live.size());
    CHECK(burst, "realtime: the burst at the player's muzzle light");
    CHECK(sparks, "and the round's sparks on the wall");
}

// ---- sub-project 11, step 3: the audio player (runtime/audio_player.hpp) ---------------------------

namespace t29 {

AudioBank bankFrom(const std::string& json) {
    auto r = parseAudioBank(json);
    return r ? std::move(r).value() : AudioBank{};
}

AudioBank slice() {
    std::ifstream f(std::filesystem::path(OHAO_SOURCE_DIR) / "assets/audio/slice.audio.json", std::ios::binary);
    return bankFrom(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
}

ecs::AudioCommand playAt(uint64_t step, uint32_t voice, uint16_t sound) {
    ecs::AudioCommand c;
    c.op = ecs::AudioCommand::Op::Play;
    c.step = step;
    c.voice = voice;
    c.sound = sound;
    c.gain = 0.85f;
    return c;
}

uint64_t startOf(const AudioSystem& audio, const AudioPlayer& p, uint32_t voice) {
    const auto h = p.handleOf(voice);
    const auto v = h ? audio.voice(*h) : std::nullopt;
    return v ? v->startFrame : ~0ull;
}

} // namespace t29

// The clock: two steps in one frame start their sounds one step apart; a command whose time has passed
// starts at once and counts as late; a pause (or lockstep) plays at once plus the horizon and takes the
// anchor again.
static void t29_player_clock() {
    AudioSystem audio;
    CHECK(audio.initialize(true), "a mixer without a device");
    AudioPlayer p;
    p.initialize(OHAO_SOURCE_DIR, audio);
    const AudioBank bank = t29::slice();
    p.useBank(bank);
    const auto fire = static_cast<uint16_t>(bank.sound("fire"));
    CHECK(p.horizonFrames() == 800 && p.stepFrames() == 800, "one step: 800 frames at 48 kHz");
    std::vector<float> scratch(48000 * 2);
    (void)audio.render(scratch, 1000);
    const uint64_t e0 = audio.engineFrame();
    const std::vector<ecs::AudioCommand> frame1{t29::playAt(10, 1, fire), t29::playAt(11, 2, fire)};
    p.submit(frame1, 11, true);
    const uint64_t s1 = t29::startOf(audio, p, 1), s2 = t29::startOf(audio, p, 2);
    std::printf("\n      engine %llu: steps 10 and 11 start at %llu and %llu", static_cast<unsigned long long>(e0),
                static_cast<unsigned long long>(s1), static_cast<unsigned long long>(s2));
    CHECK(s1 == e0 && s2 == e0 + 800 && p.late() == 0, "steps 10 and 11 of one frame: a step apart, the newest a step ahead");
    // A stall: eight steps' worth of audio played before the next frame, which brings steps 12 to 19.
    (void)audio.render(scratch, 6400);
    const std::vector<ecs::AudioCommand> frame2{t29::playAt(12, 3, fire), t29::playAt(19, 4, fire)};
    p.submit(frame2, 19, true);
    const uint64_t e1 = audio.engineFrame();
    CHECK(t29::startOf(audio, p, 3) == e1 && p.late() == 1, "step 12's time has passed: at once, counted late");
    CHECK(t29::startOf(audio, p, 4) == e0 + 800 + 8 * 800 && p.anchors() == 1, "step 19 on the same clock, no new anchor");
    // A pause (or lockstep): at once plus the horizon; then real time again from a new anchor.
    (void)audio.render(scratch, 20000);
    const std::vector<ecs::AudioCommand> paused{t29::playAt(25, 5, fire)};
    p.submit(paused, 25, false);
    const uint64_t e2 = audio.engineFrame();
    CHECK(t29::startOf(audio, p, 5) == e2 + 800, "not real time: at once plus the horizon");
    const std::vector<ecs::AudioCommand> resumed{t29::playAt(26, 6, fire)};
    p.submit(resumed, 26, true);
    CHECK(t29::startOf(audio, p, 6) == e2 + 800 && p.late() == 1 && p.anchors() == 2, "running again: a new anchor, nothing late");
    // A restore puts the schedule's step back: the anchor is taken again, nothing counted late.
    const std::vector<ecs::AudioCommand> back{t29::playAt(3, 7, fire)};
    p.submit(back, 3, true);
    CHECK(t29::startOf(audio, p, 7) == e2 + 800 && p.late() == 1 && p.anchors() == 3, "a step back: a new anchor");
}

// No file is opened by a play once the bank has loaded; a bank sound without a file is skipped with one
// warning per name.
static void t29_preloaded() {
    AudioSystem audio;
    CHECK(audio.initialize(true), "a mixer without a device");
    AudioPlayer p;
    p.initialize(OHAO_SOURCE_DIR, audio);
    ecs::World w;
    ecs::FileAssetResolver files(OHAO_SOURCE_DIR);
    reflect::TypeRegistry types;
    ecs::registerSceneTypes(types);
    level::installLevels(w, files, types);
    ecs::PhysicsScene& physics = ecs::installPhysics(w, files);
    (void)ecs::installCombat(w);
    ecs::installRoundHook(w, types);
    (void)ecs::installAnimation(w, files);
    (void)ecs::installAudio(w, files);
    CHECK(static_cast<bool>(ecs::loadSceneFile(w, std::filesystem::path(OHAO_SOURCE_DIR) / "assets/scenes/blockout_play.scene.json", types)),
          "the play scene loads");
    w.update(1.0f / 60.0f);   // the schedule reads the world's bank in its first step
    p.update(w, false);       // and the player decodes it here
    const uint64_t opened = audio.filesOpened();
    entt::entity player = entt::null;
    for (auto [e, pc] : w.registry().view<const ecs::PlayerController>().each())
        if (pc.input) player = e;
    const glm::vec3 me = physics.playerInfo(player)->pos;
    physics.teleportPlayer(player, me, 180.0f);   // toward the enemies
    std::vector<float> scratch(800 * 2);
    for (int i = 0; i < 240; ++i) {
        InputEvent fire;
        fire.type = i % 40 == 13 ? InputEvent::Type::ActionUp : InputEvent::Type::ActionDown;
        fire.name = "fire";
        w.stepInput(i % 40 == 0 || i % 40 == 13 ? std::vector<InputEvent>{fire} : std::vector<InputEvent>{});
        w.update(1.0f / 60.0f);
        p.update(w, false);
        (void)audio.render(scratch, 800);
    }
    std::printf("\n      %llu voices started; %llu files opened at the bank's load, %llu after",
                static_cast<unsigned long long>(p.played()), static_cast<unsigned long long>(opened),
                static_cast<unsigned long long>(audio.filesOpened() - opened));
    CHECK(opened > 0 && p.played() > 10 && audio.filesOpened() == opened, "a fight's voices, no file opened after the load");

    // A sound with no file in any folder: the schedule names it, the player skips it, warned once.
    const AudioBank ghost = t29::bankFrom(R"({"buses": {"master": {}}, "sounds": {
        "ghost": {"files": "nowhere/ghost", "bus": "master", "shape": "ear"},
        "fire": {"files": "weapons/fire", "bus": "master", "shape": "ear"}}})");
    p.useBank(ghost);
    const std::vector<ecs::AudioCommand> two{t29::playAt(1, 100, static_cast<uint16_t>(ghost.sound("ghost"))),
                                             t29::playAt(2, 101, static_cast<uint16_t>(ghost.sound("ghost"))),
                                             t29::playAt(2, 102, static_cast<uint16_t>(ghost.sound("fire")))};
    const uint64_t before = p.played();
    p.submit(two, 2, false);
    CHECK(p.missing() == 1 && p.played() == before + 1 && !p.handleOf(100) && p.handleOf(102), "skipped, one warning; the next plays");
}

// ---- sub-project 7, step 4: the capturing click (runtime/click_capture.hpp) -------------------------------------------------------------------

static void sp7_fx_the_capturing_click_never_fires() {
    const auto button = [](bool down, int code = 0) {
        InputEvent e;
        e.type = down ? InputEvent::Type::Down : InputEvent::Type::Up;
        e.device = InputDevice::MouseButton;
        e.code = code;
        return e;
    };
    InputEvent key;
    key.type = InputEvent::Type::Down;
    key.device = InputDevice::Key;
    key.code = 87;   // W
    ClickToCapture click;
    std::vector<InputEvent> frame{key, button(true)};
    CHECK(click.filter(frame, false), "a press on a free cursor asks for the cursor");
    CHECK(frame.size() == 1 && frame[0].device == InputDevice::Key, "the press is taken out; keys pass");
    frame = {button(false)};
    CHECK(!click.filter(frame, true) && frame.empty(), "and its release (the cursor captured since)");
    frame = {button(true), button(false)};
    CHECK(!click.filter(frame, true) && frame.size() == 2, "with the cursor captured, buttons pass (they fire)");
    frame = {button(true, 1), button(false, 1)};
    CHECK(click.filter(frame, false) && frame.empty(), "a press and release in one frame on a free cursor: both out");

    // Through an InputMap bound like the play scene (fire and capture_mouse on the left button):
    // the click that captures the cursor never presses fire.
    InputMap map;
    map.bindAction("fire", *keyBinding(Key::MouseLeft));
    ClickToCapture c2;
    bool captured = false;
    int firePresses = 0;
    for (int f = 0; f < 4; ++f) {
        std::vector<InputEvent> events;
        if (f == 0) events.push_back(button(true));
        if (f == 1) events.push_back(button(false));
        if (f == 2) events.push_back(button(true));
        if (f == 3) events.push_back(button(false));
        if (c2.filter(events, captured)) captured = true;
        map.beginStep(events);
        firePresses += map.pressed("fire") ? 1 : 0;
    }
    CHECK(captured && firePresses == 1, "the first click captures, the second fires: one press of fire");
}

int main() {
    std::printf("[combat sounds, capturing click]\n");
    RUN(sp9_t21_particles_from_events);
    RUN(sp7_fx_the_capturing_click_never_fires);
    RUN(t29_player_clock);
    RUN(t29_preloaded);
    std::printf("[footsteps]\n");
    std::printf("[audio: voices, curves, occlusion]\n");
    std::printf("[render stats]\n");
    RUN(sp2b_t14_stats_are_medians_per_pass_in_registry_order);
    std::printf("[capture images]\n");
    RUN(sp4_t16_capture_png_round_trips_losslessly);
    RUN(sp4_t16_crop_scale_and_jpeg_preview);
    std::printf("[clock]\n");
    RUN(clock_sixty_frames_of_one_sixtieth_is_sixty_steps);
    RUN(clock_half_step_frames_alternate);
    RUN(clock_clamps_a_long_stall);
    RUN(clock_rejects_negative_and_zero_dt);
    RUN(clock_alpha_always_in_unit_interval);
    RUN(frame_stats_average_and_fps);
    std::printf("[timeline]\n");
    RUN(sp4_t11_lockstep_ignores_wall_time);
    RUN(sp4_t11_realtime_counts_are_exact);
    RUN(sp4_t11_pause_and_mode_switches_neither_run_nor_drop_a_step);
    std::printf("[input]\n");
    RUN(press_hold_release_edges);
    RUN(tap_within_one_frame);
    RUN(repress_within_one_frame);
    RUN(press_after_focus_loss_in_one_frame);
    RUN(two_bindings_one_action);
    RUN(rebind_replaces);
    RUN(focus_loss_releases_everything);
    RUN(axis_from_key_pair);
    RUN(mouse_delta_no_first_jump);
    RUN(unknown_names_are_inert);
    RUN(sp4_t13_key_names_round_trip);
    RUN(sp4_t13_virtual_device_acts_like_a_device);
    RUN(sp4_t13_a_rebind_counts_at_the_next_step_without_an_event);
    std::printf("[swapchain choice]\n");
    RUN(choose_format_prefers_unorm_bgra);
    RUN(sp9_t20_present_mode_choice);
    RUN(choose_image_count_unlimited_max);
    RUN(choose_extent_uses_desired_when_surface_defers);
    RUN(zero_extent_means_minimized);
    std::printf("[frame timing]\n");
    RUN(sp9_t19_latency_stages_ordered);
    RUN(sp9_t20_pacing_stats);
    RUN(sp9_t20_frames_ahead_bounded);
    std::printf("\n");
    return ohao::test::finish("runtime_tests");
}
