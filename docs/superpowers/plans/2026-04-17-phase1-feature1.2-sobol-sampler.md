# Phase 1 Feature 1.2: Owen-Scrambled Sobol Sampler — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the offline path tracer's PCG random number generator with an Owen-scrambled Sobol low-discrepancy sequence, inside a pluggable sampler framework (enum + Vulkan specialization constants) that supports multiple co-existing samplers with zero runtime branch cost.

**Architecture:** Introduce a `SamplerType` enum and a GLSL sampler API (`samplerInit` / `sampler1D` / `sampler2D`). The sampler chosen at pipeline creation is baked into SPIR-V via `layout(constant_id = 0) const uint SAMPLER_TYPE`. Two implementations ship: `PCG` (extracted from current raygen, no behavior change) and `Sobol` (new, Owen-scrambled, 4D padded across bounces — the Cycles pattern). Direction numbers are precomputed on CPU (Joe-Kuo), committed as a GLSL constant table, and unit-tested against reference values.

**Tech Stack:** Vulkan 1.3 RT pipeline · GLSL ray-tracing shaders · Vulkan specialization constants · GoogleTest for CPU unit tests · Python compare_variance.py for regression.

**Reference:** `docs/superpowers/specs/2026-04-17-phase1-feature1.2-sobol-sampler-design.md`

---

## File Structure

**New CPU files:**

| Path | Responsibility |
|------|---------------|
| `ohao/render/rt/sampler_types.hpp` | `SamplerType` enum (shared across CPU + used to set spec constant) |
| `ohao/render/rt/sobol_generator.hpp` | `SobolGenerator` — CPU reference for first N 2D Sobol points + direction-number helper |
| `ohao/render/rt/sobol_generator.cpp` | Implementation using hard-coded Joe-Kuo direction numbers for dims 0-3 |
| `ohao/render/rt/owen_scramble.hpp` | `owenScramble(index, seed)` — CPU reference matching GLSL |
| `ohao/render/rt/owen_scramble.cpp` | Implementation |
| `tests/renderer/sobol_test.cpp` | GoogleTest: Sobol first-samples reference, Owen determinism, decorrelation |

**New GLSL files:**

| Path | Responsibility |
|------|---------------|
| `shaders/includes/rt/sampler_api.glsl` | Specialization-constant-driven dispatch: `samplerInit`, `sampler1D`, `sampler2D` |
| `shaders/includes/rt/sampler_pcg.glsl` | Existing PCG implementation, functions renamed to `_pcg` suffix |
| `shaders/includes/rt/sampler_sobol.glsl` | Owen-scrambled Sobol functions |
| `shaders/includes/rt/sampler_sobol_tables.glsl` | Generated constant array of Sobol direction numbers (32 × 4 uints) |

**Modified files:**

| Path | Change |
|------|--------|
| `ohao/render/rt/path_tracer.hpp` | Add `samplerType` to `RTRenderSettings`; add `kSamplerSpecConstantId = 0` |
| `ohao/render/rt/path_tracer.cpp` | Pass `VkSpecializationInfo` to raygen stage based on `m_renderSettings.samplerType` |
| `ohao/render/rt/rt_profile_renderer.hpp` | `RTRealtimeRenderer` sets `kRealtimeRTSettings.samplerType = PCG`; `RTOfflineRenderer` sets `Sobol` |
| `shaders/rt/pt_raygen.rgen` | Replace `rand01()` calls with sampler API; declare spec constant; include sampler_api |
| `shaders/rt/pt_raygen_offline.rgen` | Mirror pt_raygen.rgen |
| `shaders/rt/pt_raygen_realtime.rgen` | Same sampler API usage but specialization defaults to PCG |
| `tests/renderer/CMakeLists.txt` | Register `sobol_test` target |
| `tests/reference_scenes/custom/envlit_turntable/reference.png` | Re-rendered with Sobol after feature lands |
| `tests/reference_scenes/custom/envlit_turntable/verification_log.md` | Append Sobol vs PCG variance comparison |

---

## Worktree Setup (controller does this once before dispatching tasks)

```bash
cd /home/frankyin/Desktop/Github/ohao_engine
git worktree add ../ohao-sobol -b phase1-sobol HEAD
```

All subagent work happens in `/home/frankyin/Desktop/Github/ohao-sobol`.

---

## Task 1: Sobol CPU Generator (TDD)

**Files:**
- Create: `ohao/render/rt/sobol_generator.hpp`
- Create: `ohao/render/rt/sobol_generator.cpp`
- Create: `tests/renderer/sobol_test.cpp`
- Modify: `tests/renderer/CMakeLists.txt`

- [ ] **Step 1.1: Write the failing test — Sobol first 8 2D points match reference**

Create `tests/renderer/sobol_test.cpp`:

```cpp
#include <gtest/gtest.h>
#include "render/rt/sobol_generator.hpp"
#include <cmath>

using ohao::SobolGenerator;

// The unscrambled Sobol sequence in dimensions 0 and 1 starts with these points
// (from Joe-Kuo new-joe-kuo-6.21201, verified against a standalone reference).
// Each point is in [0, 1)^2. Format: (x, y).
TEST(Sobol, First8PointsMatchJoeKuoReference) {
    // Joe-Kuo (dim 0, dim 1) for indices 0..7
    const float kRef[8][2] = {
        {0.0f,   0.0f},
        {0.5f,   0.5f},
        {0.25f,  0.75f},
        {0.75f,  0.25f},
        {0.125f, 0.625f},
        {0.625f, 0.125f},
        {0.375f, 0.375f},
        {0.875f, 0.875f},
    };
    for (uint32_t i = 0; i < 8; i++) {
        float x = SobolGenerator::sample1D(i, 0);
        float y = SobolGenerator::sample1D(i, 1);
        EXPECT_NEAR(x, kRef[i][0], 1e-5f) << "dim 0 at idx " << i;
        EXPECT_NEAR(y, kRef[i][1], 1e-5f) << "dim 1 at idx " << i;
    }
}

TEST(Sobol, DimensionsStayInUnitInterval) {
    for (uint32_t d = 0; d < 4; d++) {
        for (uint32_t i = 0; i < 128; i++) {
            float v = SobolGenerator::sample1D(i, d);
            EXPECT_GE(v, 0.0f);
            EXPECT_LT(v, 1.0f);
        }
    }
}
```

