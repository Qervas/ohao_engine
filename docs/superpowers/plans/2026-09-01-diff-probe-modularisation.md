# `diff_gpu_probe.cpp` Modularisation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Break `tests/diff/diff_gpu_probe.cpp` (11,764 lines) into modules, with **byte-identical probe output** as the acceptance rule.

**Architecture:** This is an extraction, not a redesign. The file is already well-delimited — it was simply never split. Nothing about *what* is checked changes.

---

## The measured shape (survey it yourself before trusting this)

| Region | Lines | Contents |
|---|---|---|
| Includes + `namespace {` | 1–235 | — |
| **7 helper sections** | 236–3366 (~3,130) | BSDF oracle; CPU reference integrator; shared scene; CRN harness; detached instrument; emission harness; texture harness — each with a `// ====` banner |
| **`main()`** | 3368–11764 (~8,400) | **24 top-level braced scopes**, 27–688 lines each — the checks |

**Only ~20 declarations sit at `main()`'s top level**, nearly all in the first 350 lines. The genuinely shared state is: `ctx` (`GpuProbeContext`), `caps` (`DeviceCaps`), and the arena trio (`layout`, `blockA/B/C`, `arena`) used by the early checks and the gradient checks.

**Everything else is already scoped.** That is what makes this mechanical.

## THE ACCEPTANCE RULE — this is the whole plan

**`diff_gpu_probe`'s stdout must be byte-identical before and after, and so must its exit code.**

Capture a baseline first:
```
./build/Release/diff_gpu_probe.exe > baseline.txt 2>&1 ; echo "exit=$?" >> baseline.txt
```
and diff against it after every step. **A refactor that changes one character of output has changed behaviour** — the `OK:` lines carry measured values, so a differing number means a differing computation, not a cosmetic edit.

This is a stronger gate than "the tests still pass," and it is available precisely because this probe prints its measurements.

## Global Constraints

- **No check weakened, deleted, silenced, or renumbered.** Check *numbers* are load-bearing: the shaders and the monograph cite them by number, and `OK:`/`FAIL:` text is quoted in reports.
- **No behavioural change of any kind.** Not a tolerance, not a constant, not an assertion, not an order of operations. If you find a bug while moving code, **stop and report it** — do not fix it in the same commit. A fix and a move in one diff cannot be reviewed.
- **The 8 startup source-parsing ties must still run, in the same order, before any Vulkan object exists.** They are: `checkNeeStrideTie`, `checkScatterPushSizeTie`, `checkWfScatterSinkLayoutTie`, `checkParityRefConstantsTie`, `checkTraverseInstantiationTie`, `checkDrawsPerBounceTie`, `checkBsdfShaderConstantTies`, `checkTexelOrderingTie`. Several parse shader source with paths relative to the working directory — **verify those still resolve from the new file layout.**
- **`floatCount()`-style conventions and the texel ordering stay in their current homes.** `ParamShape::elementIndex` and `diffTexelElementIndex` are tied to each other by a source parse; moving either breaks the tie's regex.
- Build must stay clean at `-j8`; `diff_unit_tests` 28/28; `renderer_test` passes; `python site/tools/generate_tree.py` exits 0.

---

### Task 1: Lift the seven helper sections

**Files:** Create `tests/diff/probe/` with paired `.hpp`/`.cpp` per section; modify `tests/diff/diff_gpu_probe.cpp`, `tests/diff/CMakeLists.txt`.

Each section already has a `// ====` banner and a clean boundary. Suggested split (adjust if the boundaries argue otherwise — say so):

- `oracle_bsdf.{hpp,cpp}` — the published-formula BSDF oracle (Walter/Heitz/Schlick/Karis). **Its provenance comments are load-bearing** — they are what make it an oracle rather than a transcription. Move them with it.
- `oracle_integrator.{hpp,cpp}` — the CPU reference path tracer (checks 38–39). The largest section.
- `scene.{hpp,cpp}` — shared geometry, environment and camera builders.
- `fd_harness.{hpp,cpp}` — the CRN harness, the detached instrument, and the emission and texture harnesses. These share the `h`-derivation model; keep them together.
- `ties.{hpp,cpp}` — the eight startup source parsers.

- [ ] **Step 1: Capture the baseline** (above). Commit it nowhere; keep it in the scratchpad.
- [ ] **Step 2: Move one section**, rebuild, diff against baseline. **Byte-identical or revert and diagnose.**
- [ ] **Step 3: Repeat per section**, diffing each time. Moving all seven and diffing once tells you nothing about which move broke it.
- [ ] **Step 4: Report the line-count delta** for `diff_gpu_probe.cpp` and the new total across `tests/diff/`.
- [ ] **Step 5: Commit.**

---

### Task 2: Lift the checks out of `main()`

**Files:** Create `tests/diff/probe/checks_*.{hpp,cpp}`; modify `diff_gpu_probe.cpp`, `CMakeLists.txt`.

Each of the 24 braced scopes becomes a function returning `bool` (false on failure), taking `GpuProbeContext&` plus whatever it genuinely needs. **Do not invent a context struct that bundles everything** — a parameter list that names what a check actually touches is documentation; a god-object is not.

Group by subsystem, not by count. Natural seams from the check numbering: foundation (arena, pipeline, atomics, RNG, registry) · wavefront buffers and stages · BSDF and furnaces · environment sampling · NEE/MIS and film · parity · replay and gradients · texture.

`main()` should end as: device setup, the eight ties, then a linear sequence of check calls with early return on failure. **Target under 400 lines.**

- [ ] **Step 1: Lift one group**, rebuild, diff against baseline.
- [ ] **Step 2: Repeat per group**, diffing each time.
- [ ] **Step 3:** Verify `main()`'s remaining length and that the check call order is unchanged — **output order is part of byte-identity**, so a reordered call is a failed diff, but say explicitly that you checked rather than relying on the diff alone.
- [ ] **Step 4: Report** the final line counts per file.
- [ ] **Step 5: Commit.**

---

## Exit criteria

- [ ] `diff_gpu_probe` stdout and exit code **byte-identical** to the pre-refactor baseline.
- [ ] No file in `tests/diff/` over ~1,500 lines; `diff_gpu_probe.cpp` under 400.
- [ ] All 8 startup ties still run, in order, before any Vulkan object exists.
- [ ] `diff_unit_tests` 28/28; `renderer_test` passes; clean `-j8` build; site build exits 0.
- [ ] No check weakened, deleted, silenced or renumbered — verified by diffing the extracted `OK:`/`FAIL:` string sets, not by eye.

## What this deliberately does not do

- **No bug fixes.** Anything found gets reported, not fixed.
- **No new checks**, no changed tolerances, no renumbering.
- **No touching `gpu_probe_context.{hpp,cpp}`** (4,079 + 1,010 lines). It is the next candidate, and mixing it in would make the byte-identity gate ambiguous about which move broke what.
