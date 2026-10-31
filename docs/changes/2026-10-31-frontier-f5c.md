# Frontier F5c: the fills go

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, step F5: retires SSAO in lighting and the
constant ambient, moved to F5 by F2).
**Plan:** `docs/plans/2026-10-31-frontier-f5.md`, F5c.
**Status:** built; F5d (ReSTIR GI) next, if the referee asks for it.

## 1. Summary

**SSAO and the constant ambient are gone.** Indirect light has one source: RT GI (every bounce, F5a, and its reflection,
F5b), else, without ray tracing, the environment through the split sum, occluded only by the material's authored AO
texture. Without either a deferred frame has no indirect light, as in the path tracer. No golden moved: with RT GI,
SSAO already multiplied nothing (F5b) and the constant only lit frames with neither GI nor an environment.

## 2. What was built (and removed)

- **Deleted:** `SSAOPass` (`render/deferred/ssao_pass.*`), `shaders/compute/ssao.slang`, `SSAOPush`, the `ssao` pass and
  its budget, `render.settings ssao`, the capture post effect `ssao`, the post stack's noise seed (only SSAO read it),
  lighting's binding 10 and its `ssaoAt`, the constant ambient (`ambientIntensity`, `setAmbientIntensity`, the
  lighting pass's ambient parameter; `LightingPush.ambientIntensity` is padding now), the forward VFX shaders' copy of
  it (`push.camera.w`), and the unimplemented `IAOTechnique`/`AOInput`/`AOOutput`.
- **The API:** `render.settings {ssao}` is `E_UNKNOWN_FIELD`; a capture's `post: ["ssao"]` is `E_ENUM`; a capture that
  names no post gets none.
- 39 passes registered.

## 3. Tests

- **New** `engine_tests` `f5c_no_indirect_light_without_rt_gi_or_environment`: an unlit floor and wall equal the empty
  frame byte for byte with RT GI off and on (max_abs 0, 0), and a point light lights them (156).
- **Deleted:** the `sp2b_t05` SSAO tests, the ambient-fallback test, `deferred_determinism_gpu_test`'s SSAO noise checks
  (its other post configs are `taa,bloom`).
- Post lists elsewhere (`render_bench`'s scenes, `tests/cli/cases.json`, `check_live.py`, 17 golden commands) are
  `taa,bloom`.

## 4. Results

`tools/check.py` 57/57; render goldens 33/33 exact (none moved); `engine_tests` 84/84, `api_tests` 70/70,
`deferred_determinism_gpu_test` 84/84, `cull_parity_gpu_test` 49/49, `light_cluster_gpu_test` 22/22, `vfx_gpu_test`
44/44 (forward parity holds without the ambient), `check_validation` 19/19, `check_analytic` 16/16, `check_material`
21/21. CLI and serve expectations: the schema hash, `render.settings` without `ssao`, the unknown-field list.

## 5. Known gaps

- A frame without ray tracing and without an environment is black where no light reaches: correct, and darker than the
  old fill.
- Realtime RT GI leaves low-frequency blotches on flat untextured walls that a still camera does not average away (the
  denoisers keep 16-32 frames): the helmet room's walls show them (pixel noise 1.0% at 30 frames, 0.8% at 200; reference
  smooth). Next.
