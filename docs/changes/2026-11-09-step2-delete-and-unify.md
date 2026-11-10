# Step 2: delete and unify (parts a to h)

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.1, 4.2, 4.9 and 5 (step 2); plan
`docs/plans/2026-11-09-step2-delete-and-unify.md`. **Status:** 2a-2i built.

## 1. Summary

Facts defined once and checked at build; what nothing used deleted. The system order, the snapshot hooks, the API's
methods, the render settings and every shader binding now have one definition each, and the build fails when C++
and a shader disagree or a dependency points up a layer. `AGENTS.md` went from 1,204 lines to 203.

## 2. What was built (one commit each)

- **2a, delete** (`a65698f7`): the event bus, command history, `core.hpp`, the one-implementation GI and reflection
  technique interfaces and their concept, the uncalled shader hot-reload path, `Model`'s LOD fields, the unread
  error corpus `tests/scenes/invalid/`; the render graph cut from 1,333 lines to 224 (imported textures, passes in
  order, the barriers between them, timing). About 1,600 lines.
- **2b, one install** (`a4908779`): `ecs/system_order.hpp` is the one order of every system (an unlisted or duplicate
  name throws); snapshot hooks are named and restored in a fixed order; every host installs through
  `engine::installEngine` and `engine::installRendering` (new `ohao_engine`).
- **2c, one method table** (`6f501a33`): one row a method (doc and handler); `tools/check.py` fails the build when
  `docs/agents/ohao-cli.md`'s method list and the catalogue differ.
- **2d, one RenderSettings** (`e2885729`): `render::RenderSettings` is the only shape; the converters are gone.
- **2e, generated bindings** (`ed7f7bc1`, `e06b7c97`): `tools/gen_bindings.py` writes every shader's bindings and push
  constants from Slang's reflection; C++ names them; hand push structs carry `OHAO_CHECK_PUSH`; `check.py` fails the
  build on a stale header, a binding declared twice, or a binding number typed in C++.
- **2g, layers** (`110d6d3f`): `cmake/ohao_layers.cmake` fails configure on a link or include up a layer; one listed
  exception (`ohao_ecs -> ohao_renderer`, step 3).
- **2f, one scene writer**: the save budget raised to 11 ms (Frank); the 36 hand writers, the layout tree and the
  second save path deleted (about 1,200 lines); every checked-in scene saves byte for byte as before.
- **2h, AGENTS.md** (`c6c49f6d`): see 4.
- **2i, CI on this machine**: `.githooks/pre-commit` runs `tools/check.py --staged`, the gates the staged files reach
  (a docs commit builds and runs nothing); `.githooks/pre-push` the whole check. Two checks never overlap
  (`build/check.lock`). Spec 4.9 corrected: it planned GitHub Actions on Windows and Linux, CPU gates only; Frank
  ruled for CI on this machine (2026-11-09), where the GPU gates run too. Proven before the commit through a scratch
  index: a docs change passed in 0.17 s without building; a compile error in `ecs/physics_scene.cpp` blocked the
  commit in 3.4 s; a no-op physics restore blocked it in 10.2 s (`replay_test` failed, the one test chosen). Also
  deleted: `ohao doctor`'s `python.deps`, which ran `python` on PATH (not the configured interpreter the harnesses
  use) and warned on every run here; `check.py` checks the right one.

## 3. Bugs and drift found and fixed

- The window and the CLI ran `instantiate` and `player_spawn` in opposite orders (both now the CLI's).
- `docs/agents/ohao-cli.md` lacked `vfx.info`.
- `rt/restir_di.slang` declared set 0 binding 45 twice (`gLayers`, and `rt/lights.slang`'s `emissiveFirst`);
  `gLayers` moved to 26.
- The deferred lighting layout declared and wrote bindings 3 and 4, which its shader lacks; deleted.
- The G-buffer pass built and wrote a descriptor set (with its own pool and sampler) no shader read; deleted.
- TAA pushed 8 bytes its shader lacks; bloom's downsample flag rode in a field named `blendFactor` (now asserted to
  sit where the shader's `isFirstPass` does); the exposure push named `size` `width, height`.

## 4. AGENTS.md: where each rule went

Kept (`AGENTS.md`, "Rules that hold everywhere"): tests racing on fixed paths; ECS writes through `patch`; the
install and system order; simulate versus sync; input per step; GPU faults. Added there as one-liners (each also in
the notes): unbounded dispatches, one call site per OpenPBR entry, the display transform and the zenith row, the
renderers agreeing, Frank's assets.

Moved verbatim:
- `docs/notes/renderer.md` (519 lines): alpha modes; descriptor sets per frame in flight; path-traced windows;
  readback; GPU culling and the G-buffer's lists; `resetAccumulation`; render settings; ambient occlusion; specular
  indirect light; the sun; the equirectangular map; the renderers' agreement; back faces; reflected and indirect
  light; the G-buffer's attachments; glass; subsurface; the `[shader]` attribute; OpenPBR's layers; the referee; the
  path tracer, its display, sky, exposure, denoising, guiding, caustics, frame cost, call sites, cold compiles;
  emission; the radiance cache; RT GI's denoiser, bounces and lights; light units; emitter meshes; capture frames;
  RT direct light; clustered lighting.
- `docs/notes/simulation.md` (428 lines): Jolt; the AABB convention; levels; physics in the step; combat; frame
  timing; animation; audio; VFX.
- `docs/notes/platform.md` (81 lines): conda's cmake; Git Bash; line endings; duplicate symbols; `GLM_ENABLE_
  EXPERIMENTAL`; restored mtimes; `build/ohao`; a wedged GPU and the GSP faults; the Linux port; the monograph.

Deleted: the validation layer's report limit (already under "Gates").

## 5. Results

Gates after each part: 10 of 10 (quick, gpu, golden), validation clean, goldens unchanged, no Xid; GPU at most 51.6 C.
2f (`scene_bench --check`): `save_pretty` 10.32 ms against 11 (the hand writers had been 7.40); every other budget
as before except `restore_unchanged` at 1.054 ms against 1, which restore code not touched here measures (not
compared with the old build).

## 6. Known gaps

- 2i: the Windows clone needs `git config core.hooksPath .githooks` once; no hook has run there.
- The benches still install single systems (they time one each); with the order fixed by the list this cannot
  misorder anything.
- The path tracer's layout keeps its per-binding stage masks by hand (reflection does not say which stage reads a
  binding); a start-up check holds it to the shader's set 0 in size and names.
