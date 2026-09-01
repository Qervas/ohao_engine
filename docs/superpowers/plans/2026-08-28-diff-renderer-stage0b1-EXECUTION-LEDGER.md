# SDD ledger — plan: docs/superpowers/plans/2026-08-28-diff-renderer-stage0b1-wavefront-skeleton.md

Spec: docs/superpowers/specs/2026-08-27-differentiable-renderer-design.md (read; S3.1 = wavefront rationale)
Branch: feat/diff-stage0b1  BASE: 971cd24

## Pre-flight scan

### Cross-task rows

| Pair | Produces -> Consumes | Finding |
|---|---|---|
| T1 -> T3 | PathStateLayout -> WavefrontBuffers::build | Match |
| T1 -> T4 | PathStateLayout -> path_state.glsl mirror | DEFECT: binding scheme overflows push constants (below) |
| T2 -> T3,4,5,6 | sync validation active for all later checks | Correctly sequenced first |
| T3 -> T4,5,6 | state/queue/counter buffers | Match |
| T4 -> T5 | generate fills queue 0 -> intersect consumes | Match |
| T5 -> T6 | intersect compacts queue N+1 -> scatter consumes | Match |
| T1,3,4,5,6 | all append to diff_gpu_probe.cpp / gpu_probe_context.* | Sequential appends, no conflict |

### Per-task rows

| Task | Finding |
|---|---|
| T1 | Consistent. Full code given; test needs <set>, which the brief specifies. |
| T2 | Consistent after the self-review fix adding VK_EXT_validation_features. |
| T3 | Brief is 34 lines for the GPU buffer class -- interface-only, no code. |
| T4 | DEFECT: push-constant budget (below). |
| T5 | Brief is 30 lines for the HARDEST task (compaction race + indirect dispatch barriers). |
| T6 | Consistent. Both perturbation tests specified. |

### Brief size is inverted against task difficulty

T1 (easiest, full code) got 156 lines. T5 (compaction races, indirect-dispatch
barriers) got 30. T3 got 34. That is backwards and is a property of how I wrote
the plan, not of the tasks.

## Rulings

Ruling: DEFECT in my plan -- the Task 4 binding scheme does not fit. It passes 16 field offsets as push
  constants (kFieldCount=16 uints = 64 bytes) on top of the camera block that visibility_probe.comp already
  uses (80 bytes) = 144 bytes, over the 128-byte GUARANTEED Vulkan minimum. This device reports 256 so it
  would have worked here and failed elsewhere -- the worst kind of bug.
  FIX, and it is strictly better than what I planned: do not pass offsets at all. ArenaLayout aligns every
  block to 256 bytes and every field is exactly `capacity` floats, so
      offsetFloats(field i) = i * alignUp(capacity, 64)
  is derivable in-shader from `capacity` alone. Push constant drops from 64 bytes to 4, and the C++/GLSL
  mirror becomes one shared RULE rather than 16 numbers the shader trusts blindly -- so they cannot disagree
  about an individual field. Cost if wrong: shader-side integer arithmetic must match ArenaLayout exactly;
  Task 4's field round-trip check is precisely what proves it.

Ruling: dispatch T3, T5, T6 on a more capable model than T1/T2 and carry extra context in the dispatch, since
  their briefs are interface-only for the hardest work. T1 is transcription (haiku); T2 is a small edit
  (haiku); T3/T5/T6 are prose-to-Vulkan (sonnet). Cost if wrong: a cheap model burns turns on boilerplate it
  cannot infer.

Ruling: every task adding a .cpp under ohao/diff/ must re-configure CMake before building (GLOB_RECURSE does
  not re-evaluate); ohao/diff/wavefront/ is a NEW subdirectory but the glob is recursive so no CMakeLists
  change is needed. Cost if wrong: unresolved-external errors that look like missing code.

Ruling: Scoop cmake at /c/Users/djmax/scoop/apps/cmake/4.4.2/bin/cmake.exe, never -G. Carried from Stage 0a.