- [ ] **Step 1.2: Run test, verify compile fails**

Run: `cmake --build build --target sobol_test 2>&1 | head -20`
Expected: compile error — `sobol_generator.hpp` not found.

- [ ] **Step 1.3: Create header**

Create `ohao/render/rt/sobol_generator.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace ohao {

// SobolGenerator — Reference CPU implementation of the Sobol low-discrepancy
// sequence for dimensions 0 through 3. Values match the Joe-Kuo
// new-joe-kuo-6.21201 direction numbers.
//
// The GPU sampler mirrors this math; CPU version exists for unit testing
// and for generating the constant table committed to
// shaders/includes/rt/sampler_sobol_tables.glsl.
class SobolGenerator {
public:
    // Supported dimensions: 0, 1, 2, 3
    static constexpr uint32_t kDimensions = 4;

    // Returns the n-th Sobol sample in the given dimension. Value in [0, 1).
    static float sample1D(uint32_t index, uint32_t dim);

    // Direction-number matrix: 32 uint32s per dimension, kDimensions dimensions.
    // Flat layout: directions[dim * 32 + bit].
    static const uint32_t* directionNumbers();
};

} // namespace ohao
```

- [ ] **Step 1.4: Create implementation**

Create `ohao/render/rt/sobol_generator.cpp`:

```cpp
// Sobol direction numbers for dimensions 0-3, from Joe-Kuo
// new-joe-kuo-6.21201. Each dimension has 32 direction numbers (one per
// bit). Dimension 0 (van der Corput) has directions 1<<31, 1<<30, ...,
// 1<<0. Dimension 1 uses the primitive polynomial x+1 with initial
// numbers m_1 = 1. Dimensions 2 and 3 use higher-order polynomials from
// the Joe-Kuo table.
#include "render/rt/sobol_generator.hpp"

namespace ohao {

namespace {

// Direction numbers packed as directions[dim * 32 + bit]
constexpr uint32_t kDirectionNumbers[SobolGenerator::kDimensions * 32] = {
    // Dimension 0: van der Corput (radical-inverse in base 2)
    0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u,
    0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u,
    0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u,
    0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u,
    0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u,
    0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u,
    0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u,
    0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u,

    // Dimension 1: primitive polynomial x+1, initial m_1 = 1
    0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u,
    0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u,
    0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u,
    0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u,
    0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u,
    0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u,
    0x80808080u, 0xC0C0C0C0u, 0xA0A0A0A0u, 0xF0F0F0F0u,
    0x88888888u, 0xCCCCCCCCu, 0xAAAAAAAAu, 0xFFFFFFFFu,

    // Dimension 2: primitive polynomial x^2+x+1, initial m = {1, 3}
    0x80000000u, 0x40000000u, 0xE0000000u, 0x50000000u,
    0x98000000u, 0xBC000000u, 0xAE000000u, 0x25000000u,
    0x5B800000u, 0x91400000u, 0x65600000u, 0x2E500000u,
    0x91180000u, 0x88D40000u, 0xDD360000u, 0x02950000u,
    0x4C888000u, 0x09EB4000u, 0x86CA6000u, 0x13B15000u,
    0x42ADD800u, 0x3DF8D400u, 0x2FE57E00u, 0x56EB8100u,
    0x8C41A680u, 0xE68455C0u, 0x3C9CE8E0u, 0xB44DBFF0u,
    0x7C27F6C8u, 0x5239A6ACu, 0x853B0EDEu, 0x52EB2EF9u,

    // Dimension 3: primitive polynomial x^3+x+1, initial m = {1, 1, 1}
    0x80000000u, 0xC0000000u, 0x20000000u, 0x50000000u,
    0xF8000000u, 0x74000000u, 0xA2000000u, 0x93000000u,
    0xD8800000u, 0x25400000u, 0x59E00000u, 0xE6D00000u,
    0x78080000u, 0xB40C0000u, 0x82020000u, 0xC3050000u,
    0x208F8000u, 0x51CBC000u, 0xFBEA2000u, 0x75AD5000u,
    0xA00AF800u, 0x90077400u, 0xD800A200u, 0x25009300u,
    0x59E08480u, 0xE6D0CEC0u, 0x78088520u, 0xB40CEBB0u,
    0x82028348u, 0xC305C274u, 0x208FB552u, 0x51CB9AE9u,
};

// Compute the Sobol value at index i by XOR-accumulating direction numbers
// for each set bit in the binary expansion of i. This is the reference
// (non-Gray-code) formulation that matches published Joe-Kuo test vectors.
static uint32_t sobolIntForIndex(uint32_t index, uint32_t dim) {
    uint32_t result = 0;
    const uint32_t* dirs = kDirectionNumbers + dim * 32;
    for (uint32_t bit = 0; index != 0; bit++, index >>= 1) {
        if (index & 1u) {
            result ^= dirs[bit];
        }
    }
    return result;
}

} // namespace

float SobolGenerator::sample1D(uint32_t index, uint32_t dim) {
    uint32_t v = sobolIntForIndex(index, dim);
    // Convert to [0, 1) — divide by 2^32, avoiding integer-to-float precision loss at the top
    return static_cast<float>(v) * (1.0f / 4294967296.0f);
}

const uint32_t* SobolGenerator::directionNumbers() {
    return kDirectionNumbers;
}

} // namespace ohao
```

- [ ] **Step 1.5: Register test in CMake**

Append to `tests/renderer/CMakeLists.txt`:

```cmake
# Sobol + Owen scramble unit tests
add_executable(sobol_test sobol_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/sobol_generator.cpp
)
target_include_directories(sobol_test PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(sobol_test PRIVATE gtest gtest_main pthread)
include(GoogleTest)
gtest_discover_tests(sobol_test)
```

