# Denoiser Sub-plan 4.A: NRD Library + CMake Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire NVIDIA RayTracingDenoiser (NRD) into the OHAO build via CMake FetchContent as a hard dependency. Ship a PIMPL wrapper (`NrdDenoiser`) that can create and destroy an NRD instance against the existing Vulkan device. No denoise dispatch yet (4.B) — just lifecycle.

**Architecture:** Hard dep model. FetchContent pulls NRD from github at configure. `OHAO_NRD` CMake option (default ON) gates the whole integration; `-DOHAO_NRD=OFF` excludes the FetchContent + source file entirely (explicit opt-out, no silent fallback). Header + real impl, no stub.

**Tech Stack:** CMake FetchContent · NVIDIA NRD v4.x · Vulkan 1.3 · C++17 · no new runtime deps beyond Vulkan.

**Reference spec:** `docs/superpowers/specs/2026-04-23-denoiser-subplan4a-nrd-library-cmake-design.md`

---

## File Structure

**New files:**
- `ohao/render/rt/denoise/nrd_denoise.hpp` — public PIMPL API (2 methods: `initialize`, `shutdown`)
- `ohao/render/rt/denoise/nrd_denoise.cpp` — real impl guarded at CMake level (not included when `OHAO_NRD=OFF`)

**Modified files:**
- `CMakeLists.txt` — `OHAO_NRD` option, FetchContent block, conditional source listing, link, `OHAO_NRD_ENABLED` define
- `ohao/gpu/vulkan/renderer.cpp` — one-shot probe in `initialize()` that creates + destroys `NrdDenoiser` and logs result (removed in 4.B when real dispatch takes over)
- `CLAUDE.md` — append NRD dependency note near OptiX entry
- `tests/reference_scenes/custom/envlit_turntable/verification_log.md` — append 4.A entry

---

## Worktree Setup

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-4a-nrd -b denoiser-4a-nrd-library HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-4a-nrd`.

```bash
cd /home/frankyin/Desktop/Github/ohao-4a-nrd
cmake -B build -S . -DFETCHCONTENT_UPDATES_DISCONNECTED=ON 2>&1 | tail -10
# If build/_deps/ is empty after configure:
cp -r /home/frankyin/Desktop/Github/ohao_engine/build/_deps/. build/_deps/
```

Note: for T1 of this sub-plan, `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON` must be OMITTED at least once so NRD can be fetched. After the initial NRD clone, re-enable disconnected mode.

OptiX (still relevant, separate env var):
```bash
export OPTIX_ROOT=$HOME/optix-sdk/NVIDIA-OptiX-SDK-9.1.0-linux64-x86_64
```

---

## Task 1: CMake integration (FetchContent + option + conditional source)

**Files:**
- Modify: `CMakeLists.txt`

### Step 1.1: Locate the existing FetchContent block

```bash
cd /home/frankyin/Desktop/Github/ohao-4a-nrd
grep -n "FetchContent" CMakeLists.txt
```

The existing FetchContent calls (Jolt, tinygltf, nlohmann/json, etc.) give the style to match. Note which `FetchContent_Declare` / `FetchContent_MakeAvailable` pairs are used and where they live in the file.

### Step 1.2: Resolve the NRD release tag to use

NRD releases live at `https://github.com/NVIDIAGameWorks/RayTracingDenoiser/releases`. Pick the latest stable v4.x tag (check the repo). As of this plan's authorship the likely candidates are `v4.13`, `v4.14`, or `v4.15` — the implementer will verify via `git ls-remote --tags https://github.com/NVIDIAGameWorks/RayTracingDenoiser.git | tail -20` and pick the most recent non-`rc` / non-`pre` tag.

Record the chosen tag for later verification and for the commit message.

### Step 1.3: Add `OHAO_NRD` option + FetchContent block

Edit `CMakeLists.txt`. Near the existing FetchContent blocks (Jolt area), add:

