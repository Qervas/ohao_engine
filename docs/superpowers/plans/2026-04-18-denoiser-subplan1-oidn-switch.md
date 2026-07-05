# Denoiser Sub-plan 1: OIDN Switch — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the existing Intel OIDN integration as a first-class engine toggle (`RTRenderSettings.denoiseMode` + `VulkanRenderer::setDenoiseMode()` API + `--denoise=oidn|none` CLI flag on every example). Offline profile defaults to OIDN.

**Architecture:** Introduce `ohao/render/rt/denoise/` subfolder housing the denoiser family (starts with OIDN, will grow to include OptiX, NRD, DLSS RR). Move existing `oidn_denoise.{hpp,cpp}` into it and add `denoise_types.hpp` with the `DenoiseMode` enum + parse helpers. `VulkanRenderer::getPixels()` lazily denoises on first call after `render()`, caches result until next `render()`.

**Tech Stack:** C++20 · Vulkan 1.3 · Intel OpenImageDenoise 2.x · GoogleTest (CLI parse unit test).

**Reference spec:** `docs/superpowers/specs/2026-04-18-denoiser-subplan1-oidn-switch-design.md`

---

## File Structure

**New files:**

| Path | Responsibility |
|------|---------------|
| `ohao/render/rt/denoise/denoise_types.hpp` | `DenoiseMode` enum, `parseDenoiseMode(str)`, `denoiseModeName(mode)` |
| `tests/renderer/denoise_parse_test.cpp` | GoogleTest for `parseDenoiseMode` |

**Moved files (same content, new location):**

| Old path | New path |
|----------|----------|
| `ohao/render/rt/oidn_denoise.hpp` | `ohao/render/rt/denoise/oidn_denoise.hpp` |
| `ohao/render/rt/oidn_denoise.cpp` | `ohao/render/rt/denoise/oidn_denoise.cpp` |

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | Include `denoise_types.hpp`; add `DenoiseMode denoiseMode` to `RTRenderSettings`; update `kOfflineRTSettings` / `kRealtimeRTSettings` presets |
| `ohao/gpu/vulkan/renderer.hpp` | Include `denoise_types.hpp`; declare `setDenoiseMode/getDenoiseMode`; add `mutable` cache fields; change `getPixels()` to non-inline |
| `ohao/gpu/vulkan/renderer.cpp` | Implement `getPixels()` lazy-denoise path; implement setter/getter; invalidate cache at end of `render()` |
| `examples/cornell_box.cpp` | Add `--denoise=…` CLI arg parsing |
| `examples/env_demo.cpp` | Add `--denoise=…` CLI arg parsing |
| `examples/model_viewer.cpp` | Add `--denoise=…` CLI arg parsing; remove `constexpr bool kEnableOIDN = false`; remove manual OIDN block (now done by renderer) |
| `examples/turntable.cpp` | Add `--denoise=…` CLI arg parsing; remove manual OIDN block |
| `ohao/CMakeLists.txt` or `ohao/render/rt/CMakeLists.txt` | Update source paths for moved OIDN files (find existing glob or explicit list) |
| `examples/CMakeLists.txt` | Update `oidn_denoise.hpp` include path references (if any explicit include dirs exist) |
| `tests/renderer/CMakeLists.txt` | Register `denoise_parse_test` target |

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-oidn -b denoiser-1a-oidn HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-oidn`.

If `build/_deps/glm-src/` is empty in the fresh worktree (known bootstrap artifact), copy once:

```bash
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/glm-src/. \
      /home/frankyin/Desktop/Github/ohao-oidn/build/_deps/glm-src/
```

---

## Task 1: Create `denoise_types.hpp` + unit test (TDD)

**Files:**
- Create: `ohao/render/rt/denoise/denoise_types.hpp`
- Create: `ohao/render/rt/denoise/denoise_types.cpp`
- Create: `tests/renderer/denoise_parse_test.cpp`
- Modify: `tests/renderer/CMakeLists.txt`

- [ ] **Step 1.1: Write the failing test**

Create `tests/renderer/denoise_parse_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "render/rt/denoise/denoise_types.hpp"

