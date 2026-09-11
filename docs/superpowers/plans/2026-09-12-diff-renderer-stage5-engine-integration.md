# Stage 5 — Engine integration

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** `ohao_diff` stops being a library that only the test harness links.
A `DiffRenderer` owns a `ParamRegistry` and a `GradientArena`, runs the gradient
loop against **live engine resources**, and can be driven from outside.

**Gate:** an optimisation runs end to end against engine-owned objects — not
probe-owned buffers — and the engine's own next read of the parameter sees the
new value. Plus the full Stage 0–3 regression: `diff_gpu_probe` reproduces its
numbers byte for byte through `probe_normalise.py`.

This is item 1 of `2026-09-12-diff-renderer-remaining-road.md`, and six of the
remaining seven items are behind it.

---

## What Stages 0–3 leave standing

69 `OK:` lines from `diff_gpu_probe` (checks numbered to 64) and 58 unit tests,
covering forward parity, interior gradients for five parameter kinds, the
optimisation loop, and the boundary term. All of it against geometry and buffers
the **probe** creates. `ohao_diff` is linked by `diff_unit_tests` and
`diff_gpu_probe` and by nothing else.

So the mathematics is not what this stage risks. What it risks is everything the
probe was quietly providing: lifetime, ownership, and the question of who is
allowed to write a buffer while somebody else is reading it.

## THE STRUCTURAL FACT THIS STAGE RESTS ON

**`PathTracer` is the shape to copy, because it solved this already.**

```cpp
bool init(VkDevice, VkPhysicalDevice, uint32_t w, uint32_t h, ...);
void render(VkCommandBuffer cmd, RTAccelerationStructure* accel, ...);
void setMaterialData(const std::vector<glm::vec4>& materials);
```

It is handed a command buffer and an acceleration structure. It does not own a
submit, does not own the frame, and does not decide when it runs. Its material
data arrives through a setter that uploads a CPU-side array to a GPU buffer.

`DiffRenderer` is its sibling and should be indistinguishable in these respects.
That single decision answers all three of the ownership questions the roadmap
said had to be settled before any code:

### 1. Inside the frame loop, or its own submit?

**Neither — the class does not decide.** A step is *recorded* into a
caller-supplied `VkCommandBuffer`, exactly as `PathTracer::render` is. The
caller chooses whether that buffer is the frame's or a separate submit, and the
choice can differ between the editor and a headless fit without `DiffRenderer`
changing.

### 2. Who owns the parameter's authoritative value?

**The engine object does. The registry owns the gradient and the optimiser
state, never the value.**

This is not a preference; it is what the existing code already does.
`setMaterialData` takes a CPU-side `std::vector<glm::vec4>` and uploads it, so
the GPU material buffer is a *derived* artefact of a CPU-side authority that the
scene already maintains. A step is therefore:

```
read gradient from arena  →  Adam updates the CPU-side value  →  re-upload
```

and the re-upload is the engine's own existing path, not a second one. Spec §4.3
says the optimiser writes live engine resources; the live resource is the
material buffer, written the way the engine already writes it.

> **The failure this avoids** is worth naming, because it is the obvious design
> and it is wrong: if the registry held the value and wrote the GPU buffer
> directly, the scene's CPU copy would go stale, and the next ordinary material
> edit — a user dragging a slider — would silently overwrite the optimised
> value with the pre-optimisation one. Two authorities for one number always
> resolve to whichever wrote last.

### 3. What happens to an in-flight frame?

The value update is a CPU write plus a re-upload, which is the same class of
operation the engine already performs between frames for every other resource
edit. **The caller sequences it**, and `DiffRenderer` states in its header that
it must not be stepped while a frame reading those resources is in flight.

## Inherited hazards — read before Task 1

1. **`GradientArena::build` does not zero.** Check 61 read a destroyed position
   buffer's contents back as a gradient. Engine buffers are recycled far more
   aggressively than the probe's; zero explicitly, every step.
2. **The seed invariant (spec §4.5).** A path is a pure function of
   `(pixel, sampleIndex, iterationSeed)`. Whatever drives iterations in the
   engine must advance the iteration seed exactly once per step, or replay
   silently diverges from the forward pass.
3. **The probe is the reference implementation of the loop.** It orders the
   stages, zeroes the arena, seeds the adjoint, and steps Adam in a sequence
   that 69 checks validate. `DiffRenderer` must perform that same sequence —
   and Task 2's gate is what proves it does, by comparing against the probe on
   a scene both can express.
