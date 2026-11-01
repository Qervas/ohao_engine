# OpenPBR steps 3 and 4: the known gaps closed

**Spec:** `docs/specs/2026-10-29-openpbr-materials-design.md` (section 8, steps 3 and 4); plans
`docs/plans/2026-10-31-openpbr-step3.md` and `docs/plans/2026-11-01-openpbr-step4.md`, their change records' sections 6
("Known gaps").
**Status:** built. Ruling O14 (panes pass the path tracer's shadow rays) is built; O10–O18 stay provisional until Frank
rules on them.

## 1. Summary

The gaps steps 3 and 4 left, worked down from their change records. The path tracer's subsurface walk is spectral MIS
and lights its exit; the deferred SSS pass blurs the diffuse alone; the path tracer's bounce and shadow rays now see
glass at all (they did not: three bugs together had hidden it), panes pass shadow rays, and glass has its own bounce
budget; deferred glass is traced through the solid instead of offset in screen space.

## 2. What was built

- **The walk** (`shaders/rt/path_tracer.slang`, `walkSubsurface`): each step's channel is drawn in proportion to the
  walk's throughput (Cycles' spectral MIS): the scatter weight `albedo sigmaT tr / dot(p, sigmaT tr)`, the exit's
  `tr / dot(p, tr)`. Roulette only once the largest weight is under 1; a walk at `kWalkSteps` (1024) leaves along its
  direction at the surface it meets; walk rays start at TMin 0 (`traceWalk`). The exit is a Lambertian vertex (ruling
  O16): light and the environment are sampled there by MIS (`sampleEnvAt`, the environment from four given uniforms),
  and the next hit weighs the walk's own direction the same way.
- **The SSS pass blurs the diffuse alone** (O17 complete): `core/deferred_lighting.slang` writes a second target
  (`SV_Target1`, `R32G32B32A32_UINT`, `DeferredLightingPass::getSubsurfaceDiffuseView`) for subsurface pixels: the
  diffuse irradiance as fp16, a mask, the diffuse albedo (`packSubsurfaceDiffuse` in
  `includes/lighting/clustered_lights.slang`, with `directLightDiffuse` and `evaluateRect`'s `diffuseOnly`).
  `postprocess/sss.slang`'s horizontal pass blurs that irradiance (taps masked), the vertical one (binding 9,
  `SSSPass::setSubsurfaceDiffuse`) adds `albedo x (blurred - own)` to the lit pixel. Softbox highlights on skin stay sharp.
- **Panes pass shadow rays (O14).** `paneMaterial`: thin-walled, transmissive, not metal, roughness at most
  `kPaneRough` 0.05. `anyHit` ignores a pane on a shadow ray (`kShadowRay`) and flags it (`kPaneSeen`); `visible()` then
  walks the panes along the ray (8 at most) and multiplies their `transmissionAlbedo` into `shadowT`, which every light
  sample multiplies (the main loop, the walk's exit, the cache update). A bounce through a pane is a null crossing for
  MIS: `misOrigin` and `lastPdf` stay the last real vertex's.
- **Glass has its own bounce budget** (`kCrossings`, 16): refractions and reflections inside a solid do not count
  against `bounces` (Cycles' transmission bounces). The radiance cache is never read at a glass hit (its update paths
  stop where a surface refracts).
- **Deferred glass is traced** (`vfx/transparent_mesh.slang`; set 2 bindings 9-11, the mesh table, indices and
  vertices, `VfxRenderer::writeForwardSet`): refract in, find the instance's own far side with a ray query
  (`glassExit`, committing only its own non-opaque triangles; an opaque object inside ends it), total internal
  reflection up to `kGlassBounces` 4, refract out, find the opaque surface the ray meets (`opaqueHit`), project it and
  read the lit frame's copy there, or the prefiltered environment on a miss. Beer-Lambert over the length inside; thin
  walls pass straight. `kRefractionDepth` (the old screen offset) is gone.
- **Tests:** `check_material.py` gains `subsurface_dense` (mean free path 0.005 m, a near-white volume past the step
  cap); `check_live.py` test 11 names which part of the headless comparison failed (exit code, offline, realtime, hash).

## 3. Bugs found and fixed

- **Bounce and shadow rays never met glass.** A transmissive material is Blend on the host, so its TLAS instance
  carries `kTlasMaskTransparent` alone, and the path tracer's bounce rays (`kRayMaskScene | kTlasMaskEmitter`) and
  shadow rays (`kRayMaskScene`) passed through it: a ball refracted once (at the camera's hit) and never again, so it
  was a weak lens, cast no shadow and absorbed nothing. Every non-camera ray now traces `kPathMask`.
- **Every entry into a generated sphere shaded as an exit.** `kHitBack` came from the triangle's winding, and the
  generated sphere is wound inside out; it is now the ray running along the interpolated vertex normal. Before, a ball
  refracted the wrong way and absorbed outside itself.
- **The deferred transparent pass drew a solid's far side over its near side** (both faces drawn, no depth write
  between them); far-side fragments of a solid are discarded.
- **The walk lost energy:** its roulette (0.99 a step) cut white walks; rays from TMin 1e-4 stepped through the boundary
  unseen at small free paths.
- **The realtime profile's 2 bounces ended paths inside glass** once bounce rays met it (the realtime furnace's rough
  glass rim read 0.62).

