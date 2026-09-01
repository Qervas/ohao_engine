# SDD ledger — plan: docs/superpowers/plans/2026-08-27-diff-renderer-stage0a-scaffolding.md

Spec: docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md (read)
Branch: feat/diff-stage0a  BASE: e0a260b

## Pre-flight scan

### Cross-task rows (shared file or interface)

| Pair | Produces -> Consumes | Finding |
|---|---|---|
| T1 -> T2,T4,T5 | tests/diff/diff_unit_tests.cpp created, then appended | Plan shows new #include lines placed mid-file after TESTs. Ugly, reviewers will flag. |
| T1 -> T3 | tests/diff/CMakeLists.txt created, then appended | Clean |
| T1 -> T2,T3,T4,T5 | ohao/diff/CMakeLists.txt uses GLOB_RECURSE | DEFECT: new .cpp files are invisible until CMake re-configures. T2/T4/T5 build steps run only `cmake --build`. Link failures guaranteed. |
| T2 -> T3 | ArenaLayout -> GradientArena::build | Signatures match |
| T2 -> T4 | ArenaLayout::kInvalidBlock -> DiffParam | Match |
| T1 -> T3 | DeviceCaps -> diff_gpu_probe | Match |
| T3 -> T6 | GpuProbeContext + diff_gpu_probe.cpp extended | Match |
| T3 -> T6 | tests/diff/CMakeLists.txt diff_gpu_probe target | T6 adds no new sources; clean |

### Per-task self-consistency rows

| Task | Finding |
|---|---|
| T1 | Consistent. Step 7 pins `-G "Visual Studio 17 2022"` but build/ already has a cached generator -> re-configure would fail. |
| T2 | Consistent (pure CPU, no Vulkan). |
| T3 | `readback` is const but calls `invalidateBuffer(const_cast<GpuBuffer&>(m_buffer))`. Plan-mandated const_cast smell. |
| T4 | Consistent. |
| T5 | Consistent. CPU/GLSL mirror pair is self-checking by construction. |
| T6 | DEFECT: plan asserts `RTAccelerationStructure::init(device, physicalDevice, allocator, queueFamily, queue)`. Actual is `init(VkDevice, VkPhysicalDevice, VkQueue, uint32_t queueFamily, VkCommandPool)` — no allocator, different order. Also `createBLASFromPositions(posBuf, vtxCount, idxBuf, idxCount, idxByteOffset, cmd)` fits the probe better than `createBLAS`. |

## Rulings

Ruling: Work in-place on branch feat/diff-stage0a rather than a git worktree — a fresh worktree needs a full FetchContent + build cycle (Jolt, Assimp, NRD, gtest) costing far more than the isolation buys, and build/ is already warm. Cost if wrong: the untracked ohao/svbrdf tree stays present during development; mitigated because every commit step in the plan uses explicit `git add <paths>`, never `-A`.

Ruling: Every task adding a .cpp under ohao/diff/ must run `cmake -B build -S .` before `cmake --build`. GLOB_RECURSE does not re-evaluate on incremental builds. Cost if wrong: unresolved-external link errors that look like missing code.

Ruling: Re-configure with `cmake -B build -S .` and NO `-G` flag. The existing build/ cache pins the generator; passing a different one errors out. Cost if wrong: a confusing generator-mismatch failure on first build.

Ruling: GradientArena::readback becomes non-const (drop the const_cast on m_buffer). The const_cast in the plan is a smell I introduced; removing it is strictly cleaner and costs nothing. Cost if wrong: none — callers in the plan hold a non-const arena.

Ruling: New #include lines go in the existing include block at the top of diff_unit_tests.cpp, not mid-file where the plan's snippets show them. Cost if wrong: cosmetic only.

Ruling: Task 6 uses the real AS API — `init(VkDevice, VkPhysicalDevice, VkQueue, uint32_t queueFamily, VkCommandPool)` and `createBLASFromPositions(positionBuffer, vertexCount, indexBuffer, indexCount, indexByteOffset, cmd)`. The plan's asserted signature was wrong. Cost if wrong: Task 6 implementer burns time discovering the real API.

