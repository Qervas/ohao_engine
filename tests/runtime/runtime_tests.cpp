// Runtime host tests. No GPU, no window: every input here is synthetic.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/clock.hpp"
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

static void choose_present_mode() {
    VkPresentModeKHR all[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                              VK_PRESENT_MODE_IMMEDIATE_KHR};
    CHECK(ohao::Swapchain::choosePresentMode(all, false) == VK_PRESENT_MODE_MAILBOX_KHR, "mailbox when free");
    CHECK(ohao::Swapchain::choosePresentMode(all, true) == VK_PRESENT_MODE_FIFO_KHR, "vsync forces fifo");
    VkPresentModeKHR fifo[] = {VK_PRESENT_MODE_FIFO_KHR};
    CHECK(ohao::Swapchain::choosePresentMode(fifo, false) == VK_PRESENT_MODE_FIFO_KHR,
          "fifo is the guaranteed fallback");
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

int main() {
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
    RUN(choose_present_mode);
    RUN(choose_image_count_unlimited_max);
    RUN(choose_extent_uses_desired_when_surface_defers);
    RUN(zero_extent_means_minimized);
    std::printf("\n");
    return ohao::test::finish("runtime_tests");
}
