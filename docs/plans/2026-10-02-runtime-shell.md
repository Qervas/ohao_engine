# Runtime Shell Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** OHAO runs as a windowed game host — GLFW window, Vulkan swapchain the deferred renderer presents into by blit, fixed-step engine loop, input action map — with the Forward render path deleted.

**Architecture:** A new `ohao_runtime` static library holds the host: a GPU-free `FixedStepClock` and `InputMap` (unit-tested with synthetic input), a GLFW `Window`, and an `Application` that runs the loop. `VulkanRenderer` gains a second initialize path that takes a `PresentTarget` (instance extensions + a surface factory); in that mode `renderDeferred` blits the final deferred image into a `Swapchain` image and presents instead of copying to a readback buffer. Headless mode is untouched, so the golden corpus guards every task.

**Tech Stack:** C++20, Vulkan 1.3, GLFW 3.4 (already built from source by `external/cmake/glfw.cmake`), glm, MSVC / Visual Studio 18 2026 generator.

**Spec:** `docs/specs/2026-10-02-runtime-shell-design.md`

## Global Constraints

- Build with `/c/Users/djmax/scoop/shims/cmake` (conda's cmake shadows it). Configure: `/c/Users/djmax/scoop/shims/cmake -B build -S .`. Build: `/c/Users/djmax/scoop/shims/cmake --build build --config Release -j8`. Shaders are a separate target: `--target shaders`.
- Binaries: `build/Release/*.exe`. Run everything from the repo root.
- Fixed step default **1/60 s**; `maxSteps` default **5**.
- Swapchain: prefer **`VK_FORMAT_B8G8R8A8_SRGB` / `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`**; present mode **MAILBOX** if available else **FIFO**; `--vsync` forces FIFO; image count **`minImageCount + 1`** clamped to `maxImageCount` (0 = unlimited); usage **`TRANSFER_DST`**.
- Render-finished semaphores: **one per swapchain image**. Image-acquired semaphores: one per frame slot (`MAX_FRAMES_IN_FLIGHT = 3`).
- The renderer never includes runtime or GLFW headers. GLFW is linked only by `ohao_runtime`.
- Headless output must not change: **golden corpus `max_abs=0` on both scenes after every task** — `python tests/golden/render_golden.py tests/golden/manifest.json`.
- Done-criterion: `ohao_game` at 1080p deferred ≥ 60 fps; scripted 600-frame run with a forced resize has **zero validation messages** and **zero readback copies**.
- Commit messages end with: `Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN`

## Review Focus

1. **A frame longer than `maxSteps × fixedDt`** (debugger break, window drag on Windows stalls the loop): the clock must run at most `maxSteps` steps and discard the rest, not spiral — tested in Task 1 (`clock_clamps_a_long_stall`).
2. **A key pressed and released inside one frame** (fast tap at low fps): `pressed` and `released` must both be true that frame — tested in Task 2 (`tap_within_one_frame`).
3. **Focus loss while a key is held** (alt-tab mid-sprint): every held action must release, or the player runs forever — tested in Task 2 (`focus_loss_releases_everything`).
4. **Window minimized** (framebuffer 0×0): no swapchain recreation with a zero extent, no render, simulation keeps stepping — tested in Task 5 (`zero_extent_means_minimized`) and Task 7's scripted run.
5. **Surface capabilities with sentinel values** (`currentExtent = 0xFFFFFFFF`, `maxImageCount = 0`): extent must come from the window, image count must not clamp to 0 — tested in Task 5 (`choose_extent_uses_desired_when_surface_defers`, `choose_image_count_unlimited_max`).

---

## File Structure

```
ohao/runtime/CMakeLists.txt          NEW  ohao_runtime static lib (glob), links ohao_core glm; glfw added in Task 6
ohao/runtime/clock.hpp/.cpp          NEW  FixedStepClock, FrameStats            (no GPU, no GLFW)
ohao/runtime/input.hpp/.cpp          NEW  InputEvent, Binding, InputMap         (no GPU, no GLFW)
ohao/runtime/window.hpp/.cpp         NEW  GLFW window → InputEvents, surface    (Task 6)
ohao/runtime/application.hpp/.cpp    NEW  GameHooks, AppConfig, Application     (Task 7)
ohao/CMakeLists.txt                  MOD  add_subdirectory(runtime)

ohao/gpu/vulkan/swapchain.hpp/.cpp   NEW  Swapchain + pure choose* helpers      (Task 5)
ohao/gpu/vulkan/present_target.hpp   NEW  PresentTarget struct                  (Task 6)
ohao/gpu/vulkan/renderer.hpp/.cpp    MOD  Forward removed; resize fix; windowed init; counters
ohao/gpu/vulkan/device_setup.cpp     MOD  surface ext, swapchain ext, present support, debug messenger
ohao/gpu/vulkan/render_dispatch.cpp  MOD  Forward paths deleted; present path in renderDeferred
ohao/gpu/vulkan/pipeline.cpp         MOD  forward + forward-shadow pipelines deleted
ohao/render/deferred/deferred_renderer.hpp/.cpp  MOD  gpuFrameMs() accessor

tests/runtime/CMakeLists.txt         NEW  runtime_tests executable
tests/runtime/runtime_tests.cpp      NEW  clock, input, swapchain-choice tests
tests/engine/engine_tests.cpp        MOD  headless resize-after-initialize test
CMakeLists.txt                       MOD  BUILD_RUNTIME_TESTS option

examples/ohao_game.cpp               NEW  windowed host (Task 7)
examples/interactive.cpp             DEL
examples/CMakeLists.txt              MOD
shaders/core/forward.{vert,frag}     DEL  (Task 3)
shaders/shadow/shadow_depth.{vert,frag} DEL (Task 3)
```

---

### Task 1: `ohao_runtime` library and the fixed-step clock

**Files:**
- Create: `ohao/runtime/CMakeLists.txt`, `ohao/runtime/clock.hpp`, `ohao/runtime/clock.cpp`
- Create: `tests/runtime/CMakeLists.txt`, `tests/runtime/runtime_tests.cpp`
- Modify: `ohao/CMakeLists.txt`, `CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  namespace ohao::runtime {
  using Nanos = std::chrono::nanoseconds;
  struct ClockConfig { Nanos fixedStep{16'666'667}; int maxSteps{5}; };
  struct Tick { Nanos frameDt; int steps; float alpha; Nanos discarded; };
  class FixedStepClock {
  public:
      explicit FixedStepClock(ClockConfig cfg = {});
      Tick advance(Nanos frameDt);
      [[nodiscard]] float fixedDtSeconds() const noexcept;
      [[nodiscard]] uint64_t totalSteps() const noexcept;
      [[nodiscard]] Nanos totalDiscarded() const noexcept;
  };
  class FrameStats {
  public:
      explicit FrameStats(size_t window = 120);
      void record(double cpuMs, double gpuMs);
      [[nodiscard]] double avgCpuMs() const; [[nodiscard]] double avgGpuMs() const;
      [[nodiscard]] double fps() const;        // 1000 / avgCpuMs, 0 if no samples
      [[nodiscard]] uint64_t frames() const noexcept;
  };
  }
  ```

Integer nanoseconds, not floating seconds: 60 frames of 1/60 s must produce exactly 60 steps, and accumulating `double` thirds drifts.

- [ ] **Step 1: Create the library and test target**

`ohao/runtime/CMakeLists.txt`:
```cmake
file(GLOB_RECURSE RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE RUNTIME_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_library(ohao_runtime STATIC ${RUNTIME_SOURCES} ${RUNTIME_HEADERS})
target_include_directories(ohao_runtime PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_runtime PUBLIC cxx_std_20)
target_link_libraries(ohao_runtime PUBLIC ohao_core glm)
```

Append to `ohao/CMakeLists.txt` after `add_subdirectory(physics)`:
```cmake
add_subdirectory(runtime)
```

In root `CMakeLists.txt`, after the `BUILD_ENGINE_TESTS` block:
```cmake
option(BUILD_RUNTIME_TESTS "Build runtime host tests (clock, input, swapchain choice)" ON)
if(BUILD_RUNTIME_TESTS)
    message(STATUS "Building runtime tests")
    add_subdirectory(tests/runtime)
endif()
```

`tests/runtime/CMakeLists.txt`:
```cmake
add_executable(runtime_tests runtime_tests.cpp)
target_include_directories(runtime_tests PRIVATE ${CMAKE_SOURCE_DIR}/ohao ${Vulkan_INCLUDE_DIRS})
target_compile_features(runtime_tests PRIVATE cxx_std_20)
target_link_libraries(runtime_tests PRIVATE ohao_runtime)
add_test(NAME RuntimeTests COMMAND runtime_tests)
```

- [ ] **Step 2: Write the failing tests**

`tests/runtime/runtime_tests.cpp`:
```cpp
// Runtime host tests. No GPU, no window: every input here is synthetic.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/clock.hpp"

static int g_pass = 0, g_fail = 0;
#define CHECK(expr, msg)                                                         \
    do {                                                                         \
        if (!(expr)) { std::printf("    FAIL: %s  [%s]\n", msg, #expr); ++g_fail; return; } \
    } while (0)
#define RUN(fn) do { std::printf("  %-52s", #fn); int f0 = g_fail; fn(); \
    if (g_fail == f0) { ++g_pass; std::printf("PASS\n"); } } while (0)

using namespace ohao::runtime;
using namespace std::chrono_literals;

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
    CHECK(next.steps == 1, "the frame after a stall is normal again — no spiral");
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
    s.record(10.0, 5.0); s.record(20.0, 7.0);
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
```

- [ ] **Step 3: Configure and confirm the build fails**

Run: `/c/Users/djmax/scoop/shims/cmake -B build -S . && /c/Users/djmax/scoop/shims/cmake --build build --config Release --target runtime_tests`
Expected: compile error, `runtime/clock.hpp` not found.

- [ ] **Step 4: Implement the clock**

`ohao/runtime/clock.hpp`:
```cpp
#pragma once
#include <chrono>
#include <cstdint>
#include <deque>

namespace ohao::runtime {

using Nanos = std::chrono::nanoseconds;

struct ClockConfig {
    Nanos fixedStep{16'666'667};   // 1/60 s
    int   maxSteps{5};             // per frame; the rest is discarded
};

struct Tick {
    Nanos frameDt{0};      // clamped to >= 0
    int   steps{0};        // fixed updates to run this frame
    float alpha{0.0f};     // leftover / fixedStep, in [0,1): render interpolation
    Nanos discarded{0};    // time dropped this frame by the maxSteps clamp
};

// Fixed-step accumulator in integer nanoseconds. Floating seconds drift:
// 60 frames of 1/60 s must be exactly 60 steps, always.
class FixedStepClock {
public:
    explicit FixedStepClock(ClockConfig cfg = {}) : m_cfg(cfg) {}

    Tick advance(Nanos frameDt);

    [[nodiscard]] float    fixedDtSeconds() const noexcept {
        return std::chrono::duration<float>(m_cfg.fixedStep).count();
    }
    [[nodiscard]] uint64_t totalSteps() const noexcept { return m_totalSteps; }
    [[nodiscard]] Nanos    totalDiscarded() const noexcept { return m_totalDiscarded; }

private:
    ClockConfig m_cfg;
    Nanos       m_accumulator{0};
    uint64_t    m_totalSteps{0};
    Nanos       m_totalDiscarded{0};
};

class FrameStats {
public:
    explicit FrameStats(size_t window = 120) : m_window(window) {}
    void record(double cpuMs, double gpuMs);
    [[nodiscard]] double   avgCpuMs() const;
    [[nodiscard]] double   avgGpuMs() const;
    [[nodiscard]] double   fps() const;
    [[nodiscard]] uint64_t frames() const noexcept { return m_frames; }

private:
    size_t m_window;
    std::deque<double> m_cpu, m_gpu;
    uint64_t m_frames{0};
};

} // namespace ohao::runtime
```

`ohao/runtime/clock.cpp`:
```cpp
#include "runtime/clock.hpp"
#include <numeric>

namespace ohao::runtime {

Tick FixedStepClock::advance(Nanos frameDt) {
    Tick t;
    t.frameDt = frameDt.count() > 0 ? frameDt : Nanos{0};
    m_accumulator += t.frameDt;

    const auto step = m_cfg.fixedStep;
    int64_t due = m_accumulator / step;
    if (due > m_cfg.maxSteps) {
        // Drop whole steps beyond the clamp; keep the sub-step remainder so the
        // next frame's alpha is still meaningful.
        const Nanos excess = (due - m_cfg.maxSteps) * step;
        m_accumulator -= excess;
        t.discarded = excess;
        m_totalDiscarded += excess;
        due = m_cfg.maxSteps;
    }
    m_accumulator -= due * step;
    t.steps = static_cast<int>(due);
    m_totalSteps += static_cast<uint64_t>(due);
    t.alpha = static_cast<float>(static_cast<double>(m_accumulator.count()) /
                                 static_cast<double>(step.count()));
    return t;
}

void FrameStats::record(double cpuMs, double gpuMs) {
    m_cpu.push_back(cpuMs);
    m_gpu.push_back(gpuMs);
    while (m_cpu.size() > m_window) m_cpu.pop_front();
    while (m_gpu.size() > m_window) m_gpu.pop_front();
    ++m_frames;
}

static double mean(const std::deque<double>& d) {
    return d.empty() ? 0.0 : std::accumulate(d.begin(), d.end(), 0.0) / d.size();
}

double FrameStats::avgCpuMs() const { return mean(m_cpu); }
double FrameStats::avgGpuMs() const { return mean(m_gpu); }
double FrameStats::fps() const {
    const double ms = avgCpuMs();
    return ms > 0.0 ? 1000.0 / ms : 0.0;
}

} // namespace ohao::runtime
```

- [ ] **Step 5: Build and run — expect all pass**

Run: `/c/Users/djmax/scoop/shims/cmake --build build --config Release --target runtime_tests && ./build/Release/runtime_tests.exe`
Expected: `Results: 6/6 passed`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add ohao/runtime ohao/CMakeLists.txt CMakeLists.txt tests/runtime
git commit -m "feat(runtime): fixed-step clock and frame stats in a new ohao_runtime library

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```

---

### Task 2: Input action map

**Files:**
- Create: `ohao/runtime/input.hpp`, `ohao/runtime/input.cpp`
- Modify: `tests/runtime/runtime_tests.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  ```cpp
  namespace ohao::runtime {
  enum class InputDevice : uint8_t { Key, MouseButton };
  struct Binding { InputDevice device; int code; };     // codes are GLFW key / mouse-button values
  struct InputEvent {
      enum class Type : uint8_t { Down, Up, MouseMove, FocusLost };
      Type type; InputDevice device{InputDevice::Key}; int code{0};
      double x{0}, y{0};                                // MouseMove: absolute cursor position
  };
  class InputMap {
  public:
      void bindAction(std::string_view action, Binding b);    // adds; an action can have several
      void clearAction(std::string_view action);
      void bindAxis(std::string_view axis, Binding positive, Binding negative);
      void beginFrame(std::span<const InputEvent> events);
      [[nodiscard]] bool pressed(std::string_view action) const;
      [[nodiscard]] bool held(std::string_view action) const;
      [[nodiscard]] bool released(std::string_view action) const;
      [[nodiscard]] float axis(std::string_view axis) const;  // -1..1
      [[nodiscard]] glm::vec2 mouseDelta() const;
  };
  }
  ```

Semantics: `held` = down at end of frame. `pressed` = some binding went Down this frame (true even if it went Up again the same frame). `released` = the action stopped being held this frame, or a Down/Up pair happened within the frame. `FocusLost` releases every input. Unknown action names return false / 0, never throw. Mouse delta is the sum of MouseMove deltas this frame; the first MouseMove after construction or focus loss contributes no delta (no jump).

- [ ] **Step 1: Write the failing tests** — append to `tests/runtime/runtime_tests.cpp` above `main`, and add `#include "runtime/input.hpp"` at the top:

```cpp
constexpr int KEY_W = 87, KEY_S = 83, KEY_SPACE = 32, KEY_UP = 265;   // GLFW codes
constexpr int MOUSE_LEFT = 0;

static InputEvent down(int k) { return {InputEvent::Type::Down, InputDevice::Key, k}; }
static InputEvent up(int k)   { return {InputEvent::Type::Up,   InputDevice::Key, k}; }

static void press_hold_release_edges() {
    InputMap m; m.bindAction("jump", {InputDevice::Key, KEY_SPACE});
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
    InputMap m; m.bindAction("fire", {InputDevice::MouseButton, MOUSE_LEFT});
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
    InputMap m; m.bindAxis("move", {InputDevice::Key, KEY_W}, {InputDevice::Key, KEY_S});
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
```

Add to `main` after the clock tests:
```cpp
    std::printf("[input]\n");
    RUN(press_hold_release_edges);
    RUN(tap_within_one_frame);
    RUN(two_bindings_one_action);
    RUN(rebind_replaces);
    RUN(focus_loss_releases_everything);
    RUN(axis_from_key_pair);
    RUN(mouse_delta_no_first_jump);
    RUN(unknown_names_are_inert);
```

- [ ] **Step 2: Build — expect a compile failure** (`runtime/input.hpp` not found)

Run: `/c/Users/djmax/scoop/shims/cmake --build build --config Release --target runtime_tests`

- [ ] **Step 3: Implement**

`ohao/runtime/input.hpp`:
```cpp
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <glm/vec2.hpp>

namespace ohao::runtime {

enum class InputDevice : uint8_t { Key, MouseButton };

// Codes are GLFW's numeric key / mouse-button values. The map itself never
// includes GLFW; Window translates GLFW callbacks into InputEvents.
struct Binding {
    InputDevice device{InputDevice::Key};
    int code{0};
    auto operator<=>(const Binding&) const = default;
};

struct InputEvent {
    enum class Type : uint8_t { Down, Up, MouseMove, FocusLost };
    Type type{Type::Down};
    InputDevice device{InputDevice::Key};
    int code{0};
    double x{0.0}, y{0.0};
};

class InputMap {
public:
    void bindAction(std::string_view action, Binding b);
    void clearAction(std::string_view action);
    void bindAxis(std::string_view axis, Binding positive, Binding negative);

    void beginFrame(std::span<const InputEvent> events);

    [[nodiscard]] bool  pressed(std::string_view action) const;
    [[nodiscard]] bool  held(std::string_view action) const;
    [[nodiscard]] bool  released(std::string_view action) const;
    [[nodiscard]] float axis(std::string_view axis) const;
    [[nodiscard]] glm::vec2 mouseDelta() const { return m_mouseDelta; }

private:
    struct ActionState { bool pressed{false}, held{false}, released{false}; };
    struct AxisDef { Binding positive, negative; };

    [[nodiscard]] bool isDown(const Binding& b) const { return m_down.count(b) != 0; }

    std::map<std::string, std::vector<Binding>, std::less<>> m_actions;
    std::map<std::string, AxisDef, std::less<>> m_axes;
    std::map<std::string, ActionState, std::less<>> m_state;
    std::set<Binding> m_down;
    glm::vec2 m_mouseDelta{0.0f};
    glm::dvec2 m_lastMouse{0.0};
    bool m_haveMouse{false};
};

} // namespace ohao::runtime
```

`ohao/runtime/input.cpp`:
```cpp
#include "runtime/input.hpp"
#include <glm/vec2.hpp>

namespace ohao::runtime {

void InputMap::bindAction(std::string_view action, Binding b) {
    auto& v = m_actions[std::string(action)];
    for (const auto& e : v) if (e == b) return;
    v.push_back(b);
}

void InputMap::clearAction(std::string_view action) {
    if (auto it = m_actions.find(action); it != m_actions.end()) it->second.clear();
}

void InputMap::bindAxis(std::string_view axis, Binding positive, Binding negative) {
    m_axes[std::string(axis)] = AxisDef{positive, negative};
}

void InputMap::beginFrame(std::span<const InputEvent> events) {
    // Which bindings went down at any point in this frame, and which went up.
    std::set<Binding> wentDown, wentUp;
    bool focusLost = false;
    m_mouseDelta = glm::vec2(0.0f);

    for (const auto& e : events) {
        switch (e.type) {
        case InputEvent::Type::Down: {
            const Binding b{e.device, e.code};
            wentDown.insert(b);
            m_down.insert(b);
            break;
        }
        case InputEvent::Type::Up: {
            const Binding b{e.device, e.code};
            wentUp.insert(b);
            m_down.erase(b);
            break;
        }
        case InputEvent::Type::MouseMove: {
            const glm::dvec2 p{e.x, e.y};
            if (m_haveMouse) m_mouseDelta += glm::vec2(p - m_lastMouse);
            m_lastMouse = p;
            m_haveMouse = true;
            break;
        }
        case InputEvent::Type::FocusLost:
            focusLost = true;
            break;
        }
    }
    if (focusLost) {
        m_down.clear();
        m_haveMouse = false;
    }

    for (const auto& [name, bindings] : m_actions) {
        ActionState& s = m_state[name];
        const bool wasHeld = s.held;
        bool nowHeld = false, anyDown = false, anyUp = false;
        for (const auto& b : bindings) {
            nowHeld |= isDown(b);
            anyDown |= wentDown.count(b) != 0;
            anyUp   |= wentUp.count(b) != 0;
        }
        s.held     = nowHeld;
        // Pressed: went down this frame and was not already held at frame start
        // (OS key-repeat Downs must not re-fire it).
        s.pressed  = anyDown && !wasHeld;
        // Released: the action stopped being held, or a tap went down and up
        // inside this frame without ever being observed as held.
        s.released = (wasHeld && !nowHeld) || (anyDown && anyUp && !nowHeld);
    }
}

bool InputMap::pressed(std::string_view a) const {
    auto it = m_state.find(a); return it != m_state.end() && it->second.pressed;
}
bool InputMap::held(std::string_view a) const {
    auto it = m_state.find(a); return it != m_state.end() && it->second.held;
}
bool InputMap::released(std::string_view a) const {
    auto it = m_state.find(a); return it != m_state.end() && it->second.released;
}

float InputMap::axis(std::string_view a) const {
    auto it = m_axes.find(a);
    if (it == m_axes.end()) return 0.0f;
    return (isDown(it->second.positive) ? 1.0f : 0.0f) - (isDown(it->second.negative) ? 1.0f : 0.0f);
}

} // namespace ohao::runtime
```

- [ ] **Step 4: Run** — `./build/Release/runtime_tests.exe` → `Results: 14/14 passed`

- [ ] **Step 5: Commit**

```bash
git add ohao/runtime/input.hpp ohao/runtime/input.cpp tests/runtime/runtime_tests.cpp
git commit -m "feat(runtime): input action map with press/hold/release edges, axes, mouse delta

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```

---

### Task 3: Delete the Forward render path

**Files:**
- Modify: `ohao/gpu/vulkan/renderer.hpp`, `renderer.cpp`, `render_dispatch.cpp`, `pipeline.cpp`, `scene_upload.cpp`, `buffer_setup.cpp`, `descriptor*.cpp` (whichever define the forward descriptor functions — find with the grep in Step 1)
- Delete: `shaders/core/forward.vert`, `shaders/core/forward.frag`, `shaders/shadow/shadow_depth.vert`, `shaders/shadow/shadow_depth.frag`
- Modify: `examples/*.cpp`, `tests/**` only where they name `RenderMode::Forward`

**Interfaces:**
- Produces: `enum class RenderMode { Deferred, RTRealtime, RTOffline };` `isRasterRenderMode(m) == (m == RenderMode::Deferred)`. `VulkanRenderer::render()` = RT mode → `renderRTPipeline`, else `renderDeferred`. Default mode `Deferred`.

This is a deletion task: the "test" is the existing suite plus the golden corpus, before and after.

- [ ] **Step 1: Record the baseline and map the code**

```bash
python tests/golden/render_golden.py tests/golden/manifest.json        # expect ALL PASS
grep -rn "RenderMode::Forward\|renderLegacy\|renderMultiFrame\|renderSceneObjects\|renderShadowPass\|createPipeline\b\|createRenderPass\b\|createOffscreenFramebuffer\|createShadowRenderPass\|createShadowResources\|createShadowPipeline\|createDescriptorSetLayout\|createDescriptorPool\|createDescriptorSets\|createUniformBuffer\|createVertexBuffer\|m_pipeline\b\|m_pipelineLayout\b\|m_renderPass\b\|m_framebuffer\b\|m_descriptorSet\b\|m_uniformBuffer\|m_shadow" ohao/ examples/ tests/ --include=*.cpp --include=*.hpp
```

- [ ] **Step 2: Delete, in this order**

1. `renderer.hpp`: remove `Forward` from `RenderMode`; `isRasterRenderMode` returns `m == RenderMode::Deferred`; `m_renderMode{RenderMode::Deferred}`; delete declarations of `renderMultiFrame`, `renderLegacy`, `renderSceneObjects`, `renderShadowPass`, `createPipeline`, `createRenderPass`, `createOffscreenFramebuffer`, `cleanupFramebuffer`, `createShadowRenderPass`, `createShadowResources`, `createShadowPipeline`, `createDescriptorSetLayout`, `createDescriptorPool`, `createDescriptorSets`, `createUniformBuffer`, `createVertexBuffer` (the demo-triangle one — the scene's `m_vertexBuffer`/`m_indexBuffer` are created by scene upload and **stay**), and the members only they use (`m_pipeline`, forward `m_pipelineLayout`, `m_renderPass`, `m_framebuffer`, offscreen color/depth images, `m_descriptorSetLayout`, `m_descriptorPool`, `m_descriptorSet`, `m_uniformBuffer*`, all `m_shadow*`, `m_commandBuffer`/`m_renderFence` if only `renderLegacy` used them).
2. `renderer.cpp`: `render()` becomes:
   ```cpp
   void VulkanRenderer::render() {
       if (!m_initialized) return;
       if (const auto* rtPipeline = getRTPipeline(m_renderMode); rtPipeline) {
           renderRTPipeline(*rtPipeline);
           return;
       }
       renderDeferred();
   }
   ```
   Remove the deleted calls from `initialize()` (keep: instance, physical device, device, command pool, bindless texture manager, `createLightBuffer`, sync objects that deferred/RT use, frame resources, deferred renderer, RT). Remove their cleanup in `shutdown()`. In the constructor, the shader-path probe tests for `"core_deferred_lighting.frag.spv"` instead of `"core_forward.vert.spv"`. Delete `"Forward"` from the mode-name table and the "staying in Forward mode" fallback (deferred init failure is now an initialize failure).
3. `render_dispatch.cpp`: delete `renderMultiFrame`, `renderLegacy`. `resize` must no longer call `cleanupFramebuffer`/`createOffscreenFramebuffer` (Task 4 rewrites it; for now just remove those two calls).
4. `pipeline.cpp`: delete `createPipeline`, `createShadowPipeline`, and any forward-only helpers. `scene_upload.cpp`: delete `renderSceneObjects` and the `!m_pipeline` guard users. `buffer_setup.cpp`: delete `createVertexBuffer` (demo triangle) and `createUniformBuffer`.
5. `git rm shaders/core/forward.vert shaders/core/forward.frag shaders/shadow/shadow_depth.vert shaders/shadow/shadow_depth.frag`
6. Replace any `RenderMode::Forward` in examples/tests with `RenderMode::Deferred`.

Build after each numbered item; fix only what the deletion broke.

- [ ] **Step 3: Verify**

```bash
/c/Users/djmax/scoop/shims/cmake --build build --config Release -j8
rm -rf build/Release/bin/shaders && /c/Users/djmax/scoop/shims/cmake --build build --target shaders --config Release
grep -rn "RenderMode::Forward\|renderLegacy\|renderMultiFrame\|core_forward\|shadow_shadow_depth" ohao/ examples/ tests/   # expect no output
for t in engine_tests physics_backend_tests audio_system_tests force_generator_tests env_cdf_test denoise_parse_test sobol_test runtime_tests; do ./build/Release/$t.exe | tail -1; done
./build/Release/renderer_test.exe out_rt.png | tail -1
python tests/golden/render_golden.py tests/golden/manifest.json
./build/Release/model_viewer.exe assets/test_models/DamagedHelmet.glb deferred_check.png 4 deferred --denoise=none
```
Expected: all suites pass; golden `ALL PASS` at `max_abs=0`; the deferred render writes a non-blank PNG (open it). Delete `out_rt.png` / `deferred_check.png` afterwards.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(renderer): delete the Forward render path

Deferred is the raster path; render() dispatches RT modes or deferred, nothing
else. Forward's render pass, framebuffer, pipelines, shadow map, UBOs and
descriptor sets are gone, with forward.{vert,frag} and shadow_depth.{vert,frag}.
Headless output unchanged: golden corpus max_abs=0.

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```

---

### Task 4: `resize` reaches the deferred renderer and the camera

**Files:**
- Modify: `ohao/gpu/vulkan/render_dispatch.cpp` (`VulkanRenderer::resize`)
- Modify: `ohao/render/deferred/deferred_renderer.hpp/.cpp` (GPU frame-time accessor)
- Test: `tests/engine/engine_tests.cpp`

**Interfaces:**
- Produces: `VulkanRenderer::resize(w, h)` resizes staging buffers, the deferred renderer (`DeferredRenderer::onResize`), RT renderers, and sets `m_camera->setAspectRatio(w / float(h))`. `float DeferredRenderer::gpuFrameMs() const` = sum of the last frame's per-pass timings (0 when timing is unavailable). `float VulkanRenderer::gpuFrameMs() const` forwards it.

- [ ] **Step 1: Write the failing test** — in `tests/engine/engine_tests.cpp`, inside `runOrphanedEntryPointTests()` after the existing tests, add:

```cpp
    TEST_BEGIN("resize after initialize resizes deferred output and keeps rendering");
    {
        ohao::VulkanRenderer renderer(320, 180);
        if (!renderer.initialize()) {
            std::cout << "(no Vulkan device -- skipped) ";
            TEST_PASS();
            return;
        }
        renderer.setRenderMode(ohao::RenderMode::Deferred);
        for (int i = 0; i < 4; ++i) renderer.render();
        renderer.resize(256, 128);
        for (int i = 0; i < 4; ++i) renderer.render();   // drain the readback ring at the new size
        EXPECT_EQ(renderer.getPixelBufferSize(), static_cast<size_t>(256 * 128 * 4),
                  "readback buffer follows the resize");
        EXPECT(std::abs(renderer.getCamera()->getAspectRatio() - 2.0f) < 1e-5f,
               "resize must update the camera aspect, or the image is stretched");
        const uint8_t* px = renderer.getPixels();
        EXPECT(px != nullptr, "pixels available after resize");
        size_t nonZero = 0;
        for (size_t i = 3; i < renderer.getPixelBufferSize(); i += 4) nonZero += px[i] != 0;
        EXPECT(nonZero == static_cast<size_t>(256 * 128),
               "every pixel of the resized frame was written (alpha set); a stale 320x180 "
               "G-buffer copied into a 256x128 buffer leaves rows unwritten or crashes");
        TEST_PASS();
    }
```

If `getCamera()` does not exist on `VulkanRenderer`, add `[[nodiscard]] Camera* getCamera() const noexcept { return m_camera.get(); }` next to the other accessors in `renderer.hpp` (check the member's exact name with `grep -n "m_camera" ohao/gpu/vulkan/renderer.hpp`).

- [ ] **Step 2: Run — expect FAIL** on the aspect check (and possibly a validation/size failure on the pixel check).

Run: `/c/Users/djmax/scoop/shims/cmake --build build --config Release --target engine_tests && ./build/Release/engine_tests.exe | tail -6`

- [ ] **Step 3: Implement** — `VulkanRenderer::resize` in `render_dispatch.cpp` becomes:

```cpp
void VulkanRenderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;                 // minimized: keep the old targets
    if (width == m_width && height == m_height) return;

    m_width = width;
    m_height = height;
    m_pixelBuffer.resize(static_cast<size_t>(width) * height * 4);
    if (m_camera) m_camera->setAspectRatio(static_cast<float>(width) / static_cast<float>(height));

    if (!m_initialized) return;
    vkDeviceWaitIdle(m_device);
    m_frameResources.resizeStagingBuffers(static_cast<size_t>(width) * height * 4);
    if (m_deferredRenderer) m_deferredRenderer->onResize(width, height);
    forEachRTRenderer([&](auto& rt) { rt.resize(width, height); });
}
```
Keep the existing RT-resize mechanism if `forEachRTRenderer` has a different signature — check with `grep -n "forEachRTRenderer" ohao/gpu/vulkan/renderer.hpp` and keep whatever the old body used for RT. The existing `m_pixelBuffer.resize(width * height * 4)` early in the old body moves into this one.

In `deferred_renderer.hpp` (public section):
```cpp
    // Sum of last frame's per-pass GPU timings in ms; 0 when timing is unavailable.
    [[nodiscard]] float gpuFrameMs() const noexcept {
        float s = 0.0f;
        for (int i = 0; i < m_passTimingCount; ++i) s += m_passTimingsMs[i];
        return s;
    }
```
In `renderer.hpp`:
```cpp
    [[nodiscard]] float gpuFrameMs() const noexcept {
        return m_deferredRenderer ? m_deferredRenderer->gpuFrameMs() : 0.0f;
    }
```
(`renderer.hpp` forward-declares `DeferredRenderer`; if so, define `gpuFrameMs` in `renderer.cpp` instead.)

- [ ] **Step 4: Run** — `engine_tests` 51/51; golden `ALL PASS`; `OHAO_VALIDATION=1 ./build/Release/engine_tests.exe` prints no validation errors around the new test.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "fix(renderer): resize reaches the deferred G-buffer and the camera aspect

VulkanRenderer::resize never called DeferredRenderer::onResize, so resizing left
the G-buffer and post targets at the old size; it also never updated the camera
aspect. Adds gpuFrameMs() for frame stats.

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```

---

### Task 5: `Swapchain` and its pure choice functions

**Files:**
- Create: `ohao/gpu/vulkan/swapchain.hpp`, `ohao/gpu/vulkan/swapchain.cpp`
- Modify: `tests/runtime/CMakeLists.txt` (link `ohao_gpu_vulkan` + `${Vulkan_LIBRARIES}`), `tests/runtime/runtime_tests.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace ohao {
  class Swapchain {
  public:
      enum class Acquire { Ok, OutOfDate, Error };
      [[nodiscard]] bool create(VkPhysicalDevice, VkDevice, VkSurfaceKHR, VkExtent2D desired, bool vsync);
      void destroy();
      [[nodiscard]] bool recreate(VkExtent2D desired);            // waits idle; false if extent is 0
      [[nodiscard]] Acquire acquire(VkSemaphore imageAcquired, uint32_t& imageIndex);
      [[nodiscard]] VkResult present(VkQueue queue, uint32_t imageIndex);
      [[nodiscard]] VkImage     image(uint32_t i) const;
      [[nodiscard]] VkSemaphore renderFinished(uint32_t i) const;
      [[nodiscard]] VkExtent2D  extent() const noexcept;
      [[nodiscard]] VkFormat    format() const noexcept;
      [[nodiscard]] uint32_t    imageCount() const noexcept;
      [[nodiscard]] bool        valid() const noexcept;
      static VkSurfaceFormatKHR chooseFormat(std::span<const VkSurfaceFormatKHR>);
      static VkPresentModeKHR   choosePresentMode(std::span<const VkPresentModeKHR>, bool vsync);
      static uint32_t           chooseImageCount(const VkSurfaceCapabilitiesKHR&);
      static VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR&, VkExtent2D desired);
  };
  }
  ```

- [ ] **Step 1: Write the failing tests** — add `#include "gpu/vulkan/swapchain.hpp"` and:

```cpp
static void choose_format_prefers_srgb_bgra() {
    VkSurfaceFormatKHR fs[] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                               {VK_FORMAT_B8G8R8A8_SRGB,  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    auto f = ohao::Swapchain::chooseFormat(fs);
    CHECK(f.format == VK_FORMAT_B8G8R8A8_SRGB, "prefer BGRA8 sRGB");
    VkSurfaceFormatKHR only[] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    CHECK(ohao::Swapchain::chooseFormat(only).format == VK_FORMAT_R8G8B8A8_UNORM, "else first offered");
}

static void choose_present_mode() {
    VkPresentModeKHR all[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    CHECK(ohao::Swapchain::choosePresentMode(all, false) == VK_PRESENT_MODE_MAILBOX_KHR, "mailbox when free");
    CHECK(ohao::Swapchain::choosePresentMode(all, true) == VK_PRESENT_MODE_FIFO_KHR, "vsync forces fifo");
    VkPresentModeKHR fifo[] = {VK_PRESENT_MODE_FIFO_KHR};
    CHECK(ohao::Swapchain::choosePresentMode(fifo, false) == VK_PRESENT_MODE_FIFO_KHR, "fifo is the guaranteed fallback");
}

static void choose_image_count_unlimited_max() {
    VkSurfaceCapabilitiesKHR c{}; c.minImageCount = 2; c.maxImageCount = 0;   // 0 = no limit
    CHECK(ohao::Swapchain::chooseImageCount(c) == 3, "min+1 when max is unlimited");
    c.maxImageCount = 2;
    CHECK(ohao::Swapchain::chooseImageCount(c) == 2, "clamped to max");
}

static void choose_extent_uses_desired_when_surface_defers() {
    VkSurfaceCapabilitiesKHR c{};
    c.currentExtent = {0xFFFFFFFFu, 0xFFFFFFFFu};
    c.minImageExtent = {1, 1}; c.maxImageExtent = {4096, 4096};
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
    c.currentExtent = {0, 0}; c.minImageExtent = {0, 0}; c.maxImageExtent = {0, 0};
    auto e = ohao::Swapchain::chooseExtent(c, {1920, 1080});
    CHECK(e.width == 0 && e.height == 0, "a minimized window reports 0x0 and must not be inflated");
}
```
Register in `main` under `std::printf("[swapchain choice]\n");`.

`tests/runtime/CMakeLists.txt` link line becomes:
```cmake
target_link_libraries(runtime_tests PRIVATE ohao_runtime ohao_gpu_vulkan ${Vulkan_LIBRARIES})
if(MSVC)
    target_link_options(runtime_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
```

- [ ] **Step 2: Build — expect failure** (header missing).

- [ ] **Step 3: Implement**

`ohao/gpu/vulkan/swapchain.hpp`:
```cpp
#pragma once
#include <span>
#include <vector>
#include <vulkan/vulkan.h>

namespace ohao {

// Presentation target. Images are TRANSFER_DST only: the renderer blits its
// final image into them and never renders into them directly.
class Swapchain {
public:
    enum class Acquire { Ok, OutOfDate, Error };

    Swapchain() = default;
    ~Swapchain() { destroy(); }
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    [[nodiscard]] bool create(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface,
                              VkExtent2D desired, bool vsync);
    void destroy();
    [[nodiscard]] bool recreate(VkExtent2D desired);
    [[nodiscard]] Acquire  acquire(VkSemaphore imageAcquired, uint32_t& imageIndex);
    [[nodiscard]] VkResult present(VkQueue queue, uint32_t imageIndex);

    [[nodiscard]] VkImage     image(uint32_t i) const { return m_images[i]; }
    [[nodiscard]] VkSemaphore renderFinished(uint32_t i) const { return m_renderFinished[i]; }
    [[nodiscard]] VkExtent2D  extent() const noexcept { return m_extent; }
    [[nodiscard]] VkFormat    format() const noexcept { return m_format; }
    [[nodiscard]] uint32_t    imageCount() const noexcept { return static_cast<uint32_t>(m_images.size()); }
    [[nodiscard]] bool        valid() const noexcept { return m_swapchain != VK_NULL_HANDLE; }

    static VkSurfaceFormatKHR chooseFormat(std::span<const VkSurfaceFormatKHR> formats);
    static VkPresentModeKHR   choosePresentMode(std::span<const VkPresentModeKHR> modes, bool vsync);
    static uint32_t           chooseImageCount(const VkSurfaceCapabilitiesKHR& caps);
    static VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D desired);

private:
    [[nodiscard]] bool build(VkExtent2D desired, VkSwapchainKHR old);
    void destroySemaphores();

    VkPhysicalDevice m_physical{VK_NULL_HANDLE};
    VkDevice         m_device{VK_NULL_HANDLE};
    VkSurfaceKHR     m_surface{VK_NULL_HANDLE};
    VkSwapchainKHR   m_swapchain{VK_NULL_HANDLE};
    bool             m_vsync{false};
    VkFormat         m_format{VK_FORMAT_UNDEFINED};
    VkExtent2D       m_extent{0, 0};
    std::vector<VkImage>     m_images;
    std::vector<VkSemaphore> m_renderFinished;   // one per image, indexed by acquired image
};

} // namespace ohao
```

`ohao/gpu/vulkan/swapchain.cpp`:
```cpp
#include "gpu/vulkan/swapchain.hpp"
#include <algorithm>
#include <iostream>

namespace ohao {

VkSurfaceFormatKHR Swapchain::chooseFormat(std::span<const VkSurfaceFormatKHR> formats) {
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    return formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                           : formats.front();
}

VkPresentModeKHR Swapchain::choosePresentMode(std::span<const VkPresentModeKHR> modes, bool vsync) {
    if (!vsync)
        for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;   // the only mode the spec guarantees
}

uint32_t Swapchain::chooseImageCount(const VkSurfaceCapabilitiesKHR& caps) {
    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount != 0) n = std::min(n, caps.maxImageCount);   // 0 means unlimited
    return n;
}

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& caps, VkExtent2D desired) {
    if (caps.currentExtent.width != 0xFFFFFFFFu) return caps.currentExtent;   // surface dictates
    return {std::clamp(desired.width,  caps.minImageExtent.width,  caps.maxImageExtent.width),
            std::clamp(desired.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

bool Swapchain::create(VkPhysicalDevice physical, VkDevice device, VkSurfaceKHR surface,
                       VkExtent2D desired, bool vsync) {
    m_physical = physical; m_device = device; m_surface = surface; m_vsync = vsync;
    return build(desired, VK_NULL_HANDLE);
}

bool Swapchain::build(VkExtent2D desired, VkSwapchainKHR old) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_physical, m_surface, &caps);
    const VkExtent2D extent = chooseExtent(caps, desired);
    if (extent.width == 0 || extent.height == 0) return false;   // minimized

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_physical, m_surface, &n, formats.data());
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physical, m_surface, &n, modes.data());

    const VkSurfaceFormatKHR fmt = chooseFormat(formats);
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(m_physical, fmt.format, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        std::cerr << "[Swapchain] surface format " << fmt.format << " cannot be a blit destination" << std::endl;
        return false;
    }

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface          = m_surface;
    ci.minImageCount    = chooseImageCount(caps);
    ci.imageFormat      = fmt.format;
    ci.imageColorSpace  = fmt.colorSpace;
    ci.imageExtent      = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = choosePresentMode(modes, m_vsync);
    ci.clipped          = VK_TRUE;
    ci.oldSwapchain     = old;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(m_device, &ci, nullptr, &sc) != VK_SUCCESS) return false;
    if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, old, nullptr);
    m_swapchain = sc;
    m_format = fmt.format;
    m_extent = extent;

    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, nullptr);
    m_images.resize(n);
    vkGetSwapchainImagesKHR(m_device, m_swapchain, &n, m_images.data());

    destroySemaphores();
    m_renderFinished.resize(n);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : m_renderFinished)
        if (vkCreateSemaphore(m_device, &si, nullptr, &s) != VK_SUCCESS) return false;
    return true;
}

bool Swapchain::recreate(VkExtent2D desired) {
    vkDeviceWaitIdle(m_device);
    VkSwapchainKHR old = m_swapchain;
    m_swapchain = VK_NULL_HANDLE;
    if (!build(desired, old)) {
        // Minimized or failed: keep nothing half-built. A later recreate starts fresh.
        if (old != VK_NULL_HANDLE && m_swapchain == VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, old, nullptr);
        m_images.clear();
        destroySemaphores();
        return false;
    }
    return true;
}

Swapchain::Acquire Swapchain::acquire(VkSemaphore imageAcquired, uint32_t& imageIndex) {
    if (!valid()) return Acquire::OutOfDate;
    const VkResult r = vkAcquireNextImageKHR(m_device, m_swapchain, UINT64_MAX, imageAcquired,
                                             VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return Acquire::OutOfDate;
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) return Acquire::Ok;
    return Acquire::Error;
}

VkResult Swapchain::present(VkQueue queue, uint32_t imageIndex) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &m_renderFinished[imageIndex];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &m_swapchain;
    pi.pImageIndices      = &imageIndex;
    return vkQueuePresentKHR(queue, &pi);
}

void Swapchain::destroySemaphores() {
    for (auto s : m_renderFinished) if (s != VK_NULL_HANDLE) vkDestroySemaphore(m_device, s, nullptr);
    m_renderFinished.clear();
}

void Swapchain::destroy() {
    if (m_device == VK_NULL_HANDLE) return;
    destroySemaphores();
    if (m_swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(m_device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
    m_images.clear();
}

} // namespace ohao
```

`ohao/gpu/vulkan/CMakeLists.txt` globs its directory; reconfigure so the new files are picked up.

- [ ] **Step 4: Run** — reconfigure, build `runtime_tests`, run → `Results: 19/19 passed`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(gpu): Swapchain with per-image render-finished semaphores

Blit-destination swapchain (TRANSFER_DST), MAILBOX/FIFO selection, recreate on
resize, 0x0 treated as minimized. The format/mode/count/extent choices are pure
functions with unit tests, including the 0xFFFFFFFF extent and unlimited
maxImageCount sentinels.

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```

---

### Task 6: Window, windowed renderer initialize, and the present path

**Files:**
- Create: `ohao/runtime/window.hpp`, `ohao/runtime/window.cpp`, `ohao/gpu/vulkan/present_target.hpp`
- Modify: `ohao/runtime/CMakeLists.txt` (link `glfw`, `${Vulkan_LIBRARIES}`)
- Modify: `ohao/gpu/vulkan/renderer.hpp/.cpp`, `device_setup.cpp`, `render_dispatch.cpp`

**Interfaces:**
- Consumes: `Swapchain` (Task 5); `InputEvent` (Task 2); `resize` (Task 4).
- Produces:
  ```cpp
  // gpu/vulkan/present_target.hpp
  namespace ohao {
  struct PresentTarget {
      std::vector<const char*> instanceExtensions;                  // from the windowing system
      std::function<VkSurfaceKHR(VkInstance)> createSurface;        // called once after vkCreateInstance
      VkExtent2D extent{0, 0};
      bool vsync{false};
  };
  }
  // VulkanRenderer additions
  [[nodiscard]] bool initialize(const PresentTarget& target);       // windowed; no readback
  [[nodiscard]] bool isPresenting() const noexcept;
  [[nodiscard]] uint64_t readbackCopyCount() const noexcept;
  [[nodiscard]] uint64_t validationMessageCount() const noexcept;   // warnings+errors since init
  [[nodiscard]] bool validationEnabled() const noexcept;            // OHAO_VALIDATION=1 at init

  // runtime/window.hpp
  namespace ohao::runtime {
  struct WindowConfig { uint32_t width{1920}, height{1080}; std::string title{"OHAO"}; bool resizable{true}; };
  class Window {
  public:
      bool create(const WindowConfig&);
      void destroy();
      void poll(std::vector<InputEvent>& out);                      // appends this frame's events
      [[nodiscard]] bool shouldClose() const;
      void requestClose();
      [[nodiscard]] VkExtent2D framebufferExtent() const;           // 0x0 when minimized
      [[nodiscard]] bool consumeResized();                          // true once per size change
      void setCursorCaptured(bool);
      void setSize(uint32_t w, uint32_t h);                         // for the scripted resize
      [[nodiscard]] ohao::PresentTarget presentTarget(bool vsync) const;
  };
  }
  ```

- [ ] **Step 1: Window**

`ohao/runtime/CMakeLists.txt` — replace the link line:
```cmake
target_link_libraries(ohao_runtime PUBLIC ohao_core ohao_gpu_vulkan glm glfw ${Vulkan_LIBRARIES})
target_include_directories(ohao_runtime PUBLIC ${Vulkan_INCLUDE_DIRS})
```

`ohao/runtime/window.hpp`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "gpu/vulkan/present_target.hpp"
#include "runtime/input.hpp"

struct GLFWwindow;

namespace ohao::runtime {

struct WindowConfig {
    uint32_t    width{1920};
    uint32_t    height{1080};
    std::string title{"OHAO"};
    bool        resizable{true};
};

class Window {
public:
    Window() = default;
    ~Window() { destroy(); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(const WindowConfig& cfg);
    void destroy();
    void poll(std::vector<InputEvent>& out);
    [[nodiscard]] bool shouldClose() const;
    void requestClose();
    [[nodiscard]] VkExtent2D framebufferExtent() const;
    [[nodiscard]] bool consumeResized();
    void setCursorCaptured(bool captured);
    void setSize(uint32_t w, uint32_t h);
    [[nodiscard]] ohao::PresentTarget presentTarget(bool vsync) const;

private:
    static void onKey(GLFWwindow*, int key, int scancode, int action, int mods);
    static void onMouseButton(GLFWwindow*, int button, int action, int mods);
    static void onCursor(GLFWwindow*, double x, double y);
    static void onFocus(GLFWwindow*, int focused);
    static void onFramebufferSize(GLFWwindow*, int w, int h);

    GLFWwindow* m_window{nullptr};
    std::vector<InputEvent> m_pending;
    bool m_resized{false};
};

} // namespace ohao::runtime
```

`ohao/runtime/window.cpp`:
```cpp
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "runtime/window.hpp"
#include <iostream>

namespace ohao::runtime {

static Window* self(GLFWwindow* w) { return static_cast<Window*>(glfwGetWindowUserPointer(w)); }

bool Window::create(const WindowConfig& cfg) {
    if (!glfwInit()) { std::cerr << "[Window] glfwInit failed" << std::endl; return false; }
    if (!glfwVulkanSupported()) { std::cerr << "[Window] GLFW reports no Vulkan loader" << std::endl; return false; }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, cfg.resizable ? GLFW_TRUE : GLFW_FALSE);
    m_window = glfwCreateWindow(static_cast<int>(cfg.width), static_cast<int>(cfg.height),
                                cfg.title.c_str(), nullptr, nullptr);
    if (!m_window) { std::cerr << "[Window] glfwCreateWindow failed" << std::endl; return false; }
    glfwSetWindowUserPointer(m_window, this);
    glfwSetKeyCallback(m_window, onKey);
    glfwSetMouseButtonCallback(m_window, onMouseButton);
    glfwSetCursorPosCallback(m_window, onCursor);
    glfwSetWindowFocusCallback(m_window, onFocus);
    glfwSetFramebufferSizeCallback(m_window, onFramebufferSize);
    if (glfwRawMouseMotionSupported()) glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    return true;
}

void Window::destroy() {
    if (m_window) { glfwDestroyWindow(m_window); m_window = nullptr; glfwTerminate(); }
}

void Window::poll(std::vector<InputEvent>& out) {
    glfwPollEvents();
    out.insert(out.end(), m_pending.begin(), m_pending.end());
    m_pending.clear();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(m_window) != 0; }
void Window::requestClose() { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

VkExtent2D Window::framebufferExtent() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_window, &w, &h);
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

bool Window::consumeResized() { const bool r = m_resized; m_resized = false; return r; }

void Window::setCursorCaptured(bool c) {
    glfwSetInputMode(m_window, GLFW_CURSOR, c ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void Window::setSize(uint32_t w, uint32_t h) {
    glfwSetWindowSize(m_window, static_cast<int>(w), static_cast<int>(h));
}

ohao::PresentTarget Window::presentTarget(bool vsync) const {
    ohao::PresentTarget t;
    uint32_t n = 0;
    const char** ext = glfwGetRequiredInstanceExtensions(&n);
    t.instanceExtensions.assign(ext, ext + n);
    GLFWwindow* w = m_window;
    t.createSurface = [w](VkInstance inst) {
        VkSurfaceKHR s = VK_NULL_HANDLE;
        if (glfwCreateWindowSurface(inst, w, nullptr, &s) != VK_SUCCESS) s = VK_NULL_HANDLE;
        return s;
    };
    t.extent = framebufferExtent();
    t.vsync = vsync;
    return t;
}

void Window::onKey(GLFWwindow* w, int key, int, int action, int) {
    if (key == GLFW_KEY_UNKNOWN || action == GLFW_REPEAT) return;   // repeats are not presses
    self(w)->m_pending.push_back({action == GLFW_PRESS ? InputEvent::Type::Down : InputEvent::Type::Up,
                                  InputDevice::Key, key});
}
void Window::onMouseButton(GLFWwindow* w, int button, int action, int) {
    self(w)->m_pending.push_back({action == GLFW_PRESS ? InputEvent::Type::Down : InputEvent::Type::Up,
                                  InputDevice::MouseButton, button});
}
void Window::onCursor(GLFWwindow* w, double x, double y) {
    self(w)->m_pending.push_back({InputEvent::Type::MouseMove, InputDevice::Key, 0, x, y});
}
void Window::onFocus(GLFWwindow* w, int focused) {
    if (!focused) self(w)->m_pending.push_back({InputEvent::Type::FocusLost});
}
void Window::onFramebufferSize(GLFWwindow* w, int, int) { self(w)->m_resized = true; }

} // namespace ohao::runtime
```

`ohao/gpu/vulkan/present_target.hpp`:
```cpp
#pragma once
#include <functional>
#include <vector>
#include <vulkan/vulkan.h>

namespace ohao {

// What a windowing system hands the renderer so it can present. The renderer
// never includes a windowing header; it only calls createSurface once, right
// after the instance exists.
struct PresentTarget {
    std::vector<const char*> instanceExtensions;
    std::function<VkSurfaceKHR(VkInstance)> createSurface;
    VkExtent2D extent{0, 0};
    bool vsync{false};
};

} // namespace ohao
```

- [ ] **Step 2: Renderer — instance, device, debug messenger**

In `renderer.hpp` add includes `gpu/vulkan/present_target.hpp`, `gpu/vulkan/swapchain.hpp`, `<array>`, and members:
```cpp
    // --- presentation (windowed mode only) ---
    std::optional<PresentTarget> m_presentTarget;
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    std::unique_ptr<Swapchain> m_swapchain;
    std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> m_imageAcquired{};
    bool m_swapchainDirty{false};
    // --- counters for the host's correctness checks ---
    uint64_t m_readbackCopies{0};
    VkDebugUtilsMessengerEXT m_debugMessenger{VK_NULL_HANDLE};
    std::atomic<uint64_t> m_validationMessages{0};
    bool m_validationEnabled{false};
```
and the public methods from **Interfaces** (`isPresenting()` returns `m_swapchain != nullptr`).

`renderer.cpp`:
```cpp
bool VulkanRenderer::initialize(const PresentTarget& target) {
    m_presentTarget = target;
    return initialize();
}
```
and inside `initialize()`, after `createInstance()` succeeds:
```cpp
        if (m_presentTarget) {
            m_surface = m_presentTarget->createSurface(m_instance);
            if (m_surface == VK_NULL_HANDLE) { std::cerr << "Failed to create window surface" << std::endl; return false; }
        }
```
and after frame resources and the deferred renderer are initialized:
```cpp
        if (m_presentTarget && !createPresentation()) {
            std::cerr << "Failed to create swapchain" << std::endl;
            return false;
        }
```
with:
```cpp
bool VulkanRenderer::createPresentation() {
    m_swapchain = std::make_unique<Swapchain>();
    if (!m_swapchain->create(m_physicalDevice, m_device, m_surface, m_presentTarget->extent, m_presentTarget->vsync))
        return false;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : m_imageAcquired)
        if (vkCreateSemaphore(m_device, &si, nullptr, &s) != VK_SUCCESS) return false;
    return true;
}
```
In `shutdown()`, before the device is destroyed: `vkDeviceWaitIdle`, destroy `m_imageAcquired` semaphores, `m_swapchain.reset()`; after the device, before the instance: `vkDestroySurfaceKHR(m_instance, m_surface, nullptr)` and the debug messenger (`vkDestroyDebugUtilsMessengerEXT` via `vkGetInstanceProcAddr`).

`device_setup.cpp`:
- `createInstance()`: if `m_presentTarget`, append each of `m_presentTarget->instanceExtensions` to `m_enabledInstanceExtensions` (skip duplicates) before `vkCreateInstance`. When validation is enabled (`OHAO_VALIDATION=1`), set `m_validationEnabled = true` and after `vkCreateInstance` create a debug messenger:
  ```cpp
  static VKAPI_ATTR VkBool32 VKAPI_CALL ohaoDebugCallback(
      VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
      if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
          static_cast<std::atomic<uint64_t>*>(user)->fetch_add(1);
          std::cerr << "[validation] " << data->pMessage << std::endl;
      }
      return VK_FALSE;
  }
  // after vkCreateInstance, when m_validationEnabled:
  VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
  mi.pfnUserCallback = ohaoDebugCallback;
  mi.pUserData = &m_validationMessages;
  auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
      vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
  if (create) create(m_instance, &mi, nullptr, &m_debugMessenger);
  ```
  If `OHAO_VALIDATION=1` is set but `VK_LAYER_KHRONOS_validation` is not available, print `[OHAO] validation requested but VK_LAYER_KHRONOS_validation is not installed` and leave `m_validationEnabled = false` — the scripted run reports this rather than passing silently.
- `pickPhysicalDevice()`: when `m_surface` is set, reject a device whose graphics family fails `vkGetPhysicalDeviceSurfaceSupportKHR`, with the message `"graphics queue cannot present to this surface"`.
- `createLogicalDevice()`: when `m_surface` is set, `m_enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME)` before the create.

- [ ] **Step 3: The present path in `renderDeferred`**

Restructure `renderDeferred()` in `render_dispatch.cpp`:

```cpp
void VulkanRenderer::renderDeferred() {
    if (!m_deferredRenderer) return;
    FrameResources& frame = m_frameResources.getFrame(m_currentFrame);
    m_frameResources.waitForFrame(m_currentFrame);

    uint32_t imageIndex = 0;
    if (m_swapchain) {
        if (m_swapchainDirty || !m_swapchain->valid()) {
            if (!recreatePresentation()) return;     // minimized: skip the frame, fence untouched
        }
        const auto acq = m_swapchain->acquire(m_imageAcquired[m_currentFrame], imageIndex);
        if (acq == Swapchain::Acquire::OutOfDate) { m_swapchainDirty = true; return; }
        if (acq == Swapchain::Acquire::Error) { std::cerr << "[present] acquire failed" << std::endl; return; }
    } else if (frame.stagingBufferMapped) {
        memcpy(m_pixelBuffer.data(), frame.stagingBufferMapped, m_width * m_height * 4);
    }
    m_frameResources.resetFrame(m_currentFrame);

    // ... the existing camera / light / geometry setup, unchanged ...

    VkCommandBuffer cmd = frame.commandBuffer;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    m_deferredRenderer->render(cmd, m_currentFrame);
    if (m_swapchain) blitDeferredOutputToSwapchain(cmd, imageIndex);
    else             copyDeferredOutputToPixelBuffer(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    if (m_swapchain) {
        renderFinished = m_swapchain->renderFinished(imageIndex);
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_imageAcquired[m_currentFrame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
    }
    vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.renderFence);

    if (m_swapchain) {
        const VkResult pr = m_swapchain->present(m_graphicsQueue, imageIndex);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) m_swapchainDirty = true;
    }
    m_currentFrame = FrameResourceManager::nextFrame(m_currentFrame);
}
```

The early `return` paths happen **before** `resetFrame`, so the slot fence stays signaled and the next `waitForFrame` cannot deadlock. Increment `m_readbackCopies` at the top of `copyDeferredOutputToPixelBuffer`.

```cpp
void VulkanRenderer::blitDeferredOutputToSwapchain(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkImage src = m_deferredRenderer->getFinalOutputImage();
    VkImage dst = m_swapchain->image(imageIndex);
    if (src == VK_NULL_HANDLE) return;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier b[2]{};
    b[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[0].image = src; b[0].subresourceRange = range;
    b[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b[1].srcAccessMask = 0;
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcQueueFamilyIndex = b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b[1].image = dst; b[1].subresourceRange = range;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);

    const VkExtent2D ext = m_swapchain->extent();
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1]  = {static_cast<int32_t>(m_width), static_cast<int32_t>(m_height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1]  = {static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

    b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = 0;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 2, b);
}

bool VulkanRenderer::recreatePresentation() {
    const VkExtent2D want = m_presentTarget->extent;
    if (want.width == 0 || want.height == 0) return false;   // minimized
    if (!m_swapchain->recreate(want)) return false;
    m_swapchainDirty = false;
    resize(m_swapchain->extent().width, m_swapchain->extent().height);
    return true;
}

void VulkanRenderer::setPresentExtent(VkExtent2D extent) {
    if (!m_presentTarget) return;
    m_presentTarget->extent = extent;
    m_swapchainDirty = true;
}
```
Declare `blitDeferredOutputToSwapchain`, `recreatePresentation`, `createPresentation` (private) and `setPresentExtent` (public) in `renderer.hpp`. Also `vkGetPhysicalDeviceFormatProperties` on the deferred final image format must include `BLIT_SRC` — check once in `createPresentation()` and fail with `"deferred output format cannot be a blit source"` if not.

The final-image source layout assumption (`SHADER_READ_ONLY_OPTIMAL`) is the same one `copyDeferredOutputToPixelBuffer` already makes; if validation reports a layout mismatch, read the actual layout from the post-processing / lighting pass that produced the image and use it in `b[0].oldLayout` and the restore barrier.

- [ ] **Step 4: Verify headless is unchanged**

```bash
/c/Users/djmax/scoop/shims/cmake -B build -S . && /c/Users/djmax/scoop/shims/cmake --build build --config Release -j8
for t in engine_tests runtime_tests renderer_test; do ./build/Release/$t.exe x.png | tail -1; done
python tests/golden/render_golden.py tests/golden/manifest.json
```
Expected: all pass, golden `max_abs=0`. Windowed presentation is exercised in Task 7.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: GLFW window and deferred presentation through a swapchain blit

VulkanRenderer::initialize(PresentTarget) creates the surface, requires a
present-capable graphics queue, enables VK_KHR_swapchain, and ends deferred
frames with a blit into the acquired swapchain image instead of a readback.
Headless initialize is unchanged. Adds readback-copy and validation-message
counters for the host's correctness checks.

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```

---

### Task 7: `Application`, the `ohao_game` host, and the end-to-end gate

**Files:**
- Create: `ohao/runtime/application.hpp`, `ohao/runtime/application.cpp`, `examples/ohao_game.cpp`
- Modify: `examples/CMakeLists.txt`
- Delete: `examples/interactive.cpp`

**Interfaces:**
- Consumes: `FixedStepClock`, `FrameStats` (T1); `InputMap`, `InputEvent` (T2); `Window`, `PresentTarget` (T6); `VulkanRenderer::initialize(PresentTarget)`, `setPresentExtent`, `gpuFrameMs`, `readbackCopyCount`, `validationMessageCount`, `validationEnabled` (T4, T6).
- Produces:
  ```cpp
  namespace ohao::runtime {
  class Application;
  struct GameHooks {
      std::function<void(Application&)> start;
      std::function<void(Application&, float fixedDt)> fixedUpdate;
      std::function<void(Application&, float frameDt, float alpha)> update;
  };
  struct AppConfig {
      WindowConfig window;
      bool vsync{false};
      ClockConfig clock;
      int frameLimit{0};          // 0 = run until closed
      int resizeAtFrame{-1};      // scripted resize for the gate run
      VkExtent2D resizeTo{1280, 720};
  };
  struct RunReport { uint64_t frames, readbackCopies, validationMessages; bool validationEnabled;
                     double avgCpuMs, avgGpuMs, fps; };
  class Application {
  public:
      bool initialize(const AppConfig&);
      RunReport run(const GameHooks&);
      void shutdown();
      [[nodiscard]] VulkanRenderer& renderer();
      [[nodiscard]] InputMap& input();
      [[nodiscard]] Window& window();
      void quit();
  };
  }
  ```

- [ ] **Step 1: Application**

`ohao/runtime/application.hpp`:
```cpp
#pragma once
#include <functional>
#include <memory>
#include "gpu/vulkan/renderer.hpp"
#include "runtime/clock.hpp"
#include "runtime/input.hpp"
#include "runtime/window.hpp"

namespace ohao::runtime {

class Application;

struct GameHooks {
    std::function<void(Application&)> start;
    std::function<void(Application&, float fixedDt)> fixedUpdate;
    std::function<void(Application&, float frameDt, float alpha)> update;
};

struct AppConfig {
    WindowConfig window;
    bool vsync{false};
    ClockConfig clock;
    int frameLimit{0};
    int resizeAtFrame{-1};
    VkExtent2D resizeTo{1280, 720};
};

struct RunReport {
    uint64_t frames{0};
    uint64_t readbackCopies{0};
    uint64_t validationMessages{0};
    bool     validationEnabled{false};
    double   avgCpuMs{0.0}, avgGpuMs{0.0}, fps{0.0};
};

class Application {
public:
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
```

`ohao/runtime/application.cpp`:
```cpp
#include "runtime/application.hpp"
#include <chrono>
#include <iostream>

namespace ohao::runtime {

bool Application::initialize(const AppConfig& cfg) {
    m_cfg = cfg;
    m_clock = FixedStepClock(cfg.clock);
    if (!m_window.create(cfg.window)) return false;
    const VkExtent2D ext = m_window.framebufferExtent();
    m_renderer = std::make_unique<VulkanRenderer>(ext.width, ext.height);
    if (!m_renderer->initialize(m_window.presentTarget(cfg.vsync))) {
        std::cerr << "[App] renderer initialization failed" << std::endl;
        return false;
    }
    m_renderer->setRenderMode(RenderMode::Deferred);
    return true;
}

RunReport Application::run(const GameHooks& hooks) {
    using Clock = std::chrono::steady_clock;
    if (hooks.start) hooks.start(*this);
    auto last = Clock::now();
    int frame = 0;

    while (!m_quit && !m_window.shouldClose()) {
        if (m_cfg.frameLimit > 0 && frame >= m_cfg.frameLimit) break;
        if (frame == m_cfg.resizeAtFrame) m_window.setSize(m_cfg.resizeTo.width, m_cfg.resizeTo.height);

        m_events.clear();
        m_window.poll(m_events);
        m_input.beginFrame(m_events);
        if (m_window.consumeResized()) m_renderer->setPresentExtent(m_window.framebufferExtent());

        const auto now = Clock::now();
        const Tick tick = m_clock.advance(std::chrono::duration_cast<Nanos>(now - last));
        last = now;
        const float fixedDt = m_clock.fixedDtSeconds();
        for (int i = 0; i < tick.steps; ++i)
            if (hooks.fixedUpdate) hooks.fixedUpdate(*this, fixedDt);
        if (hooks.update)
            hooks.update(*this, std::chrono::duration<float>(tick.frameDt).count(), tick.alpha);

        const VkExtent2D ext = m_window.framebufferExtent();
        if (ext.width != 0 && ext.height != 0) {           // minimized: simulate, don't render
            m_renderer->render();
            // Whole frame time (input + simulation + render submit), so fps is the real frame rate.
            m_stats.record(std::chrono::duration<double, std::milli>(tick.frameDt).count(),
                           m_renderer->gpuFrameMs());
        }
        ++frame;
    }

    RunReport r;
    r.frames = static_cast<uint64_t>(frame);
    r.readbackCopies = m_renderer->readbackCopyCount();
    r.validationMessages = m_renderer->validationMessageCount();
    r.validationEnabled = m_renderer->validationEnabled();
    r.avgCpuMs = m_stats.avgCpuMs();
    r.avgGpuMs = m_stats.avgGpuMs();
    r.fps = m_stats.fps();
    return r;
}

void Application::shutdown() {
    m_renderer.reset();     // device and surface go before the window
    m_window.destroy();
}

} // namespace ohao::runtime
```

`FrameStats` records the whole frame time (`tick.frameDt`) as CPU frame ms, so `fps` is real frame rate; GPU ms comes from the deferred timestamp queries.

- [ ] **Step 2: The host example**

`examples/ohao_game.cpp` — load the helmet exactly the way `model_viewer.cpp` does (read that file and reuse its loading, framing, material and environment code — `ModelLoader::loadResult`, `SceneFramer::computeFraming`, `SceneFramer::applyLights`, `renderer.setEnvironmentMap("assets/test_models/env_studio.hdr")`), then:

```cpp
#include "runtime/application.hpp"
// ...plus the scene/asset includes model_viewer.cpp uses...
#include <cstring>
#include <iostream>
#include <string>

using namespace ohao;
using namespace ohao::runtime;

static int argInt(int argc, char** argv, const char* name, int def) {
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    return def;
}
static bool argFlag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

int main(int argc, char** argv) {
    AppConfig cfg;
    cfg.window.title = "OHAO";
    cfg.vsync = argFlag(argc, argv, "--vsync");
    cfg.frameLimit = argInt(argc, argv, "--frames", 0);
    cfg.resizeAtFrame = argInt(argc, argv, "--resize-at", -1);

    Application app;
    if (!app.initialize(cfg)) return 1;

    auto scene = buildHelmetScene(app.renderer());   // static helper in this file, built from model_viewer.cpp's code
    if (!scene) { app.shutdown(); return 2; }

    InputMap& in = app.input();
    in.bindAxis("move_forward", {InputDevice::Key, 87 /*W*/}, {InputDevice::Key, 83 /*S*/});
    in.bindAxis("move_right",   {InputDevice::Key, 68 /*D*/}, {InputDevice::Key, 65 /*A*/});
    in.bindAxis("move_up",      {InputDevice::Key, 69 /*E*/}, {InputDevice::Key, 81 /*Q*/});
    in.bindAction("look",  {InputDevice::MouseButton, 1 /*right*/});
    in.bindAction("quit",  {InputDevice::Key, 256 /*Escape*/});

    float yaw = -90.0f, pitch = 0.0f;
    GameHooks hooks;
    hooks.update = [&](Application& a, float dt, float) {
        if (a.input().pressed("quit")) a.quit();
        Camera* cam = a.renderer().getCamera();
        if (a.input().held("look")) {
            const glm::vec2 d = a.input().mouseDelta();
            yaw += d.x * 0.1f;
            pitch = std::clamp(pitch - d.y * 0.1f, -89.0f, 89.0f);
            cam->setRotation(pitch, yaw);
        }
        a.window().setCursorCaptured(a.input().held("look"));
        const float speed = 3.0f * dt;
        const glm::vec3 fwd = cam->getFront(), right = cam->getRight();
        cam->setPosition(cam->getPosition()
                         + fwd   * (a.input().axis("move_forward") * speed)
                         + right * (a.input().axis("move_right")   * speed)
                         + glm::vec3(0, 1, 0) * (a.input().axis("move_up") * speed));
    };

    const RunReport r = app.run(hooks);
    std::cout << "\nframes " << r.frames
              << "  fps " << r.fps << "  cpu " << r.avgCpuMs << " ms  gpu " << r.avgGpuMs << " ms\n"
              << "readback copies " << r.readbackCopies
              << "  validation " << (r.validationEnabled ? std::to_string(r.validationMessages) + " messages"
                                                         : std::string("NOT ENABLED (set OHAO_VALIDATION=1)"))
              << std::endl;
    scene.reset();
    app.shutdown();

    const bool scripted = cfg.frameLimit > 0;
    if (scripted && (r.readbackCopies != 0 || (r.validationEnabled && r.validationMessages != 0))) return 3;
    return 0;
}
```
Use the camera's real accessor names — check with `grep -nE "getFront|getRight|setRotation|setPosition|getPosition" ohao/render/camera/camera.hpp` and adapt (`setRotation(pitch, yaw)` matches the constructor call order in `renderer.cpp`).

`examples/CMakeLists.txt`: delete the interactive block (the `ohao_example(interactive)`, `find_package(OpenGL ...)` and its link line) and add:
```cmake
ohao_example(ohao_game)
target_link_libraries(ohao_game PRIVATE ohao_runtime)
```
`git rm examples/interactive.cpp`.

- [ ] **Step 3: Build and run interactively**

```bash
/c/Users/djmax/scoop/shims/cmake -B build -S . && /c/Users/djmax/scoop/shims/cmake --build build --config Release -j8
/c/Users/djmax/scoop/shims/cmake --build build --target shaders --config Release
./build/Release/ohao_game.exe
```
Expected: a 1920×1080 window showing the helmet under the studio HDRI; WASD/QE move, right-drag looks, resizing the window keeps the image correct (not stretched, no validation spam), minimizing and restoring works, Escape quits and prints frame stats.

- [ ] **Step 4: The end-to-end gate**

```bash
OHAO_VALIDATION=1 ./build/Release/ohao_game.exe --frames 600 --resize-at 200; echo "exit=$?"
```
Expected: `readback copies 0`, `validation 0 messages`, `exit=0`. If the output says `NOT ENABLED`, the Vulkan SDK validation layer is missing — install it; the gate is not passed.

Then performance, without validation:
```bash
./build/Release/ohao_game.exe --frames 600
```
Expected: `fps` ≥ 60 at 1920×1080. Record the numbers in the commit message.

Fix every validation message before proceeding — they are in scope (spec: "zero messages"). If a message is caused by an existing deferred pass rather than the presentation code, fix it in that pass.

- [ ] **Step 5: Regression**

```bash
for t in engine_tests runtime_tests physics_backend_tests audio_system_tests force_generator_tests env_cdf_test denoise_parse_test sobol_test; do ./build/Release/$t.exe | tail -1; done
./build/Release/renderer_test.exe x.png | tail -1
python tests/golden/render_golden.py tests/golden/manifest.json
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(runtime): Application loop and the ohao_game windowed host

Fixed 60 Hz simulation, variable-rate render, input actions, resize and
minimize handling. Replaces interactive.exe, which displayed frames by reading
them back to the CPU and uploading them to an OpenGL texture.

Gate: 600 frames with a forced resize under validation -- 0 messages,
0 readback copies. <fps / cpu ms / gpu ms at 1080p from Step 4>.

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```

---

### Task 8: Documentation

**Files:**
- Modify: `README.md` (run section), `docs/render_pipelines.md` (modes table), `docs/ROADMAP.md` (mark sub-project 1 done)
- Modify: monograph units that describe `RenderMode::Forward` or `interactive.cpp` — find with `grep -rln "Forward\|interactive" site/content/units`; delete those paragraphs or sections; remove any unit whose subject is gone (unit `.md` + its `page()` entry in `site/tools/monograph/tree/<module>.py` + its block in `site/tools/monograph/author_rich.py`)

- [ ] **Step 1: README** — replace the `interactive` run instructions with:
  ```
  ./build/Release/ohao_game.exe            # windowed: WASD/QE move, right-drag look, Esc quits, --vsync
  ```
  and state that the raster path is Deferred.
- [ ] **Step 2: `docs/render_pipelines.md`** — remove the Forward row; three modes remain.
- [ ] **Step 3: Monograph** — apply the deletions above, then:
  ```bash
  cd site && python -c "from tools.monograph.build import build; import sys; sys.exit(build())" && cd ..
  for t in site/tests/test_monograph_structure.py site/tests/test_blocks.py site/tests/test_cite.py; do python $t 2>&1 | tail -1; done
  grep -rln "RenderMode::Forward\|interactive.cpp\|core_forward" site/content site/m   # expect no output
  ```
- [ ] **Step 4: Roadmap** — in `docs/ROADMAP.md`, set sub-project 1's Spec cell to `[spec](specs/2026-10-02-runtime-shell-design.md) · **done**`.
- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "docs: runtime shell — ohao_game replaces interactive, Forward removed from docs

Claude-Session: https://claude.ai/code/session_01RFe6Gt1FWwxgiDmoXwE2XN"
```
