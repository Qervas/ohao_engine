# OHAO Engine API — Navigation Root

## How agents use this

1. Search `docs/INDEX.md` with your keyword to find the right file+section
2. Read only that file (or just the section offset)
3. Never load all docs at once — each branch is self-contained

```
Grep("grab", "docs/INDEX.md")       → physics.md#grab
Grep("bloom", "docs/INDEX.md")      → render.md#effects
Grep("scene build", "docs/INDEX.md")→ scene.md#builder
```

## Tree

```
docs/
  API.md        ← you are here (navigation root, always tiny)
  INDEX.md      ← keyword → file#section map (search here first)
  server.md     ← live HTTP server :9756 — read/write running engine without code
  physics.md    ← bodies, constraints, breaking, grab/throw, character, raycasting
  render.md     ← post-processing effects, materials, viewport toggles
  scene.md      ← scene builder, actors, lights, templates, sync
  audio.md      ← play sounds, 3D spatial, categories, handles
  gdscript.md   ← Ohao autoload, OhaoConst, helpers, common patterns
```

## Quick orientation

| Task | File |
|------|------|
| Add physics to an object | gdscript.md#physics-body |
| Set up a joint/hinge | physics.md#constraints |
| Grab and throw objects | physics.md#grab |
| Enable bloom/SSAO | render.md#effects |
| Build a scene from dict | scene.md#builder or POST /scene/build |
| Inspect live running engine | server.md |
| Play a sound | audio.md |
| Raycast from camera | physics.md#queries |
| FPS player movement | gdscript.md#fps-pattern |
