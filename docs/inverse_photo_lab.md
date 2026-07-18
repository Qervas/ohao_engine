# Inverse Photo Lab (H3)

**Status:** M4a recipe + M4b **photo-proxy plate** landed. Real phone/COLMAP captures plug into the same bundle format; they are not required to develop the pipeline.

**Principle:** never claim synthetic LABTEST bars (≥28/≥26/≥8) on photo data. Report **gain vs wrong-init** and show photo vs re-render stills. Failures are published, not hidden.

---

## 1. Why two capture kinds

| Kind | Source | Gate |
|------|--------|------|
| `synthetic_export` | Engine `--export-capture` | Full LABTEST (capture-gated PSNR bar) |
| `photo_proxy` | Synthetic export + **domain shift** (exposure / noise / compression) | **PHOTOTEST** — gain + visual; no fake ≥28 |
| `real_photo` | Phone / DSLR + COLMAP / board | PHOTOTEST (same) + document calibration |

`photo_proxy` exists so we can ship an honest multi-view plate **before** a full real-world capture session, while still breaking pure sim-to-sim theater.

---

## 2. Capture recipe (real photos — M4a)

### Minimum viable product shoot

| Item | Spec |
|------|------|
| Object | 1 diffuse-ish product + optional slightly glossy |
| Views | **8–20** stills, ~15–30° orbit, overlap for pose |
| Pose | COLMAP or ChArUco board → `cameras.jsonl` |
| Mesh | Fixed at fit time (phone scan / existing GLB / simplified mesh) |
| Lighting | Indoor; second session optional for “relight” |
| Color | Locked white balance; record exposure / ISO / shutter |

### Bundle layout (same spine as lab capture)

```
photo_capture/
  capture.json          # format + capture_kind + calibration notes
  cameras.jsonl         # one line per image (position / pitch / yaw / fov)
  images/train_*.png    # or .jpg converted to PNG
  images/holdout_*.png
  relight/              # optional second lighting session
  materials/            # optional init maps (not GT for real photo)
  NOTES.md              # free-text failure modes / shoot notes
```

`capture.json` must include:

```json
{
  "format": "ohao_inverse_lab_capture",
  "version": 1,
  "capture_kind": "real_photo",
  "metric_domain": "photo_images",
  "calibration": {
    "pose_source": "colmap|charuco|manual",
    "wb": "locked",
    "exposure_notes": "..."
  }
}
```

### Camera line (same as lab)

```json
{"index":0,"name":"v00","file":"train_000.png","split":"train","position":[x,y,z],"pitch_deg":-8,"yaw_deg":-90,"fov_deg":40}
```

---

## 3. Photo-proxy plate (M4b — run today)

```bash
# One-shot (default 640×360 multi-view; set SHOW_W/H for higher res)
./scripts/run_inverse_photo_plate.sh

# Manual steps (same as script)
./build/inverse_fit --export-capture --preset lantern --views 4 \
  --show-width 640 --show-height 360 --show-spp 128 --map-res 2 \
  --out-dir renders/photo_lab/lantern_export
python3 tools/inverse_lab/make_photo_proxy.py \
  renders/photo_lab/lantern_export/capture \
  renders/photo_lab/lantern_proxy/capture \
  --exposure 0.88 --noise 0.015 --jpeg-q 88
./build/inverse_fit --lab-bundle renders/photo_lab/lantern_proxy/capture \
  --preset lantern --quality draft \
  --show-width 640 --show-height 360 --show-spp 64 --fit-spp 32 \
  --iters 16 --multi-start 3 --no-visual-polish \
  --out-dir renders/photo_lab/lantern_photo_fit
python3 tools/inverse_lab/test_photo_plate.py renders/photo_lab/lantern_photo_fit \
  --capture renders/photo_lab/lantern_proxy/capture
```

Higher res (slow under PT FD): `SHOW_W=1280 SHOW_H=720 ./scripts/run_inverse_photo_plate.sh`  
Harder hero: `PRESET=helmet ./scripts/run_inverse_photo_plate.sh`

### Measured plate (lantern photo_proxy, 640×360)

| Metric | Value |
|--------|-------|
| capture_kind | `photo_proxy` |
| wrong-init holdout PSNR | 13.8 dB |
| recovered holdout PSNR | **28.7 dB** |
| holdout gain | **+14.9 dB** |
| train photo vs re-render gain | **+13.8 dB** |
| PHOTOTEST | **PASS** |
| Strip | `renders/photo_lab/lantern_photo_fit/photo_vs_rerender.png` |

---

## 4. PHOTOTEST (honest)

| Check | Rule |
|-------|------|
| Capture kind | `photo_proxy` or `real_photo` |
| Holdout gain | recovered holdout PSNR ≥ wrong-init + **3 dB** |
| Train | recovered train PSNR ≥ wrong-init train + **2 dB** (if present) |
| Stills | photo target / wrong-init / recovered side-by-side |
| Failure log | `failure_modes.json` present (known residual risks) |
| Anti-theater | **Must not** require synthetic ≥28 holdout |

Relight under photo domain is optional; if present, report gain but do not invent thresholds.

---

## 5. Known failure modes (always publish)

| Mode | Symptom | Mitigation |
|------|---------|------------|
| Exposure mismatch | Global brightness wrong | `--fit-exposure` or proxy exposure tags |
| Specular blowouts | Metal chart / hard lights | Lower key, more views, rough prior |
| Mesh / pose error | Silhouette ghosts | Better COLMAP, fix mesh |
| Domain gap | High train, weak holdout | More train views; stop claiming LABTEST |
| Shadow / GI mismatch | Floor contact wrong | Hybrid PT refine (stretch) |

---

## 6. Non-goals

- Unposed casual selfie inverse  
- Free NeRF-style camera field without mesh  
- Claiming Mitsuba-3 / public SOTA numbers  

**Dense maps under Deferred. Truth under capture images. Gates on gain + visuals. No theater.**