using ohao::DenoiseMode;
using ohao::parseDenoiseMode;
using ohao::denoiseModeName;

TEST(DenoiseTypes, ParsesKnownModes) {
    EXPECT_EQ(parseDenoiseMode("none"), DenoiseMode::None);
    EXPECT_EQ(parseDenoiseMode("oidn"), DenoiseMode::OIDN);
}

TEST(DenoiseTypes, CaseInsensitive) {
    EXPECT_EQ(parseDenoiseMode("OIDN"), DenoiseMode::OIDN);
    EXPECT_EQ(parseDenoiseMode("None"), DenoiseMode::None);
    EXPECT_EQ(parseDenoiseMode("Oidn"), DenoiseMode::OIDN);
}

TEST(DenoiseTypes, UnknownFallsBackToNone) {
    EXPECT_EQ(parseDenoiseMode("gibberish"), DenoiseMode::None);
    EXPECT_EQ(parseDenoiseMode(""), DenoiseMode::None);
}

TEST(DenoiseTypes, NameRoundTrip) {
    EXPECT_STREQ(denoiseModeName(DenoiseMode::None), "none");
    EXPECT_STREQ(denoiseModeName(DenoiseMode::OIDN), "oidn");
}
```

- [ ] **Step 1.2: Run test, verify it fails to compile**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
cmake --build build --target denoise_parse_test 2>&1 | head -15
```

Expected: CMake doesn't know the target yet (comes in step 1.5).

- [ ] **Step 1.3: Create the header**

Create `ohao/render/rt/denoise/denoise_types.hpp`:

```cpp
#pragma once

// DenoiseMode — selects the denoiser backend used by the offline path tracer.
//
// Sub-plan 1 ships OIDN. Later sub-plans add OptiX, NRD, DLSS RR.
// The enum + parse helpers live here so every backend can share them and
// the CLI surface stays consistent across examples.

#include <cstdint>
#include <string>

namespace ohao {

enum class DenoiseMode : uint32_t {
    None  = 0,
    OIDN  = 1,
    // future:
    // OptiX  = 2,
    // NRD    = 3,
    // DLSSRR = 4,
};

// Parse a CLI string (case-insensitive). Unknown values return None and
// log a warning to stderr.
DenoiseMode parseDenoiseMode(const std::string& s);

// Human-readable lowercase name. Stable, safe for CLI round-trip.
const char* denoiseModeName(DenoiseMode mode);

} // namespace ohao
```

- [ ] **Step 1.4: Create the implementation**

Create `ohao/render/rt/denoise/denoise_types.cpp`:

```cpp
#include "render/rt/denoise/denoise_types.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace ohao {

namespace {
std::string toLower(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}
} // namespace

DenoiseMode parseDenoiseMode(const std::string& s) {
    const std::string lower = toLower(s);
    if (lower == "none") return DenoiseMode::None;
    if (lower == "oidn") return DenoiseMode::OIDN;
    std::cerr << "[Denoise] Unknown mode '" << s
              << "' — falling back to None\n";
    return DenoiseMode::None;
}

const char* denoiseModeName(DenoiseMode mode) {
    switch (mode) {
        case DenoiseMode::None: return "none";
        case DenoiseMode::OIDN: return "oidn";
    }
    return "unknown";
}

} // namespace ohao
```

- [ ] **Step 1.5: Register test in CMake**

Append to `tests/renderer/CMakeLists.txt`:

```cmake
# Denoise parse unit test (Denoiser Sub-plan 1)
add_executable(denoise_parse_test denoise_parse_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/denoise/denoise_types.cpp
)
target_include_directories(denoise_parse_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(denoise_parse_test PRIVATE gtest gtest_main pthread)
include(GoogleTest)
gtest_discover_tests(denoise_parse_test)
```

