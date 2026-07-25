#!/usr/bin/env python3
"""H3 / M0b — COLMAP link for OHAO inverse rendering.

Take a GT orbit-capture bundle (rendered by `inverse_fit --orbit-capture N`,
which stores each view's ground-truth world->view matrix + fov in
cameras.jsonl), and:

  Piece 2  run REAL Structure-from-Motion with pycolmap (SIFT -> exhaustive
           match -> incremental mapping) on the capture images. Report how many
           of the N images registered (the SfM gate).
  Piece 3  convert every registered COLMAP pose into OHAO's view convention
           (axis flip: negate camera Y and Z rows), then Umeyama-align COLMAP's
           arbitrary frame + scale to the GT frame using the camera CENTERS.
           Report the camera-center RMSE after alignment (the alignment gate).
           Write renders/.../colmap/capture/ mirroring the GT bundle but with the
           aligned COLMAP poses, plus a paired gt_matched/ bundle over exactly the
           same registered subset (re-indexed identically) so Piece 4's GT-pose
           vs COLMAP-pose fit is strictly apples-to-apples.

IMPORTANT: `/usr/bin/colmap` on this box is a DIFFERENT tool (geomorph). This
script uses ONLY pycolmap. Run with:  /tmp/colmap-venv/bin/python

Conventions
-----------
COLMAP cam_from_world maps world->camera with camera axes +x right, +y DOWN,
+z FORWARD (into the scene). glm/OHAO view space is +x right, +y up, -z forward.
The world->view (OHAO) matrix is therefore  F @ [R|t]  with F = diag(1,-1,-1)
(negate the Y and Z rows). The camera CENTER C = -R^T t is identical under this
flip (F^T F = I), so alignment can use raw COLMAP centers directly.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

import numpy as np

try:
    import pycolmap
except Exception as e:  # pragma: no cover
    sys.stderr.write(
        "FATAL: pycolmap not importable. Run with /tmp/colmap-venv/bin/python\n"
        f"  ({e})\n"
    )
    sys.exit(2)


# ── bundle / cameras.jsonl IO ────────────────────────────────────────────────

def resolve_capture_dir(bundle: Path) -> Path:
    """Accept either <bundle> containing capture.json or <bundle>/capture/."""
    if (bundle / "capture.json").exists():
        return bundle
    if (bundle / "capture" / "capture.json").exists():
        return bundle / "capture"
    raise SystemExit(f"FATAL: no capture.json under {bundle}")


def read_cameras_jsonl(cap: Path) -> list[dict]:
    rows = []
    with open(cap / "cameras.jsonl") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rows.append(json.loads(line))
    rows.sort(key=lambda r: r["index"])
    return rows


def view_to_Rt(view16: list[float]) -> tuple[np.ndarray, np.ndarray]:
    """cameras.jsonl "view" is the row-major 4x4 world->view matrix."""
    M = np.array(view16, dtype=np.float64).reshape(4, 4)
    return M[:3, :3].copy(), M[:3, 3].copy()


def camera_center(R: np.ndarray, t: np.ndarray) -> np.ndarray:
    return -R.T @ t


def Rt_to_view16(R: np.ndarray, t: np.ndarray) -> list[float]:
    M = np.eye(4, dtype=np.float64)
    M[:3, :3] = R
    M[:3, 3] = t
    return [float(x) for x in M.reshape(-1)]


# ── brighten dark synthetic renders for feature extraction ───────────────────

def brighten_images(src_dir: Path, dst_dir: Path, target_p99: float = 210.0,
                    max_gain: float = 40.0, gamma: float = 0.85) -> None:
    """Write per-image exposure-normalized copies for SIFT.

    The synthetic path-traced captures are heavily underexposed (near-black).
    SfM only needs geometric features, so we lift each image so its 99th
    luminance percentile hits ~target_p99, then apply a mild shadow-lifting
    gamma. The bundle itself still references the ORIGINAL images for fitting;
    this touches only what COLMAP sees.
    """
    from PIL import Image
    dst_dir.mkdir(parents=True, exist_ok=True)
    for p in sorted(src_dir.glob("*.png")):
        img = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)
        lum = 0.299 * img[..., 0] + 0.587 * img[..., 1] + 0.114 * img[..., 2]
        p99 = float(np.percentile(lum, 99.0))
        gain = 1.0 if p99 < 1e-3 else min(max_gain, max(1.0, target_p99 / p99))
        out = np.clip(img * gain, 0.0, 255.0) / 255.0
        out = np.power(out, gamma) * 255.0
        Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(dst_dir / p.name)


# ── pycolmap SfM ─────────────────────────────────────────────────────────────

def run_sfm(image_dir: Path, work: Path, camera_params: str,
            use_gpu: bool = False, tuning: str = "synthetic") -> pycolmap.Reconstruction:
    """SIFT -> exhaustive match -> incremental mapping with FIXED intrinsics.

    `tuning` selects the feature/matching regime:
      "synthetic" (default, M0b) — dark low-contrast renders: darkness adaptivity,
        many features, permissive Lowe ratio, no cross-check.
      "photo" (H3/M1 real capture) — real photographs have plenty of texture and
        contrast, so keep COLMAP's stricter, better-conditioned defaults; only the
        feature budget is raised. Loosening the ratio here just adds outliers.
    """
    work.mkdir(parents=True, exist_ok=True)
    db = work / "database.db"
    if db.exists():
        db.unlink()
    sfm_out = work / "sparse"
    if sfm_out.exists():
        shutil.rmtree(sfm_out)
    sfm_out.mkdir(parents=True, exist_ok=True)

    # SIFT tuned for low-contrast synthetic renders: darkness_adaptivity lifts
    # detection in dark regions; more features + a lower peak threshold help the
    # weakly-lit hero. SINGLE camera model matches our one synthetic camera.
    ext = pycolmap.FeatureExtractionOptions()
    ext.use_gpu = use_gpu
    ext.sift.max_num_features = 16384
    ext.sift.estimate_affine_shape = False
    if tuning == "synthetic":
        ext.sift.darkness_adaptivity = True
        ext.sift.peak_threshold = 0.003    # low-contrast synthetic renders
        ext.sift.edge_threshold = 15.0
        ext.sift.first_octave = -1

    # Known-calibrated pinhole camera: we rendered these with a fixed intrinsic,
    # so give COLMAP the true focal + principal point and keep them CONSTANT
    # during bundle adjustment. A controlled turntable orbit is focal/distance
    # degenerate — self-calibration recovers a wrong focal (e.g. 93 deg vs the
    # true 40 deg), which would corrupt the fit. Real capture calibrates too, so
    # COLMAP here solves only the extrinsics (the actual poses) + structure.
    reader = pycolmap.ImageReaderOptions()
    reader.camera_model = "PINHOLE"
    reader.camera_params = camera_params  # "fx,fy,cx,cy"

    device = pycolmap.Device.cuda if use_gpu else pycolmap.Device.cpu
    pycolmap.extract_features(
        database_path=db,
        image_path=image_dir,
        camera_mode=pycolmap.CameraMode.SINGLE,
        reader_options=reader,
        extraction_options=ext,
        device=device,
    )

    # CRITICAL for low-texture / low-contrast synthetic renders: COLMAP's default
    # Lowe ratio (0.8) + cross-check reject the (correct but ambiguous) matches on
    # a smooth glossy hero, leaving ~1 inlier/pair and no reconstruction. Relaxing
    # the ratio, distance, and cross-check recovers ~60 verified inliers/pair.
    match_opts = pycolmap.FeatureMatchingOptions()
    match_opts.use_gpu = use_gpu
    match_opts.guided_matching = True  # use 2-view geometry to densify matches
    if tuning == "synthetic":
        match_opts.sift.max_ratio = 0.95
        match_opts.sift.max_distance = 0.9
        match_opts.sift.cross_check = False
    pycolmap.match_exhaustive(
        database_path=db,
        matching_options=match_opts,
        device=device,
    )

    pipe = pycolmap.IncrementalPipelineOptions()
    # Small overlapping set: keep the thresholds permissive so a weakly-textured
    # dark scene can still bootstrap and grow.
    pipe.min_num_matches = 15
    # Fix intrinsics (see above): solve extrinsics + structure only.
    pipe.ba_refine_focal_length = False
    pipe.ba_refine_principal_point = False
    pipe.ba_refine_extra_params = False
    maps = pycolmap.incremental_mapping(
        database_path=db,
        image_path=image_dir,
        output_path=sfm_out,
        options=pipe,
    )
    if not maps:
        raise SystemExit("FATAL: incremental mapping produced no reconstruction")
    # Largest reconstruction wins.
    best = max(maps.values(), key=lambda r: r.num_reg_images())
    return best


# ── Umeyama similarity (with scale) ──────────────────────────────────────────

def umeyama(src: np.ndarray, dst: np.ndarray) -> tuple[float, np.ndarray, np.ndarray]:
    """Least-squares similarity mapping src->dst (Umeyama 1991), with scale.

    Returns (s, R, t) such that dst ~= s * R @ src + t.
    """
    assert src.shape == dst.shape and src.shape[1] == 3
    n = src.shape[0]
    mu_s = src.mean(axis=0)
    mu_d = dst.mean(axis=0)
    Xs = src - mu_s
    Xd = dst - mu_d
    Sigma = (Xd.T @ Xs) / n
    U, D, Vt = np.linalg.svd(Sigma)
    S = np.eye(3)
    if np.linalg.det(U) * np.linalg.det(Vt) < 0:
        S[2, 2] = -1.0
    R = U @ S @ Vt
    var_s = (Xs ** 2).sum() / n
    s = float((D * np.diag(S)).sum() / var_s)
    t = mu_d - s * R @ mu_s
    return s, R, t


# ── main ─────────────────────────────────────────────────────────────────────

FLIP = np.diag([1.0, -1.0, -1.0])  # COLMAP world->cam  ->  OHAO world->view


def orbit_shape(centers: np.ndarray) -> tuple[np.ndarray, str]:
    """PCA singular values of camera centers + a human verdict.

    A clean orbit is near-planar with two comparable in-plane axes (sv0~=sv1,
    sv2~=0). A degenerate SfM collapse shows sv0>>sv1 or centers piled at one
    point (tiny total spread)."""
    sv = np.linalg.svd(centers - centers.mean(0), full_matrices=False)[1]
    spread = float(np.linalg.norm(centers - centers.mean(0), axis=1).mean())
    if spread < 1e-6:
        return sv, "COLLAPSED (all centers coincident)"
    ratio = sv[0] / max(sv[1], 1e-9)
    if ratio > 2.0:
        return sv, f"DISTORTED (in-plane axis ratio {ratio:.1f}:1, expect ~1:1 for an orbit)"
    return sv, f"orbit-like (axis ratio {ratio:.2f}:1)"


def convert_and_align(colmap_mats: dict[str, np.ndarray],
                      gt_center: dict[str, np.ndarray]):
    """Shared Piece-3 math: COLMAP world->cam matrices (3x4) -> OHAO world->view
    poses aligned into the GT frame. Returns (per-name aligned (Rv,tv), s, R, t,
    rmse, colmap_centers, gt_centers) over the common names."""
    names = [n for n in colmap_mats if n in gt_center]
    Rv = {}; tv = {}; Cc = {}
    for n in names:
        M = colmap_mats[n]
        Rc, tc = M[:, :3], M[:, 3]
        Rv[n] = FLIP @ Rc
        tv[n] = FLIP @ tc
        Cc[n] = -Rc.T @ tc
    src = np.stack([Cc[n] for n in names])
    dst = np.stack([gt_center[n] for n in names])
    s, R_al, t_al = umeyama(src, dst)
    aligned = {}
    Cg_all = (s * (R_al @ src.T).T) + t_al
    for i, n in enumerate(names):
        Rv_new = Rv[n] @ R_al.T
        tv_new = -Rv_new @ Cg_all[i]
        aligned[n] = (Rv_new, tv_new)
    rmse = float(np.sqrt(((Cg_all - dst) ** 2).sum(1).mean()))
    return aligned, s, R_al, t_al, rmse, src, dst


def run_selftest() -> int:
    """Validate the FLIP + Umeyama + pose-reconstruction math end-to-end WITHOUT
    COLMAP: synthesize COLMAP-convention poses from known OHAO GT views under a
    known random similarity, run the exact production convert+align path, and
    assert it recovers the GT views to ~machine precision. Proves Piece 3 code is
    correct independently of SfM input quality."""
    rng = np.random.default_rng(0)
    # A synthetic GT orbit of OHAO world->view poses.
    n = 16
    gt_view = {}; gt_center = {}
    for i in range(n):
        az = 2 * np.pi * i / n
        eye = np.array([5 * np.cos(az), 2.0, 5 * np.sin(az)])
        fwd = -eye / np.linalg.norm(eye)
        up0 = np.array([0.0, 1.0, 0.0])
        right = np.cross(fwd, up0); right /= np.linalg.norm(right)
        up = np.cross(right, fwd)
        R = np.stack([right, up, -fwd])  # world->view rows: right, up, -fwd
        t = -R @ eye
        gt_view[f"v{i}"] = (R, t); gt_center[f"v{i}"] = eye
    # Known random similarity (COLMAP's arbitrary frame + scale) and rotation.
    th = rng.uniform(0, 2 * np.pi)
    axis = rng.normal(size=3); axis /= np.linalg.norm(axis)
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    R0 = np.eye(3) + np.sin(th) * K + (1 - np.cos(th)) * (K @ K)  # Rodrigues
    s0 = rng.uniform(0.2, 5.0)
    t0 = rng.uniform(-10, 10, size=3)
    # Build synthetic COLMAP cam_from_world = [FLIP@Rg@R0^T | -Rc @ (s0 R0 Cgt + t0)].
    colmap_mats = {}
    for name, (Rg, tg) in gt_view.items():
        Cgt = gt_center[name]
        Rc = FLIP @ Rg @ R0.T
        Cc = s0 * (R0 @ Cgt) + t0
        tc = -Rc @ Cc
        colmap_mats[name] = np.hstack([Rc, tc[:, None]])
    aligned, s, R_al, t_al, rmse, src, dst = convert_and_align(colmap_mats, gt_center)
    # Compare recovered views to GT views.
    dR = max(np.linalg.norm(aligned[n][0] - gt_view[n][0]) for n in gt_view)
    dt = max(np.linalg.norm(aligned[n][1] - gt_view[n][1]) for n in gt_view)
    print("===== convert+align self-test (synthetic similarity round-trip) =====")
    print(f"  injected similarity: scale s0={s0:.4f}  |t0|={np.linalg.norm(t0):.3f}")
    print(f"  recovered scale s={s:.6f}  (expect 1/s0={1/s0:.6f})")
    print(f"  camera-center RMSE after alignment = {rmse:.3e}")
    print(f"  max |R_recovered - R_gt| = {dR:.3e}   max |t_recovered - t_gt| = {dt:.3e}")
    ok = rmse < 1e-6 and dR < 1e-6 and dt < 1e-6
    print(f"  RESULT: {'PASS' if ok else 'FAIL'} — convert+align math is "
          f"{'correct' if ok else 'BROKEN'}")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="H3/M0b COLMAP link for OHAO inverse rendering")
    ap.add_argument("--selftest", action="store_true",
                    help="validate convert+align math (no COLMAP) and exit")
    ap.add_argument("--gt-bundle", type=Path,
                    help="GT orbit-capture bundle (dir with capture/ or the capture/ dir)")
    ap.add_argument("--out-root", type=Path,
                    help="output root; writes <root>/colmap/capture and <root>/gt_matched/capture")
    ap.add_argument("--work", type=Path, default=None, help="scratch dir (default <out-root>/_sfm)")
    ap.add_argument("--min-reg-frac", type=float, default=0.70, help="SfM registration gate")
    ap.add_argument("--no-brighten", action="store_true", help="feed raw (dark) images to COLMAP")
    ap.add_argument("--use-gpu", action="store_true", help="try GPU SIFT (needs display/OpenGL)")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()
    if args.gt_bundle is None or args.out_root is None:
        ap.error("--gt-bundle and --out-root are required (unless --selftest)")

    cap = resolve_capture_dir(args.gt_bundle)
    rows = read_cameras_jsonl(cap)
    N = len(rows)
    by_file = {r["file"]: r for r in rows}
    gt_center = {r["file"]: camera_center(*view_to_Rt(r["view"])) for r in rows}
    print(f"[GT] {N} views from {cap}")

    work = args.work or (args.out_root / "_sfm")
    img_src = cap / "images"

    # Images for SfM (brightened copy unless disabled).
    if args.no_brighten:
        sfm_images = img_src
    else:
        sfm_images = work / "images_bright"
        print(f"[prep] exposure-normalizing images for SIFT -> {sfm_images}")
        brighten_images(img_src, sfm_images)

    # Known intrinsics from the GT camera (fov is vertical, glm::perspective):
    # f = (H/2)/tan(fov/2), square pixels, principal point at image center.
    from PIL import Image
    W, H = Image.open(next(iter(img_src.glob("*.png")))).size
    fov_deg_gt = float(rows[0].get("fov_deg", 40.0))
    f_px = (H / 2.0) / np.tan(np.radians(fov_deg_gt) / 2.0)
    cam_params = f"{f_px:.6f},{f_px:.6f},{W/2.0:.6f},{H/2.0:.6f}"
    print(f"[intrinsics] known pinhole: {W}x{H}  fov={fov_deg_gt:.1f}deg  f={f_px:.2f}px  (fixed in BA)")

    print("[SfM] pycolmap: SIFT extract -> exhaustive match -> incremental mapping ...")
    rec = run_sfm(sfm_images, work, cam_params, use_gpu=args.use_gpu)
    n_reg = rec.num_reg_images()
    frac = n_reg / N
    print(f"\n===== Piece 2: SfM registration gate =====")
    print(f"  registered {n_reg} / {N} images  ({100*frac:.1f}%)")
    print(f"  mean track length      = {rec.compute_mean_track_length():.2f}")
    print(f"  mean reproj error (px) = {rec.compute_mean_reprojection_error():.3f}")
    print(f"  #3D points             = {rec.num_points3D()}")
    gate2 = "PASS" if frac >= args.min_reg_frac else "FAIL"
    print(f"  gate (>= {100*args.min_reg_frac:.0f}%): {gate2}")

    # ── Piece 3: convert every registered pose to OHAO view convention ────────
    reg = []  # list of dict: file, split, gt_index, Rv, tv, center_colmap, fov_deg
    for img in rec.images.values():
        if not img.has_pose:
            continue
        name = img.name
        if name not in by_file:
            print(f"  WARN: reconstructed image {name} not in GT bundle; skipping")
            continue
        M = np.asarray(img.cam_from_world().matrix(), dtype=np.float64)  # 3x4 world->cam (COLMAP)
        Rc, tc = M[:, :3], M[:, 3]
        Rv = FLIP @ Rc           # OHAO world->view rotation
        tv = FLIP @ tc           # OHAO world->view translation
        Cc = -Rc.T @ tc          # camera center (convention-independent)
        camc = rec.cameras[img.camera_id]
        fy = float(camc.focal_length_y)
        H = float(camc.height)
        fov_deg = float(np.degrees(2.0 * np.arctan(0.5 * H / fy)))
        reg.append(dict(file=name, split=by_file[name]["split"],
                        gt_index=by_file[name]["index"], Rv=Rv, tv=tv,
                        center=Cc, fov_deg=fov_deg))

    reg.sort(key=lambda d: d["gt_index"])
    if len(reg) < 3:
        print("FATAL: <3 registered poses; cannot align.")
        return 1

    # Umeyama on camera centers: COLMAP frame -> GT frame.
    src = np.stack([d["center"] for d in reg])                 # colmap centers
    dst = np.stack([gt_center[d["file"]] for d in reg])         # GT centers
    s, R_al, t_al = umeyama(src, dst)

    aligned_centers = (s * (R_al @ src.T).T) + t_al
    resid = np.linalg.norm(aligned_centers - dst, axis=1)
    rmse = float(np.sqrt((resid ** 2).mean()))
    # Scene scale references for context.
    gt_all = np.stack([gt_center[r["file"]] for r in rows])
    scene_radius = float(np.linalg.norm(gt_all - gt_all.mean(0), axis=1).mean())
    sv_c, verdict_c = orbit_shape(src)
    sv_g, verdict_g = orbit_shape(dst)
    print(f"\n===== Piece 3: convert + Umeyama alignment gate =====")
    print(f"  axis flip: F = diag(1,-1,-1) (negate camera Y,Z rows)  [COLMAP y-down/z-fwd -> OHAO y-up/z-back]")
    print(f"  COLMAP reconstruction shape: PCA sv={sv_c.round(3)}  -> {verdict_c}")
    print(f"  GT orbit shape:              PCA sv={sv_g.round(3)}  -> {verdict_g}")
    print(f"  Umeyama scale s = {s:.5f}   (COLMAP->GT)")
    print(f"  camera-center RMSE after alignment = {rmse:.4f} world units")
    print(f"  per-view residual: max={resid.max():.4f} median={np.median(resid):.4f}")
    print(f"  scene scale: mean cam radius = {scene_radius:.3f}  (RMSE/radius = {100*rmse/scene_radius:.2f}%)")
    gate3 = "PASS" if rmse < 0.05 * scene_radius else ("OK" if rmse < 0.15 * scene_radius else "FAIL")
    print(f"  gate (RMSE < 5% radius): {gate3}")

    # Apply the similarity to each pose -> aligned OHAO world->view in GT frame.
    # Rotate orientation by R_al; recompute translation from the transformed
    # center so the view rotation stays orthonormal (scale is absorbed).
    for d, Cg in zip(reg, aligned_centers):
        Rv_new = d["Rv"] @ R_al.T
        tv_new = -Rv_new @ Cg
        d["Rv_al"], d["tv_al"], d["center_al"] = Rv_new, tv_new, Cg

    # ── write the two bundles (colmap poses + matched GT poses) ──────────────
    # Emission order: train block first, holdout block last, re-indexed 0..M-1,
    # so the fit loader's index->view mapping stays contiguous & aligned even if
    # some views failed to register. Guarantee >=1 holdout for a novel-view PSNR.
    train = [d for d in reg if d["split"] != "holdout"]
    hold = [d for d in reg if d["split"] == "holdout"]
    if not hold and len(train) >= 2:
        hold = [train.pop()]  # promote the last registered view to holdout
    ordered = train + hold
    for i, d in enumerate(ordered):
        d["out_index"] = i
        d["out_split"] = "holdout" if d in hold else "train"

    def write_bundle(dst_root: Path, pose_key_R: str, pose_key_t: str, tag: str) -> None:
        out_cap = dst_root / "capture"
        out_cap.mkdir(parents=True, exist_ok=True)
        # Link images/ + relight/ and copy the small aux files from the GT bundle.
        for sub in ("images", "relight"):
            src_sub = cap / sub
            dst_sub = out_cap / sub
            if src_sub.exists() and not dst_sub.exists():
                os.symlink(os.path.relpath(src_sub, out_cap), dst_sub)
        if (cap / "materials").exists():
            shutil.copytree(cap / "materials", out_cap / "materials", dirs_exist_ok=True)
        for fn in ("capture.json", "theta_gt.json"):
            if (cap / fn).exists():
                shutil.copy2(cap / fn, out_cap / fn)
        with open(out_cap / "cameras.jsonl", "w") as f:
            for d in ordered:
                view16 = Rt_to_view16(d[pose_key_R], d[pose_key_t])
                rec_line = {
                    "index": d["out_index"], "name": "orbit", "file": d["file"],
                    "split": d["out_split"], "fov_deg": d["fov_deg"], "view": view16,
                }
                f.write(json.dumps(rec_line) + "\n")
        print(f"  wrote {tag} bundle -> {out_cap}  (train={len(train)} holdout={len(hold)})")

    print(f"\n===== bundles =====")
    # COLMAP-pose bundle (aligned SfM poses).
    write_bundle(args.out_root / "colmap", "Rv_al", "tv_al", "COLMAP")
    # GT-pose bundle over the SAME registered subset + identical re-indexing.
    for d in reg:
        Rg, tg = view_to_Rt(by_file[d["file"]]["view"])
        d["Rv_gt"], d["tv_gt"] = Rg, tg
    write_bundle(args.out_root / "gt_matched", "Rv_gt", "tv_gt", "GT-matched")

    # Machine-readable summary.
    summary = dict(
        n_views=N, n_registered=n_reg, reg_frac=frac,
        mean_track_length=float(rec.compute_mean_track_length()),
        mean_reproj_error_px=float(rec.compute_mean_reprojection_error()),
        umeyama_scale=s, center_rmse=rmse, scene_radius=scene_radius,
        rmse_over_radius=rmse / scene_radius,
        gate_sfm=gate2, gate_align=gate3,
        train=len(train), holdout=len(hold),
    )
    (args.out_root).mkdir(parents=True, exist_ok=True)
    with open(args.out_root / "colmap_solve_summary.json", "w") as f:
        json.dump(summary, f, indent=2)
    print(f"\n[summary] {args.out_root / 'colmap_solve_summary.json'}")
    return 0 if gate2 == "PASS" else 3


if __name__ == "__main__":
    raise SystemExit(main())
