#!/usr/bin/env python3
"""
render-golden -- the render goldens (requirement 3, no visual accident).

Renders each scene of a manifest through one `ohao serve` process (tests/common/ohao_serve.py: one device, the
programs compiled once, the scenes loaded in turn), at the size it is compared at, and compares the PNG with the
committed golden. A golden is a hash of an unchanged renderer, not an image-quality gate.

A scene PASSES when BOTH hold:
  - max per-channel abs diff <= max_abs_diff      (catches large-magnitude changes)
  - fraction of any-differing pixels <= max_diff_frac  (catches widespread tiny shifts)

Usage (run from repo root):
  python3 tests/golden/render_golden.py tests/golden/manifest.json            # check vs goldens
  python3 tests/golden/render_golden.py tests/golden/manifest.json --update   # (re)generate goldens
  python3 tests/golden/render_golden.py tests/golden/manifest.json --selftest # render 2x, compare to each other
  ... [--only NAME]   # any mode, one scene; --only may come before or after the mode flag

Each manifest scene: `name`, `scene` (a scene file), `capture` (render.capture's parameters: mode, size, frames,
spp, post, ev100, ...), `golden`, `max_abs_diff`, `max_diff_frac`. Exit code: 0 if all scenes pass, 1 otherwise.
"""
import sys, os, json, shutil

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("render-golden needs numpy + Pillow:  pip install numpy Pillow")

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "common"))
from ohao_serve import ROOT, Serve  # noqa: E402

DEFAULT_MAX_ABS_DIFF = 4
DEFAULT_MAX_DIFF_FRAC = 0.01
OUT = "build/golden_render"   # where the renders go (the directory `ohao serve` may write)


def render(serve, sc, out):
    """Renders scene `sc` into `out` (relative to the repo) -> None, or what went wrong."""
    try:
        serve.load(sc["scene"])
        serve.call("render.capture", {"path": out, **sc["capture"]})
    except RuntimeError as e:
        return str(e)[-600:]
    if not os.path.exists(os.path.join(ROOT, out)):
        return f"render produced no output at {out}"
    return None


def degenerate(path):
    """A frame with no spatial structure is not a render of a scene.

    This is a backstop, not the primary defence -- that is the example's exit
    code, and model_viewer and turntable now return non-zero when a model fails
    to load. It exists because they did NOT, for a long time: a failed load left
    them rendering an empty scene, exiting 0 and writing a valid PNG, so the
    corpus compared a picture of nothing against a golden full of geometry and
    reported it as a large pixel difference rather than a missing asset. A frame
    that is uniformly one colour is the loudest version of that failure and
    costs one std() to catch.
    """
    a = np.asarray(Image.open(os.path.join(ROOT, path)).convert("RGB"), dtype=np.float64)
    sd = float(a.std())
    if sd < 1.0:
        return f"rendered frame is nearly uniform (std {sd:.3f} over 0-255) -- not a scene"
    return None


def load(path):
    return np.asarray(Image.open(os.path.join(ROOT, path)).convert("RGB"), dtype=np.int16)


def diff_stats(a, b):
    if a.shape != b.shape:
        return None
    d = np.abs(a - b)
    px_diff = np.any(d > 0, axis=2)
    return {
        "max_abs": int(d.max()),
        "diff_px": int(px_diff.sum()),
        "diff_frac": float(px_diff.mean()),
        "rmse": float(np.sqrt((d.astype(np.float64) ** 2).mean())),
    }


def verdict(s, max_abs_diff, max_diff_frac):
    return s is not None and s["max_abs"] <= max_abs_diff and s["diff_frac"] <= max_diff_frac


NL = chr(10)
GRID = 16            # region cell size, in compare-resolution pixels
MAX_REGIONS = 4
CROP_SCALE = 4
CROP_PAD = 8
CROP_MAX_W, CROP_MAX_H = 96, 64   # a crop strip is at most (3 x 96 + 4) x 64 pixels before scaling


def diff_map(a, b):
    """Per-pixel max channel |a - b| (int), or None for a shape mismatch."""
    if a.shape != b.shape:
        return None
    return np.abs(a.astype(np.int16) - b.astype(np.int16)).max(axis=2)