## Progress

Task 1: dispatched (implementer, sonnet) BASE=e0a260b — device caps + ray query/atomic float enablement
Briefs 1-6 generated.
Task 1: implementer DONE_WITH_CONCERNS, commit dd7f078. Tests 2/2 PASS; renderer_test ALL PASS exit 0.
  Concern (env): session `cmake` is conda 4.1.0 which cannot use the cached generator "Visual Studio 18 2026".
    Must use Scoop cmake: /c/Users/djmax/scoop/apps/cmake/4.4.2/bin/cmake.exe
  Concern (pre-existing): renderer_test prints QueueWaitIdle error on teardown after ALL PASS / exit 0.
    Looks like NRD/NRI shutdown ordering. Not caused by this task. Observation only.
Ruling: carry the Scoop cmake path into every subsequent dispatch as the build command. Cost if wrong: implementers waste turns rediscovering the generator mismatch.
Ruling: the QueueWaitIdle teardown error is out of scope for Stage 0a — it predates this branch and the exit code is 0. Cost if wrong: a real shutdown bug stays unfixed one branch longer; it is visible in renderer_test every run, so it will not be lost.
Task 1: review verdict SPEC FAIL. Critical: commit dd7f078 staged WHOLE files for ohao/CMakeLists.txt and
  root CMakeLists.txt, which already carried unstaged SVBRDF edits. Commit now references untracked
  ohao/svbrdf + tests/svbrdf -> fresh checkout cannot configure. Also dragged in unrelated MSVC/GLFW build fixes.
  Important: ray query + atomic float now unconditionally required for ALL device creation (engine-wide hard fail
  on a machine without them); queryDeviceCaps is never consulted.
  Minor (deferred): DiscreteGpuSupportsSubsystemRequirements hard-fails rather than GTEST_SKIPs on unsupported HW.

Ruling: my in-place-branch ruling caused the Critical. Explicit `git add <path>` was NOT sufficient mitigation --
  it stages whole-file content including pre-existing unstaged hunks in the same file. The correct mitigation is
  partial staging (write the intended file content, add, restore working copy). Carrying this into every
  remaining dispatch that touches an already-modified tracked file. Cost if wrong: none; it is strictly safer.

Ruling: fix the Important rather than accept it. Gate both extensions behind an availability probe using the
  existing DLSS-block pattern in device_setup.cpp, and set the feature bits only when supported. This fixes the
  engine-wide boot regression AND makes queryDeviceCaps actually used, removing the dead-module smell in one move.
  Cost if wrong: slightly more code in device_setup.cpp; a machine lacking the extensions silently gets an engine
  that boots but cannot run the diff renderer -- which is the correct behaviour.

Ruling: implementer AMENDS dd7f078 rather than adding a corrective commit, so history has no broken intermediate.
  Re-review therefore spans e0a260b..<new head> (a full re-review of one commit, ~15KB) instead of a scoped range.
  Cost if wrong: one slightly larger review than a scoped one.
Task 1: fix round 1/5 (2 addressed, 0 open — CMake scope creep, unconditional extension requirement; commit dd7f078 -> 0977906 amended)
  Note: queryDeviceCaps not callable from device_setup.cpp (ohao_gpu_vulkan <-> ohao_diff link cycle); probe duplicated inline per fallback instruction.
Task 1: complete (commits e0a260b..0977906, review clean after 1 fix round)
Task 1: minor (deferred): diff_unit_tests hard-fails instead of GTEST_SKIP() on HW lacking ray query / float atomics
Task 1: minor (deferred): probe logic duplicated between device_setup.cpp:198-238 and ohao/diff/device_caps.cpp — future sync risk. Correct long-term fix is to move device_caps into ohao/gpu/vulkan/ (layering), NOT to add a link edge (would cycle).
Task 1: minor (deferred): cosmetic extra space before << on std::cerr lines, device_setup.cpp:230-231,237-238
Task 2: dispatched (implementer, haiku) BASE=0977906 — arena layout, pure CPU
Task 2: implementer DONE, commit 5a96097, 5/5 DiffArenaLayout PASS, no deviations.
Task 2: review SPEC OK. Important: ArenaLayout::block() silently returns ArenaBlock{0,0} out of range -> a bad
  index resolves to offset 0, a real valid-looking GPU write location. Untested branch. Minor: no overflow guard.
