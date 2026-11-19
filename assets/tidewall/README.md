# assets/tidewall: Tidewall's levels and scenes

The mission demo's content (`docs/specs/2026-11-18-tidewall-design.md`; the game is `games/tidewall`). Levels are glTF
2.0 binaries built from a layout file by `tools/level/build_level.py` and the kit (`tools/level/kit`), which sets every
node's level hints; `.gitattributes` keeps them byte for byte.

| File | What it is | Made by | License |
|---|---|---|---|
| `levels/yard.glb` | B1 and B2's pump yard: 60 x 40 m inside 3 m walls (a 6 m north gate, a 2 m south entry, an east door), on a 90 x 70 m ground slab; a pump house (12 x 8 m, two doorways, a door, a roof, a pump), a 22 m catwalk at 3.2 m with a 16-step stair, 8 containers (one stacked), a high pipe rack and a low pipe run, 6 low (1.0 m) and 3 high (1.6 m) covers, a loading dock (1.2 m) with a ramp, 2 doors (navmesh obstacles, `NavObstacle`); a drop off the catwalk's west end (`NavLink`); 13 spawn points (`player`, 12 x `enemy`), 2 triggers and a view camera. 129 nodes, 114 meshes, 1,620 triangles | `tools/level/build_level.py` from `games/tidewall/level/layout.json`, Blender 5.3.0 | MIT, as the repository: geometry and materials are generated |

Rebuild (byte for byte with Blender 5.3.0, the version the tool pins; about 2 s):

```
"C:/Program Files/Blender Foundation/Blender 5.3/blender.exe" -b --factory-startup --python tools/level/build_level.py -- games/tidewall/level/layout.json assets/tidewall/levels/yard.glb
```

Scenes (play them with `--game build/Release/ohao_tidewall.dll`): `scenes/yard.scene.json`, the yard and the player
at the south entry; `scenes/yard_walkers.scene.json`, the same with 16 walkers that path to the player on the navmesh
(written by `games/tidewall/level/make_walkers.py 16 ...`).