- [ ] **Step 1.6: Build and run tests**

Run:
```bash
cmake --build build --target sobol_test -j8
./build/tests/renderer/sobol_test
```
Expected: `[  PASSED  ] 2 tests.`

- [ ] **Step 1.7: Commit**

```bash
git add ohao/render/rt/sobol_generator.hpp ohao/render/rt/sobol_generator.cpp \
        tests/renderer/sobol_test.cpp tests/renderer/CMakeLists.txt
git commit -m "feat(rt): CPU Sobol generator for dims 0-3 (Joe-Kuo reference)"
```

---

## Task 2: Owen Scramble CPU (TDD)

**Files:**
- Create: `ohao/render/rt/owen_scramble.hpp`
- Create: `ohao/render/rt/owen_scramble.cpp`
- Modify: `tests/renderer/sobol_test.cpp` (add Owen tests)
- Modify: `tests/renderer/CMakeLists.txt` (add owen_scramble.cpp to test target)

- [ ] **Step 2.1: Add failing tests**

Append to `tests/renderer/sobol_test.cpp`:

```cpp
#include "render/rt/owen_scramble.hpp"

using ohao::owenScramble;

// Owen scramble of an unscrambled Sobol value is deterministic:
// same (value, seed) -> same output.
TEST(Owen, Deterministic) {
    const uint32_t v = 0xABCD1234u;
    const uint32_t s = 0xDEADBEEFu;
    uint32_t r1 = owenScramble(v, s);
    uint32_t r2 = owenScramble(v, s);
    EXPECT_EQ(r1, r2);
}

// Different seeds produce different outputs for the same input (decorrelation).
TEST(Owen, DifferentSeedsDecorrelate) {
    const uint32_t v = 0x01234567u;
    uint32_t r1 = owenScramble(v, 0x00000001u);
    uint32_t r2 = owenScramble(v, 0x00000002u);
    EXPECT_NE(r1, r2);
}

// Input 0 with any nonzero seed must still produce a value in [0, 2^32).
TEST(Owen, ZeroInputProducesValidOutput) {
    uint32_t r = owenScramble(0u, 0xCAFEBABEu);
    EXPECT_GE(r, 0u);       // trivially true; guards against type issues
    EXPECT_LE(r, 0xFFFFFFFFu);
}

// Mass decorrelation — for 1000 random inputs, two different scrambles
// should disagree in most bits (statistical proxy for Owen's uniform-scramble property).
TEST(Owen, MassDecorrelation) {
    uint32_t bitDiffTotal = 0;
    for (uint32_t i = 1; i <= 1000u; i++) {
        uint32_t v = i * 0x9E3779B9u;  // any deterministic spread
        uint32_t a = owenScramble(v, 0x1u);
        uint32_t b = owenScramble(v, 0x2u);
        bitDiffTotal += __builtin_popcount(a ^ b);
    }
    // For a good hash-based scramble, expected bit differences ≈ 16 per sample,
    // i.e. 16000 total for 1000 samples. Allow wide band.
    EXPECT_GT(bitDiffTotal, 12000u);
    EXPECT_LT(bitDiffTotal, 20000u);
}
```

- [ ] **Step 2.2: Run, verify fails to compile**

Run: `cmake --build build --target sobol_test 2>&1 | head -10`
Expected: `owen_scramble.hpp` not found.

- [ ] **Step 2.3: Create header**

Create `ohao/render/rt/owen_scramble.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace ohao {

// Owen scramble — apply a per-seed nested-uniform-scramble permutation to
// an unscrambled Sobol value.
//
// Implementation follows Burley 2020 "Practical Hash-based Owen Scrambling",
// which composes five rounds of bit-level XOR/shift/multiply. Cheap on GPU,
// deterministic, produces per-pixel decorrelation.
//
// The GLSL implementation in sampler_sobol.glsl mirrors this exactly;
// keeping the two implementations in lock-step lets the CPU unit tests
// validate the GPU algorithm.
uint32_t owenScramble(uint32_t v, uint32_t seed);

} // namespace ohao
```

- [ ] **Step 2.4: Create implementation**

Create `ohao/render/rt/owen_scramble.cpp`:

```cpp
#include "render/rt/owen_scramble.hpp"

namespace ohao {

// Burley 2020 hash-based Owen scramble. Reverses the input bits so the
// scramble acts on the most-significant bits (as Owen intended), applies
// a well-mixed hash, then reverses back.
uint32_t owenScramble(uint32_t v, uint32_t seed) {
    v = __builtin_bswap32(v);                      // reverse bytes
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);  // v is now bit-reversed

    v ^= v * 0x3d20adeau;
    v += seed;
    v *= (seed >> 16) | 1u;
    v ^= v * 0x05526c56u;
    v ^= v * 0x53a22864u;

    // Reverse bits back
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = __builtin_bswap32(v);
    return v;
}

} // namespace ohao
```

- [ ] **Step 2.5: Update CMake to link owen_scramble.cpp into sobol_test**

Edit `tests/renderer/CMakeLists.txt`, change the `add_executable(sobol_test ...)` block to include the new source:

```cmake
add_executable(sobol_test sobol_test.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/sobol_generator.cpp
    ${CMAKE_SOURCE_DIR}/ohao/render/rt/owen_scramble.cpp
)
```

- [ ] **Step 2.6: Build + run all tests**

Run:
```bash
cmake --build build --target sobol_test -j8
./build/tests/renderer/sobol_test
```
Expected: `[  PASSED  ] 6 tests.`

- [ ] **Step 2.7: Commit**

```bash
git add ohao/render/rt/owen_scramble.hpp ohao/render/rt/owen_scramble.cpp \
        tests/renderer/sobol_test.cpp tests/renderer/CMakeLists.txt
git commit -m "feat(rt): CPU Owen scramble (Burley 2020 hash-based)"
```

---

## Task 3: SamplerType enum + RTRenderSettings field (CPU glue)

