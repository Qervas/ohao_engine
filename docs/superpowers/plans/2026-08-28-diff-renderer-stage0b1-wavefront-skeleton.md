# Differentiable Renderer — Stage 0b-1: Wavefront Skeleton Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the wavefront machinery a differentiable path tracer needs — SoA path state, queues with atomic counters, indirect per-bounce dispatch, and compaction — and prove it correct against closed-form answers before any light transport exists.

**Architecture:** Path state lives in structure-of-arrays buffers. Each bounce is a separate dispatch reading a queue of live path indices and writing the next. Dead paths are compacted out, and the next dispatch is sized from an atomic counter via `vkCmdDispatchIndirect`, so they cost nothing. Nothing survives in registers across a dispatch boundary; every stage re-derives its RNG from `(pixel, sampleIndex, bounce)`.

**Tech Stack:** C++20, Vulkan 1.3 (`VK_KHR_ray_query`, `VK_EXT_shader_atomic_float`), GLSL via `glslc --target-env=vulkan1.3`, VMA through `GpuAllocator`, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md` (see §3.1 for why wavefront)

## Global Constraints

- **C++20.** `target_compile_features(... cxx_std_20)`.
- **GLSL, not Slang.** Hand-written adjoints later; no new shader toolchain.
- **Gradients are buffer-backed.** `shaderBufferFloat32AtomicAdd` only; never `shaderImageFloat32AtomicAdd`.
- **Seed invariant.** A path is a pure function of `(pixel, sampleIndex, iterationSeed)`. **Under wavefront this hardens: nothing may rely on registers surviving a dispatch boundary.** Every stage reconstructs its RNG from the tuple.
- **Ray query, not RT pipeline.** Inline tracing in compute.
- **`ohao/diff/` owns its barriers.** Do NOT route through `RenderGraph` — its only consumer reports 29 synchronization hazards (spec §3.1).
- **Shader naming.** `shaders/diff/x.comp` → `bin/shaders/diff_x.comp.spv`.
- **Commit style.** Conventional commits with scope, e.g. `feat(diff): ...`.

**Before starting:**

```bash
git checkout -b feat/diff-stage0b1
```

**Build commands.** The `cmake` on PATH is a conda build that cannot drive the cached `Visual Studio 18 2026` generator. Use Scoop's, never pass `-G`, and re-configure before building because `ohao/diff/CMakeLists.txt` globs its sources:

```bash
/c/Users/djmax/scoop/apps/cmake/4.4.2/bin/cmake.exe -B build -S .
/c/Users/djmax/scoop/apps/cmake/4.4.2/bin/cmake.exe --build build --config Release -j8 --target shaders diff_gpu_probe diff_unit_tests
./build/Release/diff_unit_tests.exe
./build/Release/diff_gpu_probe.exe
```

**Existing state you build on (Stage 0a, all committed):**

- `ohao::diff::ArenaLayout` — `add(floatCount)` → block index or `kInvalidBlock`; `block(i)` → `{offsetBytes, sizeBytes}`, **`sizeBytes == 0` means invalid**; `blockCount()`, `totalBytes()`, `kAlignmentBytes = 256`.
- `ohao::diff::GradientArena` — `build(GpuAllocator&, const ArenaLayout&)`, `zero(VkCommandBuffer)` (records a fill **and** a TRANSFER→COMPUTE barrier), `readback(GpuAllocator&, blockIndex)`, `destroy(GpuAllocator&)`, destructor releases, copy/move deleted.
- `ohao::diff::PathRng` — `forPath(pixel, sample, seed)`, `next1D()`, `drawCount()`. GLSL mirror `shaders/includes/diff/rng.glsl` proven bit-exact by `diff_gpu_probe` check 6.
- `ohao::diff::GpuProbeContext` — headless device with validation layers + debug messenger, `runImmediate`, `dispatchStorageBufferCompute(spvName, buffer, pushData, pushSize, groupCountX)`, `runAtomicProbe`, `runVisibilityProbe`, `runRngParityProbe`.
- `RTAccelerationStructure::init(device, physicalDevice, queue, queueFamily, commandPool, asConsumerStages)` — pass `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` from the probe.

---

## File Structure

| File | Responsibility |
|---|---|
| `ohao/diff/wavefront/path_state_layout.hpp` / `.cpp` | Pure SoA offset arithmetic — no Vulkan |
| `ohao/diff/wavefront/wavefront_buffers.hpp` / `.cpp` | GPU-side path state + queues + counters |
| `shaders/includes/diff/path_state.glsl` | Shared SoA accessors — the single source of truth for layout |
| `shaders/diff/wf_generate.comp` | Seed path state, fill bounce-0 queue |
| `shaders/diff/wf_intersect.comp` | Ray query traversal, hit records, alive compaction |
| `shaders/diff/wf_scatter.comp` | Constant-albedo scatter, next-bounce queue |
| `tests/diff/diff_unit_tests.cpp` | CPU tests for the layout |
| `tests/diff/gpu_probe_context.{hpp,cpp}` | Wavefront dispatch driver + sync validation |
| `tests/diff/diff_gpu_probe.cpp` | Checks 7–10 |

---

### Task 1: Path state SoA layout

Pure arithmetic, testable in microseconds. **SoA, not AoS**: a wavefront stage touching only `throughput` should not pull a whole `PathState` cache line per lane.

**Files:**
- Create: `ohao/diff/wavefront/path_state_layout.hpp`, `.cpp`
- Test: `tests/diff/diff_unit_tests.cpp` (append; includes go in the top block)

**Interfaces:**
- Consumes: `ohao::diff::ArenaLayout`.
- Produces: `ohao::diff::PathStateField` (enum), `ohao::diff::PathStateLayout` with `explicit PathStateLayout(std::uint32_t capacity)`, `std::size_t block(PathStateField) const`, `std::uint32_t capacity() const noexcept`, `const ArenaLayout& arena() const noexcept`, `static constexpr std::uint32_t kFieldCount`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(DiffPathStateLayout, EachFieldGetsItsOwnBlockSizedByComponentCount) {
    // SoA: one contiguous block per field, so a stage touching only throughput
    // reads a dense run rather than striding over whole path structs.
    constexpr std::uint32_t kCapacity = 1024;
    ohao::diff::PathStateLayout layout(kCapacity);

    using F = ohao::diff::PathStateField;
    const F fields[] = {F::OriginX, F::OriginY, F::OriginZ,
                        F::DirX, F::DirY, F::DirZ,
                        F::ThroughputR, F::ThroughputG, F::ThroughputB,
                        F::RadianceR, F::RadianceG, F::RadianceB,
                        F::PixelIndex, F::SampleIndex, F::Bounce, F::Alive};

    std::set<std::size_t> seen;
    for (F f : fields) {
        const std::size_t b = layout.block(f);
        ASSERT_NE(b, ohao::diff::ArenaLayout::kInvalidBlock);
        EXPECT_TRUE(seen.insert(b).second) << "two fields share a block";
        EXPECT_EQ(layout.arena().block(b).sizeBytes, kCapacity * sizeof(float));
    }
    EXPECT_EQ(layout.capacity(), kCapacity);
}

TEST(DiffPathStateLayout, BlocksDoNotOverlap) {
    ohao::diff::PathStateLayout layout(256);
    const ohao::diff::ArenaLayout& a = layout.arena();
    for (std::size_t i = 1; i < a.blockCount(); ++i) {
        const auto prev = a.block(i - 1);
        const auto cur = a.block(i);
        EXPECT_GE(cur.offsetBytes, prev.offsetBytes + prev.sizeBytes);
    }
}

TEST(DiffPathStateLayout, ZeroCapacityIsRejected) {
    ohao::diff::PathStateLayout layout(0);
    EXPECT_EQ(layout.capacity(), 0u);
    EXPECT_EQ(layout.arena().blockCount(), 0u);
    EXPECT_EQ(layout.block(ohao::diff::PathStateField::OriginX),
              ohao::diff::ArenaLayout::kInvalidBlock);
}
```

