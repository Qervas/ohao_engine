#!/usr/bin/env python3
"""H3 / M1 — REAL-PHOTO ingestion for OHAO inverse rendering.

Turn a folder of phone photos of an object sitting on a printed ChArUco board
into an OHAO lab bundle (`ohao_inverse_lab_capture`) with **metric, world-anchored**
camera poses, so the dense-map fitter can be pointed at genuine photographs.

Why a ChArUco board
-------------------
`colmap_solve.py` (M0b) fixed COLMAP's 7-DOF gauge freedom by Umeyama-aligning the
SfM reconstruction to *ground-truth* camera centers exported by the renderer. Real
photos have no ground truth. The printed board replaces it:

  * detecting the board gives every photo a pose in **real millimetres**, with the
    board plane as the world origin — that fixes scale + rotation + translation;
  * it also pins the object in a known place (sitting on the board plane), so the
    fitter's proxy mesh can be placed on the same ground plane.

Stages (each logged, each independently gated)
  1. load + EXIF        photos -> working resolution, EXIF focal if present
  2. calibrate          ChArUco detections across all photos -> K + distortion
  3. board pose         solvePnP per photo -> metric world->cam (board = world)
  4. SfM refine (opt)   pycolmap SfM + Umeyama onto the ChArUco centers
  5. undistort          write pinhole-consistent images into the bundle
  6. mask (opt)         crude object/background segmentation
  7. bundle             capture/{images,masks,cameras.jsonl,capture.json}

Conventions
-----------
OpenCV camera: +x right, +y DOWN, +z FORWARD.  OHAO/glm view: +x right, +y up,
-z forward.  world->view(OHAO) = FLIP @ [R|t] with FLIP = diag(1,-1,-1) — the same
flip `colmap_solve.py` uses.

OpenCV's ChArUco *board* frame is x-right / y-down-the-printed-page / **z INTO the
board**: a camera looking at the printed face sits at board z < 0 (verified against
cv2 — solvePnP on the board's own PNG puts the camera at negative z). OHAO wants the
board plane to be the y=0 ground with the cameras above it, so world points are
rotated by BOARD_TO_OHAO = [[1,0,0],[0,0,-1],[0,1,0]], i.e.
(x,y,z)_board -> (x, -z, y)_ohao (det = +1), after being re-centered on the board
center so the object sits at the world origin on the ground plane.

Run with:  /tmp/colmap-venv/bin/python   (pycolmap 4.1.1 + OpenCV 5 aruco)
NOTE: `/usr/bin/colmap` on this box is a DIFFERENT tool. Only pycolmap is used.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
from pathlib import Path

import numpy as np

try:
    import cv2
except Exception as e:  # pragma: no cover
    sys.stderr.write(f"FATAL: OpenCV not importable ({e}). Use /tmp/colmap-venv/bin/python\n")
    raise SystemExit(2)

if not hasattr(cv2.aruco, "CharucoDetector"):  # pragma: no cover
    sys.stderr.write("FATAL: cv2.aruco.CharucoDetector missing (need OpenCV >= 4.7)\n")
    raise SystemExit(2)

# Shared math with the M0b COLMAP link — single source of truth for the axis flip,
# the Umeyama similarity and the SfM driver.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from colmap_solve import (  # noqa: E402
    FLIP,
    Rt_to_view16,
    convert_and_align,
    orbit_shape,
    run_sfm,
)

# Board frame (x right, y down the page, z INTO the board) -> OHAO world (y up).
# (x, y, z) -> (x, -z, y). Proper rotation (det = +1). Cameras that see the print
# have board z < 0, hence OHAO y > 0 (above the ground plane).
BOARD_TO_OHAO = np.array([[1.0, 0.0, 0.0],
                          [0.0, 0.0, -1.0],
                          [0.0, 1.0, 0.0]])

IMAGE_EXTS = (".jpg", ".jpeg", ".png", ".JPG", ".JPEG", ".PNG", ".heic", ".HEIC",
              ".heif", ".HEIF", ".tif", ".tiff")


# ── stage 1: load + EXIF ─────────────────────────────────────────────────────

def exif_focal_px(path: Path, width_px: int) -> tuple[float | None, dict]:
    """Best-effort focal length in ORIGINAL pixels from EXIF.

    Preference order: FocalLengthIn35mmFilm (maps directly to a 36 mm-wide frame),
    then FocalLength + a 35 mm-equivalent hint. Returns (f_px | None, info dict).
    """
    info: dict = {}
    try:
        from PIL import Image, ExifTags  # noqa: F401
        with Image.open(path) as im:
            ex = im.getexif()
            if not ex:
                return None, info
            info["make"] = str(ex.get(271, "") or "")
            info["model"] = str(ex.get(272, "") or "")
            sub = {}
            try:
                sub = ex.get_ifd(0x8769) or {}
            except Exception:
                sub = {}
            f35 = sub.get(41989) or ex.get(41989)
            fl = sub.get(37386) or ex.get(37386)
            if fl is not None:
                try:
                    info["focal_mm"] = float(fl)
                except Exception:
                    pass
            if f35:
                try:
                    f35 = float(f35)
                except Exception:
                    f35 = 0.0
                if f35 > 1.0:
                    info["focal_35mm_equiv"] = f35
                    # 35 mm full frame is 36 mm wide; the long image axis maps to it.
                    return f35 / 36.0 * float(width_px), info
    except Exception as e:  # EXIF is optional
        info["exif_error"] = str(e)
    return None, info


def load_photo(path: Path) -> np.ndarray | None:
    """Read a photo as BGR uint8. HEIC only if pillow-heif is installed."""
    suffix = path.suffix.lower()
    if suffix in (".heic", ".heif"):
        try:
            import pillow_heif  # type: ignore
            from PIL import Image
            pillow_heif.register_heif_opener()
            with Image.open(path) as im:
                rgb = np.asarray(im.convert("RGB"))
            return cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
        except Exception as e:
            sys.stderr.write(f"  SKIP {path.name}: HEIC unsupported ({e}); "
                             f"install pillow-heif or shoot JPEG\n")
            return None
    img = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if img is None:
        sys.stderr.write(f"  SKIP {path.name}: cv2.imread failed\n")
    return img


def stage_load(photos_dir: Path, max_width: int) -> tuple[list[dict], dict]:
    files = sorted([p for p in photos_dir.iterdir()
                    if p.is_file() and p.suffix in IMAGE_EXTS],
                   key=lambda p: p.name.lower())
    if not files:
        raise SystemExit(f"FATAL: no images under {photos_dir} "
                         f"(looked for {' '.join(sorted(set(e.lower() for e in IMAGE_EXTS)))})")
    out: list[dict] = []
    exif_f_px: list[float] = []
    exif_info: dict = {}
    for p in files:
        img = load_photo(p)
        if img is None:
            continue
        h0, w0 = img.shape[:2]
        scale = 1.0
        if max_width > 0 and w0 > max_width:
            scale = max_width / float(w0)
            img = cv2.resize(img, (int(round(w0 * scale)), int(round(h0 * scale))),
                             interpolation=cv2.INTER_AREA)
        f_px, info = exif_focal_px(p, w0)
        if f_px:
            exif_f_px.append(f_px * scale)
            if not exif_info:
                exif_info = info
        out.append(dict(path=p, name=p.name, img=img, orig_wh=(w0, h0), scale=scale,
                        exif_f_px=(f_px * scale) if f_px else None, exif=info))
    if not out:
        raise SystemExit("FATAL: no readable images")
    wh = {im["img"].shape[1::-1] for im in out}
    if len(wh) > 1:
        sys.stderr.write(f"  WARN: mixed working resolutions {wh}; calibration assumes one "
                         f"camera — mixed sizes will be calibrated together and may be wrong\n")
    W, H = out[0]["img"].shape[1], out[0]["img"].shape[0]
    meta = dict(n_files=len(files), n_loaded=len(out), work_width=W, work_height=H,
                exif_focal_px_median=(float(np.median(exif_f_px)) if exif_f_px else None),
                exif_n_with_focal=len(exif_f_px), exif_sample=exif_info)
    print(f"[1/7] load: {len(out)}/{len(files)} images  working {W}x{H}"
          f"  (downscaled from {out[0]['orig_wh'][0]}x{out[0]['orig_wh'][1]},"
          f" scale={out[0]['scale']:.3f})")
    if exif_f_px:
        print(f"      EXIF focal: {len(exif_f_px)}/{len(out)} photos, median "
              f"{np.median(exif_f_px):.1f} px @ working res"
              f"  ({exif_info.get('make','?')} {exif_info.get('model','?')})")
    else:
        print("      EXIF focal: none found — will seed calibration from image width")
    return out, meta


# ── stage 2: ChArUco detection + intrinsics calibration ──────────────────────

def make_board(cols: int, rows: int, square_mm: float, marker_mm: float,
               dict_name: str, legacy: bool) -> cv2.aruco.CharucoBoard:
    if not hasattr(cv2.aruco, dict_name):
        raise SystemExit(f"FATAL: unknown aruco dictionary {dict_name}")
    adict = cv2.aruco.getPredefinedDictionary(getattr(cv2.aruco, dict_name))
    board = cv2.aruco.CharucoBoard((cols, rows), square_mm / 1000.0,
                                   marker_mm / 1000.0, adict)
    if legacy:
        board.setLegacyPattern(True)
    return board


def detect_all(images: list[dict], board: cv2.aruco.CharucoBoard, min_corners: int,
               min_markers: int = 1) -> dict:
    """Detect the board in every photo (results cached on the image dicts).

    Two non-default detector settings matter for capture quality:
      * subpixel marker corner refinement — the whole pose chain inherits this
        accuracy;
      * CharucoParameters.minMarkers — how many ArUco markers must border a
        chessboard corner for it to be interpolated. The default (2) throws away
        most of the board once the object occludes its middle, which is exactly
        the situation here (the object sits ON the board). 1 keeps those corners;
        the PnP RANSAC + reprojection gate downstream rejects bad ones.
    """
    det = cv2.aruco.CharucoDetector(board)
    dp = det.getDetectorParameters()
    dp.cornerRefinementMethod = cv2.aruco.CORNER_REFINE_SUBPIX
    det.setDetectorParameters(dp)
    cp = det.getCharucoParameters()
    cp.minMarkers = int(min_markers)
    cp.tryRefineMarkers = True
    det.setCharucoParameters(cp)
    n_ok = 0
    for im in images:
        gray = cv2.cvtColor(im["img"], cv2.COLOR_BGR2GRAY)
        cc, ci, mc, mi = det.detectBoard(gray)
        n_c = 0 if cc is None else int(len(cc))
        im["charuco_corners"] = cc
        im["charuco_ids"] = ci
        im["marker_corners"] = mc
        im["n_corners"] = n_c
        im["n_markers"] = 0 if mi is None else int(len(mi))
        if n_c >= min_corners:
            n_ok += 1
    return dict(n_detected=n_ok)


def stage_calibrate(images: list[dict], board, min_corners: int, min_calib_views: int,
                    seed_f_px: float | None, W: int, H: int, free_k3: bool,
                    zero_tangent: bool) -> tuple[np.ndarray, np.ndarray, dict]:
    obj_all, img_all, used = [], [], []
    cover = np.zeros((8, 8), np.int32)   # where in the frame corners actually land
    for im in images:
        if im["n_corners"] < min_corners:
            continue
        if im["img"].shape[1] != W or im["img"].shape[0] != H:
            sys.stderr.write(f"  WARN: calibration image {im['name']} is "
                             f"{im['img'].shape[1]}x{im['img'].shape[0]}, expected {W}x{H}"
                             f" — skipped\n")
            continue
        op, ip = board.matchImagePoints(im["charuco_corners"], im["charuco_ids"])
        if op is None or len(op) < min_corners:
            continue
        # calibrateCamera needs non-degenerate (non-collinear) point sets.
        pts = op.reshape(-1, 3)[:, :2]
        if np.linalg.matrix_rank(pts - pts.mean(0), tol=1e-6) < 2:
            continue
        obj_all.append(op.astype(np.float32))
        img_all.append(ip.astype(np.float32))
        used.append(im["name"])
        uv = ip.reshape(-1, 2)
        for u, v in uv:
            cover[min(7, max(0, int(v / H * 8))), min(7, max(0, int(u / W * 8)))] += 1
    cover_frac = float((cover > 0).mean())

    # Seed focal: EXIF if we have it, else a generic phone-ish 65 deg horizontal FOV.
    f_seed = seed_f_px if seed_f_px else 0.755 * W
    K0 = np.array([[f_seed, 0, W / 2.0], [0, f_seed, H / 2.0], [0, 0, 1.0]])
    info: dict = dict(n_calib_views=len(used), calib_views=used,
                      seed_focal_px=float(f_seed),
                      seed_source=("exif" if seed_f_px else "image_width_heuristic"),
                      frame_coverage_frac=cover_frac)

    if len(used) < min_calib_views:
        print(f"[2/7] calibrate: only {len(used)} usable board views "
              f"(need >= {min_calib_views}) — FALLING BACK to EXIF/heuristic pinhole, "
              f"zero distortion")
        print(f"      *** WARNING: pose accuracy now inherits the focal-length error "
              f"DIRECTLY. Camera distance scales as 1/f: a 10% focal error is a 10% "
              f"metric error. On the synthetic rehearsal the heuristic seed "
              f"(0.755*W) was 16% low and the center RMSE went 1.2 mm -> 94 mm. "
              f"Shoot a calibration sweep (--calib-photos). ***")
        info.update(method="exif_fallback", rms_px=None,
                    fallback_reason=f"{len(used)} calib views < {min_calib_views}")
        return K0, np.zeros((1, 5)), info

    # Default to a 4-parameter model (k1, k2, p1, p2). k3 is only identifiable when
    # the board reaches the frame corners; leaving it free on an object-centred
    # orbit produces an extrapolating, physically absurd distortion curve.
    flags = cv2.CALIB_USE_INTRINSIC_GUESS | cv2.CALIB_FIX_ASPECT_RATIO
    fl = ["USE_INTRINSIC_GUESS", "FIX_ASPECT_RATIO"]
    if not free_k3:
        flags |= cv2.CALIB_FIX_K3
        fl.append("FIX_K3")
    if zero_tangent:
        flags |= cv2.CALIB_ZERO_TANGENT_DIST
        fl.append("ZERO_TANGENT_DIST")
    dist0 = np.zeros((1, 5))
    rms, K, dist, _rv, _tv = cv2.calibrateCamera(obj_all, img_all, (W, H), K0.copy(),
                                                 dist0.copy(), flags=flags)
    info.update(method="charuco_calibrateCamera", rms_px=float(rms), flags="|".join(fl))
    verdict = "good (<1 px)" if rms < 1.0 else ("usable (<2 px)" if rms < 2.0 else "POOR (>=2 px)")
    print(f"[2/7] calibrate: {len(used)} board views  RMS reprojection = {rms:.3f} px  -> {verdict}")
    print(f"      board corner coverage of the frame: {100*cover_frac:.0f}% of an 8x8 grid"
          + ("" if cover_frac >= 0.5 else
             "  <- LOW: distortion is weakly constrained; shoot a dedicated "
             "calibration set (--calib-photos) with the board filling the frame corners"))
    print(f"      K: fx={K[0,0]:.2f} fy={K[1,1]:.2f} cx={K[0,2]:.2f} cy={K[1,2]:.2f}"
          f"  dist={np.round(dist.ravel(), 5).tolist()}")
    return K, dist, info


# ── stage 3: per-photo board pose (metric) ───────────────────────────────────

def solve_board_pose(im: dict, board, K: np.ndarray, dist: np.ndarray,
                     min_corners: int, max_rms_px: float,
                     outlier_px: float = 2.0) -> dict | None:
    """Robust metric board pose from the ChArUco corners of one photo.

    Interpolated ChArUco corners are NOT all trustworthy: when the object occludes
    part of the board, a corner's subpixel refinement can latch onto the object's
    silhouette and land 10-20 px away (measured on the rehearsal). RANSAC alone did
    not reliably remove those, so after the initial solve we iteratively drop
    corners whose reprojection residual exceeds max(outlier_px, 3x median) and
    re-solve. The reported rms + inlier count are over the surviving corners.
    """
    if im["n_corners"] < min_corners:
        return None
    op, ip = board.matchImagePoints(im["charuco_corners"], im["charuco_ids"])
    if op is None or len(op) < 4:
        return None
    obj = op.reshape(-1, 3).astype(np.float64)
    img = ip.reshape(-1, 2).astype(np.float64)

    def solve(sel):
        try:
            ok, rv, tv = cv2.solvePnP(obj[sel], img[sel], K, dist, flags=cv2.SOLVEPNP_IPPE)
        except cv2.error:
            ok = False
        if not ok:
            ok, rv, tv = cv2.solvePnP(obj[sel], img[sel], K, dist,
                                      flags=cv2.SOLVEPNP_ITERATIVE)
        if not ok:
            return None
        rv, tv = cv2.solvePnPRefineLM(obj[sel], img[sel], K, dist, rv, tv)
        return rv, tv

    sel = np.arange(len(obj))
    try:
        ok, rv0, tv0, inl = cv2.solvePnPRansac(
            obj, img, K, dist, flags=cv2.SOLVEPNP_IPPE,
            reprojectionError=3.0, iterationsCount=300, confidence=0.999)
        if ok and inl is not None and len(inl) >= 4:
            sel = inl.ravel()
    except cv2.error:
        pass

    res = solve(sel)
    n_out = 0
    for _ in range(4):
        if res is None:
            return None
        rvec, tvec = res
        proj, _ = cv2.projectPoints(obj, rvec, tvec, K, dist)
        err = np.linalg.norm(proj.reshape(-1, 2) - img, axis=1)
        thr = max(outlier_px, 3.0 * float(np.median(err[sel])))
        keep = np.where(err <= thr)[0]
        if len(keep) < max(4, min_corners // 2):
            break
        if len(keep) == len(sel) and set(keep.tolist()) == set(sel.tolist()):
            break
        n_out += max(0, len(sel) - len(keep))
        sel = keep
        res = solve(sel)
    if res is None:
        return None
    rvec, tvec = res
    proj, _ = cv2.projectPoints(obj, rvec, tvec, K, dist)
    err = np.linalg.norm(proj.reshape(-1, 2) - img, axis=1)
    rms = float(np.sqrt((err[sel] ** 2).mean()))
    R, _ = cv2.Rodrigues(rvec)
    t = tvec.reshape(3)
    if len(sel) < min_corners or rms > max_rms_px:
        return dict(rejected=True, rms_px=rms, n_inliers=int(len(sel)),
                    n_corners=int(len(obj)), n_outliers=int(len(obj) - len(sel)),
                    why=("too few surviving corners" if len(sel) < min_corners
                         else f"rms {rms:.2f}px > {max_rms_px}px"))
    return dict(rejected=False, R=R, t=t, rms_px=rms, n_inliers=int(len(sel)),
                n_corners=int(len(obj)), n_outliers=int(len(obj) - len(sel)),
                center_board=(-R.T @ t))


def stage_board_poses(images: list[dict], board, K, dist, min_corners: int,
                      max_rms_px: float) -> tuple[list[dict], dict]:
    kept, dropped = [], []
    for im in images:
        res = solve_board_pose(im, board, K, dist, min_corners, max_rms_px)
        if res is None:
            dropped.append(dict(name=im["name"], reason="board not detected",
                                n_corners=im["n_corners"]))
            continue
        if res["rejected"]:
            dropped.append(dict(name=im["name"], reason=res.get("why", "PnP rejected"),
                                n_corners=res["n_corners"], n_inliers=res["n_inliers"]))
            continue
        im.update(res)
        kept.append(im)
    rmss = [im["rms_px"] for im in kept]
    inls = [im["n_inliers"] for im in kept]
    outs = sum(im["n_outliers"] for im in kept)
    print(f"[3/7] board pose: kept {len(kept)}/{len(images)}  "
          f"(dropped {len(dropped)})")
    if kept:
        print(f"      PnP reprojection rms: median {np.median(rmss):.3f} px  "
              f"max {max(rmss):.3f} px")
        print(f"      inlier corners: median {int(np.median(inls))}  min {min(inls)}  "
              f"max {max(inls)}  ({outs} corner(s) rejected as outliers overall)")
    for d in dropped:
        print(f"      DROP {d['name']}: {d['reason']} (corners={d.get('n_corners', 0)})")
    info = dict(n_kept=len(kept), n_dropped=len(dropped), dropped=dropped,
                corner_outliers_rejected=int(outs),
                pnp_rms_px_median=(float(np.median(rmss)) if kept else None),
                pnp_rms_px_max=(float(max(rmss)) if kept else None),
                inliers_median=(int(np.median(inls)) if kept else None),
                inliers_min=(int(min(inls)) if kept else None))
    return kept, info


# ── board frame -> OHAO world ────────────────────────────────────────────────

def board_center_offset(board, origin_mode: str) -> np.ndarray:
    cols, rows = board.getChessboardSize()
    s = board.getSquareLength()
    if origin_mode == "corner":
        return np.zeros(3)
    return np.array([cols * s * 0.5, rows * s * 0.5, 0.0])


def cv_pose_to_ohao(R_cv: np.ndarray, t_cv: np.ndarray, c: np.ndarray,
                    world_scale: float) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Board-frame OpenCV pose -> (R_view, t_view, center) in OHAO world units.

    World mapping: w = s * BOARD_TO_OHAO @ (p_board - c).  The camera translation
    scales with the world (rotation does not), then FLIP converts OpenCV's
    y-down/z-forward camera to OHAO's y-up/z-back view space.
    """
    Rb = BOARD_TO_OHAO
    R_w = R_cv @ Rb.T                       # world(OHAO) -> camera(OpenCV)
    t_w = world_scale * (R_cv @ c + t_cv)
    center = -R_w.T @ t_w
    return FLIP @ R_w, FLIP @ t_w, center