```cmake
# ---- NRD (NVIDIA RayTracingDenoiser) for realtime RT denoise ----
option(OHAO_NRD "Enable NRD (NVIDIA RayTracingDenoiser) integration" ON)

if(OHAO_NRD)
    include(FetchContent)
    FetchContent_Declare(
        NRD
        GIT_REPOSITORY https://github.com/NVIDIAGameWorks/RayTracingDenoiser.git
        GIT_TAG        <TAG RESOLVED IN STEP 1.2>
    )
    # Disable NRD's integration-specific features we don't need
    set(NRD_DISABLE_SHADER_COMPILATION OFF CACHE BOOL "")
    set(NRD_SHADERS_PATH ""               CACHE STRING "")

    FetchContent_MakeAvailable(NRD)
endif()
```

The implementer will check NRD's root CMakeLists for actual option names and set them appropriately. The two `set(NRD_* CACHE BOOL "")` lines are illustrative — the plan lets the implementer resolve the exact knobs once NRD is fetched.

### Step 1.4: Conditionally link + define + compile nrd_denoise.cpp

After the existing `target_link_libraries(ohao_engine PRIVATE ...)` block and alongside other engine sources:

```cmake
if(OHAO_NRD)
    target_link_libraries(ohao_engine PRIVATE NRD)
    target_compile_definitions(ohao_engine PRIVATE OHAO_NRD_ENABLED)
    target_sources(ohao_engine PRIVATE
        ohao/render/rt/denoise/nrd_denoise.cpp)
endif()
```

The exact target name `NRD` depends on what NRD's CMake exposes — may be `NRD` or `nrd` or `NRD::NRD`. The implementer verifies after FetchContent and adjusts.

### Step 1.5: Configure with NRD ON

```bash
cd /home/frankyin/Desktop/Github/ohao-4a-nrd
# First time NEEDS disconnected OFF so NRD can actually clone
rm -rf build
cmake -B build -S . 2>&1 | tail -30
```

Expected: NRD clones into `build/_deps/nrd-src/`. First configure may take 2-5 min (NRD builds its own shader compilation dependencies: slang, shaderMake, etc.).

Watch for:
- "Cloning into ... RayTracingDenoiser..."
- Any CMake error from NRD's own config
- Final `-- Configuring done` + `-- Generating done` + `-- Build files written to:`

If NRD's CMake fails (e.g. missing dep, needs Vulkan SDK headers), fix at this step. Don't move on until configure succeeds.

### Step 1.6: Build with NRD ON (engine only — no cpp yet)

At this point `nrd_denoise.cpp` doesn't exist yet (T2 creates it), so `target_sources(... nrd_denoise.cpp)` will fail CMake's glob. Temporarily comment out `target_sources` line or create an empty stub:

```bash
# Temporary empty stub so build succeeds in T1:
mkdir -p ohao/render/rt/denoise
echo "// T1 placeholder; T2 implements." > ohao/render/rt/denoise/nrd_denoise.cpp
```

Then:

```bash
cmake --build build -j8 2>&1 | tail -10
```

Expected: clean build. `libNRD.a` (or whatever NRD produces) is in `build/_deps/nrd-build/` or similar. The engine links it but doesn't reference any symbols yet.

### Step 1.7: Verify `-DOHAO_NRD=OFF` escape hatch

```bash
rm -rf build-nonrd
cmake -B build-nonrd -S . -DOHAO_NRD=OFF 2>&1 | tail -10
cmake --build build-nonrd -j8 2>&1 | tail -5
```

Expected: no NRD clone, no NRD link. Build produces `ohao_engine` without NRD. Clean.

### Step 1.8: Commit T1

Clean up the empty placeholder cpp (keep it — T2 fills it) and commit:

```bash
git add CMakeLists.txt ohao/render/rt/denoise/nrd_denoise.cpp
git commit -m "build(rt): FetchContent NRD + OHAO_NRD option (Sub-plan 4.A)

Adds NVIDIA RayTracingDenoiser as a hard CMake dependency via
FetchContent. OHAO_NRD option (default ON) controls inclusion;
-DOHAO_NRD=OFF excludes the FetchContent, link, and nrd_denoise.cpp
source entirely for an explicit opt-out (no silent fallback).

Pinned NRD tag: <tag>. Placeholder nrd_denoise.cpp added so build
succeeds; real impl lands in Sub-plan 4.A T2.

First configure pulls NRD (~2-5 min). Subsequent configures cached.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Fill `<tag>` with the resolved NRD tag from Step 1.2.

---

## Task 2: Public header + real impl + probe + CLAUDE.md + verify

**Files:**
- Create: `ohao/render/rt/denoise/nrd_denoise.hpp`
- Modify: `ohao/render/rt/denoise/nrd_denoise.cpp` (replace the T1 placeholder)
- Modify: `ohao/gpu/vulkan/renderer.cpp`
- Modify: `CLAUDE.md`
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md`