- [ ] **Step 1.6: Build + run**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
cmake --build build --target denoise_parse_test -j8
./build/denoise_parse_test 2>&1 | tail -8
```

Expected: `[  PASSED  ] 4 tests.`

- [ ] **Step 1.7: Commit**

```bash
git add ohao/render/rt/denoise/denoise_types.hpp \
        ohao/render/rt/denoise/denoise_types.cpp \
        tests/renderer/denoise_parse_test.cpp tests/renderer/CMakeLists.txt
git commit -m "feat(rt): DenoiseMode enum + CLI parse helpers

New denoise/ subfolder under render/rt/ for the denoiser family.
parseDenoiseMode (case-insensitive, unknown→None) + denoiseModeName.
GoogleTest coverage.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format used in recent commits (`git log -3 --format=%B`).

---

## Task 2: Move `oidn_denoise.{hpp,cpp}` into `denoise/` + update includes

**Files:**
- Move: `ohao/render/rt/oidn_denoise.hpp` → `ohao/render/rt/denoise/oidn_denoise.hpp`
- Move: `ohao/render/rt/oidn_denoise.cpp` → `ohao/render/rt/denoise/oidn_denoise.cpp`
- Modify: `examples/model_viewer.cpp` — update `#include`
- Modify: `examples/turntable.cpp` — update `#include`
- Modify: `ohao/render/rt/denoise/oidn_denoise.cpp` — update self-include from `"oidn_denoise.hpp"` to `"render/rt/denoise/oidn_denoise.hpp"`
- Modify: project CMake — update source path if explicit list (glob handles it automatically)

- [ ] **Step 2.1: Git-move the files**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
git mv ohao/render/rt/oidn_denoise.hpp ohao/render/rt/denoise/oidn_denoise.hpp
git mv ohao/render/rt/oidn_denoise.cpp ohao/render/rt/denoise/oidn_denoise.cpp
```

- [ ] **Step 2.2: Fix self-include in `oidn_denoise.cpp`**

Edit `ohao/render/rt/denoise/oidn_denoise.cpp`. Find line 1:

```cpp
#include "oidn_denoise.hpp"
```

Change to:

```cpp
#include "render/rt/denoise/oidn_denoise.hpp"
```

- [ ] **Step 2.3: Update `model_viewer.cpp` include**

Edit `examples/model_viewer.cpp` line 21:

```cpp
// Old:
#include "render/rt/oidn_denoise.hpp"

// New:
#include "render/rt/denoise/oidn_denoise.hpp"
```

- [ ] **Step 2.4: Update `turntable.cpp` include**

Edit `examples/turntable.cpp` line 12:

```cpp
// Old:
#include "render/rt/oidn_denoise.hpp"

// New:
#include "render/rt/denoise/oidn_denoise.hpp"
```

- [ ] **Step 2.5: Check for other references**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
grep -rn 'render/rt/oidn_denoise\|"oidn_denoise\.hpp"' ohao/ examples/ tests/ 2>&1
```

Expected: ZERO matches after steps 2.3/2.4. Any remaining match is a missed callsite — update it to use `render/rt/denoise/oidn_denoise.hpp`.

- [ ] **Step 2.6: Check CMake source lists**

Search for explicit file listings that reference the old path:

```bash
grep -rn 'oidn_denoise' ohao/render/rt/CMakeLists.txt ohao/CMakeLists.txt CMakeLists.txt examples/CMakeLists.txt 2>&1
```

If any `add_library` or `target_sources` uses `ohao/render/rt/oidn_denoise.cpp`, update to `ohao/render/rt/denoise/oidn_denoise.cpp`. If the project uses `file(GLOB_RECURSE)` pattern (most OHAO uses this), no change needed — the new location matches the glob.

- [ ] **Step 2.7: Build + verify**