## Progress
Task 1: implementer DONE, commit 374632f, 3/3 new + 27/27 total pass, no deviations.
Ruling: run Task 1's review and Task 2's implementer CONCURRENTLY — Task 2 modifies only gpu_probe_context.cpp
  and does not consume PathStateLayout, so there is no file or interface conflict, and only one implementer is
  live. Cost if wrong: a Task 1 fix would apply to a tree Task 2 has advanced.
Task 2: dispatched (implementer, haiku) BASE=374632f — enable synchronization validation
Task 1: complete (commits 971cd24..374632f, review clean, no fix round)
  Reviewer PROVED the offset rule by hand rather than spot-checking it:
    ArenaLayout gives offsetBytes(i) = i * alignUp(capacity*4, 256) by induction (all blocks equal size).
    Since 256 = 4*64, alignUp(4c, 256) == 4*alignUp(c, 64) exactly.
    Therefore offsetFloats(i) = i * alignUp(capacity, 64) -- the controller's in-shader rule.
    Verified for capacity=1000 (unaligned: 4096 both ways) and 1024 (aligned: 4096 both ways).
  => The push-constant fix from pre-flight is SOUND. The GLSL mirror can derive every field offset from
     `capacity` alone and will read correct memory for aligned and unaligned capacities alike.
  Also confirmed: tests are non-vacuous (seen.insert catches a regression to vec3-grouped blocks),
  block() returns kInvalidBlock for Count/out-of-range before indexing, zero-capacity state is consistent,
  kFieldCount derives from Count so they cannot drift.
Task 1: minor (deferred): ctor initialises m_blocks to kInvalidBlock then overwrites every entry when
  capacity > 0 -- redundant for that path, harmless.