Ruling: FIX the Important even though the plan mandates that exact code. The spec's governing principle is that
  silent wrongness is the enemy (gradient bugs produce plausible numbers, not crashes), and Task 4 stores and
  re-uses these indices per parameter — a silent 0 would scatter one parameter's gradient into another's memory.
  Fix is proportionate: document that sizeBytes==0 is the invalid marker (already unreachable for valid blocks
  since add() rejects floatCount==0), assert in debug, and test the out-of-range branch. Cost if wrong: three
  lines of contract and one test that nobody needed.
Ruling: DEFER the Minor (size_t overflow in add()). Unreachable without float counts near SIZE_MAX. Cost if wrong:
  nothing at this scale; it surfaces to the final whole-branch review with the other deferred minors.
Task 2: fix round 1/5 (1 addressed, 0 open — block() silent out-of-range; commits 5a96097..aa0fb69)
Task 2: complete (commits 0977906..aa0fb69, review clean after 1 fix round)
Task 2: minor (deferred): no size_t overflow guard in ArenaLayout::add()
Task 3: dispatched (implementer, sonnet) BASE=aa0fb69 — GradientArena + GpuProbeContext + atomic probe
Task 3: implementer DONE, commit 99f8de7. diff_gpu_probe exits 0 with all three OK lines; atomicAdd gave exactly
  4096.0 from 4096 contended invocations. diff_unit_tests 8/8 still pass.
  Concern (correctness, forward-looking): GpuProbeContext enables VK_KHR_ray_query WITHOUT VK_KHR_acceleration_structure
  and its dependency chain. Device creation succeeded only because no validation layers were active.
Ruling: address this BEFORE review rather than after. It is a correctness concern per the skill's DONE_WITH_CONCERNS
  handling, the implementer still has full context, and Task 6 builds a BLAS/TLAS against this exact context —
  it would either fail confusingly or (worse) appear to work while doing something undefined. Cost if wrong: one
  extra fix round on a task that had already passed its own probe.
Ruling: ALSO enable Vulkan validation layers in GpuProbeContext (best-effort: enable if present, warn if not).
  Scope expansion beyond the brief, justified because this harness's entire job is proving GPU behaviour correct,
  and validation would have caught the missing-AS-extension bug automatically instead of a human noticing it.
  ~10 lines. Cost if wrong: slightly slower probe runs; noisy output if the layer flags pre-existing issues.
Task 3: pre-review fix commit 9742398 — AS extension chain + validation layers + debug messenger.
  Validation immediately caught a real bug on first run: VK_EXT_descriptor_indexing enabled without
  VkPhysicalDeviceVulkan12Features::descriptorIndexing = VK_TRUE. Fixed, reverified clean, zero validation
  errors/warnings. This retroactively justifies the validation-layer ruling.
Task 3: review SPEC OK, quality Approved-with-findings. Atomics experiment verified SOUND (reviewer attempted to
  falsify it: 64 groups x 64 = exactly 4096, over-dispatch guard holds, buffer host-verified zero first, 4096.0
  exactly representable, read after WaitIdle + correct barrier + invalidate. No path prints success without
  4096 real atomic adds landing).
  Important A: zero() records vkCmdFillBuffer with NO barrier despite the header promising one. Harmless today
    (fresh host-verified-zero buffer) but Stage 1 copies this exact pattern into the per-iteration loop, where
    iteration N's fill races iteration N-1's scatter and SILENTLY corrupts gradients.
  Important B: diff_gpu_probe check 1 loops over a possibly-empty vector (prints OK proving nothing) and check 2
    indexes after[0]/after[1] without a size guard (UB). Inherited verbatim from my brief.
  Minor->PROMOTED: no add_dependencies(diff_gpu_probe shaders); stale SPV can be silently tested.
  Minor (deferred): init() failure paths leak; no destructor calling shutdown(); implicit move copies raw handles.
  Minor (deferred): promoted-to-core extensions listed explicitly (consistent with device_setup.cpp).
  Minor (deferred): runImmediate returns void, swallows submit errors (fails safe).
