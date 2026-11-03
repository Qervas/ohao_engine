# Sub-project 23 — Path guiding: implementation plan

**Spec:** `docs/specs/2026-11-02-path-guiding-design.md`. Two build steps, each ending with the engine running, every
gate green and a change record (`docs/changes/YYYY-MM-DD-path-guiding-stepN.md`).

The paper's authors' code is GPL-2.0: never read, copied or linked. Everything here is from the paper's text.

## Task 1: the guided offline tracer (build step 1)

**Files:** `shaders/sampling/vmf.slang` (new), `tools/sampling/vmf_kernel.slang` + `vmf_cpu.{hpp,cpp}` (new),
`tests/renderer/vmf_test.cpp` (new); `shaders/rt/path_guiding.slang` (new: the state, the grid's reads, the resample,
the mixture density), `shaders/rt/path_guiding_update.slang` (new: the update and commit passes);
`shaders/rt/path_tracer.slang` (guided bounces, NEE weights, proposals); `ohao/render/rt/path_guiding.{hpp,cpp}` (new:
buffers, the passes, clear); `ohao/render/rt/path_tracer*.{hpp,cpp}` (bindings 46-50, the flag, the passes);
`ohao/render/renderer/vulkan_renderer.*` and `render_dispatch.cpp` (the setting); `ohao/api/params.*`, `session.*`
(`render.settings`/`render.capture` `guiding`); `tools/ohao/main.cpp` (`--guiding`); `assets/scenes/caustics.scene.json`
(new); `tests/renderer/check_guiding.py` (new: g2, g3, g5); `check_material.py`/`check_analytic.py` (`--guiding`); the
CLI and serve expectations (the new params); `tools/check_changed.py` rules; AGENTS.md and ARCHITECTURE.md.

- [ ] **Step 1: the vMF module and g1.** `vmf.slang`: `vmfPdf(dir, mu, kappa)` in the stable form
  (`kappa / (2 pi (1 - exp(-2 kappa))) * exp(kappa (dot - 1))`), `vmfSample(mu, kappa, u2)` (Jakob 2012:
  `w = 1 + log(u + (1 - u) exp(-2 kappa)) / kappa`), `vmfKappa(rbar)` (Banerjee's approximation, capped), the state
  and its `lobeAt(x)` (spec "The light field a state holds"). The CPU twin through slangc (as `tools/sampling`'s Sobol).
  `vmf_test`: chi-square of `sample` against `pdf` (64x64 bins, Sidak 1%) at kappa 0.1, 5, 100, 10^4; the integral of
  `pdf` within 0.1%; the mean cosine of samples against `A3(kappa) = coth(kappa) - 1/kappa` and `vmfKappa` inverting
  it within 1%; a state from samples of a known lobe recovering it. Registered `quick`.
- [ ] **Step 2: the grid on the host.** `PathGuiding` (as `RadianceCache`): two state buffers (32 B a state, 2^20),
  the claim words (two u32: the key, the owner), the proposal buffer (2 a pixel, resized with the tracer), device-local; `clear()` at
  `resetAccumulation`; created on first use, so a renderer that never guides allocates nothing. Bindings 46 (read),
  47 (write), 48 (priority), 49 (proposals), 50 (per-pixel sample radiance `R_end`).
- [ ] **Step 3: guided bounces.** `kFlagGuiding` (1 << 6). At a guidable vertex (spec G4): the resample, `q`, NEE's
  weights against `q`, the coin and draw in Sobol group `kGuideGroup + depth`, the weight `f cos / q`, `lastPdf = q`.
  The proposal written for the first two guided vertices; `R_end` written at the end. Off: no change at all (the
  Sobol groups and code path of today). Measure raygen's stack (`OHAO_PIPELINE_STATS=1`) and `pt.trace` off/on.
- [ ] **Step 4: the update and commit passes** (`pt.guide.update`, `pt.guide.commit`): `write = read`; per proposal,
  `f_mc`, the acceptance, the ML step, the discard rule, the target slot, `InterlockedMax` of the key; then winners
  write; the written buffer is copied over the read one. 32-bit atomics only (the spec's deterministic commit).
- [ ] **Step 5: the surface.** `render.settings`/`render.capture` `guiding`, `ohao run --guiding on|off`,
  `render.stats` `guiding {states, accepted, discarded}` (a counter buffer read with the timings). Expectations updated
  after looking at the diff.
- [ ] **Step 6: tests g2-g5.** `assets/scenes/caustics.scene.json`; `check_guiding.py`: g2 (means against unguided on
  Cornell, the helmet room, the caustic scene), g3 (the learned lobe's direction via `OHAO_GUIDE_DEBUG`'s AOV; RMSE
  against the 2^18-spp reference, cached under `build/guiding_ref/`), g4 (two captures bit for bit; every golden with
  guiding off unchanged: the golden run), g5 (the showcase's dot count, guided N vs unguided 4N, equal time).
  `check_material.py --guiding` and `check_analytic.py --guiding`. Tune G4/G5 by g3 and g5; record every value tried.
- [ ] **Step 7: close.** Docs (AGENTS.md: the guiding gotchas; ARCHITECTURE.md), `check_changed` rules, the full gate,
  the change record with the measurements and the showcase at 1080p for Frank.

## Task 2: realtime and budgets (build step 2)

- [ ] The realtime profile: guide the first vertex (and the second before the cache ends the path); the cache's cone
  rule for a guided sample (the lobe's spread, not the BSDF's); a window's `--guiding`.
- [ ] `render_bench` with guiding on (`helmet_rt_1080p`, `play_rt_1080p`), proposed budgets (pending).
- [ ] G6 (the default), G9 (velocity prediction) measured and put to Frank.
- [ ] Change record.
