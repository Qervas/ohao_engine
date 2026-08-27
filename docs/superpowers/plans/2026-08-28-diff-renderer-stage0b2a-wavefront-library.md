# Differentiable Renderer — Stage 0b-2a: Wavefront Execution Library Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the wavefront execution machinery out of the test harness into `ohao/diff/`, and fuse the per-bounce dispatches into a single command buffer — which is where this subsystem writes its first compute→compute barriers.

**Architecture:** A small pipeline/descriptor wrapper removes 52 repeated Vulkan creation calls. A bounce-loop recorder emits `generate → [prepare_indirect → intersect → prepare_indirect → scatter] × N` into one command buffer with explicit barriers between stages. The probe becomes a *client* of that library rather than the place it lives.

**Tech Stack:** C++20, Vulkan 1.3 (`VK_KHR_ray_query`, `VK_EXT_shader_atomic_float`), GLSL via `glslc`, VMA through `GpuAllocator`, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md` (§3.1 wavefront rationale and the measured sync-validation limitation)

**Predecessor:** `docs/superpowers/plans/2026-08-28-diff-renderer-stage0b1-wavefront-skeleton.md`, and its execution ledger at `docs/superpowers/plans/2026-08-28-diff-renderer-stage0b1-EXECUTION-LEDGER.md` — read the ledger's carry-forward section before starting.

## Why this stage exists before the integrator port

Stage 0b-1's final review measured the imbalance: **`ohao/diff/` is 794 lines; `tests/diff/` is 3,400**, containing 52 pipeline/descriptor creation calls spread across four near-identical driver functions. Pipeline creation, descriptor sets, indirect dispatch, barrier recording and ring ping-pong all live in the test harness.

Stage 0b-2b ports ~1,270 lines of integrator (BSDF, NEE, MIS, environment importance sampling) into the scatter stage. If that lands before this, it either duplicates the scaffolding into `ohao/diff/` or grows a 1,270-line integrator inside a test file. Both are worse than doing this first.

## Global Constraints

- **C++20.** `target_compile_features(... cxx_std_20)`.
- **GLSL, not Slang.**
- **Gradients are buffer-backed.** `shaderBufferFloat32AtomicAdd` only.
- **Seed invariant.** A path is a pure function of `(pixel, sampleIndex, iterationSeed)`. Nothing may rely on registers surviving a dispatch boundary — this becomes *more* load-bearing here, since fusing the loop removes the `vkQueueWaitIdle` that has been doing the ordering.
- **`ohao/diff/` owns its barriers.** Do NOT route through `RenderGraph` (spec §3.1).
- **Synchronization validation is measurably blind to this code.** Neutering a real barrier produced a genuine correctness failure and *zero* diagnostics. **A clean `SYNC-` run is not evidence.** Barriers are verified by reading, and by the analytic checks that depend on them.
- **Conventional commits with scope.**

**Build commands** (the `cmake` on PATH is a conda build that cannot drive the cached "Visual Studio 18 2026" generator; never pass `-G`; re-configure because `ohao/diff/CMakeLists.txt` globs):

```bash
/c/Users/djmax/scoop/apps/cmake/4.4.2/bin/cmake.exe -B build -S .
/c/Users/djmax/scoop/apps/cmake/4.4.2/bin/cmake.exe --build build --config Release -j8 --target shaders diff_gpu_probe diff_unit_tests
./build/Release/diff_unit_tests.exe
./build/Release/diff_gpu_probe.exe
```

**Branch:** `git checkout -b feat/diff-stage0b2a`

---

## The safety property this whole stage depends on

Fusing the bounce loop into one command buffer **removes the `vkQueueWaitIdle` that currently orders every stage**. Ordering becomes the barriers' job for the first time. Sync validation will not catch a mistake.

What *will* catch it: Stage 0b-1's two analytic checks, which are already in place and already exact.

- **Throughput after 4 bounces is exactly `0.0625`**, compared with `==` and no tolerance.
- **Per-bounce RNG draws match `PathRng` exactly**, values and `drawCount`.

A missing or wrong barrier corrupts path state between stages, and both assertions fail immediately with no floating-point slack to hide in. Precedent: Stage 0b-1's missing `INDIRECT_COMMAND_READ` barrier was caught instantly by the exactly-1536-survivors check while sync validation reported nothing.

**So the acceptance rule for this stage is: those two checks must still pass, unchanged, after the loop is fused.** Do not weaken them to accommodate the new structure. If they fail, the barriers are wrong.

---

## File Structure

| File | Responsibility |
|---|---|
| `ohao/diff/wavefront/compute_pipeline.hpp` / `.cpp` | RAII wrapper: SPV → module → set layout → pipeline layout → pipeline → descriptor set |
| `ohao/diff/wavefront/wavefront_stage.hpp` / `.cpp` | One stage: its pipeline, its push-constant blob, its group-count source (fixed or indirect) |
| `ohao/diff/wavefront/wavefront_loop.hpp` / `.cpp` | Records the fused bounce loop into one command buffer, including all barriers |
| `tests/diff/gpu_probe_context.{hpp,cpp}` | Shrinks — becomes a client of the library |
| `tests/diff/diff_gpu_probe.cpp` | Checks unchanged in intent; one new fused-loop check |

---

### Task 1: `ComputePipeline` — retire the 52 repeated creation calls

**Files:** Create `ohao/diff/wavefront/compute_pipeline.hpp`, `.cpp`. Test: `tests/diff/diff_gpu_probe.cpp`.

**Interfaces produced:** `ohao::diff::ComputePipeline` with
`bool build(VkDevice, const char* spvName, std::span<const VkDescriptorType> bindings, uint32_t pushConstantSize)`,
`void destroy(VkDevice)`, `VkPipeline pipeline() const noexcept`, `VkPipelineLayout layout() const noexcept`,
`VkDescriptorSet descriptorSet() const noexcept`,
`bool bindBuffers(VkDevice, std::span<const VkBuffer>)`,
`bool bindAccelerationStructure(VkDevice, uint32_t binding, VkAccelerationStructureKHR)`,
a destructor, and deleted copy/move (it holds raw Vulkan handles — the same reason `GradientArena` and `WavefrontBuffers` delete theirs).

- [ ] **Step 1: Write the failing check** — a probe check that builds a `ComputePipeline` for `diff_atomic_probe.comp.spv`, binds one storage buffer, and confirms `pipeline()`, `layout()` and `descriptorSet()` are all non-null; then destroys it and confirms a second `destroy()` is a no-op.
- [ ] **Step 2: Run it, confirm it fails** (header missing).
- [ ] **Step 3: Implement**, lifting the existing sequence from `tests/diff/gpu_probe_context.cpp`'s `dispatchStorageBufferCompute` — that function is the reference; it already handles failure paths by destroying in reverse order. Preserve that discipline: every early return must release what it created.
- [ ] **Step 4: Migrate `dispatchStorageBufferCompute` to use it.** The atomic probe must still report **exactly 4096** contended adds — that is the regression guard for this refactor.
- [ ] **Step 5: Verify** — probe exits 0, all 17 OK lines. Commit.

---

### Task 2: `WavefrontStage` — one dispatchable stage

**Files:** Create `ohao/diff/wavefront/wavefront_stage.hpp`, `.cpp`.

**Interfaces produced:** `ohao::diff::WavefrontStage` holding a `ComputePipeline`, an opaque push-constant blob, and a group-count source that is either `Fixed{uint32_t groups}` or `Indirect{VkBuffer, VkDeviceSize offset}`. Method: `void record(VkCommandBuffer) const` — issues `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`, then either `vkCmdDispatch` or `vkCmdDispatchIndirect`.

**Deliberately not included:** barriers. A stage records *work*; the loop records *ordering*. Keeping them separate is what makes the barrier set reviewable in one place, which matters more than usual because nothing automated checks it.

- [ ] **Step 1** Write a probe check recording a single `Fixed` stage and confirming its effect (reuse the atomic probe's canary).
- [ ] **Step 2** Confirm it fails. **Step 3** Implement. **Step 4** Verify. **Step 5** Commit.

---

### Task 3: `WavefrontLoop` — the fused bounce loop and its barriers

**This is the task the stage exists for. It is also the highest-risk work in the plan.**

**Files:** Create `ohao/diff/wavefront/wavefront_loop.hpp`, `.cpp`.

**Interfaces produced:** `ohao::diff::WavefrontLoop` with
`void setGenerate(WavefrontStage)`, `void setPrepareIndirect(WavefrontStage)`, `void setIntersect(WavefrontStage)`, `void setScatter(WavefrontStage)`,
`void record(VkCommandBuffer, WavefrontBuffers&, uint32_t bounces) const`.

`record` emits, into ONE command buffer:

```
generate
  barrier: SHADER_WRITE -> SHADER_READ         (path state + queue, COMPUTE -> COMPUTE)
  barrier: SHADER_WRITE -> INDIRECT_COMMAND_READ  (counter, COMPUTE -> DRAW_INDIRECT)