```bash
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build. If the build still references the old path (stale CMake configure cache), run `cmake -B build -S .` once to reconfigure then rebuild.

- [ ] **Step 2.8: Smoke test**

```bash
./build/cornell_box /tmp/t2_cornell.png 4 2>&1 | tail -3
```

Expected: renders cleanly, no validation errors. File-move shouldn't change runtime behavior.

- [ ] **Step 2.9: Commit**

```bash
git add ohao/render/rt/denoise/oidn_denoise.hpp \
        ohao/render/rt/denoise/oidn_denoise.cpp \
        examples/model_viewer.cpp examples/turntable.cpp
git commit -m "refactor(rt): move oidn_denoise into render/rt/denoise/ subfolder

No functional change. Sets up the denoise/ directory so OptiX (Sub-plan 2),
NRD (Sub-plan 4), DLSS RR (Sub-plan 5) have a natural home.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 3: Add `denoiseMode` to `RTRenderSettings`

**Files:**
- Modify: `ohao/render/rt/path_tracer.hpp`

- [ ] **Step 3.1: Add include + field**

Edit `ohao/render/rt/path_tracer.hpp`. Near the top (after existing `render/rt/sampler_types.hpp` include), add:

```cpp
#include "render/rt/denoise/denoise_types.hpp"
```

In `struct RTRenderSettings`, after the last existing field (`SamplerType samplerType`), add:

```cpp
    DenoiseMode denoiseMode{DenoiseMode::None};
```

Final struct:

```cpp
struct RTRenderSettings {
    RTRenderProfile profile{RTRenderProfile::Offline};
    uint32_t maxBounces{4};
    bool preferAccumulation{true};
    bool enableAuxiliaryAOVs{true};
    bool allowExternalDenoiser{true};
    bool enableInternalDenoise{false};
    bool enableFireflyClamp{false};
    float fireflyClampLuminance{10.0f};
    SamplerType samplerType{SamplerType::Sobol};
    DenoiseMode denoiseMode{DenoiseMode::None};
};
```

- [ ] **Step 3.2: Update preset constants**

In the same file, find `kRealtimeRTSettings` and `kOfflineRTSettings`. Append the new trailing value:

```cpp
inline constexpr RTRenderSettings kRealtimeRTSettings{
    RTRenderProfile::Realtime,
    2,
    true,
    true,
    false,
    true,
    true,
    10.0f,
    SamplerType::PCG,
    DenoiseMode::None,          // realtime uses NRD/DLSS RR (Sub-plans 4-5), not OIDN
};

inline constexpr RTRenderSettings kOfflineRTSettings{
    RTRenderProfile::Offline,
    4,
    true,
    true,
    true,
    false,
    false,
    0.0f,
    SamplerType::Sobol,
    DenoiseMode::OIDN,          // offline default — matches Cycles
};
```

- [ ] **Step 3.3: Build + verify**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
cmake --build build -j8 2>&1 | tail -5
```

Expected: clean build. Field is unused at runtime until Task 4 wires it.

- [ ] **Step 3.4: Commit**

```bash
git add ohao/render/rt/path_tracer.hpp
git commit -m "feat(rt): RTRenderSettings.denoiseMode (offline=OIDN, realtime=None)

Adds the denoise mode to settings; offline preset now defaults to OIDN.
Consumer (VulkanRenderer::getPixels) wired in the next commit.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 4: `VulkanRenderer::setDenoiseMode` + lazy-denoise in `getPixels()`

**Files:**
- Modify: `ohao/gpu/vulkan/renderer.hpp`
- Modify: `ohao/gpu/vulkan/renderer.cpp` — find where `m_pixelBuffer` is populated at end of `render()`; add cache invalidation and new `getPixels()` out-of-line implementation

- [ ] **Step 4.1: Add include + API + cache fields to header**

Edit `ohao/gpu/vulkan/renderer.hpp`. Near the top (after existing includes), add:

```cpp
#include "render/rt/denoise/denoise_types.hpp"
```

In `class VulkanRenderer`, find the `getPixels()` accessor (around line 143):

```cpp
    const uint8_t* getPixels() const { return m_pixelBuffer.data(); }
```