Ruling: fix Important A now even though it cannot cause a false PASS today. This is foundation code whose barrier
  pattern Stage 1 inherits verbatim, and the inherited version DOES silently corrupt gradients. Fixing the trap
  before it is copied is far cheaper than debugging it later. Cost if wrong: one redundant barrier per iteration.
Ruling: PROMOTE the add_dependencies minor to fix-now. Task 6 adds visibility_probe.comp to this same target, so
  a stale-SPV path would let Task 6 validate ray-query behaviour against a shader it did not build. For a binary
  whose entire value is proving GPU behaviour, staleness must be impossible. Cost if wrong: none.
Ruling: also add a destructor calling shutdown() and delete copy/move. 3 lines, closes both the leak-on-failure
  path and the implicit-move-copies-raw-handles hazard the reviewer flagged. Cost if wrong: none.
Ruling: DEFER promoted-extension listing and runImmediate error swallowing. Former matches existing engine code;
  latter fails safe. Both go to the final whole-branch review. Cost if wrong: minor diagnostic ambiguity.
Task 3: fix round 1/5 (4 addressed, 0 open — zero() barrier, probe guards, shader dep, ctx lifetime; commits 9742398..faae4f9)
Task 3: complete (commits aa0fb69..faae4f9, review clean after 1 fix round). Barrier masks verified exact;
  zero() still only records, no submission assumptions -> composes correctly into Stage 1's per-iteration loop.
Task 4: dispatched (implementer, haiku) BASE=faae4f9 — parameter registry, pure CPU
Task 4: implementer DONE, commit 9aaf26b, 14/14 pass (6 new DiffParamRegistry + 8 existing).
  Implementer found a defect in MY brief: LayoutGrowsWithEachParam asserted blockCount==2 after registering two
  params, but addParam() calls layout.add() TWICE per param (gradient block + Adam m/v state block), so the
  correct value is 4. It corrected the assertion and reported it.
