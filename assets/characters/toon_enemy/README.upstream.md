# Upstream: Quaternius, "Character Enemy" (Toon Shooter Game Kit)

- **Author:** Quaternius (https://quaternius.com).
- **Licence:** CC0 1.0 Universal (public domain dedication), `LICENSE.md`.
- **Source:** "Character Enemy", Toon Shooter Game Kit, updated 2024-03-16.
  - Page: https://poly.pizza/m/mdGe4IN31v
  - File: https://static.poly.pizza/6d2fe602-2adf-43d2-aa20-00e68165497d.glb
  - Kit: https://poly.pizza/bundle/Toon-Shooter-Game-Kit-qraiSXoAru
  - Downloaded 2026-10-28 as `Character_Enemy.glb`, sha256
    `0af9db940907efa76662838a0718bc7b9fec6162332f31d465a856e9fb1730ab` (FBX2glTF v0.9.7 export).
- **Upstream content:** a 43-joint rig; the body and the head as two meshes on two identical
  skins; 14 weapon meshes parented to the right hand's `Index1.R`; flat-colour materials
  (no textures); clips Idle, Walk, Run, Run_Gun, Idle_Shoot, Walk_Shoot, Run_Shoot, HitReact,
  Death, Duck, Jump, Jump_Idle, Jump_Land, Punch, Wave, Yes, No, each also duplicated as
  `CharacterArmature|<name>`.

## What `tools/anim/make_enemy.py` changed

Rebuild with `python tools/anim/make_enemy.py --source <dir holding Character_Enemy.glb>`
(the script checks the sha256 above; two runs write the same bytes). Blender was not used.

- **`enemy.glb`:**
  - one mesh `Enemy` (the body and the head merged) on one skin `EnemySkin`, same 43 joints;
  - the weapon meshes and the joints' `_end` leaf nodes dropped;
  - units: the armature's ×100 and the mesh's export transform baked; fitted to 1.80 m (the
    character controller's capsule; upstream 2.28 m), so the skeleton's root is unscaled;
  - turned 180° about +Y: it faces −Z, the engine's forward;
  - inverse bind matrices recomputed for the above (the upstream bind pose, a T-pose, is kept);
  - materials kept as authored (base colour, metallic 0.4, roughness 0.27);
  - clips kept and renamed: Idle → `idle`, Walk → `walk`, Run_Gun → `run`, Walk_Shoot →
    `walk_aim`, Run_Shoot → `run_aim`, Death → `death`; the duplicates and the other clips
    dropped;
  - clips made by the script (the kit has no aim set or reload):
    - `aim_idle`: Idle_Shoot's first frame, held;
    - `rest`: the rest pose (additive zero);
    - `aim_up`, `aim_down`: the spine pitched ±60° (Abdomen 20°, Torso 25°, Neck 15°), additive;
    - `fire`: Idle_Shoot relative to its first frame, additive;
    - `hit`: HitReact relative to its first frame, additive;
    - `reload`: 2 s, additive: the torso and head dip, the left arm drops to the magazine and
      comes back up. Its events (`mag_out` 0.6 s, `mag_in` 1.3 s, `bolt` 1.75 s) are in
      `assets/anim/enemy.animgraph.json`.
- **`rifle.glb`:** the upstream `AK` mesh alone, its vertices in the `Index1.R` joint's frame,
  fitted as above: attached there with an identity `Transform` it sits in the hand as upstream.
  The kit's separate `Rifle.glb` (poly.pizza `KjHL5JJrxU`) was not used: it is texture-atlased
  in another style and has no grip transform for this rig. Upstream's `Wood` and `Grey2`
  materials say `alphaMode: BLEND` with a base colour alpha of 1 and no texture (nothing in them
  is transparent): they are written opaque (`solid()`), so the engine draws them in the G-buffer.
