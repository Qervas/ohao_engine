# NIUA museum pack (source)

Path: `/home/frankyin/Desktop/lab/game/untitled_1/assets/`

## Heroes / props (glTF + atlas textures)
| Asset | Path | Role |
|-------|------|------|
| statue.glb | props/statue.glb | Primary hero candidate |
| amphora.glb | props/amphora.glb | Primary hero candidate |
| bust.glb | props/bust.glb | Primary hero candidate |
| pedestal.glb | props/pedestal.glb | Replace procedural box pedestal |
| tablet.glb | props/tablet.glb | Optional fixed set dressing |
| column.glb / arch.glb | env/ | Optional backdrop dress (fixed, not free θ) |

## Floor
| Asset | Path | Inverse role |
|-------|------|--------------|
| floor_albedo.jpg | env/floor_albedo.jpg | Visual reference + optional GT paint seed (marble tile) |
| floor_normal.jpg | env/floor_normal.jpg | Fixed normal if engine supports; not free θ v1 |

## Import into ohao (Phase 1)
Copy selected glbs + textures into `assets/museum_studio/` (or symlink) so inverse presets are self-contained in-repo.