- **The SSS target first packed the irradiance as fp16** (`f32tof16`), which declared SPIR-V's `Float16` and `Int16`;
  the device enables neither, and `engine_tests`' validation case reported 6 messages. It is three floats' bits now.
- **`check_docs` timed out** (1200 s) running `material_tests` (1314 s alone; its chi-square and furnace cases 100-210 s
  each) and `openpbr_tables --check` (266 s), both also CTest suites of their own: their AGENTS.md lines are `# slow`
  (run by `check_docs --slow`); `check_docs --cpu-only` takes 733 s.

## 4. Spec and plan corrections

None to the spec. Step 3's change record listed O14 as not built and the deferred refraction as one screen-space
offset; step 4's listed the specular blur and the walk's colour noise. Both records' gaps now point here.

## 5. Results

- **The walk:** `subsurface_pt`'s luminance noise (per sphere, `sss_stats.py`'s measure) 9.6 to 2.2, colour noise 7.0 to
  0.5; the spheres' means within 1-2% of deferred. Furnaces (rt_offline): `subsurface_short`, `subsurface_long` and
  `subsurface_dense` within 0.1% of 1.
- **Deferred SSS furnace:** 0.9985 (0.996-0.997 before).
- **The lens:** on the glass scene with Frosted at roughness 0.02, the boundary where ball 2 shows the yellow and the red
  of what is behind it falls at pixel 55 in an exact analytic trace, the path tracer and deferred alike (71 before).
- **Glass furnaces:** rt_offline `glass_r005` 0.9997 / 0.9993 (face / rim), `glass_r05` 1.0063 / 1.0083, `glass_thin`
  0.9988 / 0.9988; rt_realtime 0.9997 / 0.9987, 1.0061 / 1.0016, 0.9986 / 0.9938; deferred 1.0000 / 1.0000 each. The
  realtime closed box x0.9860 of its closed form.
- **Goldens:** section 8.

## 6. Known gaps

- **The realtime cache's update paths still stop at refraction:** near glass the cache holds less light than it should.
- **Rough thin walls** (roughness over 0.05) still block shadow rays; their light arrives by bounce rays alone.
- **Deferred glass reads the frame's copy:** a point it finds may be hidden on screen by something nearer, and glass
  behind glass is not seen.
- **The pane loop doubles the path tracer's pipeline compile** (a stubbed loop compiled in 175 s cold against 337 s, on
  a machine with other processes using about five cores; the tree at `e7da95c5` took 187 s there).
- **The realtime determinism sensitivity** (step 3's gap) is still unexplained.

## 7. How to run

```
python tests/renderer/check_material.py
python tests/golden/render_golden.py tests/golden/manifest.json --only cli_glass_pt
build/Release/ohao run assets/scenes/glass.scene.json --render out.png --mode rt_offline --spp 512 --denoise none
```

## 8. Goldens

A full run (42 scenes) after these changes: 38 passed unchanged (deferred exact; path traced within the BVH floor), 4
changed, each looked at against the old image and updated with a note:

- `cli_glass_deferred` and `cli_glass_reference` (max 193 levels on 5.8% of the frame, all inside the balls): the balls
  show the backdrop upside down, as the path tracer does; the frosted ball blurs as the path tracer's does (the blur was
  `0.06 x roughness^2` of the frame's height, about 3 px at roughness 0.35; it is now each interface's turn,
  `alpha (1 - 1/eta)` in and `alpha (eta - 1)` out, times the distance after it, projected at that point's depth, 16 taps).
- `cli_glass_pt` (max 188 on 54%): the old golden showed the balls as mirrors (every entry shaded as an exit); they now
  refract, the amber ball absorbs, and caustics and shadows lie under them.
- `cli_transparency_pt` (max 21 on 85%, noise only): bounce rays meet the blended panes.

The three subsurface goldens were re-captured earlier in this work (their notes in the manifest).