Add `#include <set>` and `#include "diff/wavefront/path_state_layout.hpp"` to the top include block.

- [ ] **Step 2: Run to verify it fails**

`cmake --build build --config Release -j8 --target diff_unit_tests` → FAIL, header not found.

- [ ] **Step 3: Write the header**

```cpp
#pragma once

#include "diff/grad/arena_layout.hpp"

#include <cstdint>

namespace ohao::diff {

/// One scalar field per enumerator. Deliberately scalar rather than vec3:
/// a wavefront stage that only needs throughput should read one dense run,
/// not stride across interleaved path structs.
enum class PathStateField : std::uint32_t {
    OriginX, OriginY, OriginZ,
    DirX, DirY, DirZ,
    ThroughputR, ThroughputG, ThroughputB,
    RadianceR, RadianceG, RadianceB,
    PixelIndex,    // bit-cast uint
    SampleIndex,   // bit-cast uint
    Bounce,        // bit-cast uint
    Alive,         // bit-cast uint, 0 or 1
    Count
};

/// SoA offsets for `capacity` in-flight paths. Pure -- no Vulkan.
///
/// shaders/includes/diff/path_state.glsl mirrors this layout. The two are a
/// matched pair like PathRng and rng.glsl: change both or neither, and the
/// probe's field round-trip check is what proves they agree.
class PathStateLayout {
public:
    explicit PathStateLayout(std::uint32_t capacity);

    [[nodiscard]] std::size_t block(PathStateField field) const;
    [[nodiscard]] std::uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] const ArenaLayout& arena() const noexcept { return m_arena; }

    static constexpr std::uint32_t kFieldCount =
        static_cast<std::uint32_t>(PathStateField::Count);

private:
    std::uint32_t m_capacity{0};
    ArenaLayout m_arena;
    std::size_t m_blocks[kFieldCount];
};

}  // namespace ohao::diff
```