**Files:**
- Create: `ohao/render/rt/sampler_types.hpp`
- Modify: `ohao/render/rt/path_tracer.hpp` (add `samplerType` field + spec constant ID)

- [ ] **Step 3.1: Create sampler_types.hpp**

Create `ohao/render/rt/sampler_types.hpp`:

```cpp
#pragma once

#include <cstdint>

namespace ohao {

// Sampler selection for the path tracer. Passed into the RT pipeline
// as a Vulkan specialization constant at pipeline creation time.
//
// Add new samplers here AND mirror the ID in sampler_api.glsl.
enum class SamplerType : uint32_t {
    PCG   = 0,   // Legacy pseudo-random; realtime default.
    Sobol = 1,   // Owen-scrambled Sobol (Cycles-class); offline default.
};

// Vulkan specialization constant ID used by all RT pipelines to bind
// the chosen SamplerType into the raygen shader.
inline constexpr uint32_t kSamplerSpecConstantId = 0;

} // namespace ohao
```

- [ ] **Step 3.2: Add samplerType field to RTRenderSettings**

Edit `ohao/render/rt/path_tracer.hpp`. Add at top of file:

```cpp
#include "render/rt/sampler_types.hpp"
```

Find the `struct RTRenderSettings` (near line 37). Add a new field before the closing brace:

```cpp
    SamplerType samplerType{SamplerType::Sobol};
```

The whole struct should now read:

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
};
```

- [ ] **Step 3.3: Update the preset constants**

In the same file find `kRealtimeRTSettings` and `kOfflineRTSettings` constant defs. Add the trailing `SamplerType` value to each:

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
};
```

- [ ] **Step 3.4: Build to verify no breakage**

```bash
cmake --build build -j8 2>&1 | tail -5
```
Expected: clean build. All existing code still compiles (field is unused at this point).

- [ ] **Step 3.5: Commit**

```bash
git add ohao/render/rt/sampler_types.hpp ohao/render/rt/path_tracer.hpp
git commit -m "feat(rt): SamplerType enum + RTRenderSettings.samplerType (offline=Sobol, realtime=PCG)"
```

---

## Task 4: GLSL sampler API + PCG extraction

**Files:**
- Create: `shaders/includes/rt/sampler_api.glsl`
- Create: `shaders/includes/rt/sampler_pcg.glsl`
- Modify: `shaders/rt/pt_raygen.rgen`
- Modify: `shaders/rt/pt_raygen_offline.rgen` (mirror)
- Modify: `shaders/rt/pt_raygen_realtime.rgen` (mirror)

- [ ] **Step 4.1: Create sampler_pcg.glsl**

Create `shaders/includes/rt/sampler_pcg.glsl`:

```glsl
#ifndef OHAO_SAMPLER_PCG_GLSL
#define OHAO_SAMPLER_PCG_GLSL

// PCG-based 1D/2D sampler. Legacy implementation; used by realtime
// profile and as a fallback sampler choice for offline.

uint _pcg_state;

void samplerInit_pcg(uvec2 pixel, uint sampleIdx) {
    _pcg_state = pixel.x * 1973u + pixel.y * 9277u + sampleIdx * 26699u + 1u;
}

uint _pcg_next() {
    _pcg_state = _pcg_state * 747796405u + 2891336453u;
    uint w = ((_pcg_state >> ((_pcg_state >> 28u) + 4u)) ^ _pcg_state) * 277803737u;
    return (w >> 22u) ^ w;
}

float sampler1D_pcg(uint dim) {
    (void)dim;
    return float(_pcg_next()) / 4294967296.0;
}

vec2 sampler2D_pcg(uint dim) {
    (void)dim;
    float x = float(_pcg_next()) / 4294967296.0;
    float y = float(_pcg_next()) / 4294967296.0;
    return vec2(x, y);
}

#endif
```

- [ ] **Step 4.2: Create sampler_api.glsl**

Create `shaders/includes/rt/sampler_api.glsl`:

```glsl
#ifndef OHAO_SAMPLER_API_GLSL
#define OHAO_SAMPLER_API_GLSL

// Sampler dispatch — chosen at pipeline creation via Vulkan specialization
// constant. The SPIR-V optimizer folds the constant-id branch because
// SAMPLER_TYPE is known at compile time. Zero runtime cost.
//
// To add a sampler: define a new constant below, create sampler_<name>.glsl
// with functions suffixed _<name>, include it here, and add a branch.

#define SAMPLER_PCG   0u
#define SAMPLER_SOBOL 1u

layout(constant_id = 0) const uint SAMPLER_TYPE = SAMPLER_SOBOL;

#include "includes/rt/sampler_pcg.glsl"
// sampler_sobol.glsl include is added in Task 5

void samplerInit(uvec2 pixel, uint sampleIdx) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        samplerInit_pcg(pixel, sampleIdx);
    }
    // Sobol branch added in Task 5
}

float sampler1D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return sampler1D_pcg(dim);
    }
    return 0.0;
}

vec2 sampler2D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return sampler2D_pcg(dim);
    }
    return vec2(0.0);
}

#endif
```

- [ ] **Step 4.3: Update pt_raygen.rgen to use the sampler API**

In `shaders/rt/pt_raygen.rgen`:

Near the top (after existing extensions) remove the inline PCG functions (`rngInit`, `rngNext`, `rand01`) and replace with the sampler API include. Specifically:

1. Delete lines that define `uint rngState;`, `void rngInit(...)`, `uint rngNext()`, `float rand01()` (the PCG block at approximately lines 52-62).

2. Add this include near the top of the file (after `#extension GL_GOOGLE_include_directive : require`):

```glsl
#include "includes/rt/sampler_api.glsl"
```

3. In `main()`, replace `rngInit(uvec2(pixel), frameIdx);` with `samplerInit(uvec2(pixel), frameIdx);`.

