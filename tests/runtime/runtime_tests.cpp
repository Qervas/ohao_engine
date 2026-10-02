// Runtime host tests. No GPU, no window: every input here is synthetic.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/clock.hpp"
#include "runtime/input.hpp"

static int g_pass = 0, g_fail = 0;
#define CHECK(expr, msg)                                                                     \
    do {                                                                                     \
        if (!(expr)) { std::printf("    FAIL: %s  [%s]\n", msg, #expr); ++g_fail; return; }  \
    } while (0)
#define RUN(fn)                                                       \
    do {                                                              \
        std::printf("  %-52s", #fn);                                  \
        int f0 = g_fail;                                              \
        fn();                                                         \
        if (g_fail == f0) { ++g_pass; std::printf("PASS\n"); }        \
    } while (0)

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

// ---------------------------------------------------------------- input

constexpr int KEY_W = 87, KEY_S = 83, KEY_SPACE = 32, KEY_UP = 265;   // GLFW codes
constexpr int MOUSE_LEFT = 0;

static InputEvent down(int k) { return {InputEvent::Type::Down, InputDevice::Key, k}; }
static InputEvent up(int k)   { return {InputEvent::Type::Up,   InputDevice::Key, k}; }

static void press_hold_release_edges() {
    InputMap m;
    m.bindAction("jump", {InputDevice::Key, KEY_SPACE});
    std::vector<InputEvent> f1{down(KEY_SPACE)};
    m.beginFrame(f1);
    CHECK(m.pressed("jump") && m.held("jump") && !m.released("jump"), "frame of the press");
    m.beginFrame({});
    CHECK(!m.pressed("jump") && m.held("jump"), "next frame: held, not pressed again");
    std::vector<InputEvent> f3{up(KEY_SPACE)};
    m.beginFrame(f3);
    CHECK(!m.held("jump") && m.released("jump"), "frame of the release");
    m.beginFrame({});
    CHECK(!m.released("jump"), "released is a one-frame edge");
}

static void tap_within_one_frame() {
    InputMap m;
    m.bindAction("fire", {InputDevice::MouseButton, MOUSE_LEFT});
    std::vector<InputEvent> f{{InputEvent::Type::Down, InputDevice::MouseButton, MOUSE_LEFT},
                              {InputEvent::Type::Up,   InputDevice::MouseButton, MOUSE_LEFT}};
    m.beginFrame(f);
    CHECK(m.pressed("fire"), "a tap shorter than a frame must still register as pressed");
    CHECK(m.released("fire"), "...and as released");
    CHECK(!m.held("fire"), "...but not as held");
}

static void two_bindings_one_action() {
    InputMap m;
    m.bindAction("forward", {InputDevice::Key, KEY_W});
    m.bindAction("forward", {InputDevice::Key, KEY_UP});
    std::vector<InputEvent> a{down(KEY_W), down(KEY_UP)};
    m.beginFrame(a);
    std::vector<InputEvent> b{up(KEY_W)};
    m.beginFrame(b);
    CHECK(m.held("forward"), "releasing one binding keeps the action held by the other");
    CHECK(!m.released("forward"), "and is not a release of the action");
}

static void rebind_replaces() {
    InputMap m;
    m.bindAction("jump", {InputDevice::Key, KEY_SPACE});
    m.clearAction("jump");
    m.bindAction("jump", {InputDevice::Key, KEY_W});
    std::vector<InputEvent> f{down(KEY_SPACE)};
    m.beginFrame(f);
    CHECK(!m.held("jump"), "the old key no longer triggers the action");
    std::vector<InputEvent> g{down(KEY_W)};
    m.beginFrame(g);
    CHECK(m.held("jump"), "the new key does");
}

static void focus_loss_releases_everything() {
    InputMap m;
    m.bindAction("sprint", {InputDevice::Key, KEY_W});
    m.bindAxis("move", {InputDevice::Key, KEY_W}, {InputDevice::Key, KEY_S});
    std::vector<InputEvent> a{down(KEY_W)};
    m.beginFrame(a);
    std::vector<InputEvent> b{{InputEvent::Type::FocusLost}};
    m.beginFrame(b);
    CHECK(!m.held("sprint") && m.released("sprint"), "alt-tab must release held actions");
    CHECK(m.axis("move") == 0.0f, "and zero every axis");
}

static void axis_from_key_pair() {
    InputMap m;
    m.bindAxis("move", {InputDevice::Key, KEY_W}, {InputDevice::Key, KEY_S});
    std::vector<InputEvent> a{down(KEY_W)};
    m.beginFrame(a);
    CHECK(m.axis("move") == 1.0f, "positive key");
    std::vector<InputEvent> b{down(KEY_S)};
    m.beginFrame(b);
    CHECK(m.axis("move") == 0.0f, "both keys cancel");
    std::vector<InputEvent> c{up(KEY_W)};
    m.beginFrame(c);
    CHECK(m.axis("move") == -1.0f, "negative key");
}

static void mouse_delta_no_first_jump() {
    InputMap m;
    std::vector<InputEvent> a{{InputEvent::Type::MouseMove, InputDevice::Key, 0, 500.0, 300.0}};
    m.beginFrame(a);
    CHECK(m.mouseDelta() == glm::vec2(0.0f), "first move establishes position, no jump");
    std::vector<InputEvent> b{{InputEvent::Type::MouseMove, InputDevice::Key, 0, 510.0, 295.0},
                              {InputEvent::Type::MouseMove, InputDevice::Key, 0, 512.0, 290.0}};
    m.beginFrame(b);
    CHECK(m.mouseDelta() == glm::vec2(12.0f, -10.0f), "deltas sum within a frame");
    m.beginFrame({});
    CHECK(m.mouseDelta() == glm::vec2(0.0f), "no motion, no delta");
}

static void unknown_names_are_inert() {
    InputMap m;
    m.beginFrame({});
    CHECK(!m.pressed("nope") && !m.held("nope") && !m.released("nope"), "unbound action is false");
    CHECK(m.axis("nope") == 0.0f, "unbound axis is zero");
}

int main() {
    std::printf("[clock]\n");
    RUN(clock_sixty_frames_of_one_sixtieth_is_sixty_steps);
    RUN(clock_half_step_frames_alternate);
    RUN(clock_clamps_a_long_stall);
    RUN(clock_rejects_negative_and_zero_dt);
    RUN(clock_alpha_always_in_unit_interval);
    RUN(frame_stats_average_and_fps);
    std::printf("[input]\n");
    RUN(press_hold_release_edges);
    RUN(tap_within_one_frame);
    RUN(two_bindings_one_action);
    RUN(rebind_replaces);
    RUN(focus_loss_releases_everything);
    RUN(axis_from_key_pair);
    RUN(mouse_delta_no_first_jump);
    RUN(unknown_names_are_inert);
    std::printf("\nResults: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
