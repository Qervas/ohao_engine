# Frontier F5d: the referee asked, and the answer was not ReSTIR GI

**Spec:** `docs/specs/2026-10-30-frontier-renderer-design.md` (section 3, F5: "ReSTIR GI on the first" bounce); F5c's
change record: "F5d (ReSTIR GI) next, if the referee asks for it".
**Status:** decided: F5d is not built. The referee's largest gap was the path tracer's camera seeing back faces the
raster culls, fixed here; the indirect light it measures is within 3% of the path tracer over the frame.

## 1. Summary

`check_parity.py` (deferred against the path tracer, linear radiance) was run to decide F5d. Its largest gap, Sponza's
0.29 stops with the top-left vault 16% dark, was split light by light and turned out not to be indirect light at all:
the path tracer's camera saw the back face of a one-sided arch that the raster culls, and the arch's shadow on the wall
behind it (which the raster shows) read as deferred being dark. The camera ray now passes such faces, as the raster
does.

## 2. How the gap was split

All on Sponza's view, the top-left vault (rows 0-90, columns 0-160 of 640x360), mean luminance:

| | vault | frame |
|---|---|---|
| path tracer, direct only (`bounces 0`) | 0.0701 | 0.1431 |
| path tracer, all bounces | 0.0895 | 0.1654 |
| deferred (RT direct light + RT GI, 32 frames) | 0.0745 | 0.1657 |

- **Not convergence:** deferred at 256 and 1024 frames read 0.839 and 0.843 of the path tracer in the vault (0.833 at 32).
- **Direct, not indirect** (the sky removed, so RT GI off leaves direct light alone): direct 0.814 of the path tracer
  in the vault, indirect 0.912; over the frame direct 0.999, indirect 1.028.
- **Not ReSTIR DI's reuse:** reference quality (no reuse) 0.820, full-resolution tracing 0.815, no spatial reuse 0.810.
- **Not the sun:** it gives the vault nothing in either renderer; the five atrium lamps light it (0.819).
- **Not the lights' range:** at a 50 m range the region stays black.
- **The geometry:** the light loop (unshadowed) lights a brick wall there; the path tracer's camera saw an arch's
  surface in front of it. Every geometry path (meshlets, instances, occlusion culling off) draws the wall: the raster
  culls the arch's back face (22 of Sponza's 25 materials are one-sided), the TLAS does not
  (`TRIANGLE_FACING_CULL_DISABLE`). RT direct light's shadow rays meet the arch, so the wall the raster shows is in its
  shadow: black, correctly.

## 3. What was built

- Material row 10's w is 1 for a one-sided material (`MaterialTable::pack`, from `MaterialDesc::doubleSided`).
- The path tracer's closest hit flags `kHitCulled` when the material is one-sided and the world-space winding faces away
  from the ray (the projection flips y, so the raster's counter-clockwise front is the right-hand winding normal; a
  mirroring transform turns it). The camera ray passes such a hit and traces on from it, up to `kCullSkips` (8), adding
  the distance to `firstHitT`. Every other ray meets both sides.

### Found on the way: the path tracer's pipeline cached on disk

`check_live` test 11 failed in two gate runs: the headless host's path-traced captures never answered. Run alone they
took 6 s; with the test's window process alive beside it the headless host took 239 s, past the test's 120 s reply
wait, all of it creating the path tracer's ray tracing pipeline (its log stopped before `PathTracer: initialized`).
The engine passed no `VkPipelineCache`, so a process reused a compile only through the driver's own disk cache, which
the second process did not get. `render/rt/pipeline_disk_cache.*` keeps one per device and driver in a file
(`%LOCALAPPDATA%/ohao/pipeline_cache`, or `$OHAO_PIPELINE_CACHE_DIR`; 10.7 MB), loaded before the pipeline is created
and saved right after. The same window-then-headless run: 5.0 s and 4.8 s.

## 4. Results

`check_parity.py` before and after (mean |stops| over 8x8 blocks; deferred's mean over the path tracer's):

| scene | before | after |
|---|---|---|
| cornell_box | 0.075, 0.986 | 0.07, 0.986 |
| damaged_helmet | 0.145, 1.004 | 0.14, 1.004 |
| khronos_metal_rough_spheres | 0.12, 0.997 | 0.11, 0.999 |
| area_light | 0.063, 0.989 | 0.06, 0.989 |
| khronos_sponza | 0.291, 1.016 | 0.16, 1.036 |
| blockout_room | 0.068, 0.961 | 0.07, 0.961 |

Sponza's vault, the lamps' direct light alone at reference quality: deferred 0.819 of the path tracer before, 1.062
after (both now show the wall in the arch's shadow). Goldens: section 5.

## 5. Goldens

A full run after the change: 37 of 42 unchanged. `cli_anim_rig_pt` (223 px, up to 68 levels): the camera now sees
through the seams between the rig's segments and past the foot what deferred shows, not the boxes' inside faces (the
culled hits checked by marking them red). `damaged_helmet` and `cli_damaged_helmet` (17 isolated pixels, up to 6): no
camera ray meets a culled face there; the shader's code moved them. `cli_subsurface_deferred` and `_reference` (1 level,
rounding): the SSS input's full floats (`docs/changes/2026-11-01-openpbr-gaps.md`, section 3). Each looked at and noted.

## 6. Known gaps

- **A camera ray passes at most 8 culled faces** (`kCullSkips`); past them it shades the ninth as it is.
- **The F4 frame budgets** remain Frank's ruling (F4's change record).
- **Only the path tracer's pipeline is cached**; the first process after a shader change still compiles it cold
  (minutes), and the cache file only grows until it passes 64 MB, when it starts empty.

## 7. How to run

```
python tests/renderer/check_parity.py
python tests/renderer/check_parity.py --only khronos_sponza
```