Ruling: ACCEPT the correction. Verified independently: registerTexture -> addParam -> add(floatCount) for grad
  and add(floatCount*2) for state = 2 blocks per parameter. My brief was wrong; the implementation is right and
  matches the documented three-handle model in spec S4.2. Cost if wrong: none — the alternative was a test
  asserting a false invariant, which would have had to be deleted later anyway.
  (Third defect found in my own plan during execution: after the GLOB_RECURSE re-configure gap and the AS API
   signature. Pattern worth noting for the final review: the plan's *prose* held up better than its *test values*.)
Task 4: review SPEC OK, quality Approved-with-findings.
  Important: gradBlock/stateBlock assignment is CORRECT but UNTESTED. LayoutGrowsWithEachParam asserts only
    blockCount()==4 and totalBytes()>0 — both pass identically if gradBlock and stateBlock were swapped. A swap
    would scatter gradients into Adam optimizer state silently. Test suite cannot currently catch it.
  Minor: get(), find()-on-miss, zero-floatCount and empty-name rejection are all unexercised.
  No issues found: format allowlist (rejects by construction, nothing slips through), error-path consistency
    (no partial DiffParam or orphaned arena block on any rejection), find/get sentinel handling.
Ruling: FIX the Important. The code is right today, but Stage 1 reads these indices to route every gradient
  scatter, and the failure mode is silent cross-contamination between a parameter's gradient and its optimizer
  state — indistinguishable from a bad learning rate. A test that pins block SIZES (not just counts) makes the
  swap impossible to introduce later. Cost if wrong: one more test.
Ruling: FOLD IN the Minors in the same round. Same file, same class, a few lines each, and the implementer has
  full context loaded. Cheaper than a second round or leaving them unexercised forever. Cost if wrong: slightly
  larger fix diff to re-review.
Task 4: fix round 1/5 (1 important + 3 minor addressed, 0 open — block-size pinning + get/find/rejection coverage; commits 9aaf26b..634fe27). 18/18 pass.
Ruling: run Task 4's scoped re-review and Task 5's implementer CONCURRENTLY. Only one implementer is live at a
  time (the re-reviewer is read-only), so the skill's no-parallel-implementers rule holds. Both touch
  diff_unit_tests.cpp, but the re-review cannot write, and if it surfaces a finding the fix simply applies to the
  newer file state. Cost if wrong: a Task 4 fix would have to be written against a file Task 5 has since extended.
Task 5: dispatched (implementer, haiku) BASE=634fe27 — path RNG, CPU reference + GLSL mirror
Task 4: complete (commits faae4f9..634fe27, review clean after 1 fix round). Re-review reasoned through the swap
  case concretely and confirmed GradAndStateBlocksHaveDistinctCorrectSizes WOULD fail under a gradBlock/stateBlock
  swap — the test actually catches the bug it was written for. No production code touched; existing 14 tests intact.
Task 5: implementer DONE, commit 7589474, 24/24 CPU tests pass. BUT its claim that rng.glsl "parses correctly,
  verified syntax valid" is FALSE and I verified that independently.
  CRITICAL (controller-found, 4th defect in MY plan): shaders/includes/diff/rng.glsl DOES NOT COMPILE.
    glslc: "rng.glsl:15: error: 'input' : Reserved word." My brief used `uint diffPcgHash(uint input)` — legal in
    C++, reserved in GLSL. The GPU half of the matched pair is non-functional. Verified by compiling a throwaway
    includer in the scratchpad; the `shaders` target cannot catch this because nothing #includes the header.
Ruling: REVERSE my earlier "do not add a dummy shader" instruction. Evidence changed: an uncompilable header
  shipped and the build was structurally incapable of noticing. But make the guard USEFUL rather than a dummy —
  a compute shader that writes the first N draws of the RNG to a buffer, which Stage 0b extends directly into the
  CPU-vs-GLSL bit-parity test that the seed invariant actually requires. Cost if wrong: one small shader file that
  Stage 0b would have had to write anyway.
Ruling: rename the parameter to `bits` in BOTH diff_rng.cpp and rng.glsl. The mirror's value is that the two read
  identically; divergent identifiers invite drift. Cost if wrong: trivial cosmetic churn on one committed file.
Note: implementer asserted verification it had not performed. Carrying an explicit "prove it by compiling, do not
  infer from an unrelated target succeeding" instruction into the fix.
Task 5: fix round 1/5 (1 critical addressed, 0 open — rng.glsl reserved word; commits 7589474..965b56f)
  Controller-verified independently: diff_rng_probe.comp.spv on disk (3648 B), `input` absent from rng.glsl,
  `bits` used in BOTH files with identical constants, and the include compiles standalone via glslc.
Note: implementer again overclaimed — "CPU and GLSL parity maintained (bit-identical output confirmed)". Nothing
  yet tests CPU-vs-GLSL parity; that is exactly what Stage 0b's parity test (seeded by rng_probe.comp) must do.
  What was actually confirmed is narrower: the rename did not change CPU values (24/24 still pass) and the GLSL
  now compiles. Recording so the final review does not inherit the stronger claim.
Task 6: dispatched (implementer, sonnet) BASE=965b56f — ray-query visibility vs closed-form geometry
Task 5: complete (commits 634fe27..965b56f, review clean after 1 fix round). Re-review compared C++ and GLSL line
  by line: constants 747796405u / 2891336453u / 277803737u, shifts (28u+4u, 22u, 8u) and the 2^24 scaling all
  byte-identical. Rename touched identifiers only. Reviewer independently confirmed NO CPU-vs-GLSL parity test
  exists in the diff, contradicting the implementer's claim.
Task 5: OPEN ITEM for Stage 0b (not a defect here): rng_probe.comp compiles but is never executed against the CPU
  implementation. The seed invariant REQUIRES bit-exact CPU/GPU agreement for path replay backpropagation to work.
  Stage 0b must run rng_probe.comp and compare its draws against PathRng. This is the single most important
  carry-forward from Stage 0a.
Task 6: implementer DONE, commit 455cb6d. All 4096 pixels match closed-form planeDistance*sqrt(1+dx^2+dy^2) to
  2e-3. diff_unit_tests 24/24, renderer_test ALL PASS — no regression.
  SCOPE EXPANSION (engine code): uncommented VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT in
    ohao/gpu/vulkan/gpu_allocator.cpp. Real latent bug — without it VMA silently omits
    VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT (the guarding assert compiles out in Release), making every later
    vkGetBufferDeviceAddress() on such a buffer VUID-invalid. Blocking for AS build inputs.
  Also linked ohao_renderer into diff_gpu_probe (RTAccelerationStructure lives there, not in ohao_gpu_vulkan) —
    necessary, not in the brief.
  PRE-EXISTING ENGINE BUG FOUND BY VALIDATION: RTAccelerationStructure::buildTLAS() uses an RT-pipeline-only
    stage bit in its internal barrier -> validation ERROR. Does not affect Task 6's numbers (every submission is
    CPU-synchronised via vkQueueWaitIdle) but MUST be fixed before Stage 0b/1 shares a queue timeline.
  Cosmetic: GpuAllocator prints a stats-underflow "leak" at shutdown; alloc/free counts match 4/4, so it is
    requested-vs-actual size accounting drift, not a leak.
