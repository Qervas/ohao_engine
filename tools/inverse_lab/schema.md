# Capture schema v1

See `docs/inverse_lab.md` for full layout.

Required files:

| Path | Description |
|------|-------------|
| `capture.json` | Manifest (`format=ohao_inverse_lab_capture`, `version=1`) |
| `cameras.jsonl` | One camera + image file per line |
| `images/*.png` | Observations |
| `theta_gt.json` | Synthetic GT θ (optional for real capture) |
| `materials/*` | Albedo/ORM maps (constant at L1–L2) |
| `relight/*` | Optional alternate-env GT |

Camera line example:

```json
{"index":0,"name":"front","file":"train_000.png","split":"train","position":[0,1.35,7.2],"pitch_deg":-8,"yaw_deg":-90,"fov_deg":40}
```