Replace with:

```cpp
    // Denoiser control
    void        setDenoiseMode(DenoiseMode mode);
    DenoiseMode getDenoiseMode() const { return m_denoiseMode; }

    // Returns pointer to RGBA8 tonemapped pixels. If denoiseMode != None,
    // the buffer is lazily denoised on the first call after render();
    // subsequent calls return the cached result until render() is called again.
    const uint8_t* getPixels() const;
```

In the private section (search for `std::vector<uint8_t> m_pixelBuffer`), add next to it:

```cpp
    // Denoise state
    DenoiseMode                      m_denoiseMode{DenoiseMode::None};
    mutable std::vector<uint8_t>     m_denoisedPixelBuffer;
    mutable bool                     m_denoiseCacheValid{false};
```

The `mutable` is required because `getPixels()` is `const` but may update the cache.

- [ ] **Step 4.2: Implement setter + lazy denoise in `.cpp`**

Edit `ohao/gpu/vulkan/renderer.cpp`. At the top, add includes (if not already present):

```cpp
#include "render/rt/denoise/oidn_denoise.hpp"
```

Near other method definitions (anywhere after the class methods are defined — place just after `shutdown` or `render`, matches style in the file):

```cpp
void VulkanRenderer::setDenoiseMode(DenoiseMode mode) {
    if (mode != m_denoiseMode) {
        m_denoiseMode = mode;
        m_denoiseCacheValid = false;
    }
}

const uint8_t* VulkanRenderer::getPixels() const {
    if (m_denoiseMode == DenoiseMode::None) {
        return m_pixelBuffer.data();
    }
    if (m_denoiseCacheValid) {
        return m_denoisedPixelBuffer.data();
    }

    // Lazy denoise — OIDN path
    if (m_denoiseMode == DenoiseMode::OIDN) {
        std::vector<float> beautyRGBA, albedoRGBA, normalRGBA;
        uint32_t rw = 0, rh = 0;
        // Note: readbackHDRBuffers is non-const. We have to cast away const
        // here because getPixels() is the public accessor that should remain
        // const. The cache + readback state is properly mutable.
        auto* self = const_cast<VulkanRenderer*>(this);
        if (!self->readbackHDRBuffers(beautyRGBA, albedoRGBA, normalRGBA, rw, rh)) {
            std::cerr << "[Denoise] readbackHDRBuffers failed — returning noisy pixels\n";
            return m_pixelBuffer.data();
        }

        auto beauty3 = ohao::rgba32fToFloat3(beautyRGBA.data(), rw, rh);
        auto albedo3 = ohao::rgba32fToFloat3(albedoRGBA.data(), rw, rh);
        auto normal3 = ohao::rgba32fToFloat3(normalRGBA.data(), rw, rh);

        if (!ohao::oidnDenoise(beauty3.data(), albedo3.data(), normal3.data(),
                                rw, rh, /*hdr*/ true)) {
            std::cerr << "[Denoise] OIDN failed — returning noisy pixels\n";
            return m_pixelBuffer.data();
        }

        m_denoisedPixelBuffer = ohao::float3ToRGBA8(beauty3.data(), rw, rh, /*exposure*/ 0.5f);
        m_denoiseCacheValid = true;
        return m_denoisedPixelBuffer.data();
    }

    // Unknown mode — treat as None
    return m_pixelBuffer.data();
}
```

- [ ] **Step 4.3: Invalidate cache at end of `render()`**

Still in `ohao/gpu/vulkan/renderer.cpp`, find `VulkanRenderer::render()`. At the end of the function (right before the closing brace), add:

```cpp
    m_denoiseCacheValid = false;
```

This ensures every new render invalidates the stale denoise.

If the file has multiple render paths (e.g. `renderDeferred()`, `renderPathTraced()`, `renderLegacy()`), the invalidation should happen in the top-level `render()` that dispatches to them — not in each individual path.