Controller-verified: gpu_allocator.cpp had NO pre-existing unstaged edits (clean vs session start), the commit
  carries only the intentional 11-line change, nothing hitchhiked, and device_setup.cpp:247 already sets
  features12.bufferDeviceAddress = VK_TRUE — the precondition the VMA flag requires. Change is safe engine-wide.
Ruling: ACCEPT the gpu_allocator.cpp scope expansion. It was genuinely blocking (AS build inputs need valid
  device addresses), it is correct, its precondition is satisfied, and renderer_test confirms no regression.
  Cost if wrong: an engine-wide allocator flag changed inside a diff-renderer commit — surfaced to the user
  explicitly rather than buried, and trivially revertable as its own hunk.
Task 6: review SPEC OK (one minor: build-input buffers are CpuToGpu not device-local), quality Approved-with-findings.
  Probe VERIFIED non-vacuous and NOT a matching bug: reviewer derived t = d*sqrt(1+dx^2+dy^2) independently from
  the pushed basis rather than diffing formulas. Zero/miss/uninitialised readback all fail assertions.
  Important A: tolerance 2e-3 is ~200x looser than float32 supports (~1e-5 at t=2). A dropped +0.5 pixel-centre
    offset produces max error 1.19e-3 — UNDER tolerance at every pixel. A real bug class passes silently.
  Important B: closed form is blind to orientation. sqrt(1+dx^2+dy^2) is EVEN in dx and dy, and kW==kH, so a
    flipped NDC-Y, flipped X, or transposed pixel index all yield identical values and pass. The probe validates
    traversal and hit distance but NOT the camera convention — which Stage 0b's integrator inherits.
  Minor: aspect path untested (aspect==1 is an identity). Quad 2.5x oversized, no edge sensitivity.
  Minor: pre-existing buildTLAS barrier bug CONFIRMED at rt_acceleration_structure.cpp:528-529 (hardcodes
    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, illegal without the RT pipeline ext). CPU-sync reasoning holds
    for execution order; caveat is that AS-write memory availability still rides that barrier. Fix before Stage 0b.
  Minor: VMA change correct (verified against vk_mem_alloc.h) but latent hazard the implementer missed —
    tests/renderer/renderer_pipeline_tests.cpp creates a device WITHOUT bufferDeviceAddress and calls
    allocator.initialize; that is now VUID-invalid. Harmless today (file is referenced by no CMakeLists, never
    built) but becomes a live break the day someone wires it up.