4. **Release hides asserts** (commit `43d46e2`). Run
   `tests/diff/tools/portability_check.sh` before pushing.

## Global constraints

- **No check weakened, deleted, silenced or renumbered.** New probe checks take
  numbers from **65** up.
- **Pre-register every criterion** before the first run; say so in the output.
- **Every gate gets a control that must fail.**
- `probe_normalise.py --selfcheck` over **four or more** runs after adding any
  check that prints a gradient or a loss.
- Build clean at `-j8`; `diff_unit_tests`; `renderer_test`;
  `python site/tools/generate_tree.py` exits 0.
- Every new check gets a **demonstrated failure** with pasted output.

---

### Task 1: `DiffRenderer`, the facade — DONE (4 lifecycle unit tests)

**Files:** `ohao/diff/diff_renderer.{hpp,cpp}`.

Owns a `ParamRegistry`, a `GradientArena`, and the wavefront stages. `init` /
`shutdown` mirroring `PathTracer`'s, and a `step` that **records into a
caller-supplied command buffer**.

- [ ] **Step 1: Write the failing test first**, in `diff_unit_tests` where it
      needs no device: a default-constructed `DiffRenderer` refuses to `step`,
      refuses to register parameters, and reports itself invalid — the same
      two-flag discipline `PinholeProjection::valid()` uses, because a facade
      that half-initialises is the one that produces a plausible wrong number.
- [ ] **Step 2: Confirm it FAILS. Step 3: Implement. Step 4: Verify.**
- [ ] **Step 5:** State the command-buffer contract in the header — that `step`
      records and does not submit, and must not run against resources a frame in
      flight is reading. A contract not written down is not a contract.
- [ ] **Step 6: Demonstrate failure. Step 7: Commit.**

---

### Task 2: THE PARITY GATE — DONE (check 65), REFRAMED

**This is the task that makes the rest safe, and it comes before any engine
resource is involved.**

- [ ] **Step 1: Pre-register the criterion.** Same scene, same parameter, same
      seed, same iteration count, through the probe's path and through
      `DiffRenderer`. The two gradients must agree to a stated tolerance.
- [ ] **Step 2: Write it as probe check 65** and confirm it FAILS before
      `DiffRenderer` can do anything.
- [ ] **Step 3: Implement. Step 4: Verify.**
- [ ] **Step 5: The control.** Omit one stage of the sequence — the arena zero,
      or the adjoint seed — and require the check to reject it. A parity gate
      that passes with a stage missing is measuring nothing.

**Why this ordering.** After this check, any later disagreement is *the engine
resources*, not the loop — because the loop is now pinned to 69 checks' worth of
validated behaviour. Without it, an integration failure has two candidate causes
and no way to separate them.

---

### Task 3: A live parameter, end to end — DONE (check 66)

**Files:** `ohao/diff/diff_renderer.cpp`, plus whatever the scene exposes.

Register an engine-owned material scalar. Read the gradient, step Adam, write
the CPU-side value, re-upload through the engine's existing path.

- [ ] **Step 1: Write the failing check first**, headless, in the manner of
      `tests/renderer/renderer_pipeline_tests.cpp` — **it already creates a real
      `VkDevice` with no window**, so "an optimisation inside the engine" is
      automatable and does not need Godot or a human looking at a screen.
- [ ] **Step 2:** The assertion is the one that distinguishes integration from
      simulation: after a step, **the engine's own read** of the material — not
      the registry's — returns the updated value. Re-read through the scene, not
      through the optimiser.
- [ ] **Step 3: Confirm it FAILS. Step 4: Implement. Step 5: Verify.**
- [ ] **Step 6: The staleness control.** Write the value through the optimiser,
      then perform an ordinary engine-side material edit, and assert the two do
      not silently fight — this is the two-authorities failure named above, and
      it is the one bug this design exists to prevent, so it gets a test rather
      than a paragraph.

---

### Task 4: GATE — recovery against engine resources — DONE (check 67)

- [ ] **Step 1: Pre-register** `theta*`, `theta_0`, iterations, alpha, tolerance.
- [ ] **Step 2:** Detune an engine-owned material parameter, render the target
      with the engine, and recover it. Loss must fall; the parameter must land
      within tolerance.
- [ ] **Step 3: The control**, as in checks 62 and 63: a deliberately broken
      variant that must NOT recover.
- [ ] **Step 4:** State in the check's output what it cannot see.

---

### Task 5: THE REGRESSION GATE — DONE

