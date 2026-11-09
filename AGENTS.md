# AGENTS.md — working on OHAO

The entry point for any agent (or person) building, testing or changing this repo.

OHAO is a C++20 game engine: a Vulkan 1.3 hybrid renderer (deferred raster with ray-traced
direct light (ReSTIR DI) and GI, plus a path tracer), an EnTT world, reflection and canonical scene files, an
API for agents (`ohao/api`), the headless `ohao` CLI and `ohao serve`, the same API as a
JSON-RPC/MCP server on stdio (`ohao/rpc`). Fedora 45 (KDE, GCC 16, Ninja) on an RTX 4060 laptop is the
platform in daily use since 2026-11-08 (Omarchy/Arch before, from 2026-11-05); Windows/MSVC builds were not
tried since 2026-11-05.

- **How work is done:** Musk's algorithm, in order (Frank, 2026-11-08): question the requirement, delete the
  part or process, simplify, speed up, automate. Nothing is added (a test, a feature, a doc section) without
  the requirement it serves.
- **Code map, data flow, recipes:** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- **Driving the engine from an agent:** [docs/agents/ohao-cli.md](docs/agents/ohao-cli.md)
  (the CLI, every method and error code) and [docs/agents/ohao-serve.md](docs/agents/ohao-serve.md)
  (a live session over MCP: connect, orient, edit, play, look, record and replay, logs)
- **Designs and plans:** `docs/specs/`, `docs/plans/`; **what changed:** `docs/changes/`;
  **where it is going:** [docs/ROADMAP.md](docs/ROADMAP.md)
- **Monograph** (design units with code citations): `site/`, frozen during the frontier renderer work.

## Build

Fedora's packages (RPM Fusion's `akmod-nvidia` for the driver):

```
sudo dnf install gcc-c++ cmake ninja-build vulkan-headers vulkan-loader-devel vulkan-validation-layers spirv-tools \
  tbb-devel wayland-devel wayland-protocols-devel libxkbcommon-devel libX11-devel libXrandr-devel \
  libXinerama-devel libXcursor-devel libXi-devel libXext-devel
```

Two pinned slangc, each unpacked from Slang's own release into `~/.local/opt/slang-<version>`: 2026.20 for SPIR-V
(`shaders/slangc.version`, `SLANGC`) and 2025.11 for the CPU twins' C++ (`shaders/slangc_cpp.version`,
`SLANGC_CPP`; 2026.20's C++ emitter miscompiles them, "Gotchas"). Python (numpy, Pillow, jsonschema) is a uv
environment in the repo, `.venv`; name its interpreter at configure, or CTest runs the harnesses with the
system Python, which has none of them. The Ninja Multi-Config generator keeps the `build/Release/` layout every
harness reads.

```
cmake -B build -S . -G "Ninja Multi-Config" -DSLANGC=$HOME/.local/opt/slang-2026.20/bin/slangc \
  -DSLANGC_CPP=$HOME/.local/opt/slang-2025.11/bin/slangc -DPython3_EXECUTABLE=$PWD/.venv/bin/python
cmake --build build --config Release -j16
```

- **Sources are globbed with `CONFIGURE_DEPENDS`**: a new `.cpp` or shader under a globbed directory is picked up
  by the next build. A new directory or target still needs a `CMakeLists.txt` edit.
- **The `shaders` target is not in `ALL`**, but every target that loads SPIR-V depends on it, so building any of
  them builds the shaders. Binaries land in `build/Release/`; SPIR-V in `build/Release/bin/shaders/`.
- **Every shader is Slang** (no GLSL, no glslc). A `.slang` with one `[shader(...)]` entry point gives
  `<flattened path>.spv` (`postprocess/tonemap.slang` → `postprocess_tonemap.slang.spv`), with several
  `<flattened path>.<entry>.spv`, without one it is a module (`import shared.layout;`). Constants and structs
  shared with C++ live in `shaders/shared/layout.json`; regenerate their `.slang`/`.hpp` with
  `python tools/gen_layout.py` (`--check` says whether they are current). **Bindings come from the shaders:**
  `python tools/gen_bindings.py` writes `ohao/render/shader_bindings/<shader>.hpp` from Slang's reflection (each
  binding named, each set in order, the push constants); C++ names them (`shader::write`, `shader::layoutBindings`)
  and never types a binding number, and a hand-written push struct carries `OHAO_CHECK_PUSH`. `tools/check.py`
  fails the build on a stale header or a typed number. Regenerate after any shader's parameters change. See ARCHITECTURE.md, "add a render pass".
- **`GLFW_INCLUDE_NONE`** before GLFW's header (`ohao/runtime/window.cpp`): without it GLFW includes `GL/gl.h`,
  which Fedora does not ship by default. The engine is Vulkan only.

## Gates

The tests serve five requirements, and nothing else is a gate (Frank, 2026-11-08; the cut from 74 tests to these
is commit `7e7211cf`). A test is added back only when a real bug got through, never per feature or per spec step.