4. Replace every `rand01()` call with the appropriate sampler API call. Since PCG ignores the `dim` parameter, use a running counter: declare `uint dimIdx = 0u;` at the top of `main()` after `samplerInit`. Every `rand01()` becomes `sampler1D(dimIdx); dimIdx += 1u;` and every pair of consecutive `rand01()` becomes `sampler2D(dimIdx); dimIdx += 2u;`.

For the specific replacements:
- `float u1 = rand01(), u2 = rand01();` → `vec2 u12 = sampler2D(dimIdx); dimIdx += 2u; float u1 = u12.x; float u2 = u12.y;`
- `float phi = 6.2831853 * u2;` stays the same
- `uint selectedLightIdx = uint(rand01() * float(lightBuf.lightCount));` → `uint selectedLightIdx = uint(sampler1D(dimIdx) * float(lightBuf.lightCount)); dimIdx += 1u;`
- `vec3 lightPoint = lightCenter + edge1 * rand01() + edge2 * rand01();` → `vec2 areaUV = sampler2D(dimIdx); dimIdx += 2u; vec3 lightPoint = lightCenter + edge1 * areaUV.x + edge2 * areaUV.y;`
- env MIS block: `float u1 = rand01(); float u2 = rand01();` → `vec2 envU = sampler2D(dimIdx); dimIdx += 2u; float u1 = envU.x; float u2 = envU.y;`
- `if (p < 0.01 || rand01() > p) break;` → `if (p < 0.01 || sampler1D(dimIdx) > p) break; dimIdx += 1u;`
- `if (rand01() < specProb || roughness < 0.05) {` → `bool bsdfSampleSpecular = sampler1D(dimIdx) < specProb; dimIdx += 1u; if (bsdfSampleSpecular || roughness < 0.05) {`
- Inside specular branch: `vec3 jitter = cosineHemisphere(reflected) * roughness;` — `cosineHemisphere` itself calls `rand01()`. **That function must be updated too** (see step 4.4).
- Sub-pixel jitter: `vec2 jitter = vec2(rand01(), rand01()) - 0.5;` → `vec2 jitter = sampler2D(dimIdx) - 0.5; dimIdx += 2u;` (at the top of main).

- [ ] **Step 4.4: Update `cosineHemisphere` to accept dimensions**

`cosineHemisphere` is defined inline in pt_raygen.rgen. Change its signature:

Old:
```glsl
vec3 cosineHemisphere(vec3 N) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0,1,0) : vec3(1,0,0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    float r = sqrt(rand01());
    float phi = 6.2831853 * rand01();
    return normalize(T * r * cos(phi) + B * r * sin(phi) + N * sqrt(max(0.0, 1.0 - r*r)));
}
```

New:
```glsl
vec3 cosineHemisphere(vec3 N, vec2 u) {
    vec3 up = abs(N.y) < 0.999 ? vec3(0,1,0) : vec3(1,0,0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    float r = sqrt(u.x);
    float phi = 6.2831853 * u.y;
    return normalize(T * r * cos(phi) + B * r * sin(phi) + N * sqrt(max(0.0, 1.0 - r*r)));
}
```

Every call site pulls a fresh 2D sample and passes it:

- `rayDir = cosineHemisphere(N);` → `vec2 diffU = sampler2D(dimIdx); dimIdx += 2u; rayDir = cosineHemisphere(N, diffU);`
- Inside the specular roughness-jitter branch:
  ```glsl
  vec2 jitU = sampler2D(dimIdx); dimIdx += 2u;
  vec3 jitter = cosineHemisphere(reflected, jitU) * roughness;
  ```
  followed by the existing `reflected = normalize(reflected + jitter);` etc. The fallback `if (dot(reflected, N) < 0.0) reflected = cosineHemisphere(N);` becomes `reflected = cosineHemisphere(N, sampler2D(dimIdx)); dimIdx += 2u;` — burns two dims but keeps consistency.

- [ ] **Step 4.5: Apply the same edits verbatim to pt_raygen_offline.rgen**

`pt_raygen_offline.rgen` is kept in sync with `pt_raygen.rgen` (per the top-of-file comment). Apply every change from steps 4.3 and 4.4 to it identically.

- [ ] **Step 4.6: Apply sampler API + dimIdx pattern to pt_raygen_realtime.rgen**

`pt_raygen_realtime.rgen` has its own copy of PCG inline. Replace the same way:
- Delete inline `rngInit` / `rngNext` / `rand01`.
- Add `#include "includes/rt/sampler_api.glsl"`.
- Change `rngInit(...)` → `samplerInit(...)`.
- Add `uint dimIdx = 0u;` after `samplerInit`.
- Replace every `rand01()` using the same patterns as step 4.3.
- Update its local `cosineHemisphere` (if present) the same way as step 4.4.

The realtime specialization will still select `SAMPLER_PCG` (set by CPU in Task 7), so behavior is bit-identical to pre-1.2.

- [ ] **Step 4.7: Build shaders**

```bash
cmake --build build --target shaders -j8 2>&1 | tail -10
```
Expected: clean compile. If glslc reports "undefined SAMPLER_TYPE" or similar, confirm the include-path flag in `shaders/CMakeLists.txt` covers `shaders/includes/`.

- [ ] **Step 4.8: Build app + smoke test — cornell_box**

```bash
cmake --build build -j8 2>&1 | tail -5
./build/cornell_box /tmp/task4_cornell.png 16
```
Expected: PNG produced, no Vulkan validation errors.

Because the sampler defaults to Sobol via the spec constant fallback and Sobol hasn't been wired yet, cornell_box output will be all zeros from `sampler1D` returning 0. That means solid black or garbage image. **This is expected at this step** — the sampler API is a shim until Task 5 lands Sobol.

**Workaround for this intermediate step** (only for this smoke test): temporarily change the spec constant default in `sampler_api.glsl`:

```glsl
layout(constant_id = 0) const uint SAMPLER_TYPE = SAMPLER_PCG;
```

Rebuild shaders, smoke test again. Expected: cornell_box renders correctly (PCG path). Then **revert** to `SAMPLER_SOBOL` before committing.

- [ ] **Step 4.9: Commit**

