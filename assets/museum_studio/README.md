# Museum studio pack (OHAO inverse)

NIUA-generated glTF props + marble floor reference for the **cinematic museum** inverse preset.

| File | Role |
|------|------|
| `statue.glb` (+ textures) | Default hero (`--preset museum`) |
| `amphora.glb`, `bust.glb` | Alternate heroes |
| `pedestal.glb` | Fixed pedestal mesh (not free θ) |
| `floor_albedo.jpg` | Look-dev / GT seed reference |
| `floor_normal.jpg` | Reference only (not free θ v1) |

**Source:** `/home/frankyin/Desktop/lab/game/untitled_1/assets/` (NIUA MCP Godot project).

**Inverse contract:** free θ is **ground dense maps** only; hero/pedestal are beauty/occlusion.

```bash
./build/inverse_fit --backend diff --preset museum --dense-map \
  --dense-map-res 64 --dense-grid 8 \
  --fit-width 256 --fit-height 144 \
  --out-dir renders/diff_museum_smoke
```