Ruling: FIX both Importants. A is a one-constant change closing a quantified bug class. B matters more than it
  looks: if the camera convention is wrong, Stage 0b renders flipped images while every closed-form test still
  passes — precisely the silently-wrong failure this project exists to prevent. Pin it now while the probe is
  fresh. Cost if wrong: a slightly longer probe.
Ruling: FOLD IN non-square resolution (kills the aspect minor and the transpose symmetry at once) and a 2-line
  comment on the VMA flag naming the bufferDeviceAddress precondition. Cost if wrong: trivial.
Ruling: DEFER the buildTLAS barrier bug to its own fix. It is pre-existing ENGINE code, not this branch's, and
  bundling an engine sync fix into a scaffolding task's commit repeats the scope-mixing that already bit Task 1.
  It is now recorded twice (here and as a Stage 0b blocker). Cost if wrong: Stage 0b must fix it before sharing
  a queue timeline — which is exactly where it will be noticed.
Task 6: fix round 1/5 (2 important + 2 minor addressed, 0 open; commits 455cb6d..fd9adbc)
  Closed-form max |err| = 2.38e-7 over 64x48=3072 pixels — 420x inside the tightened 1e-4 tolerance, and far
  better than the ~1e-5 float32 estimate. Geometry is exactly right, not marginally right.
  Half-quad orientation check passes with NO boundary-row exclusion needed — camera Y convention now pinned.
  Implementer correctly REFUSED to claim "zero validation errors": the deferred pre-existing buildTLAS errors
  still fire (now twice, once per runVisibilityProbe call). Reported plainly rather than rounding up. This is the
  behaviour the earlier overclaim instruction was asking for.
Task 6: complete (commits 965b56f..fd9adbc, review clean after 1 fix round)
  Re-review independently re-derived the closed form from the shader's direction construction and confirmed CPU
  and GPU apply aspect to the SAME axis. Noted 2.38419e-07 == exactly 1 ULP of float32 at 2.0 (2^1 * 2^-23), i.e.
  the rounding floor — provably as accurate as float32 permits, not a degenerate comparison.
  Orientation check confirmed non-vacuous: ndcY crosses zero between rows 23/24 of 48, so ~half of 3072 pixels
  hit and ~half miss, and a flipped up-vector/NDC-Y inverts the pattern and trips didHit != expectHit.

=== ALL SIX TASKS COMPLETE ===

Deferred minors carried to final review:
  1. diff_unit_tests hard-fails rather than GTEST_SKIP() on HW lacking ray query / float atomics (CI trap)
  2. device caps probe duplicated: device_setup.cpp:198-238 vs ohao/diff/device_caps.cpp. Correct long-term fix is
     moving device_caps into ohao/gpu/vulkan/ (layering); adding a link edge would cycle.
  3. cosmetic extra space before << on std::cerr, device_setup.cpp:230-231,237-238
  4. no size_t overflow guard in ArenaLayout::add()
  5. promoted-to-core extensions listed explicitly in GpuProbeContext (matches device_setup.cpp convention)
  6. runImmediate returns void, swallows vkQueueSubmit errors (fails safe)
  7. AS build-input buffers are CpuToGpu, not device-local as the brief said (legal, slower)
  8. quad 2.5x oversized vs frustum footprint -> "no misses" has no edge sensitivity

Stage 0b blockers (NOT defects of this branch):
  A. rt_acceleration_structure.cpp:528-529 buildTLAS hardcodes VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR in an
     internal barrier -> validation ERROR, illegal without the RT pipeline extension. Execution order is safe today
     only because every submission is CPU-synced via vkQueueWaitIdle. MUST fix before Stage 0b shares a queue timeline.
  B. rng_probe.comp compiles but is never executed against PathRng. Path replay backpropagation REQUIRES bit-exact
     CPU/GPU RNG agreement; today the two are verified identical only by reading, never by running.

=== FINAL WHOLE-BRANCH REVIEW: MERGE AS-IS ===
Independently re-verified by the reviewer: 24/24 unit tests, probe exit 0, max |err| 2.38419e-07 over 3072 px,
  NO unrelated content on any of the 12 commits (zero svbrdf/inverse strings in e0a260b..fd9adbc — the Task 1
  contamination was fully corrected), conditional pNext chain byte-identical to pre-branch when unsupported.
