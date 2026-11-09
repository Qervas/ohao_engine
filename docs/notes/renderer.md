# The renderer: what has cost time

Design rules and findings for `ohao/render/` and `shaders/`: read the parts you touch before changing them. Moved from
`AGENTS.md` (system design step 2h, 2026-11-09), unchanged. Rules a build check now holds (bindings, push constants,
a `[shader]` attribute's place) are stated in `AGENTS.md`.

- **Alpha modes** (`render::AlphaMode { Opaque, Mask, Blend }`, glTF's `alphaMode`; primitives'
  `Material`'s `alpha_mode`/`geometry_opacity`): coverage is base colour alpha x the albedo texture's x
  `opacity`, in material row 3 (`mode, cutoff, opacity, base alpha`). A `Mask` draw (`kDrawMask`) is in
  no G-buffer cull list: the G-buffer draws it after the late lists with `core/gbuffer.slang`'s `masked` (a
  discard, so no forced early depth test). A `Blend` draw (`kDrawTransparent`) is in no list at all,
  cascades included: the `transparent` pass draws it. In the TLAS an instance with an alpha material is
  `FORCE_NO_OPAQUE`; inline ray queries test each candidate in `rt/alpha_test.slang` (mask: the cutoff;
  blend: never a hit, so it casts no shadow and bounces no GI), the path tracer in its `anyHit` (blend:
  stochastic opacity). A new trace must not force opacity on those instances or it shadows through
  every hole. Windows' `OPAQUE` macro is gone from this path (the enum is `Opaque`).
- Descriptor sets written at record time need one set per frame in flight
  (`MAX_FRAMES_IN_FLIGHT` = 3), or a frame rewrites the set the previous one reads (VUID 03047).
- A window presents path-traced frames too: `renderRTPipeline` acquires, blits the tracer's
  RGBA8 output (`pt.present`, which follows the tracer's passes because the pass registry is
  in frame order) and presents like the deferred path. What a window presents is a launch
  option (`ohao serve --window --mode`, `ohao_game --mode`); a lockstep capture in another
  mode switches, renders, then restores the live mode, denoiser, quality and post. The path
  tracer does not notice a moved camera itself: a host calls `notifyCameraChanged()`
  (`LiveHost` does whenever the world changed).
- Readback is on request only: `requestReadback()` before the `render()` you want,
  `readback()` after (that frame's fence only; a path-traced frame under OIDN or NRD is
  denoised there), or for an inline preview of a deferred frame
  `requestPreviewReadback({rect, maxSide})` and `readbackPreview()`: the `capture.preview`
  pass box-filters the final image straight into the staging buffer and nothing else is
  copied. No other frame copies anything, so a read after an unrequested frame is empty.
  Buffers the CPU reads must be host-cached memory: uncached host-visible memory read one
  1280x720 frame in ~20 ms. A shader writing such a buffer over PCIe wants a warp's writes
  contiguous (`capture.preview` in 8x8 groups took 0.49 ms, in rows of 64 0.056 ms).
- G-buffer and CSM draws are GPU-culled indirect draws (`GpuCulling`): firstInstance is the
  draw's index in `RenderScene::drawTable()` (`DrawRecord`), not the instance slot; a vertex
  shader reads `draws[gl_InstanceIndex].draw.w` for the slot. A draw the culling never lists
  is never drawn, so a culling bug hides geometry: compare a capture with culling on and off
  (`render.settings`), which must be the same image exactly. A mip chain halves rounding down: a pyramid whose levels assume rounding
  up reads past them (the Hi-Z is padded to powers of two for that reason).
- A G-buffer phase has four lists: instance one-sided, instance double-sided, meshlet
  one-sided, meshlet double-sided (`kCullListEarly`/`kCullListLate` + 0..3; a cascade has one,
  `kCullListCascade0 + c`, every draw instanced). A meshlet list's command is a task dispatch
  whose fourth word is the draw's index (the task shader reads it through `SV_DrawIndex`, which
  needs `shaderDrawParameters`); the fallback's per-meshlet draws carry the draw in firstVertex
  (`<< kMeshletVertexShift`) and the meshlet in firstInstance. `geometry: auto` sends draws of
  `kMeshletMinTriangles` (4,096) or more to meshlets: below that the meshlet path measured
  slower. Its images must equal the instance path's exactly: both transform vertices with one
  function, `includes/scene/gbuffer_vertex.slang` (`MatrixInverse` through `spirv_asm`; Slang has
  no `inverse`), which `core/gbuffer.slang` and `cull/meshlets.slang` call.
  Wireframe mode applies to instance draws only.
- `resetAccumulation()` never waits for the GPU: the next frame's passes start without
  history and clear what they keep in their own command buffer, after the frames in flight
  by queue order. Nothing a frame renders may depend on its frame-in-flight slot (TAA's
  jitter is indexed by the frames since the reset).
- Render settings are one struct, `render::RenderSettings` (`render/renderer/render_settings.hpp`, `render.settings`
  reads and writes it by reflection), read back from the subsystems that own them, never cached. Its defaults must
  equal what a fresh renderer starts with: a headless host reports them before its renderer exists and captures
  pass them explicitly (EV100 0.5: an exposure of 1.0 there once made headless captures brighter than the window's). A GI resolution or
  CSM size change rebuilds images after `vkDeviceWaitIdle`; nothing else idles. The GI
  shift (`flags.w`, 1 at half resolution) keeps the half-resolution expressions literally
  behind a branch: folding it into a multiply changed FMA contraction and moved goldens.
- Ambient occlusion comes only from glTF's `occlusionTexture` (`MaterialDesc::occlusionTex`,
  material row 2, w), never from a metallicRoughness map's R channel, which glTF leaves
  unused (reading it blacked out MetalRoughSpheres and had kept DamagedHelmet out of all
  ambient light). A packed ORM image is uploaded once and serves both.
- **Specular indirect light is RT GI's reflection** (frontier F5b): one more ray a pixel, its
  direction from the GGX lobe of the surface's roughness (bounded VNDF), shaded as GI's diffuse rays
  are (a hit's light, the cache, the environment on a miss), denoised by GI's chain with its a-trous
  spread by roughness (a mirror stays sharp), weighed in lighting by OpenPBR's specular albedo and
  the material's AO through `specularOcclusion`. Without ray tracing (the fallback) it is the split
  sum: `EnvPrefilter`'s images (`render/deferred/env_prefilter.*`, built at environment upload:
  GGX-prefiltered into `kEnvPrefilterLevels` mips read at `roughness * (levels - 1)`, and
  irradiance), times the environment's intensity, occluded by the material's AO. There is no SSR: the
  reflection ray sees what the screen cannot. Compare a metal against the path tracer before
  trusting its look.