- [ ] **Step 1:** `diff_gpu_probe` produces its original numbers for every check
      1–64, compared through `probe_normalise.py` against a pre-Stage-5 baseline,
      with `--selfcheck` over four runs first.
- [ ] **Step 2:** `renderer_test` passes — this stage is the first to touch code
      the renderer links, so the renderer's own tests become a gate for the first
      time.
- [ ] **Step 3:** `tests/diff/tools/portability_check.sh` clean.

---

---

## Results — recorded 2026-09-12

`diff_gpu_probe` 72 `OK:` lines (checks numbered to 67), exit 0, 19.6s.
`diff_unit_tests` 62/62 under MSVC and under GCC 15 with asserts live, 65/65
translation units clean. Full build clean at `-j8`; `renderer_test` ALL PASS;
`generate_tree.py` exit 0. Checks 1–64 byte-for-byte unchanged through
`probe_normalise.py`, verified link by link as each commit landed, with
`--selfcheck` clean over four runs at every step.

### What the stage actually delivered

The facade, the ownership contract, and the optimiser half of the loop — all
gated. `DiffRenderer` owns a registry and an arena through a three-state
lifecycle, drives Adam from the arena at registry-assigned offsets, and check
67 recovers check 54's `theta*` to 0.0063 against its unchanged pre-registered
0.03 with the value living in an engine-style owner.

### Deviations from this plan — recorded, not glossed

**Task 1 shipped no `step`.** The plan's first step said the facade must
"refuse to step". It has no `step` method: there was nothing for it to
compute, and a method whose name promises a gradient while zeroing an arena
is worse than its absence. What ships is the lifecycle plus `zeroGradients`,
and the refusals are gated by removing them and watching two tests fail.

**Task 2 was reframed once the code was read.** The plan assumed
`DiffRenderer` computing "what the probe computes" was a thin call.
`runWavefrontGradientProbe` is **640 lines and IS the orchestration** — the
library owns the pieces, the harness owns the sequence. So check 65 pins the
**optimiser half** rather than the whole loop, and the whole-loop gate becomes
the existing checks producing identical numbers through the new path, which is
stronger than the parity check the plan imagined.

**Task 3 gates a reproduction, not `PathTracer`.** Standing the real class up
needs its images and pipelines. Check 66 gates the protocol against a faithful
copy of `setMaterialData`'s map-and-memcpy, and says so in its own output.

**The headline goal is only half met, and this is the important one.**
"`ohao_diff` stops being a library that only the test harness links" is NOT
true: it is still linked by `diff_unit_tests` and `diff_gpu_probe` and nothing
else. `DiffRenderer` exists and is gated, but no engine code constructs one,
and the forward and backward dispatches still live in the harness. What this
stage proves is that **the ends are connected** — parameters, arena, optimiser
and value ownership — not that the engine runs an optimisation.

### One tension the stage resolved

`optimizer_adam.comp` says the parameters are "updated IN PLACE... the
optimizer writes live engine resources"; `DiffRenderer`'s header says the
engine object owns the value and the registry never does. Those read as
contradictory and are not: the optimiser **does** write the live device
buffer, and the host **must then** read it back into the CPU authority.
Checks 66 and 67 establish exactly that, and 67 shows the omission is not
untidiness but a severed loop.

Still unresolved, and deliberately: for a TEXTURE parameter the per-step
readback is not free, and the GPU copy would have to be the authority with the
CPU side treated as an invalidated cache. Nothing here needs that yet.

### What remains of engine integration

1. **Move the render orchestration into the library.** The 640 lines are the
   work. The gate is free and already exists: every check must produce
   identical numbers through the moved code.
2. **An actual engine call site**, so `ohao_diff` is linked by something that
   is not a test — and the CMake link list stops being the honest answer to
   "is this integrated".

Only after both is item 1 of the remaining-road plan closed. Items 2–8 of that
plan are unaffected by this distinction except item 2, which needs a call site
to draw a sensitivity map from.

## What this stage deliberately does not do

- **No GDExtension surface yet.** The headless gate is the real proof; a Godot
  binding is a convenience on top and can follow once there is something worth
  binding. Adding it now would mean debugging two layers at once.
- **No sensitivity map.** That is item 2, and it goes *after* this deliberately:
  it exercises the integration on a quantity whose correctness is already gated.
- **No deferred-pipeline parameters.** That is item 3, and it needs the
  differentiability decision made first.

## Next plan

**Item 2 — sensitivity maps.** `dpixel/dtheta` as a visualisation, gated by a
null test: a parameter affecting one object produces a map that is *exactly*
zero away from it.