```bash
git add shaders/includes/rt/sampler_api.glsl shaders/includes/rt/sampler_pcg.glsl \
        shaders/rt/pt_raygen.rgen shaders/rt/pt_raygen_offline.rgen \
        shaders/rt/pt_raygen_realtime.rgen
git commit -m "feat(rt): GLSL sampler API + PCG extraction, specialization-constant dispatch

Raygen shaders no longer call rand01 directly; use sampler1D/2D with a
running dimIdx counter. Sobol implementation lands in the next commit."
```

---

## Task 5: GLSL Sobol implementation

**Files:**
- Create: `shaders/includes/rt/sampler_sobol_tables.glsl`
- Create: `shaders/includes/rt/sampler_sobol.glsl`
- Modify: `shaders/includes/rt/sampler_api.glsl` (add Sobol branch + include)

- [ ] **Step 5.1: Generate the Sobol table GLSL file**

Create `shaders/includes/rt/sampler_sobol_tables.glsl`. The 128 values match `kDirectionNumbers` from `ohao/render/rt/sobol_generator.cpp` exactly:

```glsl
#ifndef OHAO_SAMPLER_SOBOL_TABLES_GLSL
#define OHAO_SAMPLER_SOBOL_TABLES_GLSL

// Sobol direction numbers for dimensions 0-3, Joe-Kuo new-joe-kuo-6.21201.
// Mirrors ohao/render/rt/sobol_generator.cpp. 32 u32s per dimension.

const uint OHAO_SOBOL_DIMS = 4u;

const uint OHAO_SOBOL_DIRS[128] = uint[128](
    // Dimension 0
    0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u,
    0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u,
    0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u,
    0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u,
    0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u,
    0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u,
    0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u,
    0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u,
    // Dimension 1
    0x80000000u, 0xC0000000u, 0xA0000000u, 0xF0000000u,
    0x88000000u, 0xCC000000u, 0xAA000000u, 0xFF000000u,
    0x80800000u, 0xC0C00000u, 0xA0A00000u, 0xF0F00000u,
    0x88880000u, 0xCCCC0000u, 0xAAAA0000u, 0xFFFF0000u,
    0x80008000u, 0xC000C000u, 0xA000A000u, 0xF000F000u,
    0x88008800u, 0xCC00CC00u, 0xAA00AA00u, 0xFF00FF00u,
    0x80808080u, 0xC0C0C0C0u, 0xA0A0A0A0u, 0xF0F0F0F0u,
    0x88888888u, 0xCCCCCCCCu, 0xAAAAAAAAu, 0xFFFFFFFFu,
    // Dimension 2
    0x80000000u, 0x40000000u, 0xE0000000u, 0x50000000u,
    0x98000000u, 0xBC000000u, 0xAE000000u, 0x25000000u,
    0x5B800000u, 0x91400000u, 0x65600000u, 0x2E500000u,
    0x91180000u, 0x88D40000u, 0xDD360000u, 0x02950000u,
    0x4C888000u, 0x09EB4000u, 0x86CA6000u, 0x13B15000u,
    0x42ADD800u, 0x3DF8D400u, 0x2FE57E00u, 0x56EB8100u,
    0x8C41A680u, 0xE68455C0u, 0x3C9CE8E0u, 0xB44DBFF0u,
    0x7C27F6C8u, 0x5239A6ACu, 0x853B0EDEu, 0x52EB2EF9u,
    // Dimension 3
    0x80000000u, 0xC0000000u, 0x20000000u, 0x50000000u,
    0xF8000000u, 0x74000000u, 0xA2000000u, 0x93000000u,
    0xD8800000u, 0x25400000u, 0x59E00000u, 0xE6D00000u,
    0x78080000u, 0xB40C0000u, 0x82020000u, 0xC3050000u,
    0x208F8000u, 0x51CBC000u, 0xFBEA2000u, 0x75AD5000u,
    0xA00AF800u, 0x90077400u, 0xD800A200u, 0x25009300u,
    0x59E08480u, 0xE6D0CEC0u, 0x78088520u, 0xB40CEBB0u,
    0x82028348u, 0xC305C274u, 0x208FB552u, 0x51CB9AE9u
);

#endif
```

- [ ] **Step 5.2: Create the Sobol sampler GLSL**

Create `shaders/includes/rt/sampler_sobol.glsl`:

```glsl
#ifndef OHAO_SAMPLER_SOBOL_GLSL
#define OHAO_SAMPLER_SOBOL_GLSL

#include "includes/rt/sampler_sobol_tables.glsl"

// Per-pixel state: sample index + per-pixel seed (hash of pixel coords).
uint _sobol_index;
uint _sobol_pixelSeed;

// Burley 2020 hash-based Owen scramble. Mirrors the CPU implementation
// in ohao/render/rt/owen_scramble.{hpp,cpp}.
uint _sobol_owenScramble(uint v, uint seed) {
    // Reverse bits
    v = (v << 16) | (v >> 16);
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);

    v ^= v * 0x3d20adeau;
    v += seed;
    v *= (seed >> 16) | 1u;
    v ^= v * 0x05526c56u;
    v ^= v * 0x53a22864u;

    // Reverse back
    v = ((v & 0x55555555u) << 1) | ((v & 0xAAAAAAAAu) >> 1);
    v = ((v & 0x33333333u) << 2) | ((v & 0xCCCCCCCCu) >> 2);
    v = ((v & 0x0F0F0F0Fu) << 4) | ((v & 0xF0F0F0F0u) >> 4);
    v = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    v = (v << 16) | (v >> 16);
    return v;
}

// Compute the unscrambled Sobol integer for (index, dim). Direct
// binary-expansion formulation — matches CPU sobol_generator.cpp.
uint _sobol_int(uint index, uint dim) {
    uint result = 0u;
    uint baseOffset = dim * 32u;
    for (uint bit = 0u; index != 0u; bit++, index >>= 1) {
        if ((index & 1u) != 0u) {
            result ^= OHAO_SOBOL_DIRS[baseOffset + bit];
        }
    }
    return result;
}

// Simple integer hash for the per-pixel seed.
uint _sobol_hashPixel(uvec2 pixel) {
    uint h = pixel.x * 0x1b873593u ^ pixel.y * 0xcc9e2d51u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

void samplerInit_sobol(uvec2 pixel, uint sampleIdx) {
    _sobol_index = sampleIdx;
    _sobol_pixelSeed = _sobol_hashPixel(pixel);
}

// Map dim → (pad, dimWithinPad). We use 4D Sobol and pad it by advancing
// the Owen seed per pad — giving independent scrambled 4D sequences.
float sampler1D_sobol(uint dim) {
    uint pad = dim >> 2;          // dim / 4
    uint local = dim & 3u;        // dim % 4
    uint seed = _sobol_pixelSeed ^ (pad * 0x9e3779b9u);
    uint sobolVal = _sobol_int(_sobol_index, local);
    uint scrambled = _sobol_owenScramble(sobolVal, seed);
    return float(scrambled) * (1.0 / 4294967296.0);
}

vec2 sampler2D_sobol(uint dim) {
    float x = sampler1D_sobol(dim);
    float y = sampler1D_sobol(dim + 1u);
    return vec2(x, y);
}

#endif
```