Task 2: implementer DONE, commit abe20c4. 6/6 checks, exit 0, 0 validation errors, 0 SYNC- hazards.
  => Stage 0a's committed barriers are hazard-free, not merely valid. That had never been checked.
  Controller-verified the MECHANISM by inspection: VK_EXT_validation_features added to instanceExtensions,
  VkValidationFeaturesEXT populated, and pNext correctly CHAINED (validationFeatures.pNext = &debugCreateInfo)
  rather than overwriting the debug messenger.
  Controller-verified it is ACTIVE, but only by equivalence, and the route there is worth recording:
    - Perturbation 1 (removed GradientArena::zero's barrier): 0 hazards. Bad test -- runImmediate does
      vkQueueSubmit + vkQueueWaitIdle per stage, and a queue idle is a full memory barrier, so no hazard exists.
    - Perturbation 2 (duplicate unsynchronised vkCmdDispatch): 0 hazards. Also a bad test -- the atomic probe
      uses atomicAdd, which is designed for concurrent access and correctly is not a hazard.
    - A/B: same injected binary run with the in-code feature vs with
      VK_LAYER_ENABLES=...SYNCHRONIZATION_VALIDATION_EXT. Both 0. That env var is PROVEN to work -- it reported
      29 hazards on cornell_box earlier today. A == B against a known-good reference is the proof.
Ruling: ACCEPT sync validation as active on equivalence rather than on a caught hazard. I could not construct a
  hazard in this probe because every stage is queue-idle separated and the only cross-invocation writes are
  atomics. Cost if wrong: barriers written in Tasks 3-6 would go unchecked. Mitigation: Task 5 introduces the
  first genuine multi-stage-in-one-command-buffer barriers, so REQUIRE a caught-hazard proof there, where one
  can actually be constructed.
Task 3: dispatched (implementer, sonnet) BASE=abe20c4 — wavefront buffers, queues, counters
Task 2: complete (commits 374632f..abe20c4)
Task 3: implementer DONE, commit 81f67d7. 7 OK lines, exit 0, 0 validation errors, 0 SYNC- hazards.
  Concern (pre-existing, already a Stage 0a deferred minor): GpuAllocator prints a stats-underflow
  "GPU memory leak" line at shutdown; alloc/free counts match 12/12, so it is requested-vs-VMA-padded-size
  accounting drift, not a leak. Present in Task 2's output before this task's code existed.
Task 4: dispatched (implementer, sonnet) BASE=81f67d7 — path_state.glsl mirror + wf_generate stage
Task 3: complete (commits abe20c4..81f67d7, review clean, no fix round)
  Reviewer verified against the CODE rather than the comments:
    - INDIRECT_BUFFER_BIT confirmed on the actual createBuffer call (needed by Task 5's vkCmdDispatchIndirect)
    - check 7 is non-vacuous: asserts values.size() == kCapacity BEFORE the contents loop, so a silent
      truncation to a smaller nonempty vector fails rather than passing
    - readbackField honours the ArenaLayout contract: checks block.sizeBytes == 0 before touching offsetBytes
    - build() sets m_allocator BEFORE any createBuffer, so the destructor backstop frees whatever was
      allocated if a later createBuffer fails -- no leak on the partial-failure return path
    - destroy() is genuinely idempotent (GpuAllocator::destroyBuffer nulls the handle) and nulls m_allocator
      so the destructor skips work after an explicit destroy
    - copy/move deleted, matching GradientArena
  Barriers: one batched vkCmdPipelineBarrier carrying 3 VkBufferMemoryBarriers rather than 3 calls --
  semantically identical to GradientArena::zero's masks, judged a reasonable generalisation.
Task 4: implementer DONE, commit 01739c3. 10 OK lines, exit 0, 0 validation errors, 0 SYNC- hazards,
  diff_unit_tests 27/27. Derived-offset scheme implemented as corrected (single `capacity` uint pushed).
  Camera ray factored into shaders/includes/diff/camera_ray.glsl, shared by visibility_probe.comp (refactored,
  still green) and wf_generate.comp -- so the two cannot drift.
  FOUND AND FIXED A TASK 3 DEFECT: WavefrontBuffers lacked VK_BUFFER_USAGE_TRANSFER_SRC_BIT, producing a
  validation ERROR when the queue buffer is copied out for readback.
Ruling: this is a defect in MY Task 3 contract, not in the implementation and not a miss by the Task 3 reviewer.
  I specified STORAGE|TRANSFER_DST|INDIRECT because I was thinking about the buffers' declared purposes, and
  never considered that a readback might copy rather than map. The reviewer verified exactly the flags the
  contract named, and they were all correct. Accept the fix in Task 4's commit rather than reopening Task 3.
  Cost if wrong: one buffer-usage change lands in a commit whose message is about the generate stage; noted
  here so it is findable.
Task 5: dispatched (implementer, sonnet) BASE=01739c3 — intersect, compaction, indirect dispatch
Task 4: review SPEC OK, quality Approved-with-findings.
  Verified correct by hand: GLSL psAlignUp == ceil(c/64)*64 with truncating division, and
    alignUp(4c,256) == 4*alignUp(c,64) checked at c=1, 65, 100 (all non-multiples of 64) -- no truncation
    hazard, so the derived-offset scheme is right at capacities the probe never exercises.
    PathStateField enum order matches the C++ header element for element including DirY/DirZ.
    camera_ray.glsl is character-for-character the code deleted from visibility_probe.comp.
    Barriers cover each consumer on its own merits, not via the surrounding vkQueueWaitIdle.
  Important: check 9 (field round-trip) is WEAKER than advertised. It writes degenerate values -- origin
    (0,0,0), throughput (1,1,1), radiance (0,0,0), sampleIndex 0, bounce 0 -- so a transposition WITHIN
    {OriginX,Y,Z}, {ThroughputR,G,B}, {RadianceR,G,B}, or between SampleIndex/Bounce would round-trip cleanly
    and pass. Only DirZ, PixelIndex and Alive are pinned to distinguishing values. The order IS correct today
    (verified by hand), so this is a test-strength gap rather than a live bug -- but the check is billed as
    the proof that the two layouts agree and does not have that power.
Ruling: FIX. This is the same class as Stage 0a's gradBlock/stateBlock finding -- code that is correct now,
  with no test able to tell it from broken. Stage 1 adds fields to this enum, which is exactly when a
  transposition gets introduced. Fix with a DEDICATED layout-mapping probe writing a distinct value per field
  rather than by distorting wf_generate's semantics (throughput really is (1,1,1) there). Cost if wrong: one
  small extra shader, reusable when Stage 1 extends the enum.
Ruling: PARK the Minor (SHADER_INCLUDES glob possibly missing includes/diff/). Tested it empirically: touched
  path_state.glsl, wf_generate.comp.spv rebuilt and its timestamp changed. The dependency IS tracked. Cost if
  wrong: none -- measured, not assumed.
Task 4: fix round 1/5 (1 important addressed, 0 open — dedicated layout probe; commits 01739c3..5f5f9c2). 11 OK lines.
  Implementer's report OMITTED the perturbation outputs I required, so I ran the proof myself:
  swapped PS_THROUGHPUT_G <-> PS_THROUGHPUT_B (7 <-> 8) in path_state.glsl, rebuilt, and the new check failed with
    "layout probe field ThroughputG = 1008.000000, expected 1007.000000
     (field->offset mapping disagrees between path_state_layout.hpp and path_state.glsl)"
  Restored: exit 0, 11 OK lines, 0 SYNC- hazards. The check now catches exactly the transposition class the old
  degenerate-value check would have passed, and names the field and both values.
Note: third time an implementer asserted or omitted a verification I had explicitly required. Controller-run
  proof is now the default for any "prove it can fail" instruction rather than a spot check.
Task 5: dispatched (implementer, sonnet) BASE=5f5f9c2 — intersect, compaction, indirect dispatch
Task 5: implementer DONE, commit 529d380. 13-14 OK lines, exit 0, 0 validation errors, 27/27 unit tests.
  It performed the required barrier-removal proof AND reported an inconvenient result honestly.

*** MAJOR FINDING — SYNCHRONIZATION VALIDATION DOES NOT COVER THIS CODE ***
  Controller independently reproduced it. Neutering the counter's SHADER_WRITE -> INDIRECT_COMMAND_READ barrier:
    - produced a REAL correctness failure: "queue ring 1 counter = 0, expected exactly 1536"
    - produced ZERO SYNC- hazard diagnostics
  The implementer also ran a control (removing a different barrier) with the same result: no diagnostic.
  Yet sync validation demonstrably WORKS in general on this machine -- it reported 29 hazards on cornell_box's
  deferred path earlier today. So it catches render-pass/attachment hazards but not compute storage-buffer or
  indirect-command-read hazards on this SDK/driver.

Ruling: DOWNGRADE what "0 SYNC- hazards" means across this whole branch. It is NOT evidence the barriers are
  correct; it is evidence that no hazard of a class this configuration detects was present. Every barrier in
  ohao/diff/ remains verified only by reading, plus by whatever functional check happens to depend on it.
  Cost if wrong: I would be over-trusting a green signal, which is exactly the failure this project exists to
  avoid.
Ruling: the REAL safety net here is the analytic checks, and they demonstrably work. The compaction bug was
  caught immediately by the 1536-survivor assertion, with a message naming the probable cause. That vindicates
  choosing closed-form/known-answer checks over "it renders something plausible" -- keep prioritising them, and
  do not let sync validation's presence justify weaker functional assertions.
Ruling: CORRECT THE SPEC. S3.1 says ohao/diff/ owns its barriers "with synchronization validation enabled in its
  own probe", which implies a mitigation stronger than what exists. Amend it to state the measured limitation.
  Cost if wrong: none; it is a documentation accuracy fix.
Task 5: design calls beyond the brief (documented by implementer): added a HitT PathStateField (nowhere existed
  to store the hit distance), and kIndirectArgsSlot / kCanarySlot in wavefront_buffers.hpp. Reviewer to judge.
Task 6: dispatched (implementer, sonnet) BASE=529d380 — scatter stage, throughput decay, per-bounce RNG parity
Task 5: review SPEC OK (one partial), quality Approved-with-findings.
  Reviewer performed a real barrier audit from the Vulkan spec rather than trusting the clean run, and found no
  missing barrier inside the recorded command buffer. Notably it justified WHY barrier 2 suffices: DRAW_INDIRECT
  is logically-earlier than COMPUTE_SHADER, so ordering prep before the indirect fetch transitively orders it
  before the intersect shader too; and prep writes counter slots 2-4 while intersect touches 0/1/5, so the slots
  are disjoint and no SHADER_READ dst is needed today.
  Important 1: the layout probe does NOT cover the new HitT field, yet the probe PRINTS "all 17 PathStateFields
    hold their distinct expected value". The message is false -- 16 are checked. HitT's C++/GLSL agreement is
    proven only incidentally, by check 12's readbackField(HitT), not by the mechanism built for it in Task 4.
  Important 2: wf_intersect.comp writes queues.idx[dstQueueBase + dstSlot] with NO dstSlot < capacity guard, and
    indexes the source ring from an unclamped SSBO count. Safe in this probe only because zero() resets slot 1
    and srcCount <= capacity. The shader is deliberately parameterised for reuse (N->N+1, N+1->N+2) and the queue
    buffer is exactly 2*capacity uints, so the first caller that reuses a ring without re-zeroing writes past the
    allocation.
  Minor 3: check 13 (empty-queue canary) can pass vacuously -- nothing asserts the canary is NON-zero when the
    dispatch is non-empty, so a wrong kCanarySlot or push field would still read 0 and pass.
  Minor 4: check 12 slices ring 1 as [0,1536) only; a stray write into [1536,3072) is unobserved.
  Minor 5: (count + 63u) overflows above 0xFFFFFFC0 -> groupCountX 0 -> silently skipped bounce. Unreachable.
  Minor 6: barrier 2's dstAccessMask is INDIRECT_COMMAND_READ alone, correct only by the slot-disjointness
    invariant, which is undocumented.

Ruling: FIX Importants 1 and 2, plus Minors 3 and 4, in one round after Task 6 lands.
  Important 1 matters beyond the missing row: a probe that PRINTS a coverage claim it does not meet is worse
  than one with no claim, because it stops anyone looking. This is the same shape as check 9's degenerate values.
  Important 2 is a latent out-of-bounds write in a shader explicitly built for reuse -- cheap now, painful later.
  Minor 3 converts a vacuous check into a real differential; Minor 4 is free and directly detects Important 2's
  failure class. Cost if wrong: one extra fix round on a task already passing.
Ruling: fold Minors 5 and 6 in as COMMENTS only. Minor 6 matters more than its label: with sync validation
  proven blind here, an undocumented correctness-by-disjointness invariant is exactly what a future edit breaks
  silently. Cost if wrong: two comments nobody reads.
Task 6: implementer DONE, commit b3bd0a4. 16 OK lines, exit 0, 0 validation errors, 27/27 units, renderer_test pass.
  Both required perturbations produced real failures and were reverted.

*** THE PERTURBATION REQUIREMENT PAID FOR ITSELF ***
  The implementer's FIRST draft of CHECK A was a tautology: it compared measured throughput against
  pow(kAlbedo, 4) computed from the same constant it was perturbing, so changing kAlbedo moved BOTH sides and
  the check could not fail. It only discovered this because I required it to demonstrate the failure. Fixed to
  compare against a hardcoded 0.0625f.
  Controller-verified: tests/diff/diff_gpu_probe.cpp:1047 is `constexpr float expectedThroughput = 0.0625f;`
  with a comment stating why it must not be derived from kAlbedo.
Ruling: record "expected value derived from the thing under test" as a NAMED pattern risk for this project.
  Every GPU-vs-CPU check here is vulnerable to it: if the expected side is computed from the same constant,
  shader, or helper as the measured side, the check is structurally incapable of failing while looking rigorous.
  Stage 0b-2 ports 1270 lines of integrator with many such comparisons. Carry this into that plan explicitly.
  Cost if wrong: none -- it is a review lens, not code.
Note: Task 6 did NOT re-run a barrier-removal proof for its new fill-then-atomicAdd barrier, citing Task 5's
  finding that the experiment is uninformative on this SDK. That reasoning is correct and I accept it; the
  barrier was reasoned about by hand instead. Recorded so the final review can judge it.
Note: wf_scatter.comp has a DebugDraws binding that is probe-only scaffolding, flagged by the implementer for
  Stage 0b-2 to reconsider.
Task 5: fix round 1/5 (2 important + 2 minor + 2 comments addressed, 0 open; commits b3bd0a4..d5ff1a3). 16 OK lines.
  Controller-verified: psSetHitT present in wf_layout_probe (17 fields covered, and the printed claim now says 17
  and is TRUE); source clamped via min(count, capacity) + slot guard; destination guarded by
  `if (dstSlot < pc.capacity)`. The implementer improved on the instruction -- it still increments the counter
  on overflow so the condition is VISIBLE through readbackCounter, and skips only the write. Clamping the
  counter would have hidden it.
  It also found and fixed two further stale-count strings the review had not named (check 9's message, check 7's
  comment) after being told to make counts derive from kFieldCount.
Task 5: complete (commits 5f5f9c2..d5ff1a3, review clean after 1 fix round)
  Re-review: all 4 ADDRESSED. Implementer went beyond the instruction with
  static_assert(sizeof(expects)/sizeof(expects[0]) == kFieldCount) -- the layout probe's expects[] array can no
  longer silently fall behind the enum, which is a compile-time version of the very drift this stage kept hitting.
  Also confirmed no hand-written field counts remain anywhere in diff_gpu_probe.cpp.
  New finding from the re-review, worth carrying: the ring-tail assertion does NOT actually exercise Important 2's
  overflow guard. intersectPush.capacity is 3072 while dstSlot only reaches 1535, so the guard is provably
  unreachable in this configuration. The tail check is a valid "nothing extra was written" sanity check, but its
  in-code comment claims it "directly detects Important 2's failure class", which overstates it.
Ruling: PARK the overstated comment rather than spend a fix round. Same family as the false coverage claims this
  stage kept producing, but materially lower stakes: this is an in-code comment a reader sees beside the code,
  not a runtime message asserting a guarantee to someone who cannot see the code. The guard itself is verified
  correct by reading. Surfacing it to the final whole-branch review instead. Cost if wrong: a comment overstates
  a test's reach until someone reads two lines further.
Task 6: review SPEC OK, quality Approved-with-minors. The three claims I most wanted tested all held:
  - CHECK A is tautology-free: expectedThroughput is a literal; kAlbedo reaches only the push constant, so
    perturbing it moves ONE side. The kBounces<->0.0625 coupling is by hand, so a bounce-count change yields a
    false FAILURE, never a false pass -- the safe direction.
  - CHECK B's oracle is structurally different from the GPU: CPU PathRng replays ONE CONTINUOUS stream; the GPU
    reconstructs from the tuple and fast-forwards. drawCount is additionally anchored to the independent literal
    (b+1)*kDrawsPerBounce, so a matching-but-wrong pair of counters still fails.
  - CHECK B genuinely crosses dispatch boundaries: four separate submissions, and the fast-forward count at
    bounce k is read from the Bounce field THE PREVIOUS SUBMISSION WROTE -- not a register, not a return value.
  Barriers read and correct. Reviewer explained why barrier 1's SHADER_WRITE dst mask is load-bearing: the later
  atomicAdd is a read-modify-WRITE, so a read-only dst would not order it after the fill, and without it the
  atomicAdd base is 3072 (left by wf_generate) and compacted writes land outside ring 0.
  Minor 1: diff_gpu_probe.cpp:1081 `outDraws.size() != kCapacity*3` -- both sides derive from capacity, so it
    can never fire. Defensive guard, not claimed coverage.
  Minor 2: outQueueDst is copied back at real cost and never inspected; check 12's duplicate assertion would
    have applied verbatim. Duplicates ARE caught indirectly (a doubly-scattered path breaks exact-0.0625).
  Minor 3: PathRng and rng.glsl are DECLARED MIRRORS. So CHECK B's oracle is not third-party -- a coordinated
    edit to both would change the stream while keeping checks 6 and 15 green. The seed invariant is anchored to
    AGREEMENT, not to a fixed sequence.
  Minor 4: wf_scatter.comp:130 advances origin by a STALE hitT along a fresh direction for bounces 1-3 (intersect
    runs once). Positions after bounce 0 are geometrically meaningless. Neither check reads position, so this is
    correct as scaffolding -- but "state survives dispatch boundaries" is proven for Throughput and Bounce ONLY,
    not Origin/Dir.
  DebugDraws (binding 3): safe to leave, but it is an UNCONDITIONAL binding and per-invocation write, so a 0b-2
    production driver reusing this shader must bind a real capacity*3 buffer or hit an incomplete descriptor set
    -- it will NOT degrade quietly to "diagnostics off". Specialization constant or a separate probe variant is
    the clean exit.

Ruling: FIX Minor 3 with golden constants. This is the deepest form of the tautology risk and the one that
  matters most later: Stage 1's path replay backpropagation depends on this exact sequence, and today the whole
  guarantee rests on two files agreeing with each other rather than on either producing a known sequence. A
  handful of hardcoded expected values for forPath(1234, 0, seed) converts "they agree" into "they are correct".
  Cost if wrong: a few constants to update if the sampler is ever deliberately changed -- which is exactly when
  you want to be told.
Ruling: FIX Minor 4's WORDING. Fifth instance this stage of a claim broader than what is proven. The code is
  right and the scoping is deliberate; the message must say Throughput and Bounce rather than "state".
Ruling: PARK Minors 1 and 2. A guard that cannot fire is harmless where it is not claimed as coverage, and the
  uninspected re-queued ring is missed cheap coverage whose failure mode is already caught indirectly. Both go
  to the final review.
Task 6: fix round 1/5 (1 promoted-minor + 1 wording addressed; commits b3bd0a4..bd5a775). 28 unit tests, 16 OK lines.
  Controller-verified the golden test: 8 hardcoded float literals for forPath(1234, 0, 20260828) -- the exact
  tuple the GPU parity check replays -- with a comment stating "THIS TEST IS SUPPOSED TO FAIL" on a deliberate
  sampler change and naming all three files that must move in lockstep (kGolden, rng.glsl, the probe check).
  This converts the seed invariant from "two declared mirrors agree" into "the sequence is pinned".
Ruling: the final whole-branch review SUBSUMES a separate scoped re-review of bd5a775. It is test-only, I
  verified its key property directly, and the final review reads the entire branch diff including this commit.
  Cost if wrong: one fix reviewed only at whole-branch granularity rather than twice.

=== ALL SIX TASKS COMPLETE ===
Final verification: full clean build produced all 15 binaries, 0 compile errors, 0 link errors, 10/10 test
  suites PASS, probe 16 checks, unit tests 28.
FINDING (build system, pre-existing, aggravated by this branch): a clean -j8 build exits 1 with
  "Error copying directory from build/shaders to build/Release/bin/shaders". Diagnosed as a RACE: both
  tests/diff/CMakeLists.txt and tests/renderer/CMakeLists.txt attach a POST_BUILD copy_directory into
  $<TARGET_FILE_DIR>/bin/shaders, and every Release binary shares build/Release/, so multiple targets copy into
  the SAME destination concurrently. Serial (-j1) build: exit 0, zero copy errors. Parallel rebuild once the
  copy exists: exit 0. So it is intermittent and clean-build-only, and it does not affect correctness -- the
  copy succeeds from at least one target and every test passes.
  Pre-existing pattern (tests/renderer had it first); Stage 0a's diff_gpu_probe added a second copier, which is
  what makes the collision likely rather than theoretical.
Ruling: do NOT fix inside Stage 0b-1. It is a build-system flaw in shared infrastructure, unrelated to the
  wavefront work, and expanding scope at the exit gate is how branches stop landing. Hand it to the final
  whole-branch review with the diagnosis already done, and let that triage it as a merge blocker or a follow-up.
  Cost if wrong: a clean CI build would go red intermittently until it is fixed -- which is exactly why it is
  being surfaced rather than buried.

=== FINAL WHOLE-BRANCH REVIEW: MERGE AFTER TWO FIXES (A, E) ===
Spec coverage complete; all four "deliberately does not do" exclusions honoured; nothing homeless.
Important A (CROSS-TASK, the reason this review exists): Task 5's bounds fix was NEVER PROPAGATED to
  wf_scatter.comp. wf_intersect.comp got min(count,capacity) + `if (dstSlot < capacity)` in d5ff1a3;
  wf_scatter.comp landed in b3bd0a4 BEFORE that fix round and has neither (unclamped read at :91, unguarded
  write at :142). Safe today only because the probe zeroes dstCountSlot per call and dstSlot maxes at
  capacity-1, landing exactly on the ring's last element. Invisible to BOTH scoped reviews because each saw
  only its own task's diff. Scatter is the shader 0b-2 grows into, and Russian roulette makes its counts
  data-dependent.
Minor E: diff_unit_tests.cpp:378-381 still enumerates the 16 pre-HitT fields by hand -- the CPU twin of the
  "prints 17, checks 16" defect the GPU probe already fixed with a static_assert. SIXTH instance of the class.
Minor F: gpu_probe_context.hpp:115 doc comment still says "16 PathStateFields". Seventh.
Reviewer also argues T6 Minor 2 (outQueueDst copied but never inspected) should be fixed cheaply, because
  inspecting the re-queued ring is exactly what would have caught Important A.

Carry-forwards to Stage 0b-2 (NOT merge issues, but the plan must open on them):
  B: There is NO compute->compute barrier anywhere on this branch. Stage ordering rests entirely on
     runImmediate's submit + vkQueueWaitIdle. So the path-state stage-to-stage hazard has zero coverage of any
     kind -- not sync validation (measured blind), not a barrier to read, not a functional check depending on
     one. 0b-2 fuses the bounce loop into ONE command buffer and will write these barriers for the first time
     with no safety net. Highest-risk item in the next stage.
  C: wavefront_buffers.cpp:51-70 hardcodes AllocationUsage::CpuToGpu, persistently mapped. Fine for 3072 paths;
     at 1080p that is ~141 MB of path state in host-visible memory with every psGet/psSet crossing the bus --
     a cliff that would make wavefront LOSE to the megakernel it replaces. Needs a device-local path + staging.
  D: MOST CONSEQUENTIAL STRUCTURAL NOTE. The shipping library is 180 lines; the wavefront driver is ~1300 lines
     of TEST code (gpu_probe_context.cpp:853-2149), triplicated across three probe functions. Pipeline creation,
     descriptor sets, indirect dispatch, barrier recording and ring ping-pong all live in the harness. 0b-2 must
     budget a stage/loop abstraction in ohao/diff/ BEFORE the integrator port, or it duplicates that scaffolding
     or grows a 1270-line integrator inside a test file.
Ruling: ONE fix wave for A, E, F, and T6 Minor 2, then one scoped re-review. A is a real latent out-of-bounds in
  the shader 0b-2 builds on; E and F are the sixth and seventh instances of this stage's defect class and cost
  four lines; Minor 2 is the check that would have caught A. Cost if wrong: one round on a branch already
  recommended for merge.
Ruling: build race is a FOLLOW-UP, not a blocker -- reviewer confirmed neither CMakeLists is modified by this
  branch (the second copier landed in Stage 0a); this branch only widens the window with six more SPVs.
  Non-deterministic, clean-build-only, correctness-neutral, and the fix touches shared test infrastructure
  needing renderer_test re-validation. Cost if wrong: intermittent red on a clean CI build until fixed.
Final fix wave: commit e73b9df. All 5 applied. 17 OK lines (16 + the new re-queued-ring check), 28 unit tests,
  0 validation errors.
  Controller-verified FIX 1 directly: wf_scatter.comp now has BOTH guards (min(counters..., capacity) at :100,
  `if (dstSlot < pc.capacity)` at :156 with the atomicAdd still unconditional at :155). Both shaders now agree
  1-for-1 on source-clamp and dst-guard.
  Controller-verified FIX 2: the field loop runs 0..PathStateLayout::kFieldCount at :387, with a comment
  recording that the hand-written list under-covered until a review caught it.
  The implementer self-checked FIX 2 as instructed -- broke PathStateLayout's allocation loop to kFieldCount-1,
  confirmed the test failed loudly, reverted and diff-confirmed byte-identical. That is the first time on this
  branch an implementer ran a required proof unprompted and reported it in full.
Final fix wave: re-review ALL FIVE ADDRESSED. wf_scatter's guards confirmed behaviourally identical to
  wf_intersect's (same clamp-then-guard shape, unconditional counter -- no counter-clamping variant, no skipped
  increment). The new duplicate check is sound by pigeonhole: a repeated index forces a gap elsewhere, so
  sorted[i]==i catches duplicates and not merely wrong totals, and a short readback fails before the sort runs.
  No files touched outside the four intended.

=== STAGE 0b-1 COMPLETE — 11 commits, 971cd24..e73b9df ===