| Requirement | Gate | Label | Time |
|---|---|---|---|
| 1. Never hurt the machine | every test has a time limit; `tools/check.py` fails a run during which the kernel logged an NVIDIA GPU fault (Xid); `check_validation`: the Vulkan validation layer over Cornell, the helmet, the animation rig and the play scene, deferred and path traced | gpu | 14 s |
| 2. The renderer is right | `check_analytic` (both renderers against `albedo/π × irradiance` under one light; an emissive quad's form factor), `check_env_orientation` (a sky the right way up), `check_material` (the white furnace renders 1, the closed emitting box its closed form) | gpu | 45 s |
| 3. No visual accident | `render_golden`: 5 scenes (Cornell deferred and path traced, the Damaged Helmet, the play scene, glass) | golden | 6 s |
| 4. Gameplay replays exactly | `replay_test`: the play scene's fight restored from a midway snapshot, and in a new process, reaches the same `world.hash` at every checkpoint (headless, no GPU) | quick | 0.1 s |
| 5. It builds | `tools/check.py` builds first, then runs `ohao doctor` | | |

**Temporary tests** (Frank: deleted when their work is done): `manifold_tests` and `check_caustics` (`slow`) with
caustics step 2; `check_serve`, `e2e_mcp` and `ohao_game_presentation` until the agent-interface and window work
they guard is settled.

```
python tools/check.py                    # build, ohao doctor, the default set (quick, gpu): about 2 minutes
python tools/check.py --all              # adds golden and slow
python tools/check.py --changed [REF]    # only what the files changed since REF reach (tools/check_changed.py)
```

`check.py` finds the real CMake, builds the `shaders` target and then everything (at most 6 jobs), runs
`ohao doctor` (an error-level failure stops the run), runs CTest with the chosen labels and writes one verdict,
`build/check/check.json`, also its last printed line: `{ok, labels, build, doctor, tests: [{name, labels, ok,
seconds, passed, failed, failures, tail}], counts, gpuFaults}`. A failing test carries the last 40 lines of its
output. Exit 0 = all passed, 1 = something failed (a GPU fault included), 2 = the environment is wrong.

- **Every test exits 0 on success** and nonzero on any failure, and ends with one JSON line `{"suite", "passed",
  "failed", "failures"}`. Register a new one with `ohao_add_python_test`/`ohao_add_test` (`cmake/ohao_tests.cmake`),
  with a `TIMEOUT` and, if it touches the GPU, `GPU` (GPU tests never run two at once).
- **A kept gate is proven, not trusted:** plant a bug in what it covers and watch it fail. 2026-11-08, each caught:
  a no-op physics restore (`replay_test`, all 3 checkpoints); 1% brightness in `displayPixel` (all 5 goldens);
  the path tracer's sky upside down (`check_env_orientation`); +8% on a light sample's geometry term
  (`check_analytic`, deferred x1.10); a rough metal's multiple scattering x0.9 (`check_material`, furnace 0.93);
  64x sampler anisotropy (`check_validation`, all 20 renders); a planted Xid line (`check.py` fails the run). A
  dielectric's multiple-scattering Fresnel x0.9 passed the furnace: under a white base the energy moves to the
  diffuse and the total stays 1, which is what a furnace measures.
- **`ohao doctor`** says whether this machine can build, run and test OHAO: the Vulkan instance, device, ray
  tracing and validation layer; every shader's SPIR-V present and newer than its sources; the assets the scenes
  reference; slangc at its pinned version; a 64x64 deferred frame within 2 s (a wedged GPU fails here). `--cpu-only`
  creates no device.
- **Never loop a program that opens a window** (Frank, 2026-11-08): one visible run shows it works; repeats are
  headless (`ohao run`, `ohao serve` without `--window`).
- **Validation** (`OHAO_VALIDATION=1`): zero messages, teardown included, each on the logger's `validation` channel
  (`W validation: <VUID> | <text>`). `OHAO_VALIDATION=sync` adds synchronization validation (hazards between
  commands): slower, for chasing a missing barrier. The layer stops at 10 reports per VUID;
  `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` shows them all.
- **Goldens** are a hash of an unchanged renderer, not an image-quality gate: any change to the sample sequence
  moves every pixel by the noise. Deferred scenes are exact; the path tracer holds to the BVH floor (1 level on a
  few pixels). `--update` only after looking at what drifted (`build/golden_diff/<name>.heat.png` and
  `.crop.png`); never widen a tolerance to make one pass.
- **Benchmarks** (`tools/bench/`: `render_bench`, `scene_bench`, `serve_bench`, `physics_bench`, `level_bench`,
  `audio_bench`, `feel_bench`) are tools run by hand, not gates: `<bench> --check tests/bench/<name>_budgets.json`,
  results in `build/<bench>.json`. `render_bench --only SCENE` times one scene's passes from GPU timestamps; it is
  the baseline a renderer change is judged by. A budget changes only by Frank's ruling.

## Gotchas that have cost this project time

- On Windows conda's `cmake` shadows the real one; use the scoop shim (above). `tools/check.py`
  refuses conda's.
- Tests run in parallel under CTest: a test that writes a fixed path (a scratch
  directory under `build/`, a file in `%TEMP%`) races another copy of itself. Give it a
  per-process name or a `LOCKS` entry in its registration.
- **Git Bash (Windows) rewrites arguments that start with `/`** into Windows paths. Write
  `ohao query --fields Transform/position`, not `/Transform/position`.
- **Line endings:** `core.autocrlf` is on. Byte-compared files (goldens, scene files,
  CLI expectations) are pinned to LF in `.gitattributes`; add a pattern there for any new
  byte-compared directory, or a fresh clone checks out CRLF and the comparison fails.
- `/FORCE:MULTIPLE` (MSVC) and `--allow-multiple-definition` (GNU ld, set for every target in the root
  `CMakeLists.txt`) hide duplicate symbols: stb, and a second VMA inside `NRI_VK`.
  Which copy wins depends on link order, so `gpu_allocator.cpp` uses NRI's VMA
  configuration; never change one without the other. A new target that crashes right
  after "resizing to WxH" has picked a mismatched VMA.
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
- `GLM_ENABLE_EXPERIMENTAL` before any `<glm/gtx/...>` include.
- Jolt: `dynamic_cast` on Jolt types crashes (no RTTI); `SetMotionQuality` lives on
  `BodyInterface`. The engine's physics is `physics/backend` (Jolt) and `ecs/physics_scene`; bodies
  need a shape.
- `ohao::physics::math::AABB` is (center, halfExtents); render bounds (`MeshInfo`,
  `DrawRecord`) are (min, max). A box passed between them needs converting.
- ECS writes: `Transform`, `MeshRenderer` and `Light` reach the
  renderer only through `registry.patch<T>()` /
  `replace<T>()` (or emplace/remove). A write through `get<T>()` is never seen.
- **Systems are installed by `engine::installEngine` and `engine::installRendering`** (`ohao/engine/install.hpp`) in
  every host, and run in the order of `ecs/system_order.hpp`, whoever installs them; a new system adds its name
  there (an unlisted name throws). A system with state a snapshot must carry registers `ecs::addSnapshotHook` under a
  `kSnapshotOrder` name.
- World systems have a phase: `addSimulate` (behaviour, once per fixed step, gets dt) or
  `addSync` (derived state, no dt). Before a `render()` call `world.sync()`, never
  `update()`: rendering must not step the simulation.
- Input is stepped per simulation step, not per frame: read `pressed`/`released` in a
  simulate system or `fixedUpdate`, never once per frame (two steps in one frame would
  lose a press). A world with an `InputActions` component rebuilds its bindings from it;
  one without keeps the bindings made in code.
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
- The validation layer stops at 10 reports per VUID; set
  `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` to see them all. The engine names no Vulkan
  objects: find a handle's creation site by temporarily printing handles at `vkCreate*`.
- Restoring a file from a backup copy keeps the backup's older mtime, so MSBuild keeps
  the objects built from the edited version: `touch` sources after a restore.
- **Levels and staged assets** (sub-project 5,
  `docs/specs/2026-10-26-level-pipeline-design.md`).
  - Checked-in levels are generated by Blender scripts in `tools/level/` and are ours (MIT).
  - Frank's Somnabulate assets are staged by `tools/level/stage_flat42.py` into git-ignored
    `assets/local/`, with the scenes that use them, and are never committed until he rules
    (the repo is MIT). Never write under `C:\Users\djmax\Desktop\games` or the Unreal
    project.
  - A `ModelAsset` without `mesh` merges every mesh in a file, collision hulls included;
    with `mesh` (a glTF mesh name or `#<index>`) it is that one mesh at its own origin,
    loaded by `ModelLoader::loadMesh` and cached per `(path, mesh)`. A glTF's node tree,
    `extras`, lights and cameras are read by `ohao/level`'s `readGltfStructure`, not Assimp.
  - A `Level` component is a level file in the world: the `level` sync system
    (`ohao_level_world`, `level/level_system.hpp`, before `instantiate`) makes one entity per
    node with `LevelNode`, ids `levelNodeId(level id, node index)`, and `MeshSource`
    `#<mesh index>`, `Light`, `Camera`, `SpawnPoint`, `Socket` and passed-through components
    from the node and its `extras` (`level/level_hints.hpp`; `vis: 0` and `UCX_` hulls get no
    mesh). A host must call `level::installLevels` (the CLI context and `LiveHost` do);
    `scene.load` expands levels at once (`World::runSync("level")`). Node entities are never
    saved, and a changed `Level` re-expands in place (same entities, the file's state back),
    so an unchanged file keeps `world.hash`.
  - Collision (`ecs/physics_scene.hpp`): `Collider` and `RigidBody` are saved components; the
    `physics` sync system (right after `transform`, installed by `ecs::installPhysics`, which
    the CLI context and `LiveHost` call next to `installLevels`) builds one Jolt body per
    entity with a `Collider`, fitted to its mesh at its world scale, plus `UCX_` hulls
    (`ColliderPart`) as one compound body on their target. It works from dirty state (signals
    and the transform system's `PhysicsMoved` tag), so a still world builds nothing; a moved
    body is moved, a rescaled one rebuilt (stepping: "Physics in the step", below). The
    backend runs without worker threads and room for 65,536 bodies; Jolt
    takes each step's contact buffer (sized by that cap) from the temp allocator, which falls
    back to the heap past 10 MB (a fixed 10 MB block aborted the process on the first step).
  - Agents read levels through `level.info`, `level.reload`, `physics.raycast`,
    `physics.info`, `physics.events`, `physics.contacts`, `player.info` and `player.teleport`
    (`ohao/api/level.cpp`), which reach the level system and the physics
    scene through what `installLevels` and `installPhysics` leave on the World
    (`level::levelSummary`/`levelDiagnostics`/`reloadLevel`, `ecs::physicsScene`); a World
    without them answers `E_STATE`. They run the `level`, `transform` and `physics` sync
    systems first (never instantiate or the render extract), so they answer right after
    `scene.load`. Recipe: look at a level file with `ohao level <file>`, in a session with
    `level_info` and `physics_raycast` (`docs/agents/ohao-serve.md`, "Load a level"); add
    one by `docs/ARCHITECTURE.md`, "add a level". Results stay small: `level.info` on the
    blockout ~480 bytes (budget 1 KB, `level_bench`), a ray ~100 (200).
  - Loaders keep a file's normals. `repairMeshNormals` fills only vertices left without
    one (zero or not finite) with their faces' smooth normal; a loader that finds no normal
    writes zero for it, never a placeholder direction.
- **Physics in the step** (sub-project 6, `docs/specs/2026-10-26-player-physics-design.md`).
  `installPhysics` also adds a `physics` simulate system: every `World::simulate(dt)` (the
  window's fixed step, `world.step`, `ohao run --steps`) propagates dirty transforms
  (`propagateTransforms`), brings bodies up to date, steps Jolt in substeps of at most 1/60 s
  (8 at most), writes every awake dynamic body back to its entity's `Transform`
  (`registry.patch`, through the parent), and publishes the step's events. A world without a
  collider does none of it. There is one clock: never step the backend from a host.
  - `propagateTransforms` is the transform system without the frame's bookkeeping: an
    entity's `prevWorld` is captured at its first recompute after a render and kept until the
    next `updateTransforms`, so a frame of several steps still has motion vectors spanning them
    (`t10_two_steps_one_frame_motion`). Do not call `updateTransforms` inside a step.
  - A dynamic body's entity moved by anything but the write-back (an edit, a moved parent)
    puts the body there at rest, however close: the scene marks its own `Transform` writes
    (`writePose`) and treats any other as a placement; through a parent, a pose more than 1e-4
    from what it wrote. Teleports set position and rotation at once (`setPose`). Sleeping
    bodies are not written, so a settled level writes nothing.
  - Snapshots replay exactly (step 5). A snapshot carries the simulation: `installPhysics`
    puts `PhysicsScene::saveSimulation`/`restoreSimulation` in the World's `SnapshotHook`
    (`ecs/snapshot_hook.hpp`): Jolt's state (bodies with velocities and sleep timers, the
    contact cache the solver warm-starts from), each character, the players' records, the
    contact bookkeeping. The next sync after a restore puts it back if the bodies are the
    snapshot's (same handles for the same entities) and skips their teleports; a body rebuilt
    since falls back to teleports and `BodyMotion` (runtime, snapshotted, never saved or
    hashed: what each step left a dynamic body or player moving at). Velocities alone are not
    enough: resting contacts replayed 2-4e-5 m off without the contact cache. `scene.load`
    runs `player_spawn`, so a snapshot after a load holds the spawned player. A new or
    teleported character finds its ground at once (`RefreshContacts`).
  - Events (`ecs::physicsEvents(world)`, the latest step's; `PhysicsScene::eventsSince` and
    `physics.events`, a log of 1,024): `contact_begin`/`contact_end` per body pair (Jolt's
    per-sub-shape callbacks counted per pair), `trigger_enter`/`trigger_exit` for a
    `Collider.trigger` sensor (hint `"trig": 1`). Jolt's callbacks only queue (thread-safe);
    the scene drains them on the simulating thread after the step and sorts them. Jolt removes
    a sleeping body's contacts: the scene keeps such a pair (dormant) and ends it only if the
    bodies wake apart. Removing a body ends its pairs in the next step's events.
  - Jolt's penetration slop is 5 mm (its 2 cm default left dropped crates visibly in the
    floor): a resting body sits up to 5 mm into each support.
  - **Joints** (sub-project 13, `docs/changes/2026-11-02-ragdoll-step1.md`): a saved `Joint` is one
    constraint between the body its entity is part of (its own, else its nearest ancestor's: several
    joints on a body are child entities) and `other`'s, or the world. Anchor and axis are in the Joint
    entity's frame; the other side is the same world point at build (a Distance joint's is `other`'s
    origin). Limits are this entity's turn or travel relative to the other, right-handed about the
    axis (Jolt measures the other against this one: the scene negates them). Joints are built after
    the bodies, in creation order, and a body rebuilt destroys and rebuilds its joints. **A break
    disables the constraint, never removes it** (`JointState {broken}`, a `joint_break` event):
    Jolt's solver and its saved state are ordered by the constraint list, and a removal moves the
    last constraint into the gap, so nothing may add or remove a constraint mid-round if snapshots
    are to restore. Jointed bodies never collide with each other (`collideConnected` false, rejected
    in `OnContactValidate`). The backend takes local anchors from a body's origin, not its centre of
    mass (Jolt's own local space), and saves its constraint list in Jolt's order. A 20-link chain
    stretches 2.6 mm a joint at Jolt's default iterations; more velocity iterations steady it hanging
    but stretch it more swinging (not understood yet).
  - The player (`PlayerController`, saved; `PlayerState`, runtime): a Jolt `CharacterVirtual`
    whose position is the feet, moved in the step before Jolt's substeps by the actions it
    names (`move_forward`, `move_right`, `jump`, `sprint`, `look_x`, `look_y`; mouse look
    always on). Its kinematic inner body (layer `CHARACTER`, never asleep) is what dynamic
    bodies and sensors touch: `CHARACTER` does not collide with `DYNAMIC` or itself (the
    virtual character pushes crates by `pushForce`), and a trigger sees the inner body as it
    sees any kinematic body. Vertical motion is integrated exactly (`vy + g·dt/2` per step),
    so a jump of `jumpSpeed` 4.9 peaks at 1.22 m. It spawns once, when created, at the first
    `SpawnPoint` tagged `player` (by creation order; `W_PLAYER_NO_SPAWN` else); a write to
    its `Transform` by anything but the step teleports it. It must be a root entity. A
    kinematic body moved during a step moves by `moveKinematic` (velocity, so it pushes what
    it meets) and stops after the step; moved outside a step it teleports.
  - The camera: the player's head is its first child holding a `Camera` (the scene authors
    it, `assets/scenes/blockout_play.scene.json`); the step writes it at `eyeHeight`, pitched
    within ±89°. Players spawn in the `player_spawn` sync system (before `transform`), so a
    play scene's first frame, before any step, is the spawn's. The view is the first *active*
    camera (a level's own camera stays inactive beside the head).
  - Render interpolation: `World::setRenderAlpha` (default 1; `LiveHost` sets the timeline's
    alpha in realtime, 1 in lockstep and for lockstep captures). The `interpolate` sync system
    (after `physics`, before `render_extract`) gives what the last step moved (players, heads,
    dynamic bodies) and their subtrees a runtime `RenderPose` between the last two step poses,
    which the render extract and the scene view's camera use; `Transform`, `WorldTransform`,
    `world.hash` and saved files stay the step's. A placement (spawn, teleport, edit) is never
    interpolated across. A capture syncs before it resolves the view. The input-driven player is
    the exception in a realtime window: its turn is the newest step's plus the look lead (see
    "Frame timing", the look lead), its position interpolated.
  - The window reports look input only from a captured cursor, as `MouseDelta`;
    `ohao_game` on a scene with a `PlayerController` captures the mouse while focused,
    `release_mouse` (Esc) frees it, and a click on the window takes it back
    (`LiveHost::setClickToCapture`, `runtime/click_capture.hpp`): that press and its release
    are swallowed before the input map sees them, so the capturing click never fires.
  - Footsteps: the player's step logs a `footstep` event every 0.75 m walked (0.95 m
    sprinting) on the ground and a `land` after a fall of more than 0.3 m, in the physics
    event log (`physics.events`), with the ground's `Collider.surface` (hint `surf`; empty is
    `default`). The voice schedule turns them into `footstep_<surface>` plays (a character whose
    animation graph has `foot_l`/`foot_r` events steps from those instead; "Audio", below); a
    window's `AudioPlayer` plays them from git-ignored `assets/local/audio/footsteps/` (staged
    for local listening only, never committed) else `assets/audio/footsteps/` (ours,
    `tools/audio/make_footsteps.py`).
