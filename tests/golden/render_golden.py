#!/usr/bin/env python3
"""
render-golden — OHAO Engine golden-image regression harness (renovation Phase 0).

Renders each scene in a manifest with the existing engine, then compares the
output to a committed golden PNG using a TOLERANCE compare (not bit-exact hash).

Why tolerance, not hash: the offline path tracer is deterministic up to an
irreducible GPU floating-point floor (~6 pixels at 1 LSB on a 1920x1080 frame,
from non-associative reduction order). Bit-exact hashing would flag that ghost;
a tight tolerance absorbs it while still catching any real regression (which
shifts many pixels by many LSB). This is what PBRT/Mitsuba test suites do.

A scene PASSES when BOTH hold:
  - max per-channel abs diff <= max_abs_diff      (catches large-magnitude changes)
  - fraction of any-differing pixels <= max_diff_frac  (catches widespread tiny shifts)

Usage (run from repo root):
  python3 tests/golden/render_golden.py tests/golden/manifest.json            # check vs goldens
  python3 tests/golden/render_golden.py tests/golden/manifest.json --update   # (re)generate goldens
  python3 tests/golden/render_golden.py tests/golden/manifest.json --selftest # render 2x, compare to each other

Exit code: 0 if all scenes pass, 1 otherwise. Suitable for a git pre-push hook.
"""
import sys, os, json, shlex, subprocess, tempfile

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("render-golden needs numpy + Pillow:  pip install numpy Pillow")

DEFAULT_MAX_ABS_DIFF = 4       # per-channel LSB at compare-res; absorbs the FP floor with margin
DEFAULT_MAX_DIFF_FRAC = 0.01   # 1% of (downscaled) pixels; a real regression shifts far more
DOWNSCALE_WIDTH = 640          # compare at reduced res: averages the FP ghost away + tiny goldens


def downscale(im):
    w, h = im.size
    if w > DOWNSCALE_WIDTH:
        im = im.resize((DOWNSCALE_WIDTH, max(1, round(h * DOWNSCALE_WIDTH / w))), Image.BILINEAR)
    return im


# Manifests name the binary the way a single-config generator lays it out
# (./build/cornell_box). A multi-config generator (Visual Studio, Xcode) puts it
# under a per-configuration subdirectory with a platform suffix instead, so the
# literal path in the manifest does not exist and subprocess raises
# FileNotFoundError before any comparison happens. Resolving here keeps the
# manifest generator-agnostic rather than encoding one layout into the corpus.
_CONFIG_DIRS = ("", "Release", "RelWithDebInfo", "Debug")
_SUFFIXES = ("", ".exe")


def resolve_exe(argv0):
    if os.path.exists(argv0):
        return argv0
    head, tail = os.path.split(argv0)
    for cfg in _CONFIG_DIRS:
        for sfx in _SUFFIXES:
            cand = os.path.join(head, cfg, tail + sfx)
            if os.path.exists(cand):
                return cand
    return argv0          # let subprocess report it, with the manifest's own spelling


def render(command, out_path):
    # Split FIRST, substitute after. shlex.split defaults to POSIX mode, where a
    # backslash is an escape character: passing an already-substituted Windows
    # path through it silently turns
    #   C:\Users\me\Temp\x.png  ->  C:UsersmeTempx.png
    # and the renderer then writes that as a relative filename in the current
    # directory. The harness saw no file at the path it asked for and reported
    # "render produced no output", which reads like a renderer failure and is
    # not one -- it left two stray PNGs in the repository root instead. Keeping
    # the path out of shlex entirely is the fix.
    argv = [a.replace("{out}", out_path) for a in shlex.split(command)]
    cmd = " ".join(argv)
    argv[0] = resolve_exe(argv[0])
    try:
        r = subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as e:
        return f"could not launch {argv[0]!r}: {e}"
    if r.returncode != 0:
        return f"render command failed (exit {r.returncode}): {cmd}"
    if not os.path.exists(out_path):
        return f"render produced no output at {out_path}: {cmd}"
    return None


def load(path):
    return np.asarray(downscale(Image.open(path).convert("RGB")), dtype=np.int16)


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


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: render_golden.py <manifest.json> [--update|--selftest]")
    manifest_path = sys.argv[1]
    mode = sys.argv[2] if len(sys.argv) > 2 else "check"
    with open(manifest_path) as f:
        manifest = json.load(f)

    all_ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for sc in manifest["scenes"]:
            name = sc["name"]
            golden = sc["golden"]
            mad = sc.get("max_abs_diff", DEFAULT_MAX_ABS_DIFF)
            mdf = sc.get("max_diff_frac", DEFAULT_MAX_DIFF_FRAC)

            if mode == "--update":
                full = os.path.join(tmp, name + ".full.png")
                err = render(sc["command"], full)
                if err:
                    print(f"[update] {name}: FAILED — {err}")
                    all_ok = False
                    continue
                downscale(Image.open(full).convert("RGB")).save(golden)
                print(f"[update] {name}: golden written -> {golden}")
                continue

            actual = os.path.join(tmp, name + ".actual.png")
            err = render(sc["command"], actual)
            if err:
                print(f"[FAIL]  {name}: {err}")
                all_ok = False
                continue

            if mode == "--selftest":
                # render a second time, compare the two renders to each other
                actual2 = os.path.join(tmp, name + ".actual2.png")
                err2 = render(sc["command"], actual2)
                if err2:
                    print(f"[FAIL]  {name}: {err2}")
                    all_ok = False
                    continue
                s = diff_stats(load(actual), load(actual2))
                ok = verdict(s, mad, mdf)
                print(f"[{'PASS' if ok else 'FAIL'}]  {name} (selftest): "
                      f"max_abs={s['max_abs']} diff_px={s['diff_px']} "
                      f"frac={s['diff_frac']:.6f} rmse={s['rmse']:.4f}")
                all_ok = all_ok and ok
                continue

            # default: compare against committed golden
            if not os.path.exists(golden):
                print(f"[FAIL]  {name}: golden missing ({golden}) — run with --update first")
                all_ok = False
                continue
            s = diff_stats(load(actual), load(golden))
            if s is None:
                print(f"[FAIL]  {name}: size mismatch vs golden")
                all_ok = False
                continue
            ok = verdict(s, mad, mdf)
            print(f"[{'PASS' if ok else 'FAIL'}]  {name}: "
                  f"max_abs={s['max_abs']}(<= {mad}) "
                  f"diff_frac={s['diff_frac']:.6f}(<= {mdf}) "
                  f"diff_px={s['diff_px']} rmse={s['rmse']:.4f}")
            if not ok:
                drift = golden.replace(".png", ".actual.png")
                Image.open(actual).save(drift)
                print(f"        drift written -> {drift}")
            all_ok = all_ok and ok

    print("\nrender-golden:", "ALL PASS" if all_ok else "FAILURES — see above")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