def view_to_euler(R_view: np.ndarray) -> tuple[float, float]:
    """Engine Euler fallback: front = -row2 of the view rotation."""
    front = -R_view[2, :]
    front = front / max(np.linalg.norm(front), 1e-12)
    pitch = math.degrees(math.asin(float(np.clip(front[1], -1.0, 1.0))))
    yaw = math.degrees(math.atan2(float(front[2]), float(front[0])))
    return pitch, yaw


def rot_angle_deg(A: np.ndarray, B: np.ndarray) -> float:
    c = (np.trace(A @ B.T) - 1.0) / 2.0
    return float(math.degrees(math.acos(float(np.clip(c, -1.0, 1.0)))))


# ── stage 5: undistort + aspect crop ─────────────────────────────────────────

def parse_aspect(s: str) -> float | None:
    s = (s or "").strip().lower()
    if s in ("", "native", "none", "keep"):
        return None
    if ":" in s:
        a, b = s.split(":", 1)
        return float(a) / float(b)
    return float(s)


def crop_box(W: int, H: int, aspect: float | None) -> tuple[int, int, int, int]:
    """Centered crop (x0, y0, w, h) matching `aspect` (w/h). None = no crop."""
    if aspect is None:
        return 0, 0, W, H
    if W / H > aspect:                    # too wide -> trim width
        w = int(round(H * aspect)); h = H
    else:                                 # too tall -> trim height
        w = W; h = int(round(W / aspect))
    w = min(w, W); h = min(h, H)
    return (W - w) // 2, (H - h) // 2, w, h