- **Combat** (sub-project 7, `docs/specs/2026-10-27-combat-design.md`). `ecs::installCombat`
  (the CLI context and `LiveHost` call it after `installPhysics`) adds a `combat` simulate system
  after `physics`: every `Weapon`, in id order, reads its trigger (a player's from the World's
  input, `fire`/`reload`; anything else's from `WeaponTrigger`), advances cooldown, reload, bloom
  and recoil, and fires at most one round a step. A round is one ray from the eye (a character:
  feet + `eyeHeight` along this step's yaw and pitch; anything else: its origin along −Z) through
  `PhysicsScene::raycastFrom`, which skips the shooter's own bodies and triggers (a plain
  `raycast` from the eye stops inside the shooter's capsule). What it hits, or its nearest
  ancestor, with `Health` takes the damage; at 0 it is dead and neither takes damage nor fires.
  - `Health`, `Weapon`, `WeaponState` and `WeaponTrigger` are saved, so files, `world.hash` and
    snapshots hold a fight exactly; the step emplaces `WeaponState` (a full magazine). Spread
    and recoil draw counter-based random numbers, `hash(seed, shots, i)`: no generator state.
  - The cooldown carries its remainder and compares within 1e-5 s, so 10 Hz fires on every
    sixth 60 Hz step. Bloom and the vertical kick recover only once the fire interval passes
    without a round (else sustained fire would never bloom).
  - Recoil turns the view through `PhysicsScene::turnPlayer`: the step's own write of the
    player's yaw and the head's pitch (not a placement), kept inside this step's interpolated
    move. Events (`combatEvents(world)`, the step's; `CombatSystem::eventsSince`, a log of
    1,024): `shot`, `hit`, `damage`, `death`, `reload`, `reloaded`, `explode`.
  - **The test grenade** (audio spec ruling 23, `combat::kGrenade*`): `grenade` (G in the play
    scene) throws from the eye along the look; it flies in the combat step after the weapons,
    sticks where a ray along its move first meets something, and 2.5 s after the throw writes
    `explode` at its place, which the schedule plays occluded as a world source. No damage. A
    grenade in flight is runtime only: never saved, hashed or snapshotted (a restore keeps the
    ones in flight, a restart clears them).
  - Saved types hash their names even with no instances, so a new saved component moves every
    `world.hash` in the serve and CLI expectations, not only scenes that use it.
  - Enemies (step 2) are characters on the player's code: `PlayerController` with `spawn`
    (the k-th character with a tag takes the k-th `SpawnPoint` with it, creation order; empty:
    where it stands) and `input: false` (moved by its saved `MoveIntent`, not the world's
    input; weapons fire by input only for `input: true`). The enemy AI (`ecs/enemy_ai.cpp`)
    runs at the start of the combat step, before the weapons: sight is a `raycastFrom` from its
    eye to the player's chest (in its cone only while idle); it writes `EnemyState`,
    `MoveIntent` (the next step walks it) and `WeaponTrigger` (this step fires it), all saved.
    A dead enemy loses its `PlayerController`; its `Transform` stays upright (its death clip is
    the fall, sub-project 10, OPEN ruling 7 provisional); its character is parked
    (`IPhysicsBackend::setCharacterInWorld(false)`: out of the broadphase, so it blocks nothing
    and rays pass) and comes back as the same character when a restore puts the controller back,
    so the snapshot's simulation restores exactly (a new character would leave resting crates
    3e-5 m off). Characters block each other: a player teleported into an enemy is pushed
    out. `player.info` and friends without `entity` mean the input-driven character.
  - `scene.load` runs `level`, `player_spawn`, `transform` and `physics` sync, so a snapshot
    right after a load holds the built bodies; a restore of one taken before the bodies existed
    rebuilt them, and a character standing on a slope replayed one float off.
  - The round (step 3): a world with the saved singleton `Round` starts it at the end of its
    first combat step (`round_start`, a snapshot through the `RoundHook` in the registry's
    context, which `api::Session` installs with `ecs::installRoundHook`; a bare World installs
    it itself, else the round warns once and never runs) and restarts at the end of the step
    the player has been dead `restartDelay` (3 s) or `restart` is pressed: `world.hash` after
    that step is the start's. Every enemy dead is `win` once, then the round waits for
    `restart`. A dead player neither walks, turns, jumps nor fires. A character a restore
    creates starts where its restored entity stands, never at a spawn.
  - Presentation (step 4). The muzzle flash is a `Light` with a saved `Muzzle {intensity,
    time}` below the weapon's entity: the combat step sets it to `intensity` for `time` after a
    round (`WeaponState.flash` counts the steps) and back to 0; a light at intensity 0 has no
    emitter mesh (`render::lightEmitter`). A hit's mark is the vfx set's decal ("VFX"). The HUD (`ecs/combat_hud.hpp` → `render::HudList` → the `hud` pass after the gizmo,
    `render/hud/`) is laid out from combat state and the log, read from its oldest event. A window always draws the HUD; `render.capture` and `ohao run --render` only with
    `hud: true`/`--hud`, and only on deferred frames; no crosshair or hit marker while dead or
    once the round is won. Its sounds (`assets/audio/weapons/`, ours:
    `tools/audio/make_weapons.py`) are the voice schedule's plays ("Audio", below).
  - Agents (step 5) fight through `combat.info` (the round and every combatant: health, `pos`,
    weapon, enemy state), `combat.events` (the log, as `physics.events`) and `combat.damage`
    (hit points off an entity's `Health` or an ancestor's, as a round does), all in
    `ohao/api/combat.cpp`; firing is `input.inject` (`fire`), as in a window. Aim by
    `player.teleport` to where the player stands with the yaw toward a combatant's `pos`.
  - A restore keeps restored rotations bit for bit: a character a restart brings back (a dead
    enemy's, parked) is placed where its restored entity stands without rewriting its rotation
    from the yaw read back (`yawOf` gives +180 for a quaternion stored at -180: the same
    rotation, another hash). Any code that re-derives a pose from a restored one must keep it
    exact, or `world.hash` after a restart stops being the round start's.
- **Frame timing** (sub-project 9, `runtime/frame_log.hpp`): a window host stamps each realtime
  frame (poll, view, submit, present) and the look movements it shows, in a `FrameLog` that
  `render.stats` (`latency`, `pacing`), `ohao_game --latency` and `feel_bench` read. Presents are
  confirmed with `VK_KHR_present_id` + `VK_KHR_present_wait` where the device has both: a thread
  (`gpu/vulkan/present_waiter.hpp`) polls `vkWaitForPresentKHR` with a zero timeout about every
  0.25 ms, holding `Swapchain::hostMutex()`, which every host access to the swapchain (present,
  rebuild, destroy) takes, as Vulkan requires; a present unconfirmed after 250 ms, or without
  present wait, keeps its call stamp (`source: present_call`, an underestimate). With a look-led
  player a look movement counts as shown by the first frame after its poll; without one, once the
  step after the one that applied it has run. `ohao_game --present immediate|mailbox|fifo` and
  `ohao serve --window --present` choose the present mode (`--vsync` is fifo; the default stays
  mailbox, which measured the same latency as immediate in a window).
  - **The look lead** (step 2): a realtime window hands the World the look no step has consumed
    yet (`World::setPendingLook`: the window's mouse movements since the last step and the
    `input.inject` ones due at the next step, summed in the step's order), and the `interpolate`
    sync system draws the input-driven, living player turned by it through `applyLook`, the step's
    own arithmetic, so the next step lands exactly where the frame already was. **Gotcha: that
    player's rotation is drawn from the newest step plus the lead, never interpolated** (only its
    position is); anything else interpolates as before. Presentation only: `RenderPose`, never
    `Transform`, `world.hash`, a file or a snapshot; no lead in lockstep (alpha 1, nothing pending),
    for the dead, or for characters moved by their `MoveIntent`.
  - **Late input sampling:** a realtime frame polls only once at most `LiveConfig::framesAhead`
    submitted frames are unpresented (`presentToAwait`; it waits on present wait, else the frame's
    fence, at most 20 ms). Automatic (-1): 1 while a player leads its look, unbounded otherwise.
  - **Stalls:** every window on this laptop gets a few frames a minute of 330–500 ms (5–16 a
    minute measured) in which the presentation engine holds the swapchain's images: the frame's
    GPU work is normal and starts late, nothing of the engine's is queued in the gap, and a
    30,000-frame headless loop never shows one. They are not the engine's to fix; `feel_bench`
    counts them (`stalls`).
  - **The first-person gun** (step 3, `ecs/view_model.hpp`): an entity with `ViewModel` (saved; the
    play scene's Gun) and its subtree are drawn by the `view_model` sync system (after
    `interpolate`; `installCombat` installs it): placed in the view by `viewModelRemap` (S =
    diag(d, d, d/k), k = tan(scene/2) / tan(fov/2): every point stays within `depthScale` of its
    distance, so it never reaches into a wall it stands against, and lands on screen where it would
    through `fovDeg`), swayed by the drawn turn, bobbed by ground speed, kicked by the owner's
    `shot` events, all at render rate from `World::presentationDt()` (the window's frame time; 0 in
    lockstep and captures: the rest pose). `RenderPose` only. Its instances are `kDrawViewModel`
    (no CSM cascade) with TLAS mask `kTlasMaskViewModel` (4), in only the path tracer's camera ray
    mask (`kRayMaskCamera` 7): it casts no shadow and no GI. A light below it (the Muzzle) follows
    its `RenderPose`. **Gotcha: the projection is `glm::perspective` without
    `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so under Vulkan's [0, 1] depth anything nearer than about twice
    the near plane is clipped** (0.1 m at the play scene's 0.05): a `depthScale` of 0.25 put the
    gun's near end there and it vanished; 0.55 keeps it at 0.108–0.27 m, inside the 0.3 m capsule.
  - **Effects** are the world's (sub-project 12, "VFX" below): drawn from the step's records in every
    host, windows, captures and `ohao run` alike, so a window's lockstep capture equals a headless one
    with its effects. There is no `--fx`/`--no-fx`.
- **Animation** (sub-project 10, `docs/specs/2026-10-28-animation-design.md`). `ohao/anim` (no ECS,
  no Vulkan) reads a glTF's skins and animations itself (`gltf_anim`, simdjson; Assimp's animation
  import is not used), binds clips to skeletons by joint name, and parses and validates
  `.animgraph.json` (every error with its JSON pointer). `installAnimation(world, assets)` (the CLI
  context and `LiveHost` call it after `installCombat`) adds the `animation` simulate system and the
  `pose` sync system (before `render_extract`).
  - **A pose is presentation; animator state is saved integers.** The saved `Animator` holds
    integers only: each layer's state, its steps, the state faded from and
    its steps, the fade's length and progress. Never keep a clip time or a pose in a saved
    component; force a state with `component.set` on `Animator` (`animation.info` reads it back). A pose is a function of them and alpha, sampled at
    `(steps - 1 + alpha)` steps of 1/60 s; the pose (`AnimPose`) is runtime, never saved or hashed.
    Clip events fire in the step, in id order, into a log of 1,024 (`animation.info`).
  - **A skinned instance is its own mesh.** The render extract gives an entity with `SkinnedMesh`
    a mesh from `RenderScene::registerSkinnedInstance(base)`: its own vertex range (the base's
    copied), the base's indices and triangle slots, copies of its meshlets, an updatable BLAS. The
    `skin`, `skin.bounds` and `rt.blas.refit` passes run before `scene.upload` (whose TLAS build reads
    the refit BLAS), only for the instances whose palettes changed. Nothing changes for consumers:
    they read the instance's mesh as any other.
  - The copy's `boneWeights.xyz` hold last frame's position (draw flag `kDrawSkinned`): the motion
    vectors' source in `core/gbuffer.slang` and `cull/meshlets.slang`. Nothing may read a skinned copy's
    joints or weights; the skin pass reads them from the base mesh.
  - The previous palette is the previous *sync's*, as `WorldTransform::prevWorld` is: a capture
    (which syncs for the view, then per frame) and `world.step` (which syncs) start without motion.
    A window frame (steps, one sync, the frame) has it.
  - The model loader keeps a skinned file's vertices as written (no `JoinIdenticalVertices`):
    Assimp's join ignores joints and weights and merged seam vertices with different weights.
  - **The enemy** (step 2): `assets/characters/toon_enemy/` (Quaternius, CC0, rebuilt by
    `tools/anim/make_enemy.py`, pure Python, from the staged upstream file; `README.upstream.md` says
    what it changed) and `assets/anim/enemy.animgraph.json` (body `move`/`combat`/`death`/`dead`;
    additive `aim`, `fire`, `reload`, `hit`). Its driver `enemy` (`ecs/enemy_anim.cpp`) reads saved
    state and the step's combat events **after the step's last `restart`**: a restart restores the
    round's start inside the combat step, before the animation step, so a restart step must step the
    restored Animators exactly as the round's first step did. Damage dealt between steps
    (`combat.damage`) lands in the previous step's events and flinches nothing; death still follows
    `Health`.
  - Additive clips are authored as rest × delta (the layer adds rest⁻¹ × clip): a pack clip used
    additively is re-referenced to its own first frame by the tool, never added raw.
  - `JointAttach` poses the attached entity's whole subtree (the rifle's muzzle light follows the
    hand). An enemy's body and head entities have no mesh: rounds hit its capsule (ruling 18).
  - A skinned palette holds the bones that weight vertices (Assimp's list: the enemy's 41 of 43
    joints), not every joint.
  - **The player's arms** (step 3): the play scene's `Gun` is the `ViewModel` root at the eye; its
    child `Arms` (`assets/characters/fps_arms/`, ours, `tools/anim/make_fps_arms.py` in Blender 5.2.2)
    is skinned and driven by `arms` (`ecs/arms_anim.cpp`, from the owning player's saved state and
    the step's combat events); the `Muzzle` is a `JointAttach` on its `gun` joint. `view_model` places
    the root and skips `JointAttach` children (the pose system draws them after it). A view-model rig
    is drawn as through `ViewModel.fovDeg` and pulled to 0.41 of its depth: author what should show
    at least 0.25 m from the eye and within 30° of the eye line, or the near clip and the screen's
    edge eat it. A clip's `rateFrom: {param, at}` lands its event `at` `param` seconds in (the reload's
    `mag_in` on the weapon's `reloaded`, within a step).
  - **ADS** (provisional ruling 11): `Weapon.aim` (action) and `adsTime`, `WeaponState.aim` (saved,
    stepped before the round; 0 while reloading or dead); the cone is `× (1 − 0.5 · aim)`. The narrowed
    view is a runtime `DrawnFov` on the camera, written by `view_model` in live frames only:
    `resolveSceneView` prefers it, so **lockstep frames and every capture keep `Camera.fovDeg`**.
- **Audio** (sub-project 11, `docs/specs/2026-10-28-audio-design.md`). A game's sound is one
  `.audio.json` bank (`ohao/audio/audio_bank.*`: buses as a tree with caps and a return bus, sounds,
  shapes, ducks, snapshots, reverb presets; every error at its JSON pointer; `ohao schema
  audio-bank` exports its schema), named by the saved singleton `AudioMix { asset }`; the play scene
  names `assets/audio/slice.audio.json`. Without one, the default bank: master over `sfx`, `music`,
  `ambient`.
  - **A bus is an `ma_sound_group` feeding an `AudioBusNode` of ours:** gain linear in dB and
    ramped, a low-pass ramped in log frequency, a 2 ms mute/solo gate, a send. The old
    `SoundCategory` API is a name for a default-bank bus: a category volume is its group's volume,
    master the root's.
  - **Bank voices never use miniaudio's start time or stop fade.** In 0.11.21 a start inside a
    block lands at (block end − start), and `stop_with_fade` cut at once. `AudioVoiceSource` plays
    silence until `startFrame` and fades a stop itself.
  - **Voices and groups are made with `MA_SOUND_FLAG_NO_PITCH`:** the pitch resampler delays a
    voice by a frame per node. Nothing may set pitch on the bank's path.
  - `AudioSystem::initialize(true)` is a mixer without a device; `render(out, frames)` pulls frames
    from it, the same bytes every run. Test the mix that way, never through a device.
  - **Teardown cuts every node link before any node goes.** In 0.11.21 `ma_node_uninit` detaches
    only the first node feeding each input; the others stay linked to the freed node (the return's
    group, with a sender per bus: `audio_bench` faulted in teardown one run in ten). A node with
    several inputs is detached from them explicitly first (`destroyBuses`).
  - **The reverb** (step 2, `ohao/audio/reverb_node.*`: Freeverb, ours, 8 combs and 4 allpasses a
    channel) sits on the bank's first return bus, starting at its `defaultZone`;
    `AudioSystem::setReverb(preset, frames)` crossfades two cores. The bank parser is its own
    library, `ohao_audio_bank` (no miniaudio), which `ohao_ecs` links.
  - **Reverb zones are volumes, not bodies.** `ReverbZone { preset, priority, size }` (saved): half
    extents about the entity, 0 fitted to its mesh. Physics never sees a scene zone, so it adds no
    contacts or trigger events; a level node's `verb` hint makes a `ReverbZone` on a trigger box. An
    unknown preset is `W_AUDIO_PRESET` and counts as the bank's `defaultZone`.
  - **The schedule** (`ecs/audio_schedule.hpp`, `installAudio(world, assets)`, which the CLI context
    and `LiveHost` call after `installAnimation`) is the step's last simulate system. From step 2 it
    writes `zone` commands (the listener's zone changed: preset, bed, a 30-step blend) and
    `occlusion` commands (a held voice's wall count changed on a re-cast every 6 steps). Occlusion
    is graded by walls: none; −8 dB, 1.2 kHz; −14 dB, 700 Hz for two or more. At most 16 rays a
    step: past them a new voice counts as unoccluded, a re-cast keeps its answer, both counted. The
    listener is the input-driven player's head, else the first active camera.
  - **The schedule decides every sound in the step** (step 3): footsteps and landings from the
    physics log, combat sounds from the combat log after the step's last `restart`, `mag_out`,
    `mag_in`, `bolt`, `body_fall` and `foot_l`/`foot_r` from the animation log (a character whose
    graph has foot events steps from its clips, never from the physics footstep). Each `play` has
    a voice id, a variant (round robin per sound), its occlusion and an outcome: over a bus's cap or
    the bank's `voices`, the lowest priority, then the quietest at the listener, then the oldest is
    stopped (`stop` first), or the new one is `dropped`. A sound holds its voice `holdMs` (the
    bank's, OPEN 13). Ducks (`bus` commands, the deepest rule a bus wins, attack down, release up)
    and the snapshot from the round (`default`/`dead`/`won`, OPEN 9) are commands too. A restart or
    any restore writes `stop_world`; after a restore the schedule's state comes back with the
    World's snapshot (its `SnapshotHook` chains in front of physics'), and it resends the mix as
    `resync` commands, which a player ignores when it is already there. `world.hash` never sees any
    of it: the schedule is runtime.
  - **The player** (`runtime/audio_player.hpp`): a window's `LiveHost` owns one (`LiveConfig::sound`;
    `ohao_game --mute` builds the schedule and creates no device) and plays the schedule's commands
    once a frame, one step after their step (OPEN 7: every sound, the player's own shot too), from a
    clock anchored to the device (late: at once, counted; re-anchored when not realtime, when the
    step goes back, or past a step of drift). A bed is the zone preset's looped sound. Headless
    hosts have no player; the schedule's commands are what they read. `OHAO_AUDIO_TRACE=1` logs every
    command on the `audio` channel (`schedule: ...`), so a window's schedule can be compared with a
    headless host's through `log.read`.
  - Sound shapes, curves and occlusion are the bank's (`shapes`, `occlude`), never code's: the
    slice's gunfire is 1–60 m, rolloff 1; near sounds 0.5–20 m, rolloff 1.5.
  - **Agents** (step 4, `ohao/api/audio.cpp`) hear through `audio.info {since, max}`: the
    schedule's half (snapshot, zone, listener, buses with their duck, voice and ray counts, the
    commands after a cursor) everywhere, `device: "none"` headless; a window's `api::Host` adds
    the device's half (sample rate, each bus's driven level and voices, `late`, `mixMs`). It
    stays small: a bus's `db` only when not 0, a play's variant, gain, occlusion and outcome
    only when not the default. `audio.mix {bus, db, mute, solo, snapshot, clear}` is for
    listening: the session keeps the overrides and hands them whole to the host
    (`Host::setAudioMix`), whose `AudioPlayer` applies them in the device and again when a bank
    loads. **Never put them in the World, the schedule or a snapshot**: the schedule's commands,
    `world.hash` and replays are the same with or without them. `ohao serve --window --sound`
    opens a device (a serve window is mute without it); `render.stats` `audio` (`mixMs`, the
    median time the device callback takes per 10 ms of audio; `voices`; `late`) is there only
    when a player has a device.
- **VFX** (sub-project 12, `docs/specs/2026-10-28-vfx-design.md`; steps 1–3 built, step 4 in progress). A world's
  effects are one `.vfx.json` set (`ohao/vfx/vfx_set.*`, errors at JSON pointers, `ohao schema
  vfx-set`), named by the saved singleton `VfxSet`; without one, the default set. `installVfx` (the
  CLI context and `LiveHost`, after `installAudio`) adds the `vfx` simulate system before
  `audio_schedule`: it writes **spawn records** (effect, tracer, decal, light) from the step's
  `shot`/`hit`/`explode` after its last `restart` and from placed `VfxEmitter`s by step count,
  seeded from the vfx step and the event's index, capped by kind (the oldest dropped, counted).
  **Records are runtime: never saved or hashed**; snapshots carry them (its `SnapshotHook` chains in
  front of the schedule's) and a restart puts back the round start's. Particles are a closed form
  of (record, index, age) (`vfx/particle_math.hpp`), never integrated. Agents read them through
  `vfx.info {since, max}`.
  - **The frame** (step 2, `render/vfx/vfx_renderer.*`): the `vfx_draw` sync system (`ecs/vfx_draw.*`,
    after `render_extract`; the CLI's render host and `LiveHost` install it) hands the RenderScene the
    live effect records with their age, `(steps - spawnStep - 1 + alpha) * dt` (`RenderScene::setVfx`,
    `render/scene/vfx_frame.hpp`). `vfx.expand` (after the sky) evaluates every particle slot in
    closed form, as `vfx/particle_math.hpp` does, and lights a `lit` one at its centre: the clustered
    lights through its froxel (every light when it is off screen), one shadow ray to the first
    directional light, RT GI's radiance at its pixel, else the environment's irradiance, else nothing.
    `vfx.sort` orders each alpha batch (<= `kVfxSortMax`) back to front by integer
    keys (distance bits, slot). `transparent` (after `sss`, before `post.bloom`) draws every blended
    mesh and effect batch back to front, ordered on the host by distance, then kind, then id, each
    with its own blend state (additive, alpha, premultiplied), with the G-buffer's jittered
    projection, soft against its depth, writing coverage to an R8 mask TAA lowers its history weight
    under. Blended meshes reuse `core/gbuffer.slang`'s `vertexMain` and are shaded by `vfx/transparent_mesh.slang`,
    the forward copy of the deferred light loop (`includes/lighting/clustered_lights.slang`; a plane
    forward-shaded equals the deferred one within 1 LSB). The pass needs ray queries
    and the bindless textures; without them nothing transparent draws.
  - **Gotchas:** particle slots are fixed in record order (no compaction; a dead one has size 0), and
    the slots are one buffer shared by the frames in flight (expand waits for the previous frame's
    draws). Slang decorates an unsized resource array with an `ArrayStride` spirv-val rejects
    (VUID-StandaloneSpirv-None-10684): declare the bindless table at its size, `Sampler2D t[4096]`. A
    closed blended mesh draws its far faces over its near ones (no depth write between blended
    faces): a pane is a quad. The G-buffer's normal target is `R16G16B16A16_SFLOAT` (attachment 1;
    recorded for step 3's decals, OPEN ruling 16).
  - **Smoke in RT** (step 4): a smoke record publishes closed-form proxy spheres (`ohao/vfx/smoke_math.hpp`,
    `caps.proxies` at most, 256 uploaded). They reach the RT traces through set 1 binding 7
    (`rt/smoke_proxy.slang`; the header's flag is `render.settings vfx.smokeInGi`): RT direct light's
    shading ray draws its visibility stochastically with `T` (its Z rays ignore smoke), `gi_trace` multiplies radiance by `T` and adds nothing (OPEN
    ruling 29: a flat in-scatter lit a filled doorway brighter, not darker). With no spheres `T` is exactly 1,
    so a scene without smoke is bit-identical. A sphere is
    the emitter's pose plus up to 0.6 R of offset: a cloud placed on a wall plane is half outside it.
  - **Decals are raster only** (no RT decal); tracers and particles are sorted back to front by integer
    keys (distance bits, then id).
  - **A GPU test that loads a level must install what the CLI installs** (`level::installLevels`, then
    `installPhysics`, as `tools/ohao/main.cpp` does). Without it the blockout is never expanded, the
    scene has no instances, `scene.upload` builds no TLAS, and RT direct light and RT GI silently do not run
    (the frame is the sky, the passes list has no `rt.gi`).
- `build/ohao` is the CMake binary directory of `ohao/`, not the CLI; the CLI is
  `build/Release/ohao.exe`.
- **A GPU fault is a report and an exit** (system design step 1, `gpu/vulkan/gpu_fault.*`): every submit, frame fence,
  acquire and present goes through it; a lost device, or a fence not signalled within 2 s (`kGpuTimeout`, Windows' TDR
  delay), logs one `GPU fault: {...}` line on the `gpu` channel (what each frame in flight ran, from breadcrumbs the
  pass timer writes with `VK_AMD_buffer_marker`; `VK_EXT_device_fault`'s report) and exits 5 with no Vulkan teardown.
  A new submit uses `gpu::submit` or `gpu::submitAndWait`, never `vkQueueSubmit` + `vkQueueWaitIdle`; a new pass is
  timed (`GpuPassTimer::begin`/`end`), or a fault inside it is reported as its neighbour's. A single submit that
  legitimately needs over 2 s is an unbounded dispatch: split it. `OHAO_INJECT_GPU_FAULT=lost:N` or `hang:N` fakes one
  at frame N without touching the GPU. Windows are capped at the display's refresh rate (`--fps N`, 0 uncapped; fifo
  is paced by the display).
- After a crash the GPU can stay wedged (device lost `-4` everywhere, 30 s renders of
  Cornell). It clears on reboot; it is not your change. `ohao doctor`'s `gpu.wedged`
  check says so. On Linux the NVIDIA open kernel module's GSP firmware can fail outright (`journalctl -k`:
  `Xid 120, GSP task exception`, then `NV_ERR_RESET_REQUIRED`): Vulkan then lists only the Intel GPU, every
  device creation asks the iGPU for ray tracing and the loader's error fails each GPU test. It needs a reboot (`nvidia-smi
  -r` refuses while the compositor holds the GPU). **Open (2026-11-08):** six GSP crashes, each during a GPU test, light
  ones included: five on driver 615.78.08 (2026-11-05 21:19 to 2026-11-06 18:35: Xid 120, 62, 119, and 109 context-switch
  timeouts before the fifth), one on 610.57.04 (2026-11-06 20:29, Xid 62), so no driver version is proven at fault. Two
  came during step 2b's runaway frames (dispatches of seconds; 2b is opt-in since, "Caustics"); the other four are not
  explained. Not heat (61 C at most), no PCIe errors; neither `nvidia-persistenced` nor
  `NVreg_DynamicPowerManagement=0x00` stopped one (both since undone). A failed GSP's state can survive a warm reboot
  (open-gpu-kernel-modules #1473): after a crash power the laptop off fully. **On Fedora 45 too** (RPM Fusion's
  615.78.08, 2026-11-08 19:04): Xid 119, "Timeout after 45s of waiting for RPC response from GPU0 GSP", during
  `check_env_orientation` (a sub-second render, bounded), after about 2.5 hours of GPU work; `[GPU requires reset]`.
  `nvidia-powerd` (Dynamic Boost, enabled by RPM Fusion) logged nothing before it and errors only after. So it follows
  the laptop across two distros and two driver versions; the production branch and the closed kernel module without
  GSP (`NVreg_EnableGpuFirmware=0`) are untried. **Never ship a dispatch whose work is unbounded**: a GPU dispatch
  is not preempted, and a few lanes walking for seconds hold their warps.
- **Linux, found by the port (2026-11-05):**
  - A pipeline must not outlive its layout on NVIDIA's Linux driver, `maintenance4` or not (Windows tolerated it):
    binding one whose layout was destroyed segfaulted in the driver. Keep every pipeline's layout until teardown
    (`GpuCulling`, `RTDirectTechnique` keep their extra ones in `m_extraLayouts`, `ManifoldCasters` its
    `m_groupLayout`). It is not always a crash: the manifold gather's groups dispatch also ran writing nothing, or hung
    the GPU, run to run.
  - Physics is deterministic run to run on one compiler, not across: MSVC's and GCC's builds land the blockout's
    crate one ulp apart, so `world.hash` and `tests/serve/expected` are recorded on the Linux build. The helmet's
    merged model bytes differ too.
  - `openpbr_tables --check` is byte for byte on the platform that wrote the tables; elsewhere every number within
    1e-5 (another math library's last digits: MSVC to GCC, at most 5.4e-6).
  - libstdc++ runs `std::execution::par` on oneTBB, linked where found (`TBB::tbb`).
  - Slang 2026 made `expand` a keyword; no entry point may be named so.
  - No custom command may output a path named like a target: the shader directory's `make_directory` edge was named
    `shaders` in Ninja, so `--target shaders` built the directory and no shader (made at configure time now).
  - slangc 2025.11 put an `ArrayStride` on arrays outside blocks (function-local constant arrays, the bindless table),
    which SPIRV-Tools 2026 and the validation layer 1.4.363 reject (VUID-StandaloneSpirv-None-10684) in nearly every
    shader: SPIR-V is made by 2026.20 (since 2026-11-06), whose 75 entry points all pass `spirv-val`.
  - **slangc 2026.20's C++ emitter re-reads a member after a store to it**: `Sampler.next()` (`const float u =
    point[dim & 3u]; ++dim; return u;`) became `dim = dim + 1; return point[dim & 3]` in C++, so every draw was the next
    coordinate; its SPIR-V of the same code loads first, as written. The twins (`tools/caustics`, `tools/material`)
    stay on 2025.11 (`SLANGC_CPP`) until a release's C++ passes `manifold_tests` and `openpbr_tables --check`.
  - `ohao serve` on stdio interrupts its reader with SIGUSR1 (no `SA_RESTART`), the POSIX `CancelSynchronousIo`.
  - NRI (under NRD) is told of no surface extension (`nrd_denoise.cpp`): given `VK_KHR_surface` it resolves the surface
    functions of every platform it was built for (Xlib and Wayland), a window's instance on Hyprland has Wayland's alone,
    and NRI's default handler raised SIGTRAP: a path-traced capture in a window killed `ohao serve`. NRI's messages go
    through the logger, with no abort.
  - A Wayland client cannot learn it was minimized (GLFW_ICONIFIED is always false there) and Hyprland has no minimize:
    a window never reaches the 0x0 drawable the host treats as minimized.
- Monograph: run its tests and `check_unit.py` from the repo root, never from `site/`
  (that directory shadows the stdlib `site` module and breaks `_ctypes`). The site build
  is the one exception: it imports `tools.monograph`, so it runs from `site/`.
  `build.py` has no `__main__`, so `python -m tools.monograph.build` does nothing.
  Removing a unit takes three edits: the `.md`, its `page()` entry in
  `site/tools/monograph/tree/<module>.py`, and its `author_rich.py` block, or the stale
  page keeps being served.

The monograph is frozen during the frontier renderer work (Frank, 2026-10-30): its citations
trail the code, so its checks are in no gate; run them by hand:

```bash
python site/tools/check_unit.py reflection/api
python site/tests/test_monograph_structure.py
cd site && python -c "from tools.monograph.build import build; build()"
```

## Conventions

- **Spec → plan → build.** A sub-project gets a design in `docs/specs/`, an
  implementation plan in `docs/plans/`, then a build in the plan's steps, each ending
  with the engine running and every gate green. If the spec turns out wrong, fix the spec
  in the same commit and say so.
- **A change record per sub-project (or build step)** in
  `docs/changes/YYYY-MM-DD-<subproject>[-stepN].md`, written for the project wiki. Shape:
  a title; `**Spec:**` (path and sections) and `**Status:**` lines; then numbered
  sections — summary, what was built, bugs found and fixed, spec corrections, results
  (measurements, benchmark medians/p95, golden max_abs), known gaps. Facts and numbers only,
  from runs, never estimates.
- **Commits:** local, one or a few per step, subject `type(scope): what` (`feat`, `fix`,
  `perf`, `docs`, `test`), body saying why. Never push, merge, rebase or reset without
  the owner's say-so.
- **Forward only.** Old code and docs are reference, not something to preserve: delete
  what is dead, and describe the tree as it is, not its history.
- **Engine code logs through `OHAO_LOG(level, channel, fmt, args...)`** (`core/log.hpp`),
  never `std::cout`/`std::cerr`/`printf` (stdout is the protocol's in `ohao serve`).
  Per-item and per-frame detail is `Debug` (shown with `OHAO_LOG_LEVEL=debug`).
- **Performance is measured, not assumed:** a new hot path is timed with the benchmark tools by hand
  (`render_bench --only SCENE` for the renderer). Reflection never runs per frame (the ECS system sources
  do not include `reflect/` or `api/`).
- **No test per feature or per spec step.** A step proves itself by measurement and the five gates; a new
  gate needs a requirement it serves or a bug that got through, and a planted bug it catches. A golden is
  updated only after looking at what changed.