### Step 2.1: Create nrd_denoise.hpp

Write `ohao/render/rt/denoise/nrd_denoise.hpp`:

```cpp
#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <cstdint>

namespace ohao {

/// PIMPL wrapper around NVIDIA RayTracingDenoiser (NRD).
///
/// 4.A scope: lifecycle only (initialize / shutdown).
/// Denoise dispatch added in 4.B+.
///
/// Requires OHAO_NRD=ON at CMake time. If OHAO_NRD=OFF, this header
/// compiles but instantiating NrdDenoiser will produce a link error —
/// callers must guard with `#ifdef OHAO_NRD_ENABLED`.
class NrdDenoiser {
public:
    NrdDenoiser();
    ~NrdDenoiser();

    NrdDenoiser(const NrdDenoiser&)            = delete;
    NrdDenoiser& operator=(const NrdDenoiser&) = delete;

    /// Create an NRD instance sized for w x h against the given Vulkan device.
    /// Returns false on NRD-side failure (logs error).
    bool initialize(VkDevice         device,
                    VkPhysicalDevice physicalDevice,
                    uint32_t         width,
                    uint32_t         height);

    /// Destroy the NRD instance. Safe to call multiple times; safe to call
    /// without a prior successful initialize.
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace ohao
```

### Step 2.2: Investigate NRD v4 API

Before writing the .cpp, explore the FetchContent'd NRD source to find:
- The main header(s) to `#include`
- The instance type (likely `nrd::Instance*` or `NrdInstance` handle)
- The create function (likely `nrd::CreateInstance(const InstanceCreationDesc&, Instance*&)`)
- The destroy function (likely `nrd::DestroyInstance(Instance*)` or `nrd::ReleaseInstance(Instance*)`)
- REBLUR_DIFFUSE_SPECULAR denoiser descriptor struct name + fields

```bash
cd /home/frankyin/Desktop/Github/ohao-4a-nrd
# After T1's cmake configure, NRD source is at build/_deps/nrd-src
find build/_deps/nrd-src/Include -name "*.h" | head
cat build/_deps/nrd-src/Include/NRD.h 2>/dev/null | head -100
# Search for creation pattern:
grep -n "CreateInstance\|Instance\*" build/_deps/nrd-src/Include/*.h | head
```

Record the exact types/functions for the .cpp.

### Step 2.3: Write real nrd_denoise.cpp

Replace the T1 placeholder with the real implementation. Template (implementer fills exact NRD v4 symbol names from Step 2.2):

```cpp
#include "ohao/render/rt/denoise/nrd_denoise.hpp"
#include "core/logging/logger.hpp"

#include <NRD.h>  // exact header from Step 2.2

namespace ohao {

struct NrdDenoiser::Impl {
    VkDevice         device         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t         width          = 0;
    uint32_t         height         = 0;
    nrd::Instance*   instance       = nullptr;  // VERIFY TYPE NAME from Step 2.2
};

NrdDenoiser::NrdDenoiser()  : m_impl(std::make_unique<Impl>()) {}
NrdDenoiser::~NrdDenoiser() { shutdown(); }

bool NrdDenoiser::initialize(VkDevice device,
                              VkPhysicalDevice physicalDevice,
                              uint32_t width,
                              uint32_t height) {
    m_impl->device         = device;
    m_impl->physicalDevice = physicalDevice;
    m_impl->width          = width;
    m_impl->height         = height;

    // Build NRD InstanceCreationDesc:
    //   - one DenoiserDesc for REBLUR_DIFFUSE_SPECULAR at (width, height)
    //   - identifier 0 (caller assigns)
    // Exact field names from NRD v4 header (Step 2.2).

    nrd::DenoiserDesc denoiserDescs[1] = {};
    denoiserDescs[0].identifier        = 0;
    denoiserDescs[0].denoiser          = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
    denoiserDescs[0].renderWidth       = uint16_t(width);
    denoiserDescs[0].renderHeight      = uint16_t(height);

    nrd::InstanceCreationDesc desc = {};
    desc.denoisers                 = denoiserDescs;
    desc.denoisersNum              = 1;

    nrd::Result result = nrd::CreateInstance(desc, m_impl->instance);
    if (result != nrd::Result::SUCCESS) {
        OHAO_LOG_ERROR("[NRD] CreateInstance failed: {}", int(result));
        return false;
    }

    OHAO_LOG_INFO("[NRD] initialized for {}x{}", width, height);
    return true;
}

void NrdDenoiser::shutdown() {
    if (m_impl->instance) {
        nrd::DestroyInstance(*m_impl->instance);  // VERIFY — may be nrd::ReleaseInstance(m_impl->instance)
        m_impl->instance = nullptr;
    }
}

}  // namespace ohao
```

Resolve the exact NRD v4 symbol names (`nrd::CreateInstance`, `nrd::DestroyInstance` vs `ReleaseInstance`, etc.) against the header tree. The template above shows the shape; symbols may differ.

### Step 2.4: Add probe in VulkanRenderer::initialize

Edit `ohao/gpu/vulkan/renderer.cpp`. Find `VulkanRenderer::initialize()` (the function that sets up device + resources). Near the end, AFTER the device and physical device are available, add:

```cpp
#ifdef OHAO_NRD_ENABLED
    {
        NrdDenoiser nrdProbe;
        if (nrdProbe.initialize(m_device, m_physicalDevice, m_width, m_height)) {
            OHAO_LOG_INFO("[NRD probe] 4.A lifecycle smoke passed");
        } else {
            OHAO_LOG_ERROR("[NRD probe] 4.A lifecycle smoke FAILED");
        }
        nrdProbe.shutdown();
    }
#endif
```

Include the header at the top of renderer.cpp:

```cpp
#include "ohao/render/rt/denoise/nrd_denoise.hpp"
```

The `#ifdef OHAO_NRD_ENABLED` guard is required — with `-DOHAO_NRD=OFF`, the nrd_denoise.cpp isn't compiled, so `NrdDenoiser` would fail to link without the guard.

### Step 2.5: Build + run smoke

```bash
cd /home/frankyin/Desktop/Github/ohao-4a-nrd
cmake --build build -j8 2>&1 | tail -10
./build/cornell_box /tmp/t2_4a_cornell.png 4 --denoise=none 2>&1 | grep -E "NRD|Saved"
```

Expected stdout includes both:
```
[NRD] initialized for 1920x1080
[NRD probe] 4.A lifecycle smoke passed
Saved: /tmp/t2_4a_cornell.png
```

(Resolutions depend on cornell_box's defaults; log should show whatever `m_width × m_height` is.)

No Vulkan validation errors. Clean shutdown.

### Step 2.6: Build with OHAO_NRD=OFF — confirm escape hatch still works

```bash
cd /home/frankyin/Desktop/Github/ohao-4a-nrd
rm -rf build-nonrd
cmake -B build-nonrd -S . -DOHAO_NRD=OFF 2>&1 | tail -5
cmake --build build-nonrd -j8 2>&1 | tail -5
./build-nonrd/cornell_box /tmp/t2_4a_cornell_nonrd.png 4 --denoise=none 2>&1 | grep -E "NRD|Saved"
```

Expected: no "[NRD]" log line (probe is `#ifdef`-guarded out), but `Saved: ...` still fires. Engine builds and runs without NRD.

### Step 2.7: Update CLAUDE.md

Find the OptiX dependency section in `CLAUDE.md` (grep `OptiX`). Right after the OptiX entry in the Dependencies section, add:

```markdown
- NVIDIA NRD (RayTracingDenoiser) v4.x — for realtime RT denoising (Sub-plans 4.A–4.E). Fetched via CMake FetchContent from `github.com/NVIDIAGameWorks/RayTracingDenoiser`. Pure Vulkan (no CUDA dep). Opt out with `-DOHAO_NRD=OFF`.
```

Also update the Build Issues section if the first NRD configure on this project surfaced anything new.

### Step 2.8: Append verification log entry

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append:

```markdown
## 2026-04-23: NRD library integration (Sub-plan 4.A)

NVIDIA RayTracingDenoiser wired via CMake FetchContent as a hard
dependency. `NrdDenoiser` PIMPL wrapper exposes `initialize` +
`shutdown` against the existing Vulkan device. Denoise dispatch
lands in 4.B.

Build integration:
- `OHAO_NRD=ON` (default): FetchContent clones NRD, links the static
  library into the engine, and compiles `nrd_denoise.cpp` with
  `OHAO_NRD_ENABLED` defined.
- `OHAO_NRD=OFF`: FetchContent + link + source compilation all
  skipped. `NrdDenoiser` callers are expected to `#ifdef OHAO_NRD_ENABLED`
  guard instantiations.

Pinned NRD tag: <fill from T1 commit>.

Lifecycle smoke via probe in `VulkanRenderer::initialize`:
```
[NRD] initialized for WxH
[NRD probe] 4.A lifecycle smoke passed
```
No validation errors. Probe removed in 4.B when real dispatch
replaces it.

Next: Sub-plan 4.B (NRD API expansion — set image views, populate
per-frame inputs from our AOVs).
```

Fill `<fill from T1 commit>` with the NRD tag you pinned.

### Step 2.9: Commit T2

```bash
cd /home/frankyin/Desktop/Github/ohao-4a-nrd
git add ohao/render/rt/denoise/nrd_denoise.hpp \
        ohao/render/rt/denoise/nrd_denoise.cpp \
        ohao/gpu/vulkan/renderer.cpp \
        CLAUDE.md \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "feat(rt): NrdDenoiser PIMPL + lifecycle probe (Sub-plan 4.A)

Adds ohao/render/rt/denoise/nrd_denoise.hpp public API (initialize +
shutdown) and nrd_denoise.cpp real impl calling NRD v4's CreateInstance
with a REBLUR_DIFFUSE_SPECULAR denoiser descriptor sized to the render
viewport.

VulkanRenderer::initialize runs a one-shot probe (guarded by
OHAO_NRD_ENABLED) that creates and destroys an NrdDenoiser and logs
success. Probe removed in Sub-plan 4.B when real per-frame dispatch
takes over.

CLAUDE.md updated with NRD dependency entry alongside existing OptiX.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>"
```

Match Co-Authored-By from `git log -3 --format=%B`.

---

## Plan Self-Review

**Spec coverage:**

| Spec requirement | Task |
|---|---|
| §3 library source = FetchContent | T1 Step 1.3 |
| §3 hard dep, no stubs | T1 + T2 (no stub file created) |
| §3 opt-out `-DOHAO_NRD=OFF` | T1 Step 1.7 + T2 Step 2.6 |
| §3 API shape (initialize/shutdown) | T2 Step 2.1 |
| §3 Linux-first, pure Vulkan | implicit — no Windows/CUDA paths added |
| §4.1 CMake integration | T1 |
| §4.2 public header | T2 Step 2.1 |
| §4.3 real impl with NRD CreateInstance | T2 Step 2.3 |
| §5.1 file map (hpp, cpp, CMake, probe, CLAUDE.md, log) | T1 + T2 |
| §6 verification (configure, link, opt-out, probe smoke) | T1 Steps 1.5–1.7; T2 Steps 2.5–2.6 |
| §8 success criteria 1–8 | T1 + T2 collectively |

**Placeholder scan:**
- `<tag>` in T1 Step 1.8 commit message — intentional, resolved at Step 1.2 by the implementer.
- `<fill from T1 commit>` in T2 Step 2.8 verification log — same.
- `nrd::Instance*` / `nrd::CreateInstance` / `nrd::DestroyInstance` in T2 Step 2.3 — template shape; exact NRD v4 names resolved against the source tree at Step 2.2 before writing. This is a legitimate investigation step, not a placeholder-to-fix.

**Type consistency:**
- `NrdDenoiser` (the public class name) used identically in hpp, cpp, probe. Namespace `ohao::`.
- `OHAO_NRD` CMake option, `OHAO_NRD_ENABLED` preprocessor define — spelled consistently.
- Header path `ohao/render/rt/denoise/nrd_denoise.hpp` consistent in CMake target_sources, `#include` sites, `git add` commands.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-23-denoiser-subplan4a-nrd-library-cmake.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — Fresh subagent per task with two-stage review. Matches the pattern that shipped the 3.x chain cleanly.

**2. Inline Execution** — Batch with checkpoints.

**Which approach?**