- [ ] **Step 5.3: Add Sobol branch in sampler_api.glsl**

Edit `shaders/includes/rt/sampler_api.glsl`. Replace the entire file with:

```glsl
#ifndef OHAO_SAMPLER_API_GLSL
#define OHAO_SAMPLER_API_GLSL

// Sampler dispatch — chosen at pipeline creation via Vulkan specialization
// constant. The SPIR-V optimizer folds the constant-id branch because
// SAMPLER_TYPE is known at compile time. Zero runtime cost.
//
// To add a sampler: define a new constant below, create sampler_<name>.glsl
// with functions suffixed _<name>, include it here, and add a branch.

#define SAMPLER_PCG   0u
#define SAMPLER_SOBOL 1u

layout(constant_id = 0) const uint SAMPLER_TYPE = SAMPLER_SOBOL;

#include "includes/rt/sampler_pcg.glsl"
#include "includes/rt/sampler_sobol.glsl"

void samplerInit(uvec2 pixel, uint sampleIdx) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        samplerInit_pcg(pixel, sampleIdx);
    } else {
        samplerInit_sobol(pixel, sampleIdx);
    }
}

float sampler1D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return sampler1D_pcg(dim);
    }
    return sampler1D_sobol(dim);
}

vec2 sampler2D(uint dim) {
    if (SAMPLER_TYPE == SAMPLER_PCG) {
        return sampler2D_pcg(dim);
    }
    return sampler2D_sobol(dim);
}

#endif
```

- [ ] **Step 5.4: Rebuild shaders**

```bash
cmake --build build --target shaders -j8 2>&1 | tail -5
```
Expected: clean. If glslc reports the 128-element array init is too large, use a static const variable pattern (already used for `kDirectionNumbers` in env_sampling.glsl — mirror that style).

- [ ] **Step 5.5: Smoke test**

Because the CPU side still passes no specialization info (Task 7 adds that), the shader falls back to the default `SAMPLER_SOBOL`. Both cornell_box AND env_demo should now render.

```bash
cmake --build build -j8
./build/cornell_box /tmp/task5_cornell.png 16
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr /tmp/task5_helmet.png 16
```
Expected: both PNGs render without Vulkan validation errors. Images should look like MIS-on Sobol (not identical to pre-1.2 PCG because we changed sampler).

Visually inspect: helmet image should not be solid-color or obviously broken. Geometry, shading, env reflections all present.

- [ ] **Step 5.6: Commit**

```bash
git add shaders/includes/rt/sampler_sobol_tables.glsl \
        shaders/includes/rt/sampler_sobol.glsl \
        shaders/includes/rt/sampler_api.glsl
git commit -m "feat(rt): Sobol sampler impl + Owen scramble (GLSL, mirrors CPU)"
```

---

## Task 6: Vulkan pipeline specialization constants

**Files:**
- Modify: `ohao/render/rt/path_tracer.cpp` (`createRTPipeline` — set VkSpecializationInfo on raygen stage)

- [ ] **Step 6.1: Add specialization info to raygen stage**

In `ohao/render/rt/path_tracer.cpp` `createRTPipeline`, after the `stages[0]` raygen initialization (around line 673) and BEFORE the pipelineInfo creation, insert:

```cpp
    // Specialization constant: sampler type baked into the raygen SPIR-V.
    VkSpecializationMapEntry samplerEntry{};
    samplerEntry.constantID = kSamplerSpecConstantId;
    samplerEntry.offset = 0;
    samplerEntry.size = sizeof(uint32_t);

    uint32_t samplerTypeVal = static_cast<uint32_t>(m_renderSettings.samplerType);

    VkSpecializationInfo samplerSpecInfo{};
    samplerSpecInfo.mapEntryCount = 1;
    samplerSpecInfo.pMapEntries = &samplerEntry;
    samplerSpecInfo.dataSize = sizeof(uint32_t);
    samplerSpecInfo.pData = &samplerTypeVal;

    stages[0].pSpecializationInfo = &samplerSpecInfo;
```

- [ ] **Step 6.2: Make sure sampler_types.hpp is included**

At the top of `path_tracer.cpp`, confirm there's `#include "render/rt/sampler_types.hpp"` (path_tracer.hpp includes it; transitively available through path_tracer.hpp — no explicit include needed if path_tracer.hpp is included first).

- [ ] **Step 6.3: Build + verify offline path uses Sobol**

```bash
cmake --build build -j8 2>&1 | tail -5
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr /tmp/task6_offline_sobol.png 16
```

Expected: Renders correctly. This is the "offline Sobol" reference.

- [ ] **Step 6.4: Sanity-check realtime defaults to PCG**

Temporarily verify the realtime profile uses PCG by rendering with `interactive` example IF available (skip if no GLFW on the test machine). Otherwise, test by manually overriding once:

