# assets/local: staged assets that stay out of git

Everything here except this file is git-ignored. It holds assets OHAO may load on this
machine but may not publish. Today that means Frank's Somnabulate props, staged by the level
pipeline (`docs/specs/2026-10-26-level-pipeline-design.md`, "Assets and staging"). The
repository is MIT and has a GitHub origin, so a committed asset would be published under
MIT by the next push. Somnabulate assets stay here until Frank rules otherwise (that spec's
Open decision 1).

Rules:
- **Sources are read only.** Nothing under `C:\Users\djmax\Desktop\games` or
  `D:\Unreal Projects\somnabulate` is written. Staging copies or converts into this folder.
- **Scenes that use these assets live here too** (`assets/local/*.scene.json`). Checked-in
  scenes, goldens and `ohao doctor` load only tracked assets.
- **Tests skip without them.** A test that needs a staged asset reports `skip` when it is
  missing, never a pass.

## Flat42 props (Olivier, Somnabulate dream 1)

- **Source:** `C:\Users\djmax\Desktop\games\somnabulate\assets\dreams\dream_01_held_second\flat42_o33\Assets\Olivier\Props\Flat42\`.
  Each prop has an `SM_<Prop>.fbx` (LOD0; `_LOD1` is not staged) and `T_<Prop>_BC/N/ORM.png`.
  These are Frank's assets; their license is Frank's to state.
- **Staged into:** `flat42/<Prop>.glb`, plus `flat42/_T_<Prop>_N_gl.png`, the normal map
  flipped to OpenGL's convention.
- **Staged by** Blender 5.2.2, headless:

```
C:/Users/djmax/scoop/shims/blender.exe -b --factory-startup --python tools/level/stage_flat42.py -- "C:/Users/djmax/Desktop/games/somnabulate/assets/dreams/dream_01_held_second/flat42_o33/Assets/Olivier/Props/Flat42" assets/local/flat42 PinnChair KitchenTable BookStack Kettle
```

- **What the conversion does:** rebuilds the material from the three textures and flips the
  normal map's green channel. It keeps the `UCX_` convex hulls with
  `{"col":"convex","vis":0}` and the `SOCKET_` empties with `{"socket":…}`. The hint
  schema is in the spec.
