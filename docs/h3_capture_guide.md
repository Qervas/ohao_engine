# H3 / M1 — Real-photo capture guide (ChArUco)

How to shoot a real object with a phone and turn it into an OHAO lab bundle with
**metric** camera poses, using `tools/inverse_lab/photo_ingest.py`.

Why a board at all: `colmap_solve.py` (M0b) could only fix COLMAP's arbitrary
scale/rotation/translation because the renderer handed it ground-truth camera
centers. Real photos have none. The printed ChArUco board *is* the ruler — every
photo that sees it gets a pose in real millimetres, anchored to the board plane.

---

## 1. Print the board

`renders/h3_m1/charuco_board.png` — 5×7 squares, `DICT_4X4_50`, 35 mm square /
26 mm marker, A4. `renders/` is gitignored, so regenerate it whenever it is missing:

```bash
/tmp/colmap-venv/bin/python tools/inverse_lab/photo_ingest.py \
  --make-board renders/h3_m1/charuco_board.png \
  --square-mm 35 --marker-mm 26 --board-cols 5 --board-rows 7 --board-dpi 300
```

1. Print at **100% / "Actual size"** — turn OFF "fit to page", "shrink to fit"
   and any scaling.
2. **Measure a printed square with a ruler.** If it is not 35.0 mm, do not
   reprint — just pass what you measured:
   `--square-mm 34.6 --marker-mm 25.7` (scale the marker by the same ratio).
   Every metric number downstream is proportional to this value.
3. Tape it flat to a rigid board or table. A curled sheet is a bent world; it
   shows up as pose error and cannot be fixed later.

## 2. Shoot a calibration sweep first (~14 frames, 2 minutes)

**Do not skip this.** An orbit around an object keeps the board near the middle of
the frame, which leaves lens distortion essentially unconstrained — calibrated on
orbit frames alone the solver produces a physically absurd distortion curve that
happens to fit the centre (measured on the rehearsal: focal off by 5%, wild `k3`).

Board only, no object:

* 12–16 frames, board **filling the frame** and pushed into every corner —
  top-left, top-right, bottom-left, bottom-right, centre.
* Tilt hard: ±30–45° in both axes, several rolls (portrait-ish tilts too).
* Same phone, **same lens, same zoom** as the object shoot (do not switch between
  the 1× and 0.5× camera midway — that is a different camera).
* Save them in their own folder, e.g. `calib/`.

## 3. Shoot the object (20–30 frames)

| Item | Spec |
|------|------|
| Object | Matte, textured, opaque. Avoid mirrors, clear glass, and fur for the first shoot. |
| Placement | Standing on the board, roughly centred, small enough that ≥ 8 board corners stay visible in every frame. |
| Path | An arc or full circle around the object, 20–30 frames, **10–18° apart** (~70% overlap between neighbours). |
| Height | Vary elevation between passes — e.g. one lap at ~30°, one at ~55°. Do not shoot a single flat ring. |
| Framing | **Board AND object visible in every frame.** A frame without the board is a frame without a pose. |
| Distance | Fill the frame with board + object; keep the whole board inside the image if you can. |
| Lighting | Even, diffuse, static (overcast window light, or a room light bounced off a ceiling). No moving shadows, no flash. |
| Exposure | **LOCK exposure, focus and white balance** (iOS: long-press → AE/AF Lock; Android: pro mode, manual ISO/shutter/WB). Auto-exposure changes brightness between frames and corrupts the appearance model. |
| Format | JPEG or HEIC, highest quality. 16:9 if your camera offers it (otherwise the ingest centre-crops — see below). |
| Orientation | **All frames the same orientation** (all landscape or all portrait). One K is calibrated per run; frames at a different pixel size are dropped with a message. |
| Steadiness | Enough light to avoid motion blur. Blurred board corners = degraded pose. |

Also worth doing, costs nothing: photograph a colour/grey card once under the same
light, and write down the phone model + any manual ISO/shutter you set.