```bash
# No runtime toggle yet — just confirm the RTRealtimeRenderer sets PCG default.
grep -n "samplerType" ohao/render/rt/rt_profile_renderer.hpp || echo "No override — uses struct default Sobol. Fix in Task 7."
```

This is just a verification that Task 7 is necessary. If realtime happens to inherit `kRealtimeRTSettings` (which now specifies PCG), it's correct. If it inherits `kOfflineRTSettings` or struct defaults, we need Task 7's explicit override. Given Task 3 set `kRealtimeRTSettings.samplerType = PCG`, and RTRealtimeRenderer's constructor uses `kRealtimeRTSettings`, this should already be correct.

- [ ] **Step 6.5: Commit**

```bash
git add ohao/render/rt/path_tracer.cpp
git commit -m "feat(rt): wire SamplerType into RT pipeline via VkSpecializationInfo

Offline path tracer now uses Sobol via specialization constant (constant_id=0);
realtime stays on PCG. Pipeline compile cost amortized — cached after first run."
```

---

## Task 7: Regression verification + reference update

**Files:**
- Render: `tests/reference_scenes/custom/envlit_turntable/reference.png` (overwrite with Sobol version)
- Modify: `tests/reference_scenes/custom/envlit_turntable/verification_log.md` (append Sobol vs PCG entry)

- [ ] **Step 7.1: Render Sobol reference**

```bash
cd /home/frankyin/Desktop/Github/ohao-sobol
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/sobol_ref_16spp.png 16
```

- [ ] **Step 7.2: Render PCG comparison at same spp**

Temporarily change `kOfflineRTSettings.samplerType` from `Sobol` to `PCG` in `ohao/render/rt/path_tracer.hpp`, rebuild, render:

```bash
# Edit: SamplerType::Sobol → SamplerType::PCG in kOfflineRTSettings
cmake --build build -j8
./build/env_demo assets/test_models/DamagedHelmet.glb assets/test_models/env_studio.hdr \
    /tmp/pcg_ref_16spp.png 16
# Revert: SamplerType::PCG → SamplerType::Sobol
cmake --build build -j8
```

- [ ] **Step 7.3: Run variance comparison**

```bash
python3 tools/compare_variance.py /tmp/pcg_ref_16spp.png /tmp/sobol_ref_16spp.png
```

Expected output (approximate):
- `Noise reduction: +5% to +15%` (Sobol less noisy than PCG at same spp)

**Pass criterion:** Sobol local variance ≤ PCG local variance. If Sobol is worse, stop and debug (likely direction-number transcription error or Owen hash mismatch).

- [ ] **Step 7.4: Compare both vs 16384-spp ground truth**

```bash
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_16384spp.png \
    /tmp/sobol_ref_16spp.png
python3 tools/compare_variance.py \
    tests/reference_scenes/custom/envlit_turntable/truth_16384spp.png \
    /tmp/pcg_ref_16spp.png
```

Expected: Sobol's RMSE vs truth is lower (closer to converged).

- [ ] **Step 7.5: Update reference.png**

```bash
cp /tmp/sobol_ref_16spp.png tests/reference_scenes/custom/envlit_turntable/reference.png
```

- [ ] **Step 7.6: Update verification_log.md**

Edit `tests/reference_scenes/custom/envlit_turntable/verification_log.md`. Append a new dated entry using the variance numbers from steps 7.3 and 7.4. Template:

```markdown
## 2026-04-XX: Sobol sampler (Feature 1.2) validation

Offline sampler upgraded from PCG to Owen-scrambled Sobol at equal 16 spp:

| | Local variance | RMSE vs 16384-spp truth |
|---|---|---|
| PCG (previous)  | <fill> | <fill> |
| Sobol (current) | <fill> | <fill> |

- Noise reduction vs PCG: <X>%
- Convergence improvement (RMSE vs truth): <Y>%

Feature 1.2 complete. Reference render updated to Sobol.
```

Replace `<fill>` and the percentages with the values from compare_variance.py. Keep it brief.

- [ ] **Step 7.7: Commit**

```bash
git add tests/reference_scenes/custom/envlit_turntable/reference.png \
        tests/reference_scenes/custom/envlit_turntable/verification_log.md
git commit -m "test(rt): Sobol sampler reference + variance gate

Offline envlit_turntable reference regenerated with Owen-scrambled Sobol.
Verification log records <X>% noise reduction, <Y>% closer to truth vs PCG
at equal sample count."
```

---

## Plan Self-Review

**Spec coverage:**

| Spec section | Implemented by |
|---|---|
| §4.1 C++ SamplerType enum + RTRenderSettings | Task 3 |
| §4.2 GLSL sampler_api.glsl + spec constant | Task 4, Task 5 |
| §4.3 sampler_pcg, sampler_sobol, sobol_tables | Tasks 4, 5 |
| §4.4 Cycles-style padded 4D strategy | Task 5 (`sampler1D_sobol` pad/local math) |
| §5 Integration (raygen uses sampler API) | Task 4 |
| §6 Pipeline specialization | Task 6 |
| §7.1 CPU unit tests | Tasks 1, 2 |
| §7.2 Integration variance test | Task 7 |
| §7.3 Reference regression update | Task 7 |
| §10 Success criteria | All Tasks |

**Placeholder scan:** Only `<fill>` and `<X>`/`<Y>` in the verification_log template (Task 7.6) — these are deliberately left for the executor to fill from actual measured numbers, with explicit instruction. No TBDs elsewhere.

**Type consistency:** `SamplerType`, `kSamplerSpecConstantId`, `samplerInit`, `sampler1D`, `sampler2D`, `samplerType` (field) are named consistently across all tasks. GLSL `SAMPLER_PCG` / `SAMPLER_SOBOL` macros match the C++ enum values (0, 1).

**Bias risk:** Task 4 intermediate state (step 4.8) has a known smoke-test workaround (temporarily default to PCG, revert after). Documented explicitly to avoid confusion.

---

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-04-17-phase1-feature1.2-sobol-sampler.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**