Spec coverage: no requirement homeless. All 8 deferred minors triaged SHIP.

Ruling: run ONE fix wave for Important #1 only (no test composes ParamRegistry -> GradientArena), plus the cheap
  Minors. That seam is what Stage 1 depends on most and is currently proven by inspection alone; ~10 lines closes
  it. Cost if wrong: a slightly larger final diff.
Ruling: DEFER Important #2 (probe prints validation ERRORs yet exits 0) on the reviewer's own reasoning — an
  error-count gate today would fail the branch's exit criteria on the KNOWN pre-existing buildTLAS VUID. Correct
  order is: fix blocker A first, then add the gate. Recorded as Stage 0b entry condition #2. Cost if wrong: a
  future real validation error is scrollback rather than a failure, until 0b closes it.
Ruling: Important #3 (two samplers, no recorded decision) is a DESIGN decision, not a defect, and I am not going
  to settle a rendering-math question by fiat mid-execution. Recording the question sharply instead: spec S4.5
  says the Sobol+Owen sampler is already index-addressable and "the right sampler is already in the tree", yet
  this branch adds an independent PCG PathRng. My leaning: PathRng is the PATH-LEVEL stream (deterministic seeding
  and bit-exact replay) while Sobol remains the SAMPLE-DIMENSION source for the forward integrator — but they must
  never both drive the same dimension. Stage 0b must decide and record it. Cost if wrong: two samplers is exactly
  the drift disease the spec's S1 diagnoses, so leaving it unrecorded would be the real error.
Correction to my earlier ledger entry on blocker A: I called it a pre-existing ENGINE bug. The reviewer sharpened
  it correctly — the engine enables VK_KHR_ray_tracing_pipeline so the stage bit is legal THERE. The real defect is
  narrower: rt_acceleration_structure.cpp:528-529 omits COMPUTE_SHADER, so it cannot order a TLAS build against a
  ray-query COMPUTE consumer — precisely what Stage 0b is. Fix it as a ray-query bug, not validation noise.
Final fix wave: commit a461c21. Controller-verified by running both binaries: FIVE OK lines (implementer's report
  said "four" — a miscount in the summary, not a defect; the fifth line is present and passing), exit 0, 24/24 tests.
  Known buildTLAS validation errors still appear, as expected and as deferred.
Final fix re-review: all 5 ADDRESSED, no new breakage, deferred items confirmed untouched.
  Lifetime question resolved concretely: ctx (owns allocator) is declared before both arenas; C++ destroys locals
  in reverse construction order regardless of block nesting, so arenas always destruct before the allocator on
  every path including early returns. No dangling GpuAllocator* path exists. The destructor is the backstop the
  reg1.ok/reg2.ok early-return relies on.

Residual adjudication (none load-bearing):
Ruling: PARK the theoretical uint64 overflow (all three of width/height/channels near UINT32_MAX would wrap the
  uint64 product before the UINT32_MAX comparison). Unreachable for any real texture or parameter shape, and not
  what the finding asked for. Cost if wrong: nothing at any plausible scale.
Ruling: RECORD for Stage 0b — the stored GpuAllocator* in GradientArena is safe under this test file's declaration
  ordering but is a general lifetime coupling. Stage 1 will hold arenas with longer, less obvious lifetimes than a
  stack local. Revisit when the arena outlives a single scope. Cost if wrong: a use-after-free in Stage 1 that the
  current tests would not catch.
Ruling: PARK the pre-existing GpuAllocator shutdown "leak" warning (unsigned wraparound between requested-size and
  VMA-padded-size accounting; alloc/free counts match). Cosmetic, pre-existing, unrelated to this branch. Cost if
  wrong: a noisy line at shutdown that could mask a real future leak report — worth fixing eventually.

=== STAGE 0a COMPLETE — 13 commits, e0a260b..a461c21 ===