- [ ] **Step 4: Write the implementation**

```cpp
#include "diff/wavefront/path_state_layout.hpp"

namespace ohao::diff {

PathStateLayout::PathStateLayout(std::uint32_t capacity) {
    for (std::uint32_t i = 0; i < kFieldCount; ++i) {
        m_blocks[i] = ArenaLayout::kInvalidBlock;
    }
    if (capacity == 0) return;

    m_capacity = capacity;
    for (std::uint32_t i = 0; i < kFieldCount; ++i) {
        m_blocks[i] = m_arena.add(capacity);
    }
}

std::size_t PathStateLayout::block(PathStateField field) const {
    const auto i = static_cast<std::uint32_t>(field);
    if (i >= kFieldCount) return ArenaLayout::kInvalidBlock;
    return m_blocks[i];
}

}  // namespace ohao::diff
```

- [ ] **Step 5: Run tests**

Re-configure (GLOB_RECURSE), build, then:
`./build/Release/diff_unit_tests.exe --gtest_filter=DiffPathStateLayout.*` → 3 PASS.

- [ ] **Step 6: Commit**

```bash
git add ohao/diff/wavefront/path_state_layout.hpp ohao/diff/wavefront/path_state_layout.cpp tests/diff/diff_unit_tests.cpp
git commit -m "feat(diff): SoA path state layout for the wavefront integrator"
```

---

### Task 2: Enable synchronization validation in the probe

Before any multi-dispatch machinery exists. A wavefront integrator has far more barriers than a single kernel — between every bounce stage, around every compaction, on every path-state buffer — and hand-written barriers are exactly where the `buildTLAS` bug came from. Sync validation must be on *before* the barriers are written, not after.

**Files:** Modify `tests/diff/gpu_probe_context.cpp`

- [ ] **Step 1: Enable the feature**

In `init()`, where the validation layer is enabled, also request synchronization validation via `VkValidationFeaturesEXT` chained into `VkInstanceCreateInfo::pNext`:

```cpp
    // Synchronization validation finds read-after-write and write-after-write
    // hazards that plain validation does not. Enabled here deliberately before
    // the wavefront stages exist: this subsystem hand-writes its barriers, and
    // the engine's RenderGraph -- the alternative -- reports 29 such hazards.
    const VkValidationFeatureEnableEXT enabledFeatures[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = enabledFeatures;
```

Chain it only when the validation layer is actually available; leave `pNext` untouched otherwise.

`VkValidationFeaturesEXT` requires the **`VK_EXT_validation_features`** instance
extension. Add it alongside `VK_EXT_DEBUG_UTILS_EXTENSION_NAME` in the same
availability check — chaining the struct without the extension is invalid usage
and the layer may silently ignore it, which would leave you believing sync
validation is on when it is not.

- [ ] **Step 2: Verify the existing six checks stay clean**

```bash
./build/Release/diff_gpu_probe.exe
```
Expected: exit 0, six OK lines, **0 validation errors**. If sync validation now reports hazards in the *existing* checks, stop and report them — that is a real finding about Stage 0a's barriers, not something to work around.

- [ ] **Step 3: Commit**

```bash
git add tests/diff/gpu_probe_context.cpp
git commit -m "test(diff): enable synchronization validation in the GPU probe"
```

---

### Task 3: GPU wavefront buffers and queues

**Files:**
- Create: `ohao/diff/wavefront/wavefront_buffers.hpp`, `.cpp`
- Test: probe check 7

**Interfaces:**
- Produces: `ohao::diff::WavefrontBuffers` with `bool build(GpuAllocator&, std::uint32_t capacity)`, `void destroy(GpuAllocator&)`, `void zero(VkCommandBuffer)`, `VkBuffer stateBuffer() const noexcept`, `VkBuffer queueBuffer() const noexcept`, `VkBuffer counterBuffer() const noexcept`, `const PathStateLayout& layout() const noexcept`, `std::vector<float> readbackField(GpuAllocator&, PathStateField)`, `std::uint32_t readbackCounter(GpuAllocator&, std::uint32_t slot)`, plus a destructor and deleted copy/move (mirroring `GradientArena`).

Three separate buffers:
- **state** — the `PathStateLayout` arena
- **queue** — two `capacity`-sized `uint` rings (current bounce, next bounce)
- **counter** — a small `uint` buffer whose first three slots are `{currentCount, nextCount, dispatchIndirectPadding}`, created with `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` so `vkCmdDispatchIndirect` can read it

> **Implementer note:** the indirect dispatch command is `{groupCountX, groupCountY, groupCountZ}`. Store the queue count and derive group count in a tiny `wf_prepare_indirect.comp` (one invocation: `groups = (count + 63) / 64`), rather than making the CPU read the counter back — a readback would serialise every bounce and defeat the design.

- [ ] **Step 1: Write the probe check (fails first)**

Add check 7 to `diff_gpu_probe.cpp`: build with capacity 4096, `zero`, then assert every field reads back as all-zero and both counters are 0.

- [ ] **Step 2: Implement, following `GradientArena`'s shape**

Same ownership discipline: destructor releases, copy/move deleted, `destroy()` idempotent, `zero()` records a fill plus a `TRANSFER_WRITE → SHADER_READ|SHADER_WRITE` barrier over each buffer.

- [ ] **Step 3: Verify and commit**

`./build/Release/diff_gpu_probe.exe` → 7 OK lines, 0 validation errors.

```bash
git commit -m "feat(diff): wavefront path state, queue and counter buffers"
```

---

### Task 4: Generate stage, validated against the existing closed form

**Files:**
- Create: `shaders/includes/diff/path_state.glsl`, `shaders/diff/wf_generate.comp`
- Modify: `tests/diff/gpu_probe_context.{hpp,cpp}`, `tests/diff/diff_gpu_probe.cpp`

`path_state.glsl` is the GLSL mirror of `PathStateLayout` — accessors
`psGetOrigin(i)`, `psSetOrigin(i, v)`, `psGetThroughput(i)` and so on.