- **A directional light stands for the sun.** While the scene has one
  (`RenderScene::hasDirectionalLight`), the HDRI is uploaded with its sun painted out
  (`render/rt/env_sun.hpp`: the peak's connected texels over 64 x the median sky, if the peak is
  over 10^4 x it and they cover at most 0.02 sr), so RT GI's rays, the deferred IBL and the path
  tracer light with the sky alone and the sun arrives once. GI's cosine rays met the outdoor
  HDRI's sun (60,450, 51% of its power on 0.003% of the sphere) about once in 10^4 rays: the
  outdoor fireflies (`docs/changes/2026-10-28-rt-gi-fireflies.md`). The log says what was taken
  out (`gpu` channel). A directional light added or removed reloads the map.
- **An equirectangular environment's row 0 is the zenith**, as the file stores it and `stbi_loadf`
  loads it: every lookup is `u = atan2(z, x) / 2pi + 0.5`, `v = 0.5 - asin(y) / pi` (deferred
  lighting, `equirectUV` in `clustered_lights.slang`, the prefilter's both ways, `gi_trace`,
  the path tracer's miss, the cinematic composite, `env_sun.cpp`), the same number as the path tracer's
  sampling (`envPdf` in `path_tracer.slang`, `acos(y) / pi`). A new lookup writes that expression; `asin(y) / pi + 0.5`
  turns the sky upside down (it did, everywhere, until 2026-10-29) and the unit tests cannot see
  it. `check_env_orientation.py` can: a sky blue above the horizon and orange below must light a
  floor blue in deferred (RT GI and IBL) and the path tracer.
- **Deferred and the path tracer light and show the same scene.** A scene without lights has
  none in either (no fallback sun: `setFrameLights` hands over the scene's raster lights only), and
  with an environment the deferred frame draws it behind the scene (lighting writes prefiltered
  level 0, the source, into sky pixels; without one they are black), as the path tracer's missing
  camera rays show it. `check_env_orientation.py` renders a scene with no
  light and checks the sky band in all three captures.
- **The camera sees no back face of a one-sided material, in either renderer.** glTF's `doubleSided: false` culls
  back faces in the raster; the TLAS disables facing culls (`TRIANGLE_FACING_CULL_DISABLE`), so the path tracer's camera
  ray passes such a face (`kHitCulled`: the material's row 10 w is 1, and the world-space winding faces away; a mirroring
  transform turns it) and goes on, up to `kCullSkips`. Every other ray meets both sides: one-sided geometry still
  shadows and reflects, in both renderers. Without it the path tracer saw the back of a Sponza arch the raster culls, and
  the referee read the arch's shadow on the wall behind as deferred being 18% dark.
- Reflected light is counted once: RT direct light reflects the lights (and emissive triangles),
  RT GI's reflection everything else (its rays skip lights' emitter meshes and drop what a hit
  emits when RT direct light samples it). A new reflection source must fit that split, never be
  added on top in post.
- Indirect diffuse light has one source per frame: RT GI's rays, else the environment's
  irradiance, else none (neither: no `VK_KHR_ray_query` or `rtGI` off, and no environment): the
  frame then has no indirect light, as the path tracer has none (frontier F5c: no fill; the only
  occlusion that is not traced is the material's AO texture). RT
  GI carries every bounce through its radiance cache (below). A test that wants indirect light without RT GI gives the scene an environment. Both are
  diffuse only, weighed by OpenPBR's diffuse directional albedo beneath the specular layer
  (`diffuseAlbedo`; the split sum's weight is `specularAlbedo`, both from
  `shaders/material/openpbr.slang` and its energy tables). RT GI's rays bounce off the hit
  triangle's own material: `rt/alpha_test.slang`'s `hitMaterial` resolves it at the hit's uv
  (textures included) and the hit returns its OpenPBR albedo at normal incidence over π times
  the direct light it receives, plus its emission.
- **The G-buffer has 9 attachments** (`GBufferPass::GBUFFER_COUNT`; depth is attachment 4,
  so the fragment shader writes 8 outputs, the GPU's limit of colour attachments, and a pipeline
  drawn in its render pass needs `GBUFFER_COUNT - 1` blend states). Emission is `kEmissive` (5, RGBA16F, output 4):
  `emission_color × emission_luminance × the emissive texture` (engine units, ruling O4), the
  same radiance the path tracer's closest hit emits, added by lighting unlit; the normal
  target's alpha keeps only its luminance, to keep emitters out of SSS. `kSpecular` (6, RGBA8:
  `specular_color`, `specular_weight / 1.5`, so 1.0 is code 170) and `kOpenPBR` (7, RG8:
  `base_diffuse_roughness`, `(specular_ior − 1) / 2.55`, so 1.5 is code 50) carry the rest of
  OpenPBR's base to `core/deferred_lighting.slang`; albedo is `base_color × base_weight`, and
  roughness is stored as authored (OpenPBR floors it at 0.01, nothing at 0.04). A back face is
  shaded on its own side: the G-buffer flips the normal when it faces away from the triangle's
  side the camera sees (`cross(dFdx(pos), dFdy(pos))`; `gl_FrontFacing` disagreed with the
  generated sphere's winding). `kLayers` (8, RG32_UINT, output 7) carries OpenPBR step 2's
  layers (`material/material_layers.slang` `packLayers`): the material's index (12 bits), the
  anisotropy and its tangent's angle in a basis made from the decoded normal, the coat's and the
  fuzz's weight and roughness; their colours, IORs and darkening come from the material table by the
  index (ruling O6), so lighting, RT direct light and RT GI bind the table and that target. There
  is no room for another target: what a later lobe needs goes into the table or a visibility buffer
  (ruling O3).
- **Glass** (OpenPBR step 3, `docs/changes/2026-11-01-openpbr-step3.md`, gaps closed in
  `docs/changes/2026-11-01-openpbr-gaps.md`): `transmission_weight` refracts the dielectric's share under its specular
  (`OpenPBRBase.evalTransmission`, lobe 4; white glass passes all of it), from either side of a solid (the path tracer
  shades a back-face hit, `kHitBack`, in its outward frame) or straight through a thin wall; material rows 12-14
  (`applyTransmissionRows`; `kMaterialRows` is 15). **A transmissive material reads as Blend on the host**
  (`MaterialTable::alphaMode`), so its TLAS instance carries `kTlasMaskTransparent` alone: **every path tracer ray but the
  camera's traces `kPathMask` (`kRayMaskScene | kTlasMaskTransparent`)**, or bounce and shadow rays pass glass unseen (one
  refraction, no lens, no shadow). `kHitBack` is the ray running along the interpolated vertex normal (the geometric
  winding only without normals: the generated sphere is wound inside out, and every entry into a ball shaded as an exit).
  **Glass has its own bounce budget** (`kCrossings`, 16: refractions and reflections inside a solid, beyond `bounces`; the
  realtime profile's 2 bounces ended paths inside a ball). In the path tracer a ray that crossed a surface counts what it
  meets in full, not by MIS (light sampling only looks above the surface), and a solid's inside absorbs by
  `transmission_color` at `transmission_depth`. **Panes pass shadow rays** (ruling O14, `paneMaterial`: thin-walled,
  transmissive, roughness at most `kPaneRough` 0.05): on a shadow ray `anyHit` multiplies the pane's `transmissionAlbedo`
  at the ray's angle into the payload's `emission` (1 at the start) and ignores the hit, and `visible()` hands it back as
  `T`, which every light sample multiplies (the BLAS geometry is `NO_DUPLICATE_ANY_HIT`, so each pane counts once; never
  carry a shadow ray's result in `flags`, which the miss shader clears); a bounce through a pane is a null crossing for MIS (`misOrigin`, `lastPdf` kept). The radiance cache is
  never read at glass. **Deferred glass is traced** (`vfx/transparent_mesh.slang`): the fragment refracts into the
  solid, finds its own instance's far side with a ray query (`glassExit`, committing only its own non-opaque triangles;
  total internal reflection up to `kGlassBounces` 4), refracts out, finds the opaque surface the ray meets, projects it
  and reads the lit frame's copy there (the prefiltered environment on a miss), spread over a disc by each interface's
  turn (`alpha (1 - 1/eta)` in, `alpha (eta - 1)` out) times the distance after it; Beer-Lambert over the length inside; a
  solid's far-side fragments are discarded (the pass draws both faces). Other RT rays pass glass dithered by its
  transmittance (`rt/alpha_test.slang`). **The light loop takes no light from below a surface** (`evaluateLight`):
  through glass it is in the refracted frame. **Orient a transparent pixel's normal by `cross(ddx(P), ddy(P))`, never
  `SV_IsFrontFace`.**
- **Subsurface** (OpenPBR step 4, `docs/changes/2026-11-01-openpbr-step4.md`): `subsurface_weight` takes the dielectric's
  diffuse slot. **The path tracer walks it** (lobe 5, `MaterialInputs.subsurfaceLocal` false in
  `inputsOf`) as a mode of raygen's path loop (`walkSteps`): each step's channel is drawn in proportion to the walk's throughput (spectral MIS, Cycles' scheme: the
  scatter weight `albedo sigmaT tr / dot(p, sigmaT tr)`, the exit's `tr / dot(p, tr)`), roulette only once the largest
  weight is under 1 (a white walk is never cut), rays from TMin 0 (a step must not pass the boundary
  unseen), at most `kWalkSteps` 1024 steps, then out along its direction. The exit is a Lambertian vertex: light and the
  environment are sampled there by MIS, by the loop's own light sampling. **Raygen has one trace and one shadow ray**:
  every `TraceRay` site inlined into raygen spills its live state for every pixel, so a walk of its own beside the loop
  made the whole tracer 8x slower on scenes without subsurface (213 ms against 27 on the helmet room); the light's and
  the environment's samples share the vertex's one shadow ray in its weighing loop ("Every call site is compiled
  whole", below). A new feature in raygen reuses those sites; `OHAO_PIPELINE_STATS=1` logs raygen's stack (`raygen stack N bytes`),
  the cost's measure (callable shaders cannot trace in Vulkan). **Every other reader shades a local diffuse lobe** in `subsurface_color` (the
  default), and the deferred frame's SSS pass spreads it by Burley's profile at the pixel's free path, read from material
  rows 15-16 through the layers target (`kMaterialRows` is 17). **The SSS pass blurs the diffuse alone:** lighting writes
  a second target for subsurface pixels (`SV_Target1`, `R32G32B32A32_UINT`: the diffuse irradiance as fp16, a mask, the
  diffuse albedo, `packSubsurfaceDiffuse` in `clustered_lights.slang`), the horizontal pass blurs that irradiance, the
  vertical one adds `albedo x (blurred - own)` to the lit pixel, so the specular stays sharp. The tiles are still flagged
  by the G-buffer normal's w (minus the weight).
- **A `[shader(...)]` attribute must sit directly above its entry function.** CMake finds entry points by parsing; an
  attribute above anything else (a struct) once left a shader with no build rule and its old SPIR-V in use for a whole step,
  unnoticed by `ohao doctor`. The configure step now fails when a file's attributes and parsed entries differ.
- **OpenPBR's layers** (step 2, `docs/changes/2026-10-31-openpbr-step2.md`): every renderer shades
  `OpenPBRSurface` (`material/openpbr.slang`: fuzz over coat over the base), built from
  `MaterialInputs`, whose step-2 fields a reader fills from the table's rows 7-11
  (`applyLayerRows`, with textures). **A `MaterialInputs` must start from
  `defaultMaterialInputs()`**: Slang leaves unset fields undefined, and a garbage coat weight
  layers a coat on anything. A material with neither coat nor fuzz runs the base's code
  unchanged. Anisotropy (`specular_roughness_anisotropy`, the coat's) stretches GGX along the
  shading frame's x, the mesh's tangent turned by `geometry_tangent_rotation`; an anisotropic
  lobe reads the isotropic energy tables at `AnisoRough`, the roughness whose E1 is its own, and
  its Fresnel-weighted moments from `AnisoS56` (tables generated by `openpbr_tables`). Under a coat RT GI's one reflection ray follows the
  coat (ruling O8) and the base's specular takes GI's mean incoming radiance.
- **The referee.** A renderer is judged against the closed form first, then the path tracer:
  `check_analytic.py` renders a white floor under one sun, point or spot light (no other surface,
  so no bounce) in both renderers through `ohao run --hdr` and holds each within 5% of
  `albedo/π × irradiance`. Compare renderers in linear radiance (`--hdr`): their PNGs
  share one display transform, but deferred's carries post (bloom, TAA) the path tracer's does not.
  The path tracer is ground truth where closed forms hold it: `check_analytic` (one light, no
  bounce) and `check_material.py`: a white furnace (white OpenPBR materials under a uniform sky
  of radiance 1 render as 1, within 1%) and a closed box of emitting Lambertian walls
  (`Le / (1 - rho)` with every bounce, `Le (1 - rho^(N+2)) / (1 - rho)` with `--bounces N`).
- **The path tracer** is `shaders/rt/path_tracer.slang` (`PathTracerShaderSet`'s default, both
  profiles), a correct estimator of OpenPBR's base
  (`shaders/material/openpbr.slang`): one path a sample, light and environment sampling weighed
  against BSDF sampling by the power heuristic, Russian roulette from the third vertex, 32
  bounces. Its random numbers are Owen-scrambled Sobol (Burley 2020, `shaders/sampling/sobol.slang`):
  draws come in groups of four, one 4-D point each, seeded by the pixel and the group; a bounce's
  draws sit in fixed groups (light, environment, BSDF and roulette), so a branch never shifts the
  next bounce's dimensions. A new draw goes into a group (`Sampler.group`), never after another
  bounce's.
- **The display transform** (frontier F2 T1, `shaders/display/display.slang`) is the only way a
  frame becomes pixels: exposure from EV100 (`render.settings`/`render.capture` `ev100`, default
  0.5; radiance `L` shows as `L / (1.2 x 2^EV100)`, Lagarde and de Rousiers 2014), the curve
  (`tonemap`: `agx`, the default, or `neutral`, Khronos PBR Neutral), then the sRGB transfer
  function. The deferred post (`postprocess/tonemap.slang`), the path tracer's output,
  `nrd_compose` and DLSS's display pass import it; the post stack owns EV100 and the curve and the
  path tracer reads them each frame (`setDisplay`), so a capture in any mode displays alike. OIDN's
  CPU frames use the C++ twin `render/display/display.hpp`, which must match the module (change
  both together). A new path that shows a frame
  imports the module; never write a curve or `pow(1/2.2)` again.
- **The physical sky** (frontier F2 T3, `render/sky/atmosphere.*`: Hillaire 2020, Earth's
  atmosphere in RGB, on the CPU) is the environment of a scene without an HDRI: `Environment.sky`
  (default true; a world without an `Environment` has the default) lit by the scene's first
  directional light, whose colour x intensity is the sun's illuminance outside the atmosphere.
  The renderer renders it into a 512x256 equirectangular map (row 0 the zenith) and uploads it
  exactly as an HDRI (`environmentKey()`: `file:` or `sky:<sun>`), so the CDF, the IBL prefilter,
  RT GI's and the path tracer's misses read it unchanged, and both renderers show and are lit by
  the same sky. No sun, or `sky: false`: no environment (black), and an environment that goes
  away is unloaded. A moved sun re-renders it (about 30 ms with the device idle): a day cycle is
  not cheap yet. The map has no sun disc (the directional light is the sun). The referee's
  scenes that need no environment say `"Environment": {"sky": false}` (`check_analytic.py`).
- **Auto exposure** (frontier F2 T2, `render/deferred/auto_exposure.*`, `postprocess/exposure.slang`, the
  `exposure` pass before `post.tonemap`): a window's deferred frames are metered
  (`render.settings autoExposure`, on in windows, `LiveConfig::autoExposure`; `exposureCompensation`
  in EV). A 128-bin histogram of log2 luminance over [-10, 20] holds each bin's exact sum in
  integers (1/16 EV: order-free, so repeatable), and one thread meters the mean between the 50th and
  95th percentiles to middle grey 0.18 (`EV100 = mean + log2(1 / (0.18 x 1.2)) - compensation`),
  easing toward it at 3/s brighter, 1/s darker. The EV100 never leaves the GPU (the display pass
  reads it); `render.stats` reports the newest finished frame's as `ev100`. **Captures never meter**
  (`captureFrame` turns it off; a window restores it), so goldens stay at their EV100. The
  shader's `displayExposure` runs only for metered frames; a capture's exposure comes from the
  host (`render::displayExposure` in C++). A realtime path-traced
  window is not metered yet (its display runs in the tracer's own passes).
- **Realtime denoising and display** (frontier F1 T4). A realtime window's denoiser is DLSS Ray
  Reconstruction where the build has the SDK (`python tools/fetch_dlss.py` into the git-ignored
  `external/DLSS`; never commit it: NVIDIA's licence) and the GPU is NVIDIA's, else NRD
  (`VulkanRenderer::rtDefaults`); captures default to `denoise: none`. DLSS RR upscales: it traces
  at its preset's size and writes the output's (`OHAO_DLSS_QUALITY`: `quality`, the default, 2/3 a
  side; `dlaa` native, `balanced`, `performance`, `ultraperf`), so the tracer's images are at the
  render size and only DLSS's output and the frame's are at the output's. Every denoiser writes the
  frame's output (`m_outputImage`) through the display transform, so a window presents what a capture
  reads. Under NRD the camera ray's
  emission (lights, emissive surfaces, the sky) goes to its own guide (binding 13) and never
  through NRD: `nrd_compose` adds it to diffuse x albedo + specular. The first hit's direct light
  is split by lobe (`evalLobes`). NGX logs through the engine's logger (stdout is the protocol's).
- **Path guiding** (sub-project 23, `docs/specs/2026-11-02-path-guiding-design.md`; in progress, **off by default**:
  `ohao run --guiding`, `render.capture {guiding}`). Alber, Hanika and Dachsbacher 2025's Markov chain states in a hash
  grid (`shaders/rt/path_guiding.slang`, `render/rt/path_guiding.*`), read at an opaque vertex into a mixture of vMF lobes
  (`sampling/vmf.slang`) that the bounce draws from with probability `OHAO_GUIDE_PROB` (0.5) and weighs by the mixture's
  exact density, so any grid content is unbiased; learned after the trace from the path's own light
  (`path_guiding_update.slang`: update, owner, write; deterministic). The paper's authors' code is GPL-2.0: never read it.
  **Russian roulette at a guided vertex uses `throughput * max(rrRatio, 1)`**, `rrRatio` the product of `q / p_bsdf`:
  roulette by the true throughput killed exactly the paths guiding aimed at a small source (a large `q`, a small weight).
  Unguided, `rrRatio` is exactly 1 and the tracer is bit for bit what it was. **Guiding is compiled into its own
  raygens only** (`tracePixel`'s `GUIDING`: `raygenGuided`, `raygenCausticsGuided`, each a program made the first time
  a frame guides, `PathTracer::ensureGuidedProgram`): its code behind the flag in the default raygen, off, was 18 ms of
  the helmet's realtime `pt.trace` (`docs/changes/2026-11-06-pt-trace-bisect.md`). Code the default path never runs
  belongs in a variant, not behind a flag. Measured: the spot-lit patch room 1.8x lower
  error at full depth; the Sponza showcase about 1.4x higher (typical pixels), so it stays off.
- **Caustics** (sub-project 24, `docs/specs/2026-11-03-caustics-design.md`; step 1 built, **on by default** in `rt_offline` (ruling C1): `ohao run
  --no-caustics`, `render.capture {caustics: false}`, `render.stats` `caustics {casters, clamped}`). Light traced from each
  directional light (`causticTrace`, `rt/caustics.slang`, `render/rt/caustic_pass.*`) estimates every term sun -> smooth
  parts -> the rough part of the camera ray's first hit, and `raygenCaustics` (raygen compiled with the split) drops
  exactly those. **The camera side cannot render sun through glass at all** (light sampling looks above a surface only,
  and no ray hits a delta sun): the pass removes a bias there, not only noise. Gotchas:
  - `chainThroughput` takes **every factor `throughput` takes** (roulette, absorption, the dispersion channel), with
    `f_rough / pdf` at the first hit and `f_smooth / pdf` after; the pass evaluates OpenPBR **in the camera's roles**
    (`f(wo = toward the eye, wi = toward the light)`, the module is not reciprocal) and applies raygen's own bounce limit.
  - **`RenderScene::Draw::aabbMin/aabbMax` are the mesh's own bounds**; world bounds go through `Draw::world`. Taken raw,
    a moved caster was aimed at where its mesh sits untransformed (half a glass ball's caustic missing; `c3_sphere`).
  - **The caustics program compiles cold in about ten seconds** (`PathTracer: <name> pipeline in N s (T threads)` in the
    log: 10.0 s with other processes using four cores; about 8 minutes before every call site was made single, below),
    once per shader change: later processes load it from the pipeline cache file (below). Keep the caustics raygens free
    of a second material evaluation site.
  - `OHAO_CAUSTICS_SPLIT_ONLY=1` drops the pass's terms without running it (`c1_split_only`); `OHAO_CAUSTIC_RAYS` overrides
    the rays a pixel (`kCausticRaysPerPixel`).
  Measured on the showcase at equal time: the whole image's error 190x lower, the sun's glints off the helmet 33x their
  neighbours to 1.8x, the glass ball's 1,283x to 1x, typical pixels unchanged.
  **Step 2a** (`docs/specs/2026-11-04-caustics-step2-design.md`, `docs/changes/2026-11-05-caustics-step2a.md`): the sun
  through the core of a smooth reflection onto a rough reflection at camera depth 1-2 is solved for (`manifoldConnect`, its
  own program; `rt/manifold.slang`; casters gathered a meshlet a cluster by `pt.manifold.gather`), and `raygenCaustics`
  drops it (`chainOne`). Owned only at materials without a normal map whose every draw is gathered (ruling M7).
  `OHAO_MANIFOLD_OFF=1`, `OHAO_MANIFOLD_SPLIT_ONLY=1`; `OHAO_MANIFOLD_STAGE` for profiling. Gotchas: a slope's frame is
  fixed per cluster (`ManifoldTri.c`), or pruning tests a ring round a sphere; the pass's cost is its pruning, not its
  tests (time `pt.manifold` with `render.capture {timings}`, never wall time: a cold compile hides in it).
  **Step 2b** (same spec, "Step 2b as designed", M12): the sun through an owned solid (two refractions) onto X, which the
  camera side never renders (nothing to drop), by the seeded walk (`manifoldGlass`, `rt/manifold_cert.slang`'s
  `manifoldWalkPair<S, false>`; `manifold_tests` t15 holds its form to light traced through a glass ball; `check_caustics`
  m4-m6 on the GPU). **Opt-in, `OHAO_MANIFOLD_GLASS=1`** (since 2026-11-08; its tests set it): its re-find walks ran some
  frames for seconds, a dispatch the GPU cannot preempt (two firmware crashes came during them), until the work is bounded
  per dispatch. Without it nothing renders that light: the frame is darker, not split. Gotchas: it walks a whole instance, so one draw of each (instance, material) runs it (the
  host's mark in the draw record's word 7: a merged model's runs of one glass are several draws, which counted the light
  once each, `m7_split_ball`); it walks at the IOR the BSDF refracts at (`modulatedIor` of the specular IOR and weight,
  `m9_ball_weight`) and refuses a coat or a specular-weight texture, whose IOR no single walk has; under NRD the manifold
  pass (2a and 2b) stays off, since it adds to the running mean after the denoiser's inputs are packed; an uncertified
  root must be polished in its own pair's parameters and kept only inside both triangles, or a focus counts a root in two
  pairs; one frame reference for every walk of a request; the walk's dual-number map and the BSDFs each at one call site
  (manifoldConnect 36.5k instructions), the path checked before its re-find walks (`manifoldGlassPath`), the walk's rays
  clipped to the solid's bounding sphere.
- **The path tracer's frame cost** (frontier F1 T6) is timed by stage: `pt.cache.update` (with the
  frame's setup), `pt.cache.resolve`, `pt.trace`, `pt.denoise` (a denoiser and the display
  transform); `render_bench`'s `helmet_rt_1080p`, `play_rt_1080p` and `play_rt_dlss_1080p`. **Any
  buffer a shader reads at random must be device-local**: the environment's sampling table in host
  memory (read over PCIe at every draw) was 85% of a realtime frame. It is a 2D CDF
  (`render/rt/env_cdf.hpp`, binding 17: the marginal, then each row's conditional), uploaded through
  a staging buffer with the map. Keep the draw a continuous warp: an alias table was 1.1 ms faster
  but scattered neighbouring Sobol points across the map, 14% more error at 16 samples. A view of a 1080p render through an image tool may be palette-reduced and look
  posterized; judge renders from crops or pixel statistics.
- **Every call site is compiled whole** (sub-project 25, W1c). Slang inlines every call for SPIR-V (`[noinline]` is
  not honoured there), so a function reached from k sites is compiled k times, and the driver's cold compile grows
  faster than the code (about size^1.8: 62,000 instructions took 4.4 s, 1.1 million 11 minutes). OpenPBR's entries
  each build one `OpenPBRView` (the surface seen from wo) and call every helper from one site; a path vertex builds its
  view once, draws its light's, the environment's and the bounce's directions, then weighs them in one loop through one
  `terms` and one `pdfFrom` (nothing draws inside the loop, so every Sobol group and roulette's draw is as it was), and
  only then traces the light's and the environment's shadow rays, at one site in a loop of their own that multiplies
  each sample's light (kept before visibility: every term is linear in it) by what the ray brings back. **Nothing of the
  surface or its view may be live across a trace**: each holds an `OpenPBRBase` (49 values), and with the shadow ray
  inside the weighing loop they were live across it: the helmet's realtime `pt.trace` went from 68 to 53 ms on the RTX
  4060 when the shadow rays moved after the loop (`docs/changes/2026-11-06-pt-trace-bisect.md`). **A view of wo is the base's tables read once**: `OpenPBRBase.viewedFrom` also works out the material's
  constants (`vE1Avg`, `vMsD`, `vMsM`) and the albedos at the view's mu (`vEd`, `vEm`, `vSlot`), and the base's
  functions read them, so **call a base's functions only on a view of the same wo** (`OpenPBRSurface.view`/`baseAt`
  do it; a base no view made holds NaN there, which shows). A program's stages compile side by side: the pipeline is
  created through a deferred operation joined by 4 threads (`createRtPipelineJoined`; `OHAO_RT_COMPILE_THREADS` 1-8:
  cold, the default program 13.7 s on 1, 8.5 s on 4; caustics' 18.3 s and 10.0 s). raygen went from 1,101,869 SPIR-V
  instructions to 27,895 and the default program's cold compile from 11 minutes to 8.5 s, with Unreal Editor and
  others using three to four cores. **Never give an OpenPBR entry a second call site in a kernel**: weigh a new
  direction in the vertex's loop. Count a kernel's instructions with `spirv-dis --no-color X.spv | grep -vc '^\s*;'`.
- **A cold compile happens once per shader change, for every executable.** Our `VkPipelineCache` file
  (`render/rt/pipeline_disk_cache.*`: per pipeline, shader version, device and driver, under `$OHAO_PIPELINE_CACHE_DIR`
  or `%LOCALAPPDATA%/ohao/pipeline_cache`) holds the path tracer's programs since they became small (W1d): a fresh
  executable loads each in 0.1-0.2 s, and so does a second process beside a live one holding them (both measured
  2026-11-05). At the old size NVIDIA ignored the file and kept the binary in its own cache
  (`%LOCALAPPDATA%/NVIDIA/DXCache`), one per executable, so every executable compiled the path tracer cold and a second
  process beside a live one compiled again (625 s); `VK_KHR_pipeline_binary` was refused across executables. A truly
  cold compile, for timing: a fresh copy of the CLI with `OHAO_PIPELINE_CACHE_DIR` set to an empty directory.
- **Emission is light** (frontier F3, ruling F-R7; `render/rt/emissive_lights.*`). Every triangle of an
  emissive material (`MaterialTable::emissive`: emission colour and luminance above 0) is a light of the
  path tracer: the `emissive.lights` pass (before `pt.cache.update`) writes each one's world corners, uvs
  and material (`rt/emissive_collect.slang`, bindings 42 and 45) and its power, pi x luminance x area
  with the emissive texture's mean over it (one sample at the uv centroid at the mip matching its uv
  area, RTXDI's estimate), into an R32F image whose mips sum 2x2 (`rt/emissive_reduce.slang`, binding
  43). A light sample picks the analytic lights or the triangles by power (`analyticShare`: the analytic
  lights' power, `RenderScene::analyticLightPower`, over the whole), a triangle by descending the mips,
  then a point uniformly on it; a BSDF ray that hits an emissive triangle (`kHitEmissive`, its light
  index `emissiveFirst[InstanceID()] + PrimitiveIndex()`) is weighed against that by the power
  heuristic. A triangle whose estimate is 0 is never drawn, and its BSDF hits count in full, so an
  estimate only moves noise, never the mean. Light samples are drawn from the shadow ray's origin:
  drawn from the shading point, a triangle in the surface's own plane was taken as facing it. The
  table is the `VulkanRenderer`'s, recorded by whichever renderer draws a frame: the deferred
  frame's RT direct light samples the same triangles (below), and RT GI's bounce then drops what a
  hit emits (`GIInput::emissionIsDirect`), else it would count twice. There is no proxy light.
  `check_analytic`'s `emissive_quad` holds both renderers to a rectangle's form factor. The path
  tracer's selection by power ignores distance: the helmet's emissive panels draw samples from its
  point lights.
- **The radiance cache** (frontier F1 T3, `shaders/rt/radiance_cache.slang`, `render/rt/radiance_cache.*`;
  SHaRC's design, our code: RTXGI's licence bars shipping its source in this MIT repo). The realtime
  profile ends a path at a vertex past the first in a world-space hash grid of reflected radiance
  (cells of 0.05 m x 2^level, level = log2 of the distance from the camera, keyed by the side the
  surface faces) when the segment is a voxel long and the last lobe spreads over the voxel; a
  sparse update trace (`cacheUpdate`, 1 pixel in 5x5) fills it every frame and a resolve pass blends
  and evicts. **Integers only**: radiance is summed x1024 by `InterlockedAdd` (no float atomics; the
  sum is order-free, so two captures in one process give the same bits) and `resetAccumulation`
  empties it, so a capture repeats in any host up to the BVH floor (below): across processes an
  ulp moved at a shared edge can flip one sum's rounding, and the cache spreads it over the frame
  (about 1 run in 10 on the closed box, 0.25% mean; `check_material` repeats in one process). The offline profile never reads it (its slot holds one entry: the
  shaders bind it either way). The realtime closed box reaches its closed form within 3%
  (`check_material`); realtime Cornell is within 0.3% of the reference. Push constants are shared by
  the bind points: anything recorded between pushing the trace's constants and tracing that pushes
  its own (the resolve) must push the trace's again. A bounce ray (`kPathMask | kTlasMaskEmitter`) that meets a light's emitter mesh
  weighs it by the light's index in `InstanceGpu.emitterLight.w`; a light without a mesh (a
  point of radius 0, emitters off) is flagged by `LightTable::setEmitterLights` and sampled at
  full weight. `--bounces N`
  (`render.capture {bounces}`) for the referee: 0 direct light only. The realtime profile is the
  same tracer at 2 bounces with NRD by default, and `check_material.py` holds it to the same closed
  forms. **Two clocks:** the trace's running mean (`pc.control.y` frames) restarts when the view
  moves (`notifyViewChanged`), but a temporal denoiser's history (`m_historyFrameCount`) restarts
  only at `resetAccumulation`, reprojecting through the motion vectors, which are written whenever
  last frame's view exists (`pc.control.z`). Under NRD (realtime) or DLSS RR the trace hands the
  denoiser this frame's sample alone (`tracerAccumulates`), and only then does the firefly clamp
  apply; offline NRD denoises the mean of a still view (`tracerAccumulatesAOVs`). A `--denoise`
  override reaches the tracer whatever it is. Its floor
  across hosts: a ray exactly on a shared triangle edge may take either triangle (the driver
  builds BVHs per process), and the reference tests paths against the hit triangle's geometric
  normal, so a few pixels may differ by any amount between a window and a headless host (one
  process repeats itself bit for bit).
- **RT GI's denoiser** (`rt/gi_denoise.slang`): a reprojected short (4 frames) and long (32) history,
  four a-trous steps (1, 2, 4, 8) on the diffuse and two on the reflection, a joint-bilateral upsample.
  Two rays a pixel are heavy-tailed (a few find a bright patch), so **never clamp the long history to the
  short one's neighbourhood range**: a few frames of 3x3 rarely hold the bright samples, the range's top
  sits below the mean, and the clamp darkened every wall 10% (the referee's helmet 0.925, spheres 0.906,
  now 0.982 and 0.969). It is clamped up to the 3x3 minimum and down to twice the maximum. Two a-trous
  steps left the noise as blotches some 20 pixels wide on flat walls (the helmet room's left wall: 1% at
  frame 60 now, the reference 0.5%).
- **RT GI carries every bounce** (frontier F5a, `gi_trace.slang`): a GI ray's hit reflects its OpenPBR
  albedo x (its direct light, `directAt` at the hit's geometric normal from the triangle's vertices,
  `hitGeometricNormal`, + what one more cosine ray brings back: the radiance cache's cell where it
  lands, at least a voxel away, and that surface's emission, or the environment). A pixel's first ray
  computes it and adds it to the hit's cell; its other rays read their hit's cell (computing only
  where it is empty or within a voxel). The cache is RT GI's own `RadianceCache` (the path tracer's
  class: bindings 12-14, resolved after the trace, a history of 8 samples, cleared by a temporal reset
  and by a reference run's start), so each frame carries light one bounce further: a still view
  converges to every bounce, slowly where a cell takes few samples a frame (the closed box at 320x180:
  0.937 of `Le / (1 - rho)` at 32 frames, 0.986 at 256). Pixels nearer than 0.3 m (the first-person view
  model, drawn remapped) read the cache but never feed it. Direct light at the hit's true normal, not
  "facing the ray": that approximation darkened every bounce (the referee went 0.926 to 0.967 on Cornell).
- **RT GI's bounce sees every light** (`gi_trace.slang` `directAt`): up to `kGIWeighedLights`
  (16) each is weighed in one pass, past that `kGILightCandidates` (8) are drawn from an alias
  table in `GILight.cone.zw` and resampled; one shadow ray either way. `check_analytic.py` holds
  it at reference quality: spots aimed at a wall must bounce, together, what one spot of their sum
  does and what each does alone. A one-pass reservoir must always take the first light that gives
  any: a GPU's `w / w` can round below 1 (it left pixels with no light chosen, NaN, which
  reference quality's running mean keeps and TAA and bloom spread).
- Point and spot lights mean the same in both renderers (Frank's ruling, 2026-10-26): a
  light of intensity `I` lights a surface `d` away with `I/d²`, windowed to exactly 0 at
  its `range` (default 10 m) by `(1-(d/range)⁴)²`; a directional light of intensity `I` gives a
  surface facing it irradiance `I`. The path tracer's sphere emitter has
  radiance `I/(πr²)` (`path_tracer.slang`'s light sampling); albedo is base colour × albedo texture
  in both, never the vertex colour (primitives carry their colour as both). An area light
  is the same rectangle in both (Frank's ruling): uniform radiance `I/(4πr²)`, one-sided
  toward `edge1 × edge2`, no range and no `1/d²`; deferred lights it as light type 3
  (`evaluateRect` in `includes/lighting/clustered_lights.slang`: the exact polygon irradiance,
  the specular by linearly transformed cosines per lobe, `ltcQuad` over the energy tables' fitted
  `Ltc`; only where RT direct light does not light it: the fallback, the transparent pass, lit
  particles), clustered lighting lists it in every froxel, RT GI's
  bounce sees a cosine-profile point at its centre. Both renderers shade with the same BSDF,
  OpenPBR's base (`shaders/material/openpbr.slang`); what stays between them is one GI bounce
  against many and the representative-point specular of sphere and area lights
  (`docs/changes/2026-10-29-openpbr-step1.md`).
- Every point, spot and area light is also a mesh the camera sees (`RenderScene::setLights`
  makes an emitter instance per light: a sphere of its radius, or its rectangle, with the
  light's radiance, `kInstanceEmitter`). It is pure emission and changes no other pixel: its
  TLAS mask (`kTlasMaskEmitter`) is in the path tracer's camera and bounce rays' masks only (a
  bounce ray that meets it is weighed against the light's sampling, so it counts once), so no
  shadow or GI ray hits it (else the light would shadow itself or be counted twice); no CSM cascade draws it
  (`kDrawEmitter`); the
  G-buffer marks it with emissive alpha 1 and metallic −1, so lighting outputs its radiance
  alone, and RT direct light and GI skip it as sky. A new trace passes
  `kRayMaskScene`; a new screen-space pass that reads the lit image must decide what an
  emitter pixel is to it. `setLightEmitters(false)` turns the meshes off (the lights stay).
  A mesh moves with its light and writes motion vectors as any mesh does: the extract passes
  the light entity's `prevWorld * inverse(world)` as `LightDesc::lastFrameMotion`, and the
  emitter's prevWorld is that times its world (identity, so prevWorld = world, when still).
  Mirrors reflect it: in deferred a point or spot light's specular is its sphere's
  (`LightGpu.params.z` = the mesh's radius, `evaluateSphere`: the representative point,
  the lobe widened to `a + r/(2d)`), on screen or off. A spot's mesh shines toward the camera by
  its cone (`RenderScene::setViewerPosition`, each frame): dark from outside it, in both
  renderers; its reflection uses the same per-frame value in the path tracer.
  Bloom is by energy (frontier F2 T4): no threshold, no clamp. The frame goes into the pyramid
  as it is (`postprocess/bloom_input.slang`), the chain is normalised by its level weights
  (`BloomPass::levelWeightSum`) and the display pass shows `(1 - s) frame + s bloom`
  (`PostProcessingPipeline::kBloomStrength`, 0.04): energy moves, none is added, so a frame of
  one radiance is the same with bloom on and off. The first
  downsample's Karis average keeps a small mesh at radiance ~19,000 from flooding the frame.
  Lighting never reads bloom; tone glare there, never the mesh's radiance.
- A deferred capture renders 1 frame by default (10 with TAA), but the hybrid's RT GI
  keeps a history: it traces half resolution at 2 rays a pixel and accumulates (up to 32
  frames), so frame 1 and frame 10 of Cornell differ (max_abs 10 at 1920x1080). Pass
  `frames` for a converged image; the deferred goldens state theirs (`--frames 10`,
  `--frames 30`). Without `VK_KHR_ray_query` there is no RT GI. With it, lighting drops its
  diffuse IBL term: GI rays that leave the scene bring the environment back, occluded.
- **RT direct light** (frontier F4 and F4b, `render/rt/rt_direct_technique.*`, `rt/restir_di.slang`,
  `rt/di_denoise.slang`): ReSTIR DI (Bitterli et al. 2020) over the path tracer's lights (`rt/lights.slang`,
  one module both import: every light but flashes, and the emissive triangles), in MegaLights'
  structure: resampling and tracing at half resolution (`rtDirect.resolution`; one pixel of each 2x2
  block, picked by a hash each frame, stands for it), shading and denoising at full. `di.sample`:
  `rtDirect.candidates` (8) light points by power (`drawLightPoint`: a point on the light, so a
  neighbour re-evaluates the same point) kept by RIS against a cheap target (luminance of Le x
  Lambert under GGX x G from a compact surface: RIS needs the shape, W carries the rest), last
  frame's reservoir where last frame's surface is the same (M capped at 20x), one visibility ray.
  `di.trace`: `rtDirect.spatial` (2) neighbours within 30 frame pixels, the final shadow ray.
  `di.shade`: every pixel shades two of its 4 nearest trace pixels' samples (drawn by their bilinear
  weights, on its own surface) with the exact OpenPBR lobes, diffuse and specular each over its
  directional albedo (lighting multiplies them back); a sample shades at most 4x the target it had
  at its trace pixel (1x from another surface), else a grazing column's sample lit the ceiling beside
  it 100x over. `di.denoise`: a reprojected accumulation (16 frames) and three 3x3 a-trous steps
  stopped by luminance once a pixel has 4 frames (SVGF, the larger of the two pixels' deviations:
  symmetric, or bright pixels take dark neighbours that refuse them and the frame darkens); the
  specular spreads only as far as the surface is rough. **Shade's speed is its occupancy**: 72
  registers (`OHAO_PIPELINE_STATS=1` logs every compute pipeline's; a ray query there cost 32 more,
  the view's table lookups hoisted 16, OpenPBR's layers 54), so it is two kernels: `shade`, and
  `shadeLayered` (126), dispatched only while the material table holds a coat, fuzz or anisotropy
  (`RenderScene::anyLayeredMaterial`); and it loops over its two draws, not the 4 trace pixels, or a
  warp runs all four evaluations. **Temporal reuse: Z, not 1/M**: it counts the M of the inputs whose
  own surface could have drawn the chosen point, its target above 0 and the point visible from it (a
  ray from last frame's surface, another pick of the block; Bitterli's Algorithm 6): 1/M darkened
  penumbrae, more with every frame (0.89 of reference at 16 frames). **Spatial reuse: pairwise MIS**
  (Bitterli 2022; RTXDI's default): each neighbour weighed against this pixel's reservoir alone by
  the balance heuristic over `k x M x target x visibility` (a ray from the neighbour) against `M x
  target`. **Spatial output feeds temporal reuse, so a bad weight travels**: under Z a neighbour's dim
  point landing head-on kept its whole brightness ratio, and reuse fed it back frame after frame (a
  wall 750x its light at frame 60); the boiling filter (RTXDI's: a reservoir over 20x its 8x8
  group's mean dropped, after each reuse) that had to drop such points dropped legitimate ones too,
  and the frame settled 2% dark. It stays for the outliers RIS itself makes (a dim light's point with
  an occluded light's weight: specks on Cornell's back wall and the helmet's visor without it), and
  drops little now. Pairwise MIS in the temporal reuse made the frame noisier (relative MSE 0.045
  against 0.039): it stays Z. An occluded final point leaves the kept reservoir (weight 0). Its showcase is Sponza at night
  (`sponza_night.scene.json`, 37 lamps), judged against reference quality. Lighting leaves every light but flashes out of its loop when RT direct
  light ran (`LightGpu.params.w`: 1 casts shadows, 0 not, -1 a flash); without ray queries, or
  `rtDirect.enabled` off, the loop lights everything and CSM shadows a casting sun. **castShadows
  only matters to that fallback**: RT direct light shadows every light, as the path tracer always
  has. The sun is a delta, as in the path tracer (its shadows are hard). Reference
  quality reuses nothing and averages the frames (`check_analytic` holds deferred to the closed
  forms at realtime and reference). Rays start 5 cm off the surface, as RT GI's do. The frame is
  the same bits run to run (integer hashes, no atomics, a fixed-order tile sum).
- Lighting is clustered (`lights.cluster`, `LightClusters`): raster lights are uncapped (up
  to 65,536) in a `LightGpu` storage buffer per frame slot, and each 16x9x24 froxel lists up
  to 256 of them. The lists equal summing every light only because a light's attenuation is
  exactly 0 past its range and the loop skips exact zeros: keep both, and keep the lists in
  light order, or a froxel's lighting stops equalling the sum. A froxel past 256 drops lights and
  `render.stats` reports `lightsOverflow`. The slices run to the camera's far plane; the
  sky is only the cleared depth (1.0), so geometry at any distance keeps its pixel.
