#!/usr/bin/env python3
"""test_golden_diff -- the golden harness's diff report (sub-project 4, test 4). CPU only.

    python tests/golden/test_golden_diff.py     last line is a JSON summary; exit 0 = pass

  t4_* check render_golden.py's diff functions on synthetic image pairs with known
  differences (statistics, regions, heatmap pixels, crop), and run the harness end to end
  on a fake manifest whose "renderer" copies a prepared PNG: --json and the diff images
  for a failing scene, none for a passing one, and the selftest floor.
Needs numpy and Pillow (as render_golden.py does).
"""
import json
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import render_golden as rg  # noqa: E402

W, H = 640, 360
passed, failed, failures = 0, 0, []


def run(fn):
    global passed, failed
    print(f"  {fn.__name__:<62}", end="", flush=True)
    try:
        fn()
    except AssertionError as e:
        failed += 1
        failures.append(f"{fn.__name__}: {e}")
        print(f"FAIL\n    {e}")
        return
    passed += 1
    print("PASS")


def base():
    """A structured 640x360 image (a gradient), as int16 like render_golden.load()."""
    y, x = np.mgrid[0:H, 0:W]
    img = np.stack([(x * 255) // (W - 1), (y * 255) // (H - 1), ((x + y) * 127) // (W + H)], axis=2)
    return img.astype(np.int16)


def two_blobs():
    """base() with a 30x10 block raised by 5 at (100, 50) and one pixel by 1 at (600, 300)."""
    a = base()
    b = a.copy()
    b[50:60, 100:130, 0] += 5
    b[300, 600, 1] += 1
    return a, b


def t4_identical_images_have_no_difference():
    a = base()
    s = rg.diff_stats(a, a.copy())
    assert s == {"max_abs": 0, "diff_px": 0, "diff_frac": 0.0, "rmse": 0.0}, f"stats {s}"
    reg, total = rg.regions(rg.diff_map(a, a.copy()))
    assert reg == [] and total == 0, f"no regions (got {reg})"


def t4_statistics_of_a_known_difference():
    a, b = two_blobs()
    s = rg.diff_stats(a, b)
    assert s["max_abs"] == 5 and s["diff_px"] == 301, f"max_abs 5 over 301 pixels (got {s})"
    assert abs(s["diff_frac"] - 301 / (W * H)) < 1e-12, "diff_frac is the differing-pixel fraction"
    assert not rg.verdict(s, 4, 0.01) and rg.verdict(s, 5, 0.01), "max_abs 5 fails at 4 and passes at 5"


def t4_regions_are_the_differing_cells_largest_first():
    a, b = two_blobs()
    reg, total = rg.regions(rg.diff_map(a, b))
    assert total == 2 and len(reg) == 2, f"two regions (got {total})"
    # The block spans pixels x 100..129, y 50..59: cells x 96..143, y 48..63.
    assert reg[0] == {"x": 96, "y": 48, "w": 48, "h": 16, "max_abs": 5, "px": 300}, f"the block first: {reg[0]}"
    # The pixel at (600, 300): cell x 592..607, y 288..303.
    assert reg[1] == {"x": 592, "y": 288, "w": 16, "h": 16, "max_abs": 1, "px": 1}, f"then the pixel: {reg[1]}"


def t4_regions_are_capped_and_counted():
    a = base()
    b = a.copy()
    for i in range(6):                       # six separate single-pixel differences
        b[20 + 50 * i, 20 + 100 * i, 2] += 1 + i
    reg, total = rg.regions(rg.diff_map(a, b))
    assert total == 6 and len(reg) == rg.MAX_REGIONS, f"6 found, {rg.MAX_REGIONS} kept (got {total}, {len(reg)})"
    assert [r["max_abs"] for r in reg] == [6, 5, 4, 3], f"ties on px broken by max_abs: {[r['max_abs'] for r in reg]}"


def t4_regions_join_touching_cells_and_clip_to_the_image():
    a = base()
    b = a.copy()
    b[340:360, 620:640, 0] += 2              # the bottom-right corner; the last cell row is 8 px tall
    reg, total = rg.regions(rg.diff_map(a, b))
    assert total == 1, f"one region (got {total})"
    r = reg[0]
    assert (r["x"], r["y"], r["x"] + r["w"], r["y"] + r["h"]) == (608, 336, 640, 360), f"clipped to the image: {r}"
    assert r["px"] == 400 and r["max_abs"] == 2, f"its pixels: {r}"


def t4_heatmap_is_black_where_equal_and_white_at_the_peak():
    a, b = two_blobs()
    heat = rg.heatmap(rg.diff_map(a, b))
    assert heat.shape == (H, W, 3) and heat.dtype == np.uint8, "an RGB image at the compare resolution"
    assert not heat[0:40, 0:90].any(), "black where the images agree"
    assert tuple(heat[55, 110]) == (255, 255, 255), f"white at the largest difference (got {tuple(heat[55, 110])})"
    small = tuple(int(c) for c in heat[300, 600])
    assert small[0] >= 191, f"the smallest difference is plainly visible: red at least 191 (got {small})"
    assert sum(small) < 3 * 255, f"and dimmer than the peak (got {small})"


def t4_crop_shows_expected_actual_and_heat_side_by_side():
    a, b = two_blobs()
    d = rg.diff_map(a, b)
    reg, _ = rg.regions(d)
    strip = rg.crop_strip(a, b, d, reg[0])
    # region 48x16 padded by 8 on each side: 64x32; three panels and two 2-px separators; x4.
    assert strip.shape == (32 * rg.CROP_SCALE, (64 * 3 + 4) * rg.CROP_SCALE, 3), f"shape {strip.shape}"
    left = strip[:, :64 * rg.CROP_SCALE]
    assert np.array_equal(left[::rg.CROP_SCALE, ::rg.CROP_SCALE], a[40:72, 88:152].astype(np.uint8)), \
        "the left panel is the expected image"


def t4_a_large_region_is_cropped_around_its_worst_pixel():
    a = base()
    b = a.copy()
    b[::7, ::5, 0] += 1                       # scattered noise over the whole image: one large region
    b[200, 300, 1] += 9                       # its worst pixel
    d = rg.diff_map(a, b)
    reg, total = rg.regions(d)
    assert total == 1 and reg[0]["w"] == W, f"the noise is one region over the whole width (got {reg})"
    x0, y0, x1, y1 = rg.crop_window(d, reg[0])
    assert (x1 - x0, y1 - y0) == (rg.CROP_MAX_W, rg.CROP_MAX_H), f"a bounded window: {(x1 - x0, y1 - y0)}"
    assert x0 <= 300 < x1 and y0 <= 200 < y1, f"around the worst pixel: {(x0, y0, x1, y1)}"
    strip = rg.crop_strip(a, b, d, reg[0])
    assert strip.shape == (rg.CROP_MAX_H * rg.CROP_SCALE, (rg.CROP_MAX_W * 3 + 4) * rg.CROP_SCALE, 3), f"{strip.shape}"


def write_png(path, img):
    Image.fromarray(img.astype(np.uint8)).save(path)


def harness(tmp, golden_img, render_img, extra):
    """Runs render_golden.py on a one-scene manifest whose command copies render_img."""
    golden = os.path.join(tmp, "g.png")
    rendered = os.path.join(tmp, "r.png")
    write_png(golden, golden_img)
    write_png(rendered, render_img)
    copier = os.path.join(tmp, "copy.py")
    with open(copier, "w") as f:
        f.write("import shutil, sys\nshutil.copyfile(sys.argv[1], sys.argv[2])\n")
    manifest = os.path.join(tmp, "manifest.json")
    cmd = f'"{sys.executable}" "{copier}" "{rendered}" {{out}}'.replace("\\", "/")
    with open(manifest, "w") as f:
        json.dump({"scenes": [{"name": "fake", "command": cmd, "golden": golden.replace("\\", "/"),
                               "max_abs_diff": 4, "max_diff_frac": 0.01}]}, f)
    out_json = os.path.join(tmp, "golden.json")
    diff_dir = os.path.join(tmp, "diff")
    r = subprocess.run([sys.executable, os.path.join(HERE, "render_golden.py"), manifest, "--json", out_json,
                        "--diff-dir", diff_dir, *extra], capture_output=True, text=True, cwd=ROOT)
    doc = json.load(open(out_json, encoding="utf-8")) if os.path.exists(out_json) else None
    return r, doc, diff_dir


def t4_a_failing_scene_reports_regions_and_writes_its_diff_images():
    a, b = two_blobs()
    with tempfile.TemporaryDirectory() as tmp:
        r, doc, diff_dir = harness(tmp, a, b, [])
        assert r.returncode == 1, f"exit 1 for a failing scene (got {r.returncode}): {r.stderr[-300:]}"
        scene = doc["scenes"][0]
        assert scene["pass"] is False and scene["max_abs"] == 5 and scene["diff_px"] == 301, f"stats {scene}"
        assert scene["tolerance"] == {"max_abs": 4, "diff_frac": 0.01}, "the tolerance it was held to"
        assert len(scene["regions"]) == 2 and scene["regions"][0]["px"] == 300, f"regions {scene['regions']}"
        assert os.path.isfile(scene["heatmap"]) and os.path.isfile(scene["crop"]), "heatmap and crop written"
        heat = np.asarray(Image.open(scene["heatmap"]))
        assert heat.shape == (H, W, 3) and tuple(heat[55, 110]) == (255, 255, 255), "the heatmap file is the map"
        summary = json.loads(r.stdout.strip().splitlines()[-1])
        assert summary == {"suite": "render_golden", "passed": 0, "failed": 1, "failures": ["fake"]}, f"{summary}"
        drift = os.path.join(tmp, "g.actual.png")
        assert os.path.isfile(drift), "the failing render is still written beside the golden"


def t4_a_passing_scene_writes_no_diff_images_unless_asked():
    a = base()
    b = a.copy()
    b[10, 10, 0] += 1                         # within tolerance
    with tempfile.TemporaryDirectory() as tmp:
        r, doc, diff_dir = harness(tmp, a, b, [])
        scene = doc["scenes"][0]
        assert r.returncode == 0 and scene["pass"] is True, f"passes (exit {r.returncode})"
        assert "heatmap" not in scene and not os.path.exists(diff_dir), "no diff images for a pass"
        assert scene["regions"] == [{"x": 0, "y": 0, "w": 16, "h": 16, "max_abs": 1, "px": 1}], "regions still listed"
    with tempfile.TemporaryDirectory() as tmp:
        r, doc, diff_dir = harness(tmp, a, b, ["--heatmaps", "all"])
        assert os.path.isfile(doc["scenes"][0]["heatmap"]), "--heatmaps all writes them for a pass too"


def t4_selftest_reports_the_run_to_run_floor():
    a, b = two_blobs()
    with tempfile.TemporaryDirectory() as tmp:
        r, doc, _ = harness(tmp, a, b, ["--selftest"])
        scene = doc["scenes"][0]
        assert doc["mode"] == "selftest" and scene["mode"] == "selftest", "selftest mode"
        assert scene["max_abs"] == 0 and scene["diff_px"] == 0 and scene["pass"] is True, \
            f"the same render twice: floor 0 (got {scene})"


def main():
    print("test_golden_diff (render_golden.py)")
    for fn in (t4_identical_images_have_no_difference, t4_statistics_of_a_known_difference,
               t4_regions_are_the_differing_cells_largest_first, t4_regions_are_capped_and_counted,
               t4_regions_join_touching_cells_and_clip_to_the_image,
               t4_heatmap_is_black_where_equal_and_white_at_the_peak,
               t4_crop_shows_expected_actual_and_heat_side_by_side,
               t4_a_large_region_is_cropped_around_its_worst_pixel,
               t4_a_failing_scene_reports_regions_and_writes_its_diff_images,
               t4_a_passing_scene_writes_no_diff_images_unless_asked, t4_selftest_reports_the_run_to_run_floor):
        run(fn)
    print(f"Results: {passed}/{passed + failed} passed")
    print(json.dumps({"suite": "test_golden_diff", "passed": passed, "failed": failed, "failures": failures},
                     separators=(",", ":")))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