**Binding scheme (specify once here; every wavefront stage reuses it):**

- The state arena is one `std::430` storage buffer of `float` at **binding 0**.
  Integer fields (`PixelIndex`, `SampleIndex`, `Bounce`, `Alive`) are accessed
  through `floatBitsToUint` / `uintBitsToFloat`, so one buffer serves both — this
  is why the layout gives every field the same stride.
- Queues are a `uint` storage buffer at **binding 1**; counters at **binding 2**.
- Field base offsets arrive as a push-constant array of `kFieldCount` uints, in
  `PathStateField` enum order, each the block offset **in floats** (not bytes).
  `PathStateLayout` supplies them; the C++ side must divide `offsetBytes` by
  `sizeof(float)` exactly once, and a test should assert that division is exact.

Push-constant blocks are limited to 128 bytes guaranteed; `kFieldCount` is 16, so
16 uints (64 bytes) leaves room for per-stage parameters. If a later stage needs
more, move the offsets to a UBO rather than growing the push block.

`path_state.glsl` and `path_state_layout.hpp` are a matched pair like `PathRng`
and `rng.glsl`; the round-trip check below is what proves they agree.

`wf_generate.comp` seeds one path per pixel: origin at the camera, direction from the same construction `visibility_probe.comp` uses, throughput `(1,1,1)`, radiance `(0,0,0)`, bounce 0, alive 1 — and pushes its index into queue 0, incrementing the counter with `atomicAdd`.

- [ ] **Step 1: Reuse the closed form already proven**

Check 8 asserts the generated directions reproduce the same hit distances check 3 validated: `t = planeDistance * sqrt(1 + dx^2 + dy^2)` at 1e-4. **Do not invent a new camera construction** — `visibility_probe.comp` already has one whose orientation is pinned by the half-quad check. Share it via an include rather than copying, so the two cannot drift.

- [ ] **Step 2: Field round-trip check**

Also assert every `PathStateField` written by the shader reads back on the CPU as the value the shader wrote (throughput exactly `1.0`, bounce exactly `0`, alive exactly `1`, `pixelIndex` equal to the linear index). This is what catches a C++/GLSL layout disagreement — the same failure mode as `rng.glsl`, where the mirror compiled but nothing executed it.

- [ ] **Step 3: Queue population check**

Assert the counter equals the pixel count exactly, and that the queue contains each path index exactly once (sort a readback copy and compare against `iota`). An `atomicAdd` race would show up as duplicates or a short count.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(diff): wavefront generate stage with path state GLSL mirror"
```

---

### Task 5: Intersect stage with compaction and indirect dispatch

**Files:**
- Create: `shaders/diff/wf_intersect.comp`, `shaders/diff/wf_prepare_indirect.comp`
- Modify: probe context and `diff_gpu_probe.cpp`

`wf_intersect.comp` consumes queue N via `rayQueryEXT`, writes hit distance into path state, sets `Alive = 0` on miss, and compacts survivors into queue N+1 with `atomicAdd` on the next counter.

- [ ] **Step 1: Compaction correctness against a known-fraction scene**

Use the existing half-quad (`quadMinY = 0.0`): exactly the rays with `ndcY > 0` hit. With `kW × kH = 64 × 48`, that is exactly `kW * kH / 2 = 1536` survivors. Assert the next counter equals 1536 **exactly**, and that the compacted queue contains precisely the surviving indices — no duplicates, no dead paths.

This is the check that catches a compaction race, and it works because the surviving set is known analytically rather than measured.

- [ ] **Step 2: Indirect dispatch**

`wf_prepare_indirect.comp` converts the counter to a group count. Assert a bounce dispatched indirectly from a counter of 0 executes zero invocations — the property that makes dead paths free. Prove it by having the stage `atomicAdd` a canary and asserting the canary stays 0.

- [ ] **Step 3: Barriers**

Every stage boundary needs a `SHADER_WRITE → SHADER_READ` buffer barrier, and the counter needs `SHADER_WRITE → INDIRECT_COMMAND_READ` before `vkCmdDispatchIndirect`. That second one is easy to miss and is exactly what synchronization validation (Task 2) exists to catch. Expected: 0 hazards.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(diff): wavefront intersect stage with compaction and indirect dispatch"
```

