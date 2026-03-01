# OHAO Live Engine Server

Parent: docs/API.md | Search: docs/INDEX.md

OhaoServer autoload — starts automatically at http://127.0.0.1:9756/
Lets AI agents read and modify the live running engine via HTTP.

---

## #discovery

```
WebFetch("http://localhost:9756/", "list all endpoints and what they do")
```

Returns the full endpoint manifest. Always call this first if unsure what's available.

---

## #scene

```
# What's in the scene right now?
WebFetch("http://localhost:9756/scene", "list actors")

# Add a red cube at y=1
POST http://localhost:9756/scene/actor
{
  "type": "cube",
  "name": "RedBox",
  "pos": [0, 1, 0],
  "scale": [2, 2, 2],
  "color": [1, 0, 0, 1]
}

# Add with material preset
POST http://localhost:9756/scene/actor
{"type": "cube", "name": "StoneWall", "pos": [5, 0, 0], "material": "stone"}

# Remove
DELETE http://localhost:9756/scene/actor?name=RedBox

# Build a full scene from a dict (same as Ohao.scene().build())
POST http://localhost:9756/scene/build
{
  "template": "arena",
  "rendering": "cyberpunk",
  "objects": [
    {"type": "cube", "name": "Box", "pos": [0,1,0], "color": [1,0.5,0,1]}
  ],
  "lights": [
    {"type": "point", "name": "L1", "pos": [3,4,0], "color": [1,1,1], "intensity": 2.0}
  ]
}

# Clear everything
POST http://localhost:9756/scene/clear
```

Actor type values: `cube` | `sphere` | `plane` | `cylinder`
pos/rot/scale: JSON array `[x, y, z]`
color: JSON array `[r, g, b, a]` (0..1)

---

## #camera

```
# Where is the camera?
GET http://localhost:9756/camera
→ {"position": [0,2,5], "forward": [0,0,-1], "mode": 0}

# Move camera
POST http://localhost:9756/camera
{"position": [10, 5, 10], "rotation_deg": [-20, 45, 0]}

# Focus on scene (auto-frame all actors)
POST http://localhost:9756/camera
{"focus": true}
```

---

## #effects

```
# What effects are currently on?
GET http://localhost:9756/effects
→ {"bloom": {"enabled": false, "bloom_threshold": 0.8, ...}, "ssao": {...}, ...}

# Enable bloom with params
POST http://localhost:9756/effects
{"bloom": {"enabled": true, "bloom_threshold": 0.4, "bloom_intensity": 1.5}}

# Enable multiple effects at once
POST http://localhost:9756/effects
{
  "bloom":      {"enabled": true, "bloom_threshold": 0.5},
  "ssao":       {"enabled": true, "ssao_intensity": 1.2},
  "tonemapping":{"enabled": true}
}

# Apply a named rendering preset
POST http://localhost:9756/effects/preset
{"rendering": "horror"}
# presets: horror | cyberpunk | bright | cinematic | fps_action | minimal
```

---

## #physics

```
# Start simulation
POST http://localhost:9756/physics/play

# Pause
POST http://localhost:9756/physics/pause

# Step exactly 60 frames (1 second at 60fps)
POST http://localhost:9756/physics/step
{"steps": 60}

# Stop and reset
POST http://localhost:9756/physics/stop

# Raycast
POST http://localhost:9756/physics/raycast
{
  "origin": [0, 5, 0],
  "direction": [0, -1, 0],
  "max_dist": 20.0,
  "layer_mask": 65535
}
→ {"hit": true, "position": [0,0,0], "normal": [0,1,0], "body_handle": 3}
```

---

## #agent-workflow

Typical agent session to set up a scene:

```
1. GET  /               → understand what's available
2. POST /scene/clear    → fresh start
3. POST /scene/build    → build from template dict
4. GET  /scene          → verify actors spawned
5. POST /effects        → configure post-processing
6. POST /camera         → {"focus": true}
7. POST /physics/play   → start simulation
8. (iterate) POST /physics/step {"steps": 10} → observe changes
9. POST /physics/stop   → reset when done
```