- [ ] **Step 4.4: Build + smoke test**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/t4_cornell.png 16 2>&1 | tail -3
```

Expected: clean build. `cornell_box` uses `kOfflineRTSettings.denoiseMode = OIDN` by default — it should produce a denoised image now.

Visual check: Cornell walls should look clean, no firefly noise. This is the first visible "adaptive works" moment for this sub-plan.

If the image looks like noisy path-traced output (heavy firefly), OIDN isn't running — check stderr for `[Denoise]` messages.

- [ ] **Step 4.5: Commit**

```bash
git add ohao/gpu/vulkan/renderer.hpp ohao/gpu/vulkan/renderer.cpp
git commit -m "feat(renderer): lazy-denoise getPixels() + setDenoiseMode API

When RTRenderSettings.denoiseMode != None, getPixels() readback the
HDR beauty + AOV buffers, runs OIDN on the CPU float3 pipeline, and
caches the tonemapped result. Cache invalidated on every render()
call. Failure falls back to noisy pixels with a stderr warning.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 5: Wire `--denoise=…` CLI into all 4 examples + remove hacks

**Files:**
- Modify: `examples/cornell_box.cpp`
- Modify: `examples/env_demo.cpp`
- Modify: `examples/model_viewer.cpp` (also removes `constexpr bool kEnableOIDN = false` and its manual-OIDN block)
- Modify: `examples/turntable.cpp` (also removes manual-OIDN block)

- [ ] **Step 5.1: Common pattern — add to cornell_box.cpp**

Edit `examples/cornell_box.cpp`. Near the top of `main()`, after existing arg parsing (`output`, `samples`), insert:

```cpp
    // --denoise=oidn|none  (overrides RTRenderSettings default)
    std::optional<ohao::DenoiseMode> denoiseOverride;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--denoise=", 0) == 0) {
            denoiseOverride = ohao::parseDenoiseMode(arg.substr(10));
        }
    }
```

Add include at top:

```cpp
#include <optional>
#include "render/rt/denoise/denoise_types.hpp"
```

After `renderer.setScene(scene);` (or equivalent initialisation; place AFTER the renderer is fully set up but BEFORE `renderer.render()`), add:

```cpp
    if (denoiseOverride.has_value()) {
        renderer.setDenoiseMode(*denoiseOverride);
        std::cout << "Denoise mode (CLI override): "
                  << ohao::denoiseModeName(*denoiseOverride) << std::endl;
    } else {
        std::cout << "Denoise mode (preset): "
                  << ohao::denoiseModeName(renderer.getDenoiseMode()) << std::endl;
    }
```

Note: `renderer.getDenoiseMode()` reads whatever `kOfflineRTSettings.denoiseMode` set it to during renderer init — which requires the renderer to initialise `m_denoiseMode` from the profile's settings. If the renderer doesn't do this today, add a line in renderer init / `setScene` / similar:

```cpp
    // somewhere in VulkanRenderer init
    if (m_pathTracerSettings) m_denoiseMode = m_pathTracerSettings->denoiseMode;
```

Check the renderer's existing handling — it may already be wired via a profile change path, in which case no init change is needed. If unclear, skip the preset-default line and always require an explicit CLI flag for now (you can then ship preset-propagation as a tiny follow-up).

- [ ] **Step 5.2: env_demo.cpp — same pattern**

Edit `examples/env_demo.cpp`. Identical addition as step 5.1 — add the include, parse loop, and `setDenoiseMode` call.

The existing arg-parse loop in `env_demo.cpp` starts at line 35 (`for (int i = 5; i < argc; i++)`). Add the `--denoise=` check there:

```cpp
    std::optional<ohao::DenoiseMode> denoiseOverride;
    for (int i = 5; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--denoise=", 0) == 0) {
            denoiseOverride = ohao::parseDenoiseMode(arg.substr(10));
        }
        // ... existing arg handling ...
    }
```

Apply `setDenoiseMode` before `renderer.render()`.

- [ ] **Step 5.3: model_viewer.cpp — remove the old OIDN hack + add CLI**