def regions(d, limit=MAX_REGIONS):
    """Connected components (4-neighbour) of GRID x GRID cells holding any difference.
    -> (regions, total) where regions are the `limit` largest by differing pixels:
    {x, y, w, h, max_abs, px}, x/y/w/h in compare-resolution pixels."""
    h, w = d.shape
    gh, gw = -(-h // GRID), -(-w // GRID)
    padded = np.zeros((gh * GRID, gw * GRID), dtype=d.dtype)
    padded[:h, :w] = d
    cells = padded.reshape(gh, GRID, gw, GRID)
    cell_max = cells.max(axis=(1, 3))
    cell_px = (cells > 0).sum(axis=(1, 3))
    seen = np.zeros((gh, gw), dtype=bool)
    found = []
    for cy in range(gh):
        for cx in range(gw):
            if seen[cy, cx] or cell_max[cy, cx] == 0:
                continue
            stack, members = [(cy, cx)], []
            seen[cy, cx] = True
            while stack:
                y, x = stack.pop()
                members.append((y, x))
                for ny, nx in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                    if 0 <= ny < gh and 0 <= nx < gw and not seen[ny, nx] and cell_max[ny, nx] > 0:
                        seen[ny, nx] = True
                        stack.append((ny, nx))
            ys = [m[0] for m in members]
            xs = [m[1] for m in members]
            x0, y0 = min(xs) * GRID, min(ys) * GRID
            x1, y1 = min((max(xs) + 1) * GRID, w), min((max(ys) + 1) * GRID, h)
            found.append({"x": int(x0), "y": int(y0), "w": int(x1 - x0), "h": int(y1 - y0),
                          "max_abs": int(max(cell_max[m] for m in members)),
                          "px": int(sum(cell_px[m] for m in members))})
    found.sort(key=lambda r: (-r["px"], -r["max_abs"], r["y"], r["x"]))
    return found[:limit], len(found)


def heatmap(d):
    """RGB uint8 image of d: black where equal; otherwise a ramp from dark red (the
    smallest difference) through red and yellow to white (the largest)."""
    peak = int(d.max())
    t = np.zeros(d.shape, dtype=np.float64)
    if peak > 0:
        nz = d > 0
        t[nz] = 0.25 + 0.75 * (d[nz].astype(np.float64) / peak)
    r = np.clip(t * 3.0, 0.0, 1.0)
    g = np.clip(t * 3.0 - 1.0, 0.0, 1.0)
    b = np.clip(t * 3.0 - 2.0, 0.0, 1.0)
    return (np.stack([r, g, b], axis=2) * 255.0 + 0.5).astype(np.uint8)


def crop_window(d, region):
    """The crop for `region`: the region padded by CROP_PAD, or, when that is larger than
    CROP_MAX_W x CROP_MAX_H (scattered noise merges into one large region), a window of
    that size centred on the region's largest difference. -> (x0, y0, x1, y1)."""
    h, w = d.shape
    x0, y0 = max(0, region["x"] - CROP_PAD), max(0, region["y"] - CROP_PAD)
    x1, y1 = min(w, region["x"] + region["w"] + CROP_PAD), min(h, region["y"] + region["h"] + CROP_PAD)
    if x1 - x0 <= CROP_MAX_W and y1 - y0 <= CROP_MAX_H:
        return x0, y0, x1, y1
    sub = d[region["y"]:region["y"] + region["h"], region["x"]:region["x"] + region["w"]]
    py, px = np.unravel_index(int(np.argmax(sub)), sub.shape)
    cx, cy = region["x"] + int(px), region["y"] + int(py)
    cw, ch = min(CROP_MAX_W, w), min(CROP_MAX_H, h)
    x0 = min(max(0, cx - cw // 2), w - cw)
    y0 = min(max(0, cy - ch // 2), h - ch)
    return x0, y0, x0 + cw, y0 + ch


def crop_strip(expected, actual, d, region):
    """expected | actual | heat of the region's crop window, CROP_SCALE x nearest."""
    x0, y0, x1, y1 = crop_window(d, region)
    parts = [expected[y0:y1, x0:x1].astype(np.uint8), actual[y0:y1, x0:x1].astype(np.uint8),
             heatmap(d[y0:y1, x0:x1])]
    sep = np.full((y1 - y0, 2, 3), 128, dtype=np.uint8)
    strip = np.concatenate([parts[0], sep, parts[1], sep, parts[2]], axis=1)
    return np.repeat(np.repeat(strip, CROP_SCALE, axis=0), CROP_SCALE, axis=1)


def write_diff_images(name, expected, actual, d, diff_dir, reg):
    """Writes <name>.heat.png and, when there is a region, <name>.crop.png; -> their paths."""
    os.makedirs(diff_dir, exist_ok=True)
    out = {"heatmap": os.path.join(diff_dir, name + ".heat.png").replace(chr(92), "/")}
    Image.fromarray(heatmap(d)).save(out["heatmap"])
    if reg:
        out["crop"] = os.path.join(diff_dir, name + ".crop.png").replace(chr(92), "/")
        Image.fromarray(crop_strip(expected, actual, d, reg[0])).save(out["crop"])
    return out


def scene_record(name, mode, expected, actual, mad, mdf, diff_dir=None, heatmaps="failing"):
    """The JSON object for one compared scene; writes its diff images when asked to."""
    s = diff_stats(expected, actual)
    if s is None:
        return {"name": name, "mode": mode, "pass": False, "error": "size mismatch vs golden",
                "tolerance": {"max_abs": mad, "diff_frac": mdf}}
    ok = verdict(s, mad, mdf)
    d = diff_map(expected, actual)
    reg, total = regions(d)
    rec = {"name": name, "mode": mode, "pass": ok, "max_abs": s["max_abs"], "diff_frac": round(s["diff_frac"], 6),
           "diff_px": s["diff_px"], "rmse": round(s["rmse"], 4), "tolerance": {"max_abs": mad, "diff_frac": mdf},
           "regions": reg}
    if total > len(reg):
        rec["more_regions"] = total - len(reg)
    if diff_dir and (heatmaps == "all" or not ok):
        rec.update(write_diff_images(name, expected, actual, d, diff_dir, reg))
    return rec


USAGE = ("usage: render_golden.py <manifest.json> [--update|--selftest] [--only NAME] [--json FILE]"
         " [--heatmaps failing|all] [--diff-dir DIR]")


def parse_args(argv):
    """-> options {manifest, mode, only, json, heatmaps, diff_dir}. Exits with the usage line on bad arguments."""
    if len(argv) < 2 or argv[1].startswith("--"):
        sys.exit(USAGE)
    o = {"manifest": argv[1], "mode": "check", "only": None, "json": None, "heatmaps": "failing",
         "diff_dir": os.path.join("build", "golden_diff")}
    valued = {"--only": "only", "--json": "json", "--heatmaps": "heatmaps", "--diff-dir": "diff_dir"}
    rest = argv[2:]
    i = 0
    while i < len(rest):
        a = rest[i]
        if a in valued:
            if i + 1 >= len(rest) or rest[i + 1].startswith("--"):
                sys.exit(f"{a} needs a value" + NL + USAGE)
            o[valued[a]] = rest[i + 1]
            i += 2
            continue
        if a in ("--update", "--selftest") and o["mode"] == "check":
            o["mode"] = a
        else:
            sys.exit(f"unexpected argument {a!r}" + NL + USAGE)
        i += 1
    if o["heatmaps"] not in ("failing", "all"):
        sys.exit("--heatmaps is failing or all" + NL + USAGE)
    return o


def main():
    o = parse_args(sys.argv)
    manifest_path, mode, only = o["manifest"], o["mode"], o["only"]
    with open(manifest_path) as f:
        manifest = json.load(f)
    if only is not None and only not in [sc["name"] for sc in manifest["scenes"]]:
        names = ", ".join(sc["name"] for sc in manifest["scenes"])
        sys.exit(f"--only {only!r}: no such scene in {manifest_path} (scenes: {names})")

    records = []

    def failed(name, err):
        print(f"[FAIL]  {name}: {err}")
        records.append({"name": name, "mode": mode.lstrip("-"), "pass": False, "error": err})

    with Serve(OUT) as serve:
        for sc in manifest["scenes"]:
            if only is not None and sc["name"] != only:
                continue
            name = sc["name"]
            golden = sc["golden"]
            mad = sc.get("max_abs_diff", DEFAULT_MAX_ABS_DIFF)
            mdf = sc.get("max_diff_frac", DEFAULT_MAX_DIFF_FRAC)

            if mode == "--update":
                full = f"{OUT}/{name}.full.png"
                # Guard --update too, and hardest: writing a golden is the one
                # operation that cannot be caught later. A blank frame baked in
                # here becomes the thing every future run is measured against.
                err = render(serve, sc, full) or degenerate(full)
                if err:
                    print(f"[update] {name}: FAILED — {err}")
                    records.append({"name": name, "mode": "update", "pass": False, "error": err})
                    continue
                # Re-encoded, the same pixels: the engine's PNG writer favours speed over size.
                Image.open(os.path.join(ROOT, full)).convert("RGB").save(os.path.join(ROOT, golden), optimize=True)
                print(f"[update] {name}: golden written -> {golden}")
                records.append({"name": name, "mode": "update", "pass": True})
                continue

            actual = f"{OUT}/{name}.actual.png"
            err = render(serve, sc, actual) or degenerate(actual)
            if err:
                failed(name, err)
                continue

            if mode == "--selftest":
                # render a second time, compare the two renders to each other: the run-to-run floor
                actual2 = f"{OUT}/{name}.actual2.png"
                err2 = render(serve, sc, actual2)
                if err2:
                    failed(name, err2)
                    continue
                rec = scene_record(name, "selftest", load(actual), load(actual2), mad, mdf, o["diff_dir"], o["heatmaps"])
                records.append(rec)
                print(f"[{'PASS' if rec['pass'] else 'FAIL'}]  {name} (selftest): "
                      f"max_abs={rec['max_abs']} diff_px={rec['diff_px']} "
                      f"frac={rec['diff_frac']:.6f} rmse={rec['rmse']:.4f}")
                continue

            # default: compare against committed golden
            if not os.path.exists(os.path.join(ROOT, golden)):
                failed(name, f"golden missing ({golden}) — run with --update first")
                continue
            rec = scene_record(name, "check", load(golden), load(actual), mad, mdf, o["diff_dir"], o["heatmaps"])
            records.append(rec)
            if "error" in rec:
                print(f"[FAIL]  {name}: {rec['error']}")
                continue
            ok = rec["pass"]
            print(f"[{'PASS' if ok else 'FAIL'}]  {name}: "
                  f"max_abs={rec['max_abs']}(<= {mad}) "
                  f"diff_frac={rec['diff_frac']:.6f}(<= {mdf}) "
                  f"diff_px={rec['diff_px']} rmse={rec['rmse']:.4f}")
            if not ok:
                drift = golden.replace(".png", ".actual.png")
                shutil.copyfile(os.path.join(ROOT, actual), os.path.join(ROOT, drift))
                print(f"        drift written -> {drift}")
                for r in rec["regions"]:
                    print(f"        region x={r['x']} y={r['y']} {r['w']}x{r['h']}: max_abs {r['max_abs']}, {r['px']} px")
                if "heatmap" in rec:
                    print(f"        heatmap -> {rec['heatmap']}" + (f", crop -> {rec['crop']}" if "crop" in rec else ""))

    all_ok = all(r["pass"] for r in records)
    passed = sum(1 for r in records if r["pass"])
    if o["json"]:
        os.makedirs(os.path.dirname(os.path.abspath(o["json"])), exist_ok=True)
        with open(o["json"], "w", encoding="utf-8") as f:
            json.dump({"suite": "render_golden", "mode": mode.lstrip("-"), "passed": passed,
                       "failed": len(records) - passed, "scenes": records}, f, indent=1)
            f.write(NL)
    print(NL + "render-golden:", "ALL PASS" if all_ok else "FAILURES — see above")
    print(json.dumps({"suite": "render_golden", "passed": passed, "failed": len(records) - passed,
                      "failures": [r["name"] for r in records if not r["pass"]]}, separators=(",", ":")))
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