## 4. Ingest

```bash
/tmp/colmap-venv/bin/python tools/inverse_lab/photo_ingest.py \
  --photos       /path/to/object_photos \
  --calib-photos /path/to/calib \
  --out          renders/h3_m1/mycapture \
  --square-mm 35 --marker-mm 26 \
  --max-width 1600 --holdout-frac 0.2 --mask auto
```

Useful flags:

| Flag | Meaning |
|------|---------|
| `--square-mm / --marker-mm` | **Measured** print size. Everything metric scales with it. |
| `--board-cols / --board-rows / --dict` | If you print a different board. |
| `--no-refine-sfm` | Skip the pycolmap pass (faster; ChArUco poses only). |
| `--sfm-accept-mm 6` | How closely SfM must agree with the board before its poses are preferred. |
| `--out-aspect 16:9 \| native` | Centre-crop the output. The fitter resizes targets to a 16:9 render, so a 4:3 photo would otherwise be *stretched*. |
| `--world-scale` | OHAO world units per metre (default 1.0 = the bundle is in metres). |
| `--mask none` | Skip the (heuristic) object masks. |
| `--min-markers 2` | Stricter ChArUco corner interpolation; fewer, cleaner corners. |

Read the numbers it prints — they are the honest gates:

* `[2/7] calibrate … RMS reprojection` — want **< 1 px**. Above ~2 px, reshoot the
  calibration sweep.
* `board corner coverage of the frame` — want **≥ 50%**. Low coverage means the
  distortion estimate is extrapolating.
* `[3/7] board pose: kept N/M` — dropped photos are listed with a reason.
* `PnP reprojection rms` — median well under 1 px on a good shoot.
* `[4/7] SfM … center RMSE` — ChArUco vs SfM disagreement in mm; the script says
  explicitly which pose set it used.
* `POSE SOURCE USED:` — `charuco` or `sfm_aligned_to_charuco`. Quote this.

Everything also lands in `<out>/photo_ingest_summary.json`.

## 5. Fit

```bash
./build/inverse_fit --lab-bundle renders/h3_m1/mycapture/capture \
  --dense-map --dense-map-res 128 --hd 720
```

The fitter prints `Lab bundle: injected N full 6-DOF camera pose(s)` — if N is 0,
the poses did not parse and it silently fell back to synthetic cameras. Check.

## 6. What the bundle contains

```
<out>/
  capture/
    images/train_XXX.png, holdout_XXX.png   # undistorted, cropped
    masks/  same names                      # white = object (heuristic!)
    cameras.jsonl                           # index/name/file/split/position/
                                            #   pitch_deg/yaw_deg/fov_deg/view[16]
    capture.json                            # capture_kind: real_photo + calibration
  photo_ingest_summary.json                 # every gate number
  NOTES.md
```

`view` is the OHAO **world→view** matrix (row-major), i.e. OpenCV's world→cam with
the `diag(1,-1,-1)` flip applied — the same convention `colmap_solve.py` emits and
`fit_targets.hpp` parses. World axes: y up, **board plane = y = 0**, origin at the
board centre, cameras at y > 0. So a proxy mesh of your object belongs at the
origin, sitting on the ground plane.

Masks are a heuristic (GrabCut from a centred seed rectangle, with the detected
ArUco markers forced to background). They are not verified segmentation; the fit
may use or ignore them.

## 7. Honesty rules for anything you publish from this

* Real photos have **no ground-truth θ**. The synthetic LABTEST bars (≥28 dB etc.)
  do not apply and must never be quoted on a real-photo fit.
* Report: gain vs a wrong initialisation, photo-vs-re-render stills, holdout views,
  and the pose gate numbers above.
* Publish failures. A capture that ends at `POSE SOURCE USED: charuco` with 6 of 25
  frames dropped is a result, not something to hide.

## 8. Common failures