Edit `examples/model_viewer.cpp`.

1. Delete the block around line 408-435 (the `constexpr bool kEnableOIDN = false` and the manual readback + OIDN call). Replace it with the standard `getPixels` + save pattern:

```cpp
    // Write pixels — getPixels() handles OIDN transparently if denoiseMode is set
    const uint8_t* pixels = renderer.getPixels();
    if (pixels) {
        stbi_write_png(output.c_str(), W, H, 4, pixels, W * 4);
        std::cout << "Saved"
                  << (renderer.getDenoiseMode() == ohao::DenoiseMode::None ? "" : " (denoised)")
                  << ": " << output << std::endl;
    }
```

2. Add the `--denoise=` CLI parse + `setDenoiseMode` call same as step 5.1.

3. The existing `#include "render/rt/denoise/oidn_denoise.hpp"` is now unused — remove it.

- [ ] **Step 5.4: turntable.cpp — remove old OIDN block + add CLI**

Edit `examples/turntable.cpp`. Similar to step 5.3: find the manual OIDN block (around line 227), replace with `renderer.getPixels()` path. Add `--denoise=` CLI.

Turntable renders multiple frames in a loop — the denoise happens per-frame when `getPixels()` is called, cached per render. No additional logic needed.

- [ ] **Step 5.5: Build + check for unused include warnings**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
cmake --build build -j8 2>&1 | tail -10
```

Expected: clean build, no warnings about unused `oidn_denoise.hpp` includes in the examples.

- [ ] **Step 5.6: Runtime sanity — each example accepts the flag**

```bash
./build/cornell_box  /tmp/t5_c_none.png 4 --denoise=none  2>&1 | tail -2
./build/cornell_box  /tmp/t5_c_oidn.png 4 --denoise=oidn  2>&1 | tail -2
./build/cornell_box  /tmp/t5_c_def.png  4                 2>&1 | tail -2
```

Expected: all 3 runs succeed. The `none` output should match the default `def` output ONLY if the preset default is `None` (which it isn't for offline) — so `none` and `oidn` produce visibly different images, and `def` matches `oidn`.

Stderr should include `[Denoise] Unknown mode ...` for no invocations (we only pass `none` and `oidn`).

- [ ] **Step 5.7: Commit**

```bash
git add examples/cornell_box.cpp examples/env_demo.cpp \
        examples/model_viewer.cpp examples/turntable.cpp
git commit -m "feat(examples): --denoise=oidn|none CLI flag in all examples

Each example parses --denoise=… and calls renderer.setDenoiseMode to
override the preset default. Removes the hardcoded
constexpr bool kEnableOIDN = false hack from model_viewer and the
manual OIDN readback block from turntable — VulkanRenderer::getPixels
handles it transparently now.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Task 6: Verification + reference scene update

**Files:**
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

- [ ] **Step 6.1: Render both modes at 16 spp**

```bash
cd /home/frankyin/Desktop/Github/ohao-oidn
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/oidn_off.png 16 --denoise=none
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/oidn_on.png  16 --denoise=oidn
```

Expected: both produce PNGs. Visually, `/tmp/oidn_on.png` should look dramatically cleaner than `/tmp/oidn_off.png`.

- [ ] **Step 6.2: RMSE vs 4096-spp truth**

```bash
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png /tmp/oidn_off.png
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_4096spp.png /tmp/oidn_on.png
```

Record `Global RMSE` for each. Expected: OIDN ≥ 30% lower RMSE than noisy. Typical: 50%.

- [ ] **Step 6.3: Regression check — `--denoise=none` is bit-identical**

```bash
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/oidn_regression.png 16 --denoise=none
# Baseline needs to be pre-change — use the committed envlit_turntable reference
# (which was rendered without OIDN) as the comparison point.
python3 -c "
import hashlib
for p in ['/tmp/oidn_regression.png',
         'tests/reference_scenes/custom/envlit_turntable/reference.png']:
    with open(p, 'rb') as f:
        print(p, hashlib.md5(f.read()).hexdigest())
"
```

