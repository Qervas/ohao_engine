// Runtime host tests. No GPU, no window: every input here is synthetic.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/clock.hpp"

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

int main() {
    std::printf("[clock]\n");
    RUN(clock_sixty_frames_of_one_sixtieth_is_sixty_steps);
    RUN(clock_half_step_frames_alternate);
    RUN(clock_clamps_a_long_stall);
    RUN(clock_rejects_negative_and_zero_dt);
    RUN(clock_alpha_always_in_unit_interval);
    RUN(frame_stats_average_and_fps);
    std::printf("\nResults: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