| Symptom | Cause / fix |
|---------|-------------|
| `board detected in 0/N photos` | Board mirrored (photo of a screen / mirror), wrong `--dict`, or a board generated by OpenCV < 4.6 → try `--legacy-board`. |
| Many `DROP … board not detected` | Object too big, board cut off, motion blur, or glare on the print. Move back, use matte paper. |
| Calibration RMS > 2 px | No dedicated calibration sweep, or the board never reached the frame corners. |
| `FALLING BACK to EXIF/heuristic pinhole` | Too few board views. Poses then inherit the focal error one-for-one: on the rehearsal a 16%-low focal seed turned a 1.2 mm center RMSE into **94 mm**. Reshoot the calibration sweep. |
| `SfM FAILED … FALLING BACK` | Texture-poor object/scene, or too little overlap. ChArUco poses are still used — the run is not lost. |
| `DROP … != working WxH (one camera per run)` | Mixed orientations or a second lens. Split the shoot per camera/orientation and ingest separately. |
| Metric scale looks wrong | The print scaled. Measure a square and pass `--square-mm`. |
| Re-render is systematically brighter/darker | Auto-exposure was on, or the fit's exposure is unconstrained (`--fit-exposure`). |

---

## Appendix — validating without a shoot

`tools/inverse_lab/make_charuco_rehearsal.py` renders synthetic views of the board
(plus a textured box standing on it) through a real pinhole+distortion camera model
with **known** K and poses, runs the full ingest on them, and scores the recovered
poses:

```bash
/tmp/colmap-venv/bin/python tools/inverse_lab/make_charuco_rehearsal.py generate \
  --views 20 --out renders/h3_m1/rehearsal
/tmp/colmap-venv/bin/python tools/inverse_lab/photo_ingest.py \
  --photos renders/h3_m1/rehearsal/photos \
  --calib-photos renders/h3_m1/rehearsal/calib_photos \
  --out renders/h3_m1/rehearsal/bundle
/tmp/colmap-venv/bin/python tools/inverse_lab/make_charuco_rehearsal.py compare \
  --bundle renders/h3_m1/rehearsal/bundle --gt renders/h3_m1/rehearsal/rehearsal_gt.json
```

This validates the pose math and the bundle format. It says **nothing** about real
lenses, real lighting, or recovering materials from real photographs.

### Measured — 2026-07-25, 20 views @ 1600×900, board 5×7 @ 35 mm, box 60×50×70 mm

Synthetic camera: f = 1440.0 px, principal point (806, 446), distortion
`k1,k2,p1,p2 = −0.09, 0.04, 0.001, −0.0008`, rig radius ≈ 600 mm, JPEG q92 + 2 LSB
noise. Calibrated from the 14-frame board-only sweep; the focal *seed* was the
heuristic 0.755·W = 1208 px (16% low), so the calibration did real work.

| | ChArUco only (`--no-refine-sfm`) | + SfM refine (used) |
|---|---|---|
| intrinsics RMS | 0.148 px | 0.148 px |
| recovered focal | 1438.38 px (GT 1440.0, −0.11%) | same |
| board views kept | 20 / 20 | 20 / 20 |
| PnP rms (median / max) | 0.090 / 0.468 px | same |
| SfM registered | — | 20 / 20, 2372 points, 0.90 px |
| ChArUco↔SfM center RMSE | — | 1.04 mm |
| **camera-center RMSE vs GT** | **1.174 mm** (0.195% of rig radius) | **1.170 mm** (0.194%) |
| **rotation error (median / max)** | **0.045° / 0.258°** | **0.061° / 0.125°** |

Of that ~1.17 mm, a **+0.17% global metric scale bias** (consistent with the
−0.11% focal error) accounts for ~1.03 mm; the non-similarity pose noise is
**0.75 mm**. In other words the residual is dominated by the intrinsics, not by
the pose solver — which is exactly why the calibration sweep in §2 matters.

Skipping the calibration sweep (heuristic focal, no distortion) on the same images:
center RMSE **94 mm**, rotation error up to **3.4°**. Same code, same photos.