for each bounce:
  prepare_indirect
  barrier: SHADER_WRITE -> INDIRECT_COMMAND_READ
  intersect            (indirect)
  barrier: SHADER_WRITE -> SHADER_READ | SHADER_WRITE
  prepare_indirect
  barrier: SHADER_WRITE -> INDIRECT_COMMAND_READ
  scatter              (indirect)
  barrier: SHADER_WRITE -> SHADER_READ | SHADER_WRITE
  (ping-pong the ring bases and counter slots)
```

**Three things to get right, none of which any tool will check for you:**

1. **`dstAccessMask` must include `SHADER_WRITE`, not just `SHADER_READ`,** wherever the next stage does an `atomicAdd` on a value the previous stage wrote. An atomic is a read-modify-**write**; a read-only destination does not order it. Stage 0b-1's `gpu_probe_context.cpp` gets this right for the fill→atomicAdd case and its comment explains why — follow that precedent.
2. **The counter's `INDIRECT_COMMAND_READ` barrier is required before every indirect dispatch.** Omitting it is invalid usage that produces no diagnostic here.
3. **Ping-pong correctness.** Ring bases and counter slots must alternate so bounce *k* reads what bounce *k−1* wrote. An off-by-one reads the previous bounce's survivors and will *look* plausible.

- [ ] **Step 1: Write the acceptance check first.** Extend `diff_gpu_probe.cpp` with a fused-loop check that runs 4 bounces through `WavefrontLoop::record` in a single `runImmediate`, then asserts **the same two properties Stage 0b-1 already proves stage-by-stage**: throughput exactly `0.0625` for every surviving path, and per-bounce RNG draws matching `PathRng` exactly. Reuse the existing assertion code — do not write new, weaker ones.
- [ ] **Step 2: Confirm it fails** (loop not implemented).
- [ ] **Step 3: Implement `record`.**
- [ ] **Step 4: Verify** the two assertions pass with the loop fused. If they fail, a barrier is wrong or the ping-pong is off — do not adjust the assertions.
- [ ] **Step 5: REQUIRED PROOF.** Remove one `SHADER_WRITE -> SHADER_READ|SHADER_WRITE` barrier between intersect and scatter, rebuild, and record what happens. Paste the output. Then restore and confirm clean.
      **Report honestly which of these you observe**, because it determines what guards this code from here on:
      (a) the throughput or RNG assertion fails — the analytic checks cover the fused loop;
      (b) nothing fails — the barrier is not load-bearing in this configuration, which means these checks do **not** cover it and that must be written down;
      (c) a `SYNC-` hazard appears — sync validation covers more than Stage 0b-1 measured, which would be good news worth recording in the spec.
      Any of the three is a valid result. Guessing is not.
- [ ] **Step 6: Commit.**

---

### Task 4: Migrate the probe onto the library

**Files:** Modify `tests/diff/gpu_probe_context.{hpp,cpp}`, `tests/diff/diff_gpu_probe.cpp`.

Replace `runWavefrontGenerateProbe`, `runWavefrontIntersectProbe`, `runWavefrontScatterProbe` and `runWavefrontLayoutProbe`'s hand-rolled pipeline/descriptor/barrier code with `ComputePipeline`, `WavefrontStage` and `WavefrontLoop`.

**The acceptance rule is that every existing check still passes, unchanged.** All 17 OK lines, same expected values, same exactness. A refactor that requires loosening an assertion is a refactor that changed behaviour.

- [ ] **Step 1** Migrate one driver, verify its checks. **Step 2** Migrate the rest. **Step 3** Report the line-count delta for `tests/diff/` and `ohao/diff/` — the point of this stage is that the second grows and the first shrinks. **Step 4** Commit.

---

### Task 5: Fix the shader-copy race

**Files:** Modify `tests/diff/CMakeLists.txt`, `tests/renderer/CMakeLists.txt`, `shaders/CMakeLists.txt`.

Deferred from Stage 0b-1 as out of scope; in scope here because this stage already touches the test build. A clean `-j8` build exits 1 with `Error copying directory ... build/Release/bin/shaders`: both CMakeLists attach a `POST_BUILD copy_directory` into the same destination, so parallel targets collide. `-j1` is clean.

Make the `shaders` target own the copy once, and have the test targets depend on it rather than each copying.

- [ ] **Step 1** Implement. **Step 2** Verify with a clean `-j8` build from an empty `build/Release`, repeated 3 times — a race that reproduces intermittently is not fixed by one green run. **Step 3** Confirm `renderer_test` still finds its SPVs. **Step 4** Commit.

---

## Exit criteria

- [ ] `diff_gpu_probe` exits 0 with all 17 checks plus the fused-loop check, 0 validation errors.
- [ ] Throughput and RNG-parity assertions pass **with the bounce loop fused into one command buffer**, using the same exactness as before.
- [ ] The barrier-removal proof (Task 3 Step 5) is recorded with its actual observed outcome.
- [ ] `ohao/diff/` grew and `tests/diff/` shrank; report both numbers.
- [ ] Clean `-j8` build succeeds three times consecutively.
- [ ] `diff_unit_tests` and `renderer_test` still pass.

## What this stage deliberately does not do

- **No BSDF, NEE, MIS, or environment sampling.** That is 0b-2b.
- **No PathTracer parity.** Also 0b-2b.
- **No gradients.** Stage 1.
- **No device-local state arena.** The host-visible arena is a known 0b-2b sizing issue (~141 MB at 1080p); changing it here would confound this stage's refactor with a performance change.
- **No material sorting.** Still one material.

## Next plan

**Stage 0b-2b — the integrator and PathTracer parity.** Port BSDF evaluation, NEE, MIS and environment importance sampling into the scatter stage, reusing `shaders/includes/rt/env_sampling.glsl` (96 lines), `mis.glsl` (18) and the Sobol includes rather than reimplementing them. Gate on matching `PathTracer` output within Monte Carlo noise on a probe-owned scene — **not** through `examples/cornell_box.cpp`.

**Carry this lens into that plan explicitly:** the port is full of GPU-vs-CPU comparisons, and Stage 0b-1 produced six instances of one defect class — *a check whose expected value derives from the same constant, shader or helper as the measured value cannot fail, while looking rigorous*. One of them was a throughput check comparing against `pow(albedo, 4)` computed from the constant being perturbed. Every parity assertion in 0b-2b needs an independent oracle, and every one needs a demonstrated failure.
