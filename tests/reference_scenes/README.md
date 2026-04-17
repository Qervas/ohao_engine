# Reference Scenes

Maintained scene library for per-feature visual verification of the rendering pipeline.

## Layout

- `cornell/` — controlled Cornell box variants (unit-level correctness; none yet)
- `community/` — curated Blender community scenes (Bistro, Sponza, Classroom; none yet)
- `custom/` — OHAO feature regression suite, one directory per scene

## Per-scene contents

Every `custom/` scene directory contains:

- `scene.md` — description: what features it validates, camera, lighting, command
- `source/` — source assets (GLB, HDR, etc) or a path pointing to existing `assets/`
- `reference.png` — known-good render (committed PNG; update only when a feature passes review and the change is intentional)
- `cycles_reference.png` (optional) — Blender Cycles render at matched sample count for cross-engine comparison
- `verification_log.md` — appended entries each time a feature is verified against this scene

## Adding a scene

1. Point `scene.md` at assets already in `assets/` where possible; otherwise place source under `source/`
2. Render it with the current engine state using the command documented in `scene.md`
3. Render a Cycles-equivalent if possible
4. Commit both as `reference.png` and `cycles_reference.png`
5. Tag which feature(s) the scene validates

## Running the variance tool

```
python3 tools/compare_variance.py baseline.png candidate.png
```

Reports RMSE, local variance for each image, and a percentage noise reduction. Writes a side-by-side diff next to the candidate PNG.