# ── stage 6: object mask ─────────────────────────────────────────────────────

def make_mask(bgr: np.ndarray, marker_polys: list[np.ndarray], rect_frac: float,
              iters: int = 4) -> np.ndarray:
    """Crude object mask: GrabCut seeded by a centered rectangle, with every
    detected ArUco marker forced to definite BACKGROUND (those pixels are the
    printed board, never the object).

    This is a heuristic, NOT a verified segmentation: it assumes the object is
    roughly centered and that the board fills the rest of the frame. The fitter
    may use it or ignore it.
    """
    H, W = bgr.shape[:2]
    gc = np.full((H, W), cv2.GC_PR_BGD, np.uint8)
    rw, rh = int(W * rect_frac), int(H * rect_frac)
    x0, y0 = (W - rw) // 2, (H - rh) // 2
    gc[y0:y0 + rh, x0:x0 + rw] = cv2.GC_PR_FGD
    # A small core is definitely foreground; the frame border definitely background.
    cw, ch = int(W * rect_frac * 0.25), int(H * rect_frac * 0.25)
    gc[(H - ch) // 2:(H + ch) // 2, (W - cw) // 2:(W + cw) // 2] = cv2.GC_FGD
    b = max(2, int(0.02 * min(W, H)))
    gc[:b, :] = cv2.GC_BGD; gc[-b:, :] = cv2.GC_BGD
    gc[:, :b] = cv2.GC_BGD; gc[:, -b:] = cv2.GC_BGD
    for poly in marker_polys:
        p = poly.reshape(-1, 2).astype(np.int32)
        cv2.fillConvexPoly(gc, p, int(cv2.GC_BGD))
    bgd, fgd = np.zeros((1, 65), np.float64), np.zeros((1, 65), np.float64)
    try:
        cv2.grabCut(bgr, gc, None, bgd, fgd, iters, cv2.GC_INIT_WITH_MASK)
    except cv2.error as e:
        sys.stderr.write(f"  WARN grabCut failed ({e}); emitting rectangle mask\n")
        out = np.zeros((H, W), np.uint8)
        out[y0:y0 + rh, x0:x0 + rw] = 255
        return out
    return np.where((gc == cv2.GC_FGD) | (gc == cv2.GC_PR_FGD), 255, 0).astype(np.uint8)


# ── stage 7: bundle writing ──────────────────────────────────────────────────

def write_bundle(out_root: Path, records: list[dict], capture_meta: dict) -> Path:
    cap = out_root / "capture"
    (cap / "images").mkdir(parents=True, exist_ok=True)
    with open(cap / "cameras.jsonl", "w") as f:
        for r in records:
            # Key order matters: the C++ loader (fit_targets.hpp) substring-matches
            # keys, so the standard keys come FIRST and extras last.
            line = {
                "index": r["index"], "name": "photo", "file": r["file"],
                "split": r["split"],
                "position": [float(x) for x in r["center"]],
                "pitch_deg": r["pitch_deg"], "yaw_deg": r["yaw_deg"],
                "fov_deg": r["fov_deg"],
                "view": Rt_to_view16(r["R_view"], r["t_view"]),
                "source_photo": r["source"],
                "pose_source": r["pose_source"],
                "board_corners": r["n_corners"],
                "pnp_rms_px": round(r["rms_px"], 4),
            }
            f.write(json.dumps(line) + "\n")
    with open(cap / "capture.json", "w") as f:
        json.dump(capture_meta, f, indent=2)
        f.write("\n")
    return cap


# ── selftest: frame conversion round-trip (no photos, no COLMAP) ─────────────

def run_selftest() -> int:
    """Validate board->OHAO->view conversion + Euler extraction analytically.

    Builds random OpenCV board poses, converts them, then checks that (a) the view
    rotation is orthonormal with det=+1, (b) the camera center recovered from the
    view matrix matches the independently computed OHAO center, (c) the board
    plane really lands on the OHAO ground plane y=0, and (d) a point projected
    through the OpenCV pose and through the OHAO view+projection lands on the same
    pixel.
    """
    rng = np.random.default_rng(3)
    c = np.array([0.0875, 0.1225, 0.0])   # 5x7 @ 35 mm board center
    s = 1.0
    worst = dict(orth=0.0, center=0.0, plane=0.0, pixel=0.0)

    # A camera that can SEE the printed face sits at board z < 0 (OpenCV board
    # frame: +z points into the board). It must land ABOVE the OHAO ground plane.
    eye_b = np.array([0.05, 0.05, -0.40])
    f = (c - eye_b); f /= np.linalg.norm(f)
    r = np.cross(f, np.array([0.0, 0.0, -1.0])); r /= np.linalg.norm(r)
    R_look = np.stack([r, np.cross(f, r), f])
    _Rv, _tv, C_above = cv_pose_to_ohao(R_look, -R_look @ eye_b, c, 1.0)
    side_ok = C_above[1] > 0.3
    for _ in range(200):
        rv = rng.normal(size=3)
        R_cv, _ = cv2.Rodrigues(rv)
        t_cv = np.array([rng.normal(0, 0.05), rng.normal(0, 0.05), rng.uniform(0.3, 0.8)])
        R_view, t_view, center = cv_pose_to_ohao(R_cv, t_cv, c, s)
        worst["orth"] = max(worst["orth"], float(np.abs(R_view @ R_view.T - np.eye(3)).max()))
        assert np.linalg.det(R_view) > 0
        worst["center"] = max(worst["center"], float(np.linalg.norm(-R_view.T @ t_view - center)))
        # A board point (X, Y, 0) must map to OHAO y == 0.
        p_b = np.array([rng.uniform(0, 0.175), rng.uniform(0, 0.245), 0.0])
        p_o = s * BOARD_TO_OHAO @ (p_b - c)
        worst["plane"] = max(worst["plane"], abs(float(p_o[1])))
        # Same 3D point, two pipelines -> same pixel.
        f, W, H = 1200.0, 1600.0, 900.0
        K = np.array([[f, 0, W / 2], [0, f, H / 2], [0, 0, 1.0]])
        p3 = np.array([rng.uniform(0, 0.175), rng.uniform(0, 0.245), rng.uniform(0, 0.08)])
        x_cv = R_cv @ p3 + t_cv
        if x_cv[2] < 0.05:
            continue
        px_cv = (K @ x_cv)[:2] / x_cv[2]
        p_world = s * BOARD_TO_OHAO @ (p3 - c)
        x_view = R_view @ p_world + t_view          # OHAO view space (y up, -z fwd)
        x_back = FLIP @ x_view                      # -> OpenCV camera space
        px_ohao = (K @ x_back)[:2] / x_back[2]
        worst["pixel"] = max(worst["pixel"], float(np.linalg.norm(px_cv - px_ohao)))
    # Euler round-trip through the engine's front-vector convention.
    worst_euler = 0.0
    for _ in range(200):
        R_cv, _ = cv2.Rodrigues(rng.normal(size=3))
        R_view, _t, _c = cv_pose_to_ohao(R_cv, np.array([0, 0, 0.5]), c, 1.0)
        pitch, yaw = view_to_euler(R_view)
        f = np.array([math.cos(math.radians(yaw)) * math.cos(math.radians(pitch)),
                      math.sin(math.radians(pitch)),
                      math.sin(math.radians(yaw)) * math.cos(math.radians(pitch))])
        worst_euler = max(worst_euler, float(np.linalg.norm(f - (-R_view[2, :]))))
    print("===== photo_ingest selftest (frame conversions, analytic) =====")
    print(f"  max |R Rᵀ − I|                    = {worst['orth']:.3e}")
    print(f"  max center mismatch (view vs calc)= {worst['center']:.3e} m")
    print(f"  max |y| of board points in OHAO   = {worst['plane']:.3e} m")
    print(f"  max pixel disagreement CV vs OHAO = {worst['pixel']:.3e} px")
    print(f"  max Euler front-vector error      = {worst_euler:.3e}")
    print(f"  camera on the printed side is above ground: {side_ok} "
          f"(y = {C_above[1]:+.3f} m)")
    ok = (worst["orth"] < 1e-9 and worst["center"] < 1e-9 and worst["plane"] < 1e-12
          and worst["pixel"] < 1e-6 and worst_euler < 1e-9 and side_ok)
    print(f"  RESULT: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


# ── main ─────────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(
        description="H3/M1 real-photo -> OHAO lab bundle via a printed ChArUco board")
    ap.add_argument("--selftest", action="store_true",
                    help="validate the frame conversions analytically and exit")
    ap.add_argument("--make-board", type=Path, default=None,
                    help="write a printable board PNG at --board-dpi and exit")
    ap.add_argument("--board-dpi", type=float, default=300.0,
                    help="print resolution for --make-board")
    ap.add_argument("--board-margin-mm", type=float, default=10.0,
                    help="white margin around the board for --make-board")
    ap.add_argument("--photos", type=Path, help="directory of photos")
    ap.add_argument("--out", type=Path, help="output bundle root (writes <out>/capture/)")
    # Board geometry — printers scale, so these are CLI args. MEASURE your print.
    ap.add_argument("--square-mm", type=float, default=35.0, help="printed square size (mm)")
    ap.add_argument("--marker-mm", type=float, default=26.0, help="printed marker size (mm)")
    ap.add_argument("--board-cols", type=int, default=5, help="squares across")
    ap.add_argument("--board-rows", type=int, default=7, help="squares down")
    ap.add_argument("--dict", default="DICT_4X4_50", help="cv2.aruco predefined dictionary")
    ap.add_argument("--legacy-board", action="store_true",
                    help="board PNG made by OpenCV < 4.6 (legacy marker layout)")
    # Working resolution / intrinsics
    ap.add_argument("--max-width", type=int, default=1600, help="working width (0 = native)")
    ap.add_argument("--min-corners", type=int, default=8,
                    help="min ChArUco corners to trust a photo")
    ap.add_argument("--min-markers", type=int, default=1, choices=(1, 2),
                    help="ArUco markers required around a chessboard corner "
                         "(OpenCV default 2; 1 survives an object occluding the board)")
    ap.add_argument("--min-calib-views", type=int, default=6,
                    help="min board views for intrinsics calibration (else EXIF fallback)")
    ap.add_argument("--calib-photos", type=Path, default=None,
                    help="separate directory of board-only calibration shots (same camera, "
                         "same zoom). Strongly recommended: an orbit keeps the board near the "
                         "frame center, which leaves lens distortion unconstrained")
    ap.add_argument("--calib-free-k3", action="store_true",
                    help="let the k3 radial term float (default: fixed at 0)")
    ap.add_argument("--calib-zero-tangent", action="store_true",
                    help="force p1=p2=0 (well-centered lens)")
    ap.add_argument("--max-pnp-rms", type=float, default=2.5,
                    help="drop photos whose PnP reprojection rms (px) exceeds this")
    # SfM refinement
    ap.add_argument("--refine-sfm", dest="refine_sfm", action="store_true", default=True,
                    help="run pycolmap SfM and prefer its poses when consistent (default)")
    ap.add_argument("--no-refine-sfm", dest="refine_sfm", action="store_false")
    ap.add_argument("--sfm-accept-mm", type=float, default=6.0,
                    help="accept SfM poses when the ChArUco-vs-SfM center RMSE is below this (mm)")
    ap.add_argument("--sfm-min-reg-frac", type=float, default=0.8,
                    help="min fraction of kept photos SfM must register to be considered")
    ap.add_argument("--use-gpu", action="store_true", help="GPU SIFT (needs OpenGL)")
    # Output shaping
    ap.add_argument("--out-aspect", default="16:9",
                    help="center-crop output to this aspect (w:h) or 'native'. The fitter "
                         "resizes targets to a 16:9 render, so a 4:3 photo would be stretched")
    ap.add_argument("--world-scale", type=float, default=1.0,
                    help="OHAO world units per metre (1.0 = bundle in metres)")
    ap.add_argument("--board-origin", choices=("center", "corner"), default="center",
                    help="world origin at the board center (default) or its corner")
    ap.add_argument("--holdout-frac", type=float, default=0.2, help="fraction of views held out")
    ap.add_argument("--mask", choices=("auto", "none"), default="auto",
                    help="auto = GrabCut object mask (heuristic); none = skip masks/")
    ap.add_argument("--mask-rect-frac", type=float, default=0.55,
                    help="centered GrabCut seed rectangle as a fraction of the frame")
    ap.add_argument("--work", type=Path, default=None, help="scratch dir (default <out>/_work)")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()
    if args.make_board is not None:
        board = make_board(args.board_cols, args.board_rows, args.square_mm,
                           args.marker_mm, args.dict, args.legacy_board)
        ppmm = args.board_dpi / 25.4
        margin = int(round(args.board_margin_mm * ppmm))
        w = int(round(args.board_cols * args.square_mm * ppmm)) + 2 * margin
        h = int(round(args.board_rows * args.square_mm * ppmm)) + 2 * margin
        img = board.generateImage((w, h), marginSize=margin)
        args.make_board.parent.mkdir(parents=True, exist_ok=True)
        cv2.imwrite(str(args.make_board), img)
        print(f"board -> {args.make_board}  {w}x{h}px @ {args.board_dpi:g} dpi  "
              f"({args.board_cols}x{args.board_rows}, {args.square_mm} mm square / "
              f"{args.marker_mm} mm marker, {args.dict})")
        print(f"  printable size: {args.board_cols*args.square_mm + 2*args.board_margin_mm:.1f} "
              f"x {args.board_rows*args.square_mm + 2*args.board_margin_mm:.1f} mm")
        print("  PRINT AT 100% (no scaling), then MEASURE a square and pass the measured "
              "--square-mm to the ingest.")
        return 0
    if args.photos is None or args.out is None:
        ap.error("--photos and --out are required (unless --selftest / --make-board)")

    out_root: Path = args.out
    work: Path = args.work or (out_root / "_work")
    out_root.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)

    board = make_board(args.board_cols, args.board_rows, args.square_mm, args.marker_mm,
                       args.dict, args.legacy_board)
    print(f"ChArUco board: {args.board_cols}x{args.board_rows} squares, "
          f"square={args.square_mm} mm, marker={args.marker_mm} mm, {args.dict}"
          f"{' (legacy)' if args.legacy_board else ''}")

    # ── 1. load ──────────────────────────────────────────────────────────────
    images, load_meta = stage_load(args.photos, args.max_width)

    # ── 2. detect + calibrate ────────────────────────────────────────────────
    det_meta = detect_all(images, board, args.min_corners, args.min_markers)
    print(f"      board detected in {det_meta['n_detected']}/{len(images)} photos "
          f"(>= {args.min_corners} corners)")
    W_work, H_work = images[0]["img"].shape[1], images[0]["img"].shape[0]
    # One K for all photos: anything not at the working resolution (portrait frames,
    # a different lens, a cropped export) cannot share those intrinsics. Drop it
    # loudly rather than solve it with the wrong camera.
    wrong_size = [im for im in images
                  if im["img"].shape[1] != W_work or im["img"].shape[0] != H_work]
    if wrong_size:
        for im in wrong_size:
            print(f"      DROP {im['name']}: {im['img'].shape[1]}x{im['img'].shape[0]} "
                  f"!= working {W_work}x{H_work} (one camera per run)")
        images = [im for im in images if im not in wrong_size]
        det_meta["n_wrong_size_dropped"] = len(wrong_size)
        if not images:
            raise SystemExit("FATAL: no images at a consistent resolution")
    calib_images, calib_seed_f = images, load_meta["exif_focal_px_median"]
    if args.calib_photos is not None:
        print(f"      calibration set: {args.calib_photos}")
        calib_images, calib_load = stage_load(args.calib_photos, args.max_width)
        cmeta = detect_all(calib_images, board, args.min_corners, args.min_markers)
        print(f"      board detected in {cmeta['n_detected']}/{len(calib_images)} "
              f"calibration shots")
        det_meta["calib_set"] = dict(n_images=len(calib_images), **cmeta)
        if calib_load["exif_focal_px_median"]:
            calib_seed_f = calib_load["exif_focal_px_median"]
    K, dist, calib = stage_calibrate(calib_images, board, args.min_corners,
                                     args.min_calib_views, calib_seed_f, W_work, H_work,
                                     args.calib_free_k3, args.calib_zero_tangent)
    calib["calib_photos_dir"] = str(args.calib_photos) if args.calib_photos else None

    # ── 3. per-photo metric board pose ───────────────────────────────────────
    kept, pose_meta = stage_board_poses(images, board, K, dist, args.min_corners,
                                        args.max_pnp_rms)
    if len(kept) < 3:
        print("FATAL: fewer than 3 photos with a reliable board pose — nothing to write.")
        return 1

    c_off = board_center_offset(board, args.board_origin)
    for im in kept:
        Rv, tv, ctr = cv_pose_to_ohao(im["R"], im["t"], c_off, args.world_scale)
        im["R_charuco"], im["t_charuco"], im["center_charuco"] = Rv, tv, ctr

    cams = np.stack([im["center_charuco"] for im in kept])
    radius = float(np.linalg.norm(cams - cams.mean(0), axis=1).mean())
    dist_origin = float(np.linalg.norm(cams, axis=1).mean())
    sv, verdict = orbit_shape(cams)
    print(f"      camera rig: mean distance to board center = {1000*dist_origin/args.world_scale:.1f} mm"
          f"  spread = {1000*radius/args.world_scale:.1f} mm  PCA sv={np.round(sv, 4)} -> {verdict}")

    # ── 4. optional SfM refinement ───────────────────────────────────────────
    pose_source = "charuco"
    sfm_meta: dict = dict(attempted=bool(args.refine_sfm))
    if args.refine_sfm:
        sfm_dir = work / "sfm_images"
        if sfm_dir.exists():
            shutil.rmtree(sfm_dir)
        sfm_dir.mkdir(parents=True, exist_ok=True)
        for i, im in enumerate(kept):
            # SfM sees the UNDISTORTED image with the calibrated pinhole K held
            # fixed, so COLMAP solves extrinsics + structure only. The index prefix
            # keeps names unique even if two photos share a stem (a.jpg / a.png).
            und = cv2.undistort(im["img"], K, dist, None, K)
            im["undist"] = und
            im["sfm_key"] = f"{i:04d}_{Path(im['name']).stem}"
            cv2.imwrite(str(sfm_dir / (im["sfm_key"] + ".png")), und)
        cam_params = f"{K[0,0]:.6f},{K[1,1]:.6f},{K[0,2]:.6f},{K[1,2]:.6f}"
        print(f"[4/7] SfM refine: pycolmap on {len(kept)} undistorted images "
              f"(PINHOLE {cam_params}, fixed)")
        try:
            rec = run_sfm(sfm_dir, work / "colmap", cam_params, use_gpu=args.use_gpu,
                          tuning="photo")
            colmap_mats, by_stem = {}, {im["sfm_key"]: im for im in kept}
            for img in rec.images.values():
                if not img.has_pose:
                    continue
                stem = Path(img.name).stem
                if stem in by_stem:
                    colmap_mats[stem] = np.asarray(img.cam_from_world().matrix(), np.float64)
            n_reg = len(colmap_mats)
            frac = n_reg / len(kept)
            target = {im["sfm_key"]: im["center_charuco"] for im in kept}
            print(f"      registered {n_reg}/{len(kept)} ({100*frac:.1f}%)  "
                  f"mean reproj {rec.compute_mean_reprojection_error():.3f} px  "
                  f"points3D {rec.num_points3D()}")
            sfm_meta.update(n_registered=n_reg, reg_frac=frac,
                            mean_reproj_px=float(rec.compute_mean_reprojection_error()),
                            n_points3d=int(rec.num_points3D()))
            if n_reg >= 3:
                aligned, s_al, R_al, t_al, rmse, src, dst = convert_and_align(colmap_mats, target)
                rmse_mm = 1000.0 * rmse / args.world_scale
                rot_err = [rot_angle_deg(aligned[k][0], by_stem[k]["R_charuco"])
                           for k in aligned]
                print(f"      Umeyama(ChArUco centers <- SfM): scale={s_al:.5f}  "
                      f"center RMSE = {rmse_mm:.2f} mm")
                print(f"      rotation disagreement vs ChArUco: median "
                      f"{np.median(rot_err):.3f} deg  max {max(rot_err):.3f} deg")
                sfm_meta.update(umeyama_scale=float(s_al), center_rmse_mm=float(rmse_mm),
                                rot_disagree_deg_median=float(np.median(rot_err)),
                                rot_disagree_deg_max=float(max(rot_err)))
                accept = (frac >= args.sfm_min_reg_frac and rmse_mm <= args.sfm_accept_mm
                          and n_reg == len(kept))
                if accept:
                    pose_source = "sfm_aligned_to_charuco"
                    for k, (Rv, tv) in aligned.items():
                        im = by_stem[k]
                        im["R_view"], im["t_view"] = Rv, tv
                        im["center"] = -Rv.T @ tv
                    print(f"      -> USING SfM poses (registered all {n_reg}, RMSE "
                          f"{rmse_mm:.2f} mm <= {args.sfm_accept_mm} mm)")
                else:
                    why = []
                    if frac < args.sfm_min_reg_frac:
                        why.append(f"only {100*frac:.0f}% registered")
                    if n_reg != len(kept):
                        why.append(f"{len(kept)-n_reg} photo(s) unregistered "
                                   f"(would lose views)")
                    if rmse_mm > args.sfm_accept_mm:
                        why.append(f"center RMSE {rmse_mm:.2f} mm > {args.sfm_accept_mm} mm")
                    print(f"      -> FALLING BACK to raw ChArUco poses ({'; '.join(why)})")
                    sfm_meta["rejected_because"] = why
            else:
                print("      -> SfM registered < 3 images; FALLING BACK to ChArUco poses")
                sfm_meta["rejected_because"] = ["<3 registered"]
        except Exception as e:
            print(f"      SfM FAILED ({type(e).__name__}: {e}) -> FALLING BACK to ChArUco poses")
            sfm_meta.update(failed=True, error=f"{type(e).__name__}: {e}")
    else:
        print("[4/7] SfM refine: disabled (--no-refine-sfm) — using raw ChArUco poses")

    for im in kept:
        if "R_view" not in im:
            im["R_view"], im["t_view"] = im["R_charuco"], im["t_charuco"]
            im["center"] = im["center_charuco"]
    print(f"      POSE SOURCE USED: {pose_source}")

    # ── 5. undistort + crop ──────────────────────────────────────────────────
    aspect = parse_aspect(args.out_aspect)
    W0, H0 = kept[0]["img"].shape[1], kept[0]["img"].shape[0]
    x0, y0, cw, ch = crop_box(W0, H0, aspect)
    K_out = K.copy()
    K_out[0, 2] -= x0
    K_out[1, 2] -= y0
    fov_deg = float(math.degrees(2.0 * math.atan(0.5 * ch / K_out[1, 1])))
    fov_h_deg = float(math.degrees(2.0 * math.atan(0.5 * cw / K_out[0, 0])))
    print(f"[5/7] undistort: newCameraMatrix = K (intrinsics preserved); "
          f"crop {W0}x{H0} -> {cw}x{ch} at ({x0},{y0}) for aspect "
          f"{args.out_aspect}")
    print(f"      vertical fov = {fov_deg:.3f} deg  (horizontal {fov_h_deg:.3f} deg)")

    for im in kept:
        und = im.get("undist")
        if und is None:
            und = cv2.undistort(im["img"], K, dist, None, K)
        im["out_img"] = und[y0:y0 + ch, x0:x0 + cw]

    # ── 6. masks ─────────────────────────────────────────────────────────────
    if args.mask == "auto":
        print(f"[6/7] mask: GrabCut, centered seed rect {args.mask_rect_frac:.2f} of frame, "
              f"detected ArUco markers forced to background (heuristic)")
        for im in kept:
            polys = []
            mcs = im.get("marker_corners")
            for mc in ([] if mcs is None else mcs):
                p = np.asarray(mc, np.float64).reshape(-1, 1, 2)
                q = cv2.undistortPoints(p, K, dist, P=K).reshape(-1, 2)
                q[:, 0] -= x0
                q[:, 1] -= y0
                polys.append(q)
            im["mask"] = make_mask(im["out_img"], polys, args.mask_rect_frac)
        fr = [float((im["mask"] > 127).mean()) for im in kept]
        print(f"      foreground fraction: median {np.median(fr)*100:.1f}%  "
              f"min {min(fr)*100:.1f}%  max {max(fr)*100:.1f}%")
    else:
        print("[6/7] mask: skipped (--mask none)")

    # ── 7. bundle ────────────────────────────────────────────────────────────
    kept.sort(key=lambda im: im["name"].lower())
    n = len(kept)
    n_hold = 0
    if n >= 3 and args.holdout_frac > 0:
        n_hold = min(max(1, int(round(args.holdout_frac * n))), n - 2)
    # Spread the holdouts evenly around the arc rather than taking a contiguous
    # block, so novel-view eval covers the whole sweep.
    hold_idx = set()
    if n_hold > 0:
        step = n / float(n_hold)
        hold_idx = {int(math.floor((i + 0.5) * step)) % n for i in range(n_hold)}
    train = [im for i, im in enumerate(kept) if i not in hold_idx]
    hold = [im for i, im in enumerate(kept) if i in hold_idx]

    cap = out_root / "capture"
    if cap.exists():
        shutil.rmtree(cap)
    (cap / "images").mkdir(parents=True, exist_ok=True)
    if args.mask == "auto":
        (cap / "masks").mkdir(parents=True, exist_ok=True)

    records = []
    for i, im in enumerate(train + hold):
        split = "train" if i < len(train) else "holdout"
        fname = (f"train_{i:03d}.png" if split == "train"
                 else f"holdout_{i-len(train):03d}.png")
        cv2.imwrite(str(cap / "images" / fname), im["out_img"])
        if args.mask == "auto":
            cv2.imwrite(str(cap / "masks" / fname), im["mask"])
        pitch, yaw = view_to_euler(im["R_view"])
        records.append(dict(index=i, file=fname, split=split, center=im["center"],
                            pitch_deg=pitch, yaw_deg=yaw, fov_deg=fov_deg,
                            R_view=im["R_view"], t_view=im["t_view"],
                            source=im["name"], pose_source=pose_source,
                            n_corners=int(im["n_corners"]), rms_px=float(im["rms_px"])))

    capture_meta = {
        "format": "ohao_inverse_lab_capture",
        "version": 1,
        "capture_kind": "real_photo",
        "metric_domain": "photo_images",
        "preset": "photo",
        "scene": "photo",
        "n_train": len(train),
        "n_holdout": len(hold),
        "map_ground": False,
        "map_res": 0,
        "theta_dims": 0,
        "show": {"width": cw, "height": ch, "spp": 256},
        "fit": {"width": max(320, cw // 2), "height": max(180, ch // 2), "spp": 128},
        "calibration": {
            "pose_source": pose_source,
            "board": {"cols": args.board_cols, "rows": args.board_rows,
                      "square_mm": args.square_mm, "marker_mm": args.marker_mm,
                      "dict": args.dict, "origin": args.board_origin},
            "world_units_per_meter": args.world_scale,
            "world_axes": ("OHAO y-up; board plane = y=0; +x = board right, "
                           "+z = down the printed page; cameras have y > 0"),
            "K": [[float(v) for v in row] for row in K_out],
            "dist": [float(v) for v in np.asarray(dist).ravel()],
            "intrinsics_rms_px": calib.get("rms_px"),
            "intrinsics_method": calib.get("method"),
            "image_wh": [cw, ch],
            "fov_deg_vertical": fov_deg,
            "fov_deg_horizontal": fov_h_deg,
            "undistorted": True,
            "wb": "assumed locked by the shooter — verify",
            "exposure_notes": "EXIF only; no radiometric calibration performed",
        },
        "masks": ("masks/ (GrabCut heuristic, white=object; may be wrong — optional)"
                  if args.mask == "auto" else None),
        "notes": ("REAL PHOTO capture ingested by tools/inverse_lab/photo_ingest.py. "
                  "Poses are metric (board-anchored). No ground-truth material θ exists; "
                  "do NOT apply synthetic LABTEST bars to fits on this bundle."),
    }
    write_bundle(out_root, records, capture_meta)

    (out_root / "NOTES.md").write_text("\n".join([
        "# Real-photo capture bundle",
        "",
        f"- Source photos: `{args.photos}`  ({load_meta['n_loaded']} loaded, {n} kept)",
        f"- Board: {args.board_cols}x{args.board_rows} @ {args.square_mm} mm square / "
        f"{args.marker_mm} mm marker ({args.dict})",
        f"- Intrinsics: {calib.get('method')}  RMS = {calib.get('rms_px')}",
        f"- Pose source: **{pose_source}**",
        f"- World: metric, board plane = y=0, origin at board {args.board_origin}, "
        f"{args.world_scale} unit(s) per metre",
        f"- Images: undistorted, {cw}x{ch}, vertical fov {fov_deg:.2f} deg",
        "",
        "No ground-truth materials exist for real photos. Report gain vs wrong-init and",
        "photo-vs-re-render stills; never quote synthetic LABTEST dB bars here.",
        "",
    ]) + "\n")

    summary = dict(
        tool="photo_ingest.py", photos_dir=str(args.photos), out=str(out_root),
        board=dict(cols=args.board_cols, rows=args.board_rows, square_mm=args.square_mm,
                   marker_mm=args.marker_mm, dictionary=args.dict,
                   legacy=bool(args.legacy_board), origin=args.board_origin),
        stage1_load=load_meta,
        stage2_detect=det_meta,
        stage2_calibration=calib,
        stage3_board_pose=pose_meta,
        stage4_sfm=sfm_meta,
        pose_source_used=pose_source,
        stage5_output=dict(crop_xywh=[x0, y0, cw, ch], out_wh=[cw, ch],
                           fov_deg_vertical=fov_deg, fov_deg_horizontal=fov_h_deg,
                           K=[[float(v) for v in row] for row in K_out],
                           dist=[float(v) for v in np.asarray(dist).ravel()]),
        stage6_mask=dict(mode=args.mask, rect_frac=args.mask_rect_frac),
        stage7_bundle=dict(n_train=len(train), n_holdout=len(hold),
                           capture=str(cap), world_scale=args.world_scale,
                           mean_cam_distance_mm=1000.0 * dist_origin / args.world_scale,
                           rig_spread_mm=1000.0 * radius / args.world_scale,
                           rig_shape=verdict),
        per_view=[dict(index=r["index"], file=r["file"], split=r["split"],
                       source=r["source"], corners=r["n_corners"],
                       pnp_rms_px=r["rms_px"],
                       center=[float(v) for v in r["center"]]) for r in records],
    )
    with open(out_root / "photo_ingest_summary.json", "w") as f:
        json.dump(summary, f, indent=2)

    print(f"[7/7] bundle -> {cap}  train={len(train)} holdout={len(hold)}")
    print(f"      summary -> {out_root / 'photo_ingest_summary.json'}")
    print(f"\nFit with:  ./build/inverse_fit --lab-bundle {cap} --dense-map "
          f"--dense-map-res 128\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
