#!/usr/bin/env python3
"""H3 / M1 — synthetic ChArUco rehearsal: a falsifiable gate for photo_ingest.py.

No real photos exist yet, so we cannot claim the ingestion works on real capture.
What we CAN do is manufacture images whose camera poses are known exactly, push
them through the real `photo_ingest.py`, and measure the pose error.

`generate` renders N virtual views of the printed board PNG lying on z=0 with a
textured box standing on it. Rendering is an exact inverse-mapped ray/plane trace:
for every output pixel we undistort it to an ideal ray, intersect it with each
textured plane (board + 5 box faces), keep the nearest hit and sample that plane's
texture. That means the images obey a REAL pinhole+distortion camera model with
known K, dist, R, t — including correct occlusion of the board by the box, which
also gives SfM genuine off-plane structure to reconstruct.

`compare` reads the bundle photo_ingest.py wrote and reports:
  * camera-center RMSE in millimetres
  * camera rotation error in degrees

What this proves: the ChArUco detection -> calibration -> PnP -> frame-conversion
-> bundle math is correct end to end.
What it does NOT prove: anything about real photographs (real lenses, rolling
shutter, motion blur, printer scale error, non-flat paper) or about recovering
materials from them.

Run with: /tmp/colmap-venv/bin/python
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from photo_ingest import (  # noqa: E402
    board_center_offset,
    cv_pose_to_ohao,
    make_board,
    rot_angle_deg,
)


# ── textured planes ──────────────────────────────────────────────────────────

class Plane:
    """Textured parallelogram: p(a,b) = O + a*U + b*V, (a,b) in [0,1]^2."""

    def __init__(self, O, U, V, tex):
        self.O = np.asarray(O, np.float64)
        self.U = np.asarray(U, np.float64)
        self.V = np.asarray(V, np.float64)
        self.tex = np.asarray(tex, np.float32)


def sample_bilinear(tex: np.ndarray, x: np.ndarray, y: np.ndarray) -> np.ndarray:
    h, w = tex.shape[:2]
    x = np.clip(x, 0, w - 1)
    y = np.clip(y, 0, h - 1)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)
    fx = (x - x0)[:, None]
    fy = (y - y0)[:, None]
    return (tex[y0, x0] * (1 - fx) * (1 - fy) + tex[y0, x1] * fx * (1 - fy)
            + tex[y1, x0] * (1 - fx) * fy + tex[y1, x1] * fx * fy)


def noise_texture(res: int, seed: int, base_hue: float) -> np.ndarray:
    """Deterministic multi-scale colour noise — plenty of SIFT-able structure."""
    rng = np.random.default_rng(seed)
    out = np.zeros((res, res, 3), np.float32)
    amp = 1.0
    for lvl, n in enumerate((4, 8, 16, 48, 128, 256)):
        layer = rng.random((n, n, 3)).astype(np.float32)
        layer = cv2.resize(layer, (res, res), interpolation=cv2.INTER_CUBIC)
        out += amp * layer
        amp *= 0.72
    out -= out.min()
    out /= max(out.max(), 1e-6)
    tint = np.array([0.5 + 0.5 * math.cos(base_hue),
                     0.5 + 0.5 * math.cos(base_hue + 2.1),
                     0.5 + 0.5 * math.cos(base_hue + 4.2)], np.float32)
    out = 0.25 + 0.7 * out * tint
    return np.clip(out[:, :, ::-1] * 255.0, 0, 255).astype(np.uint8)  # BGR


def board_plane_from_png(board_png: Path, board) -> tuple[Plane, np.ndarray]:
    """Fit the metric(board frame) -> PNG-pixel affine map by detecting the board
    inside its own PNG, then express the whole PNG as one textured plane."""
    img = cv2.imread(str(board_png), cv2.IMREAD_COLOR)
    if img is None:
        raise SystemExit(f"FATAL: cannot read board PNG {board_png}")
    det = cv2.aruco.CharucoDetector(board)
    cc, ci, _mc, _mi = det.detectBoard(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
    if cc is None or len(cc) < 8:
        raise SystemExit("FATAL: could not detect the board inside its own PNG")
    op, ip = board.matchImagePoints(cc, ci)
    XY = op.reshape(-1, 3)[:, :2]
    UV = ip.reshape(-1, 2)
    # px = A @ [X, Y] + b   (a scan of a flat board is affine to machine precision)
    M = np.hstack([XY, np.ones((len(XY), 1))])
    sol, *_ = np.linalg.lstsq(M, UV, rcond=None)          # 3x2
    A = sol[:2, :].T                                       # 2x2
    b = sol[2, :]                                          # 2
    resid = float(np.abs(M @ sol - UV).max())
    Ainv = np.linalg.inv(A)
    H, W = img.shape[:2]

    def metric(px):
        xy = Ainv @ (np.asarray(px, np.float64) - b)
        return np.array([xy[0], xy[1], 0.0])

    O = metric([0.0, 0.0])
    U = metric([W, 0.0]) - O
    V = metric([0.0, H]) - O
    return Plane(O, U, V, img), np.array([resid, W, H])


def box_planes(center_xy: np.ndarray, size: np.ndarray, seed: int,
               tex_res: int = 384) -> list[Plane]:
    """Axis-aligned box standing on the board (z=0): 4 sides + top.

    The board's printed face is the -z side (OpenCV board frame), so the box grows
    toward -z. Each face gets its own texture."""
    cx, cy = center_xy
    sx, sy, sz = size
    x0, x1 = cx - sx / 2, cx + sx / 2
    y0, y1 = cy - sy / 2, cy + sy / 2
    up = np.array([0, 0, -sz])
    faces = [
        # (origin, U, V)
        (np.array([x0, y0, 0.0]), np.array([sx, 0, 0]), up),    # -y side
        (np.array([x1, y0, 0.0]), np.array([0, sy, 0]), up),    # +x side
        (np.array([x1, y1, 0.0]), np.array([-sx, 0, 0]), up),   # +y side
        (np.array([x0, y1, 0.0]), np.array([0, -sy, 0]), up),   # -x side
        (np.array([x0, y0, -sz]), np.array([sx, 0, 0]), np.array([0, sy, 0])),  # top
    ]
    return [Plane(O, U, V, noise_texture(tex_res, seed + 11 * i, 0.9 * i))
            for i, (O, U, V) in enumerate(faces)]


# ── renderer ─────────────────────────────────────────────────────────────────

def render_view(planes: list[Plane], K: np.ndarray, dist: np.ndarray,
                R: np.ndarray, t: np.ndarray, W: int, H: int,
                bg=(140, 140, 140), ss: int = 2, rows_per_chunk: int = 128) -> np.ndarray:
    """Exact inverse-mapped render: distorted pixel -> ideal ray -> nearest plane.

    Renders at `ss`x supersampling and box-filters down, so the printed board's
    fine marker edges do not alias into undetectable mush.
    """
    Ws, Hs = W * ss, H * ss
    Ks = K.copy()
    Ks[:2, :] *= ss                       # same camera, ss times finer sampling
    Ks[0, 2] += 0.5 * (ss - 1)
    Ks[1, 2] += 0.5 * (ss - 1)
    big = np.empty((Hs, Ws, 3), np.float32)
    Ps = [np.stack([R @ pl.U, R @ pl.V, R @ pl.O + t], 1) for pl in planes]
    for y0 in range(0, Hs, rows_per_chunk):
        y1 = min(y0 + rows_per_chunk, Hs)
        us, vs = np.meshgrid(np.arange(Ws, dtype=np.float64),
                             np.arange(y0, y1, dtype=np.float64))
        pix = np.stack([us.ravel(), vs.ravel()], 1).reshape(-1, 1, 2)
        norm = cv2.undistortPoints(pix, Ks, dist).reshape(-1, 2)      # ideal (x, y)
        rays = np.hstack([norm, np.ones((len(norm), 1))]).T           # 3xN
        n = rays.shape[1]
        out = np.tile(np.asarray(bg, np.float32), (n, 1))
        best = np.full(n, np.inf)
        for pl, P in zip(planes, Ps):
            try:
                q = np.linalg.solve(P, rays)
            except np.linalg.LinAlgError:
                continue
            w = q[2]
            with np.errstate(divide="ignore", invalid="ignore"):
                a, b = q[0] / w, q[1] / w
                depth = 1.0 / w
            hit = (w > 0) & (a >= 0) & (a <= 1) & (b >= 0) & (b <= 1) & (depth < best)
            if not hit.any():
                continue
            th, tw = pl.tex.shape[:2]
            out[hit] = sample_bilinear(pl.tex, a[hit] * (tw - 1), b[hit] * (th - 1))
            best[hit] = depth[hit]
        big[y0:y1] = out.reshape(y1 - y0, Ws, 3)
    small = big.reshape(H, ss, W, ss, 3).mean(axis=(1, 3))
    return np.clip(small, 0, 255).astype(np.uint8)


def look_at_cv(eye: np.ndarray, target: np.ndarray, roll_deg: float,
               up=np.array([0.0, 0.0, -1.0])) -> tuple[np.ndarray, np.ndarray]:
    """OpenCV-convention world->cam (x right, y DOWN, z FORWARD)."""
    f = target - eye
    f /= np.linalg.norm(f)
    r = np.cross(f, up)
    if np.linalg.norm(r) < 1e-6:
        r = np.cross(f, np.array([0.0, 1.0, 0.0]))
    r /= np.linalg.norm(r)
    d = np.cross(f, r)                    # camera "down"
    R = np.stack([r, d, f])               # rows = camera axes in world
    if abs(roll_deg) > 1e-9:
        a = math.radians(roll_deg)
        Rr = np.array([[math.cos(a), -math.sin(a), 0],
                       [math.sin(a), math.cos(a), 0],
                       [0, 0, 1.0]])
        R = Rr @ R
    return R, -R @ eye


# ── generate ─────────────────────────────────────────────────────────────────

def cmd_generate(args) -> int:
    board = make_board(args.board_cols, args.board_rows, args.square_mm,
                       args.marker_mm, args.dict, False)
    bp, meta = board_plane_from_png(args.board_png, board)
    print(f"board PNG {int(meta[1])}x{int(meta[2])}  metric-affine fit residual "
          f"{meta[0]:.3f} px")

    cols, rows = board.getChessboardSize()
    s = board.getSquareLength()
    center = np.array([cols * s * 0.5, rows * s * 0.5, 0.0])
    planes = [bp] + box_planes(center[:2], np.array(args.box_mm) / 1000.0, seed=args.seed)

    W, H = args.width, args.height
    f = args.focal_frac * W
    K = np.array([[f, 0, W / 2.0 + args.cx_off], [0, f, H / 2.0 + args.cy_off], [0, 0, 1.0]])
    dist = np.array(args.dist, np.float64).reshape(1, -1)

    out: Path = args.out
    if out.exists():
        shutil.rmtree(out)
    (out / "photos").mkdir(parents=True)

    rng = np.random.default_rng(args.seed)
    gt = []
    for i in range(args.views):
        az = 2 * math.pi * i / args.views + rng.normal(0, 0.02)
        el = math.radians(args.elev_min + (args.elev_max - args.elev_min)
                          * (0.5 + 0.5 * math.sin(3.1 * i / args.views)))
        rad = args.radius_mm / 1000.0 * (1.0 + rng.normal(0, 0.04))
        # OpenCV board frame: +z points INTO the board, so a camera that sees the
        # print is at negative z. Objects standing on the board also grow toward -z.
        eye = center + np.array([rad * math.cos(el) * math.cos(az),
                                 rad * math.cos(el) * math.sin(az),
                                 -rad * math.sin(el)])
        tgt = center + np.array([rng.normal(0, 0.008), rng.normal(0, 0.008),
                                 -rng.uniform(0.0, 0.03)])
        R, t = look_at_cv(eye, tgt, rng.normal(0, args.roll_deg))
        img = render_view(planes, K, dist, R, t, W, H)
        if args.noise > 0:
            img = np.clip(img.astype(np.float32)
                          + rng.normal(0, args.noise, img.shape), 0, 255).astype(np.uint8)
        name = f"view_{i:03d}.jpg"
        cv2.imwrite(str(out / "photos" / name), img,
                    [int(cv2.IMWRITE_JPEG_QUALITY), args.jpeg_q])
        gt.append(dict(file=name, R_cv=R.reshape(-1).tolist(), t_cv=t.tolist(),
                       center_board=(-R.T @ t).tolist(),
                       eye=eye.tolist()))
        print(f"  view {i:3d}  az={math.degrees(az):6.1f}  el={math.degrees(el):5.1f}  "
              f"r={1000*rad:5.1f} mm")

    # Dedicated calibration sweep: board alone (no object), deliberately shoved into
    # every corner of the frame and tilted hard, which is what actually constrains
    # lens distortion. Mirrors the recipe in docs/h3_capture_guide.md.
    if args.calib_views > 0:
        (out / "calib_photos").mkdir(parents=True, exist_ok=True)
        for i in range(args.calib_views):
            az = 2 * math.pi * (i * 0.37)
            el = math.radians(rng.uniform(35.0, 88.0))
            rad = rng.uniform(0.20, 0.36)
            eye = center + np.array([rad * math.cos(el) * math.cos(az),
                                     rad * math.cos(el) * math.sin(az),
                                     -rad * math.sin(el)])
            # Aim off-center so the board lands in a different frame region each time.
            off = np.array([rng.uniform(-0.08, 0.08), rng.uniform(-0.10, 0.10), 0.0])
            R, t = look_at_cv(eye, center + off, rng.normal(0, 12.0))
            img = render_view([bp], K, dist, R, t, W, H)
            if args.noise > 0:
                img = np.clip(img.astype(np.float32)
                              + rng.normal(0, args.noise, img.shape), 0, 255).astype(np.uint8)
            cv2.imwrite(str(out / "calib_photos" / f"calib_{i:03d}.jpg"), img,
                        [int(cv2.IMWRITE_JPEG_QUALITY), args.jpeg_q])
        print(f"calibration sweep -> {out / 'calib_photos'}  ({args.calib_views} board-only "
              f"frames)")

    (out / "rehearsal_gt.json").write_text(json.dumps(dict(
        note="Synthetic ChArUco rehearsal ground truth. Board frame = OpenCV world "
             "(z up, origin at the board's first square corner), metres.",
        board=dict(cols=args.board_cols, rows=args.board_rows,
                   square_mm=args.square_mm, marker_mm=args.marker_mm, dict=args.dict),
        image_wh=[W, H], K=K.tolist(), dist=dist.ravel().tolist(),
        box_mm=list(args.box_mm), jpeg_q=args.jpeg_q, noise=args.noise,
        views=gt), indent=2) + "\n")
    print(f"\nrehearsal -> {out / 'photos'}  ({args.views} views, {W}x{H}, "
          f"f={f:.1f}px, dist={dist.ravel().tolist()})")
    print(f"GT -> {out / 'rehearsal_gt.json'}")
    return 0


# ── compare ──────────────────────────────────────────────────────────────────

def cmd_compare(args) -> int:
    gt = json.loads(Path(args.gt).read_text())
    cap = args.bundle
    if (cap / "capture" / "capture.json").exists():
        cap = cap / "capture"
    man = json.loads((cap / "capture.json").read_text())
    calib = man.get("calibration", {})
    world_scale = float(calib.get("world_units_per_meter", 1.0))
    origin = calib.get("board", {}).get("origin", "center")
    board = make_board(int(calib["board"]["cols"]), int(calib["board"]["rows"]),
                       float(calib["board"]["square_mm"]),
                       float(calib["board"]["marker_mm"]), calib["board"]["dict"], False)
    c_off = board_center_offset(board, origin)

    gt_by_file = {v["file"]: v for v in gt["views"]}
    rows = [json.loads(l) for l in (cap / "cameras.jsonl").read_text().splitlines() if l.strip()]

    dc, dr, per = [], [], []
    for r in rows:
        src = r.get("source_photo")
        if src not in gt_by_file:
            print(f"  WARN: {r['file']} has no GT (source={src})")
            continue
        g = gt_by_file[src]
        R_cv = np.array(g["R_cv"], np.float64).reshape(3, 3)
        t_cv = np.array(g["t_cv"], np.float64)
        R_gt, t_gt, C_gt = cv_pose_to_ohao(R_cv, t_cv, c_off, world_scale)
        M = np.array(r["view"], np.float64).reshape(4, 4)
        R_est, t_est = M[:3, :3], M[:3, 3]
        C_est = -R_est.T @ t_est
        e_mm = 1000.0 * float(np.linalg.norm(C_est - C_gt)) / world_scale
        e_deg = rot_angle_deg(R_est, R_gt)
        dc.append(e_mm)
        dr.append(e_deg)
        per.append(dict(file=r["file"], source=src, center_err_mm=e_mm, rot_err_deg=e_deg))

    if not dc:
        print("FATAL: no overlapping views between bundle and GT")
        return 1
    dc = np.array(dc)
    dr = np.array(dr)
    rmse = float(np.sqrt((dc ** 2).mean()))
    # Scene scale for context: mean camera distance from the board origin.
    cams = np.array([np.array(json.loads(l)["position"]) for l in
                     (cap / "cameras.jsonl").read_text().splitlines() if l.strip()])
    radius_mm = 1000.0 * float(np.linalg.norm(cams, axis=1).mean()) / world_scale

    print("===== SYNTHETIC CHARUCO REHEARSAL GATE =====")
    print(f"  views compared            : {len(dc)} / {len(rows)} bundle lines"
          f"  ({len(gt['views'])} rendered)")
    print(f"  pose source in bundle     : {calib.get('pose_source')}")
    print(f"  camera-center RMSE        : {rmse:.3f} mm   "
          f"(median {np.median(dc):.3f}, max {dc.max():.3f})")
    print(f"  mean camera distance      : {radius_mm:.1f} mm  "
          f"-> RMSE = {100*rmse/radius_mm:.3f}% of rig radius")
    print(f"  rotation error            : median {np.median(dr):.4f} deg  "
          f"max {dr.max():.4f} deg")
    print(f"  intrinsics RMS (reported) : {calib.get('intrinsics_rms_px')} px")
    fx_gt = gt["K"][0][0]
    fx_est = calib["K"][0][0]
    print(f"  focal fx: GT {fx_gt:.2f} px @ {gt['image_wh'][0]}px wide  vs recovered "
          f"{fx_est:.2f} px @ {calib['image_wh'][0]}px wide")
    ok = rmse <= args.max_rmse_mm and float(dr.max()) <= args.max_rot_deg
    print(f"  gate (RMSE <= {args.max_rmse_mm} mm AND max rot <= {args.max_rot_deg} deg): "
          f"{'PASS' if ok else 'FAIL'}")
    res = dict(n_compared=int(len(dc)), pose_source=calib.get("pose_source"),
               center_rmse_mm=rmse, center_median_mm=float(np.median(dc)),
               center_max_mm=float(dc.max()), rot_median_deg=float(np.median(dr)),
               rot_max_deg=float(dr.max()), rig_radius_mm=radius_mm,
               intrinsics_rms_px=calib.get("intrinsics_rms_px"),
               focal_gt_px=fx_gt, focal_recovered_px=fx_est,
               gate_max_rmse_mm=args.max_rmse_mm, gate_max_rot_deg=args.max_rot_deg,
               passed=bool(ok), per_view=per)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(res, indent=2) + "\n")
        print(f"  report -> {args.report}")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("generate", help="render synthetic ChArUco views")
    g.add_argument("--board-png", type=Path, default=Path("renders/h3_m1/charuco_board.png"))
    g.add_argument("--out", type=Path, default=Path("renders/h3_m1/rehearsal"))
    g.add_argument("--views", type=int, default=20)
    g.add_argument("--calib-views", type=int, default=14,
                   help="extra board-only frames for intrinsics calibration (0 = none)")
    g.add_argument("--width", type=int, default=1600)
    g.add_argument("--height", type=int, default=900)
    g.add_argument("--focal-frac", type=float, default=0.90,
                   help="focal length as a fraction of image width")
    g.add_argument("--cx-off", type=float, default=6.0, help="principal point offset x (px)")
    g.add_argument("--cy-off", type=float, default=-4.0, help="principal point offset y (px)")
    g.add_argument("--dist", type=float, nargs=5, default=[-0.09, 0.04, 0.001, -0.0008, 0.0],
                   help="k1 k2 p1 p2 k3 baked into the synthetic images")
    g.add_argument("--radius-mm", type=float, default=600.0)
    g.add_argument("--elev-min", type=float, default=25.0)
    g.add_argument("--elev-max", type=float, default=58.0)
    g.add_argument("--roll-deg", type=float, default=3.0, help="sigma of random camera roll")
    g.add_argument("--box-mm", type=float, nargs=3, default=[60.0, 50.0, 70.0])
    g.add_argument("--noise", type=float, default=2.0, help="sensor noise sigma (8-bit)")
    g.add_argument("--jpeg-q", type=int, default=92)
    g.add_argument("--seed", type=int, default=11)
    g.add_argument("--square-mm", type=float, default=35.0)
    g.add_argument("--marker-mm", type=float, default=26.0)
    g.add_argument("--board-cols", type=int, default=5)
    g.add_argument("--board-rows", type=int, default=7)
    g.add_argument("--dict", default="DICT_4X4_50")
    g.set_defaults(func=cmd_generate)

    c = sub.add_parser("compare", help="score a photo_ingest bundle against the GT")
    c.add_argument("--bundle", type=Path, required=True)
    c.add_argument("--gt", type=Path, required=True)
    c.add_argument("--max-rmse-mm", type=float, default=3.0)
    c.add_argument("--max-rot-deg", type=float, default=0.5)
    c.add_argument("--report", type=Path, default=None)
    c.set_defaults(func=cmd_compare)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
