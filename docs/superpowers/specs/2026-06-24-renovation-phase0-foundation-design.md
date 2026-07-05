# Renovation Phase 0 — Deterministic Render Contract + Golden Net

**Date:** 2026-06-24
**Status:** In progress — ✅ determinism fix + ✅ golden harness + ✅ pre-push hook (2026-06-25); remaining: CI workflow, trunk cleanup
**Phase:** 0 of the engine renovation (see `STATUS.md` and memory `renovation_plan`)

---

## North star (why this phase exists)

The engine is being re-architected toward a **differentiable, inverse-rendering, offline-first**
renderer (pbrt-style `.ohao` scenes, Slang shaders, one unified integrator). Before any of that
rewrite happens, we establish the **render contract** that the entire architecture stands on:

> `render(scene, config, seed) → exact, reproducible image`

This is **not** "add some tests." Determinism is a hard prerequisite of the differentiable soul:
gradient validation uses finite differences — `∂image/∂θ ≈ [render(θ+ε) − render(θ−ε)] / 2ε` —
which is meaningless if `render()` output wobbles run-to-run. Determinism is the cornerstone of
inverse rendering; the regression-test "golden net" is a free by-product of getting it.

Principle: **net before surgery.** Wrap today's *working* path tracer in a deterministic, automated
safety net so every later re-architecture step is validated against pinned truth and never unsafe.

## Goal

Wrap the existing (working, correct) offline path tracer in:
1. a **deterministic forward-render contract**, and
2. an **automated golden-image safety net** that runs locally (GPU) and in CI (no GPU),

so the subsequent Slang / `.ohao` / unified-integrator work is always regression-checked.

## In scope