---

### Task 6: Scatter stage, multi-bounce, and the two invariants that matter

**Files:**
- Create: `shaders/diff/wf_scatter.comp`
- Modify: probe context and `diff_gpu_probe.cpp`

`wf_scatter.comp` applies a **constant albedo** ρ — no real BSDF yet; that is Stage 0b-2 — multiplies throughput, samples a new direction from the RNG, increments bounce, and re-queues.

- [ ] **Step 1: Throughput decay — the analytic check**

With constant albedo ρ and every ray hitting (full quad, `quadMinY = -1`), throughput after `k` bounces is exactly `ρ^k`. Use `ρ = 0.5` so every value is exactly representable in float32 and the comparison can be exact rather than tolerance-based.

Run 4 bounces, assert every surviving path's throughput is exactly `0.0625`. This validates that state genuinely survives dispatch boundaries — the core wavefront claim — with no floating-point slack to hide behind.

- [ ] **Step 2: Per-bounce RNG parity — the invariant PRB depends on**

Extend check 6's guarantee across dispatch boundaries. For a chosen path, assert the GPU's RNG draws at bounce `k` equal `PathRng::forPath(pixel, sample, seed)` advanced by the number of draws bounces `0..k-1` consumed.

**This is the highest-value check in the plan.** A megakernel keeps the RNG in a register, so order is trivially preserved; a wavefront design must reconstruct it every stage, and getting that wrong produces a plausible image with silently wrong gradients later. Assert `drawCount()` agreement per bounce, not just the values.

- [ ] **Step 3: Confirm the checks can fail**

As with the RNG parity check in Stage 0a, deliberately perturb and confirm each fires, then revert:
- change ρ to `0.5000001` → throughput check must fail
- advance the GPU RNG one extra draw at one bounce → parity check must fail

Report both failure messages in the task report. A check that cannot fail is not coverage.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(diff): wavefront scatter stage, multi-bounce state and RNG parity"
```

---

## Stage 0b-1 exit criteria

- [ ] `diff_unit_tests` passes, including the new `DiffPathStateLayout` suite.
- [ ] `diff_gpu_probe` exits 0 with all checks and **0 validation errors, including 0 SYNC- hazards**.
- [ ] Throughput after 4 bounces is exactly `ρ^4`, compared without tolerance.
- [ ] Per-bounce RNG draws match `PathRng` exactly, and both perturbation tests were shown to fail.
- [ ] Compaction survivor count matches the analytic 1536 exactly.
- [ ] `renderer_test` still passes — nothing here touches the engine's RT path.

## What Stage 0b-1 deliberately does not do

- **No BSDF.** Scatter applies a constant albedo. Lambert and GGX arrive in 0b-2.
- **No NEE, MIS, or environment sampling.** Those are the bulk of the 1,270-line `pt_raygen.rgen` port.
- **No PathTracer parity.** That is the Stage 0 gate proper and belongs to 0b-2, once there is light transport to compare.
- **No material sorting.** Wavefront's largest win, but pointless with one material. Revisit when the BSDF set grows.
- **No gradients.** Still forward-only. The interior kernel is Stage 1.

## Next plan

**Stage 0b-2 — the integrator and PathTracer parity.** Port BSDF evaluation, next-event estimation, MIS, and environment importance sampling into the scatter stage, reusing `shaders/includes/rt/env_sampling.glsl` (96 lines), `mis.glsl` (18) and the Sobol sampler includes rather than reimplementing them. Gate on matching `PathTracer` output within Monte Carlo noise on a probe-owned scene — **not** through `examples/cornell_box.cpp`. Write that plan once the skeleton is real code, so its steps can reference actual stage signatures instead of predicted ones.