Expected: MD5 hashes MATCH. If they don't, something in the non-denoise path silently changed — investigate.

(If the committed reference was rendered with different flags or a slightly different seed, this check may false-negative — in that case, re-render the pre-OIDN baseline before the first implementation commit and compare against that.)

- [ ] **Step 6.4: Cornell box visual check**

```bash
./build/cornell_box /tmp/oidn_cornell_off.png 16 --denoise=none
./build/cornell_box /tmp/oidn_cornell_on.png  16 --denoise=oidn
```

Open both. Expected: walls are visibly smoother with OIDN. No firefly speckles. Edges preserved.

- [ ] **Step 6.5: Append to verification log**

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. APPEND (do NOT replace existing entries):

```markdown
## 2026-04-18: OIDN denoiser switch (Denoiser Sub-plan 1) validation

Offline profile now denoises via Intel OIDN by default. Comparison at
16 spp on DamagedHelmet + env_studio:

| Mode | RMSE vs 4096-spp truth |
|------|------------------------|
| --denoise=none (noisy)  | <fill>  |
| --denoise=oidn (default)| <fill>  |

- RMSE reduction: <X>%
- Visual: Cornell walls + helmet edges look clean at 16 spp

Denoiser Sub-plan 1 complete. Next: Sub-plan 2 (OptiX denoiser).
```

Fill `<fill>` and `<X>` with numbers from step 6.2.

- [ ] **Step 6.6: Commit**

```bash
git add tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): OIDN denoiser quality gate (Denoiser Sub-plan 1)

On envlit_turntable at 16 spp, OIDN reduces RMSE vs 4096-spp truth by
<X>% compared to noisy path-traced output. Offline profile now
denoises by default.

Co-Authored-By: Claude <model> <noreply@anthropic.com>"
```

Replace `<X>` with the measured number. Match the Co-Authored-By trailer format from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §4.1 `denoise/` subfolder | Task 1, 2 |
| §4.2 `DenoiseMode` enum + parse helpers | Task 1 |
| §4.3 `RTRenderSettings.denoiseMode` + preset defaults | Task 3 |
| §4.4 `VulkanRenderer::setDenoiseMode` + lazy denoise | Task 4 |
| §4.5 Error handling (stderr + fallback) | Task 4 |
| §4.6 CLI `--denoise=…` in all examples | Task 5 |
| §4.7 Remove `kEnableOIDN = false` hack | Task 5 |
| §5 File structure changes | Tasks 1, 2 |
| §6.1 Regression (bit-identical `--denoise=none`) | Task 6.3 |
| §6.2 OIDN quality at low spp | Task 6.1, 6.2 |
| §6.3 Cornell visual check | Task 6.4 |
| §6.4 Failure mode fallback | Covered by code in Task 4; no explicit test task (matches spec — spec says unit test; added to CLI parse test would be out-of-scope for this sub-plan. Flag for follow-up.) |
| §6.5 CLI parse unit test | Task 1 |
| §9 Success criteria | All tasks |

**Placeholder scan:** Only `<fill>` and `<X>` in Task 6.5/6.6 — deliberately for measured numbers.

**Type consistency:**
- `DenoiseMode` spelled identically across tasks (C++ enum + GLSL-free; no shader touches it).
- `setDenoiseMode` / `getDenoiseMode` / `denoiseMode` names consistent.
- Include path `"render/rt/denoise/denoise_types.hpp"` matches everywhere.

**Flagged:** Task 6.4 (Failure mode — OIDN returns false) is tested via code path but doesn't have a dedicated unit test. The spec calls this a "unit test" but making OIDN fail deterministically requires a mock which is out-of-scope. Current behavior: code falls back gracefully (covered by inline logic); any future OIDN mock is a follow-up.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-18-denoiser-subplan1-oidn-switch.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Same pattern that shipped Features 1.1 and 1.2.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