1. **Determinism.** Same `(scene, config, seed)` → **bit-identical pixels on the same machine.**
   Root-cause and fix the known non-determinism ("3 back-to-back cornell_box runs → 3 different
   sha256s"). Denoiser is **excluded** from the contract (OIDN/NRD are not bit-deterministic) —
   we pin the **raw accumulated beauty** at a fixed spp.
2. **Golden-image harness.** A `render-golden` tool driven by a **manifest** (scene + config +
   committed golden PNG + tolerance). Renders each case, compares to its golden, emits a pass/fail
   and a visual diff on drift. Same-machine: exact hash compare. Cross-machine fallback: RMSE/SSIM
   under a threshold.
3. **Two-tier safety net.**
   - **Pre-push git hook** — runs `render-golden` on the manifest locally (uses the GPU); non-zero
     exit blocks the push.
   - **Cloud CI** (GitHub Actions) — runs `build` + C++ unit tests (`ctest`) + lint/format check.
     **No GPU**, so no golden renders here.
4. **Trunk discipline.** Establish a canonical branch and prune the divergent dead branches
   (`dev`, `feat`, `scene_switching_fix`). Decide the `master` vs `animation` reconciliation.

## Out of scope (later phases)

- Slang, `.ohao` format, unified integrator, RHI abstraction, differentiability proper.
- Denoiser determinism (pin raw beauty instead).
- Fixing the deferred-metals bug (handed to Codex; deferred pipeline is frozen).
- Touching physics (quarantined) or audio (kept).

## Components

### 1. Determinism contract
- Fixed seed plumbed through the integrator; **per-pixel-independent accumulation** (no cross-thread
  atomic reduction into shared accumulators — that's a prime non-determinism suspect via
  floating-point non-associativity).
- No wall-clock / `time()`-based seeding anywhere in the render path.
- Accumulation / AOV images explicitly cleared (UNDEFINED→GENERAL + zero) before first sample.
- **First implementation task is an investigation** (root-cause the "3 hashes"). Likely suspects,
  in order: (a) time-seeded RNG, (b) racy GPU accumulation / atomics, (c) uninitialized buffers,
  (d) non-deterministic work ordering. The fix depends on findings — `systematic-debugging`.

### 2. `render-golden` harness  ✅ IMPLEMENTED
- `tests/golden/render_golden.py <manifest.json> [--update|--selftest]` — a Python tool (it runs
  in the local pre-push hook, not GPU-less CI; "run command + diff PNG" is a scripting job).
- Each manifest entry has a `command` (an existing example invocation with an `{out}` placeholder),
  a `golden` path, and per-scene `max_abs_diff` / `max_diff_frac`. The command shells out to the
  current example binaries with `--denoise=none` (raw beauty). When `.ohao` lands (Phase 1) the
  command just becomes the new renderer invocation — the harness doesn't change.
- **Tolerance compare, downscaled.** Both images are downscaled to 640px wide before diffing — this
  averages the FP ghost away (it vanishes to max_abs=0 at compare-res) AND shrinks goldens from
  ~30 MB to ~450 KB (committable, no git bloat). PASS iff `max_abs_diff` AND `diff_frac` both within
  bounds — the first catches magnitude, the second catches widespread tiny shifts.
- `--selftest` renders each scene twice and compares the two (direct determinism check).
- On drift: writes `<golden>.actual.png` and exits non-zero. Validated: a wrong-spp render is caught
  (max_abs=90, 99.98% pixels differ, exit 1).

### 3. Manifest + golden corpus
- `tests/golden/manifest.json`: list of `{ name, scene, spp, seed, mode, golden, compare, threshold }`.
- Seed corpus from **current example scenes** (cornell box, MetalRoughSpheres) at a fixed low spp —
  these already render correctly and are cheap. The corpus grows as features land (each fixed bug
  adds a golden so it can't silently return).

### 4. Pre-push hook
- `.githooks/pre-push` → runs `render-golden tests/golden/manifest.json`; blocks on failure.
- Installed via documented `git config core.hooksPath .githooks` (committed, opt-in, discoverable).

### 5. CI workflow
- `.github/workflows/ci.yml`: configure + `cmake --build` + `ctest` (unit tests) + a lint/format
  check (e.g. clang-format dry-run). Runs on push/PR. No GPU steps.

## Key design decisions

- **Tolerance compare, not bit-exact hash (decided from data).** Empirically the offline path tracer
  is deterministic up to ~6 pixels at 1 LSB on a 1920×1080 frame — irreducible GPU floating-point
  non-associativity (e.g. multi-light NEE reduction order). PBRT/Mitsuba test suites hit the same wall
  and also use tolerance. We downscale to 640px (the ghost averages to 0) and bound both magnitude and
  spread. Chasing true bit-exactness (forced reduction order, no fast-math) is large effort + perf cost
  for a 6-pixel ghost — not worth it.
- **Denoiser excluded from goldens.** Pin the raw deterministic beauty; denoised output is a
  separate, non-pinned concern.
- **Wrap, don't rewrite.** Phase 0 changes the *render loop's determinism* and adds *tooling*; it
  does not restructure the renderer. The big structural moves are Phases 1+, under this net.

## Exit criteria

1. `render-golden` produces **bit-identical** output across repeated same-machine runs of the
   manifest (or, where bit-exactness proves infeasible, RMSE/SSIM within a documented tight bound).
2. A deliberately-introduced render regression is **caught** by `render-golden` and **blocked** by
   the pre-push hook.
3. Cloud CI is **green** on build + unit tests + lint.
4. Canonical trunk established; `dev`/`feat`/`scene_switching_fix` pruned or archived.
5. Docs: "how determinism is guaranteed" + "how to add a golden scene."

## Risks & mitigations

- **Non-determinism is deep (GPU FP non-associativity in parallel accumulation).** Mitigation: if
  true bit-exactness is infeasible at acceptable cost, fall back to a tight RMSE/SSIM threshold —
  still a real regression net, just not hash-compare. Decide after the root-cause investigation.
- **CI can't run goldens (no GPU).** Accepted: goldens run at the pre-push hook on real hardware;
  CI covers build/unit/lint. Documented as the intentional split.

## Testing

- The golden harness **is** the integration test. Plus a **determinism self-test**: render the same
  manifest entry twice in one process and assert identical output (guards the contract directly).
- Existing unit tests (`engine_tests`, sobol, env-cdf, denoise-parse) continue to run in CI.

## First implementation tasks (for the plan)

1. Root-cause the non-determinism (investigation) and make a fixed-seed forward render bit-identical.
2. Build `render-golden` + the manifest format; seed the corpus from cornell + MetalRoughSpheres.
3. Add the determinism self-test.
4. Wire the pre-push hook + the CI workflow.
5. Trunk reconciliation (separate, low-risk, can run in parallel).
