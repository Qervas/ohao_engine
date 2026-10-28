"""The test grenade placed about a fixed listener, rendered and measured (audio ruling 23; Frank's
hand test of 2026-10-28 asked for the space to be sampled, not only listened to).

    python tools/audio/grenade_spatial.py [--out DIR] [--exe build/Release/audio_bench.exe] [--no-run]

Runs `audio_bench --grenade-spatial DIR`: the play scene's player stands outdoors north of the room
facing north, the ambience bus muted, and the grenade goes off 4 m and 20 m to its left (west) and to
its right (east) and 4 m in front, each in its own 3 s window, through the schedule and the window's
AudioPlayer on a mixer without a device. Then measures every window of grenade_spatial.wav:

    L RMS, R RMS        each channel's RMS over the window (linear, full scale 1)
    ILD dB              20 log10(L RMS / R RMS): positive is louder on the left
    peak                the channel holding the window's largest sample
    overall dBFS        RMS of both channels together
    azimuth guess       left/right only (front and back alike): a constant-power pan inverted,
                        pan = atan2(R, L) / 45 deg - 1, azimuth = asin(pan); negative is left

and judges, PASS or FAIL each:

    LEFT windows        ILD >= +6 dB
    RIGHT windows       ILD <= -6 dB
    far against near    overall dBFS at 20 m at least 6 dB below 4 m, same side
    FRONT window        |ILD| <= 2 dB
    clean windows       no other sound starts inside a boom's window

Writes grenade_spatial.csv and grenade_spatial.json beside the WAV. DIR defaults to
C:/Users/djmax/ohao_refs/listen/sp11_audio (local, never committed); CTest runs it into
build/grenade_spatial. Exit 0 when every check passes, 1 otherwise; the last line is the harness's
{"suite", "passed", "failed", "failures"}.
"""
import argparse
import csv
import json
import math
import os
import struct
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_OUT = "C:/Users/djmax/ohao_refs/listen/sp11_audio"
LR_DB = 6.0       # a side window's ILD at least this far to its side
FAR_DB = 6.0      # 20 m at least this far below 4 m
FRONT_DB = 2.0    # the front window's |ILD| at most this


def read_wav(path):
    """A WAV of 32-bit float (format 3) or 16-bit PCM: (rate, samples [frames, channels])."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAV")
    pos, fmt, samples = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif cid == b"data":
            samples = body
        pos += 8 + size + (size & 1)
    if fmt is None or samples is None:
        raise ValueError(f"{path}: no fmt or data chunk")
    tag, channels, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(samples, dtype="<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(samples, dtype="<i2").astype(np.float64) / 32768.0
    else:
        raise ValueError(f"{path}: format {tag}/{bits} bits not read")
    return rate, x.reshape(-1, channels)


def db(x):
    return 20.0 * math.log10(max(x, 1e-12))


def measure(stereo, start, frames):
    w = stereo[start:start + frames]
    left, right = w[:, 0], w[:, 1]
    l_rms, r_rms = float(np.sqrt(np.mean(left ** 2))), float(np.sqrt(np.mean(right ** 2)))
    pan = math.atan2(r_rms, l_rms) / (math.pi / 4.0) - 1.0
    return {
        "l_rms": l_rms,
        "r_rms": r_rms,
        "ild_db": db(l_rms) - db(r_rms),
        "peak": "L" if np.max(np.abs(left)) >= np.max(np.abs(right)) else "R",
        "overall_dbfs": 10.0 * math.log10(max(float(np.mean(w ** 2)), 1e-24)),
        "azimuth_guess_deg": math.degrees(math.asin(max(-1.0, min(1.0, pan)))),
    }


def main():
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n")[0])
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "Release", "audio_bench.exe"))
    ap.add_argument("--no-run", action="store_true", help="measure the WAV already in --out")
    args = ap.parse_args()
    out = os.path.abspath(args.out)

    if not args.no_run:
        r = subprocess.run([args.exe, "--grenade-spatial", out], cwd=ROOT, capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        if r.returncode != 0:
            sys.stdout.write(r.stderr)
            print(json.dumps({"suite": "grenade_spatial", "passed": 0, "failed": 1,
                              "failures": [f"audio_bench --grenade-spatial exited {r.returncode}"]}))
            return 1

    rate, stereo = read_wav(os.path.join(out, "grenade_spatial.wav"))
    with open(os.path.join(out, "grenade_spatial_schedule.json"), encoding="utf-8") as f:
        sched = json.load(f)
    step_frames = sched["framesPerStep"]

    rows = []
    for b in sched["booms"]:
        m = measure(stereo, b["startFrame"], b["frames"])
        first, last = b["runStep"], b["runStep"] + b["frames"] // step_frames
        others = [c["sound"] for c in sched["commands"]
                  if c.get("sound") and c["sound"] != "explode" and first <= c["runStep"] < last]
        rows.append({"label": b["label"], "plannedAzimuthDeg": b["plannedAzimuthDeg"],
                     "plannedDistance": b["plannedDistance"], "azimuthDeg": b.get("azimuthDeg"),
                     "distance": b.get("distance"), "walls": b["walls"], "runStep": b["runStep"],
                     "otherSounds": others, **m})

    checks = []

    def check(name, ok, detail):
        checks.append({"check": name, "pass": bool(ok), "detail": detail})

    by = {r["label"]: r for r in rows}
    for r in rows:
        side = r["label"].split("-")[0]
        if side == "LEFT":
            check(f"{r['label']} ILD >= +{LR_DB:g} dB", r["ild_db"] >= LR_DB, f"{r['ild_db']:+.2f} dB")
        elif side == "RIGHT":
            check(f"{r['label']} ILD <= -{LR_DB:g} dB", r["ild_db"] <= -LR_DB, f"{r['ild_db']:+.2f} dB")
        elif side == "FRONT":
            check(f"{r['label']} |ILD| <= {FRONT_DB:g} dB", abs(r["ild_db"]) <= FRONT_DB, f"{r['ild_db']:+.2f} dB")
        check(f"{r['label']} window holds only its boom", not r["otherSounds"],
              ", ".join(r["otherSounds"]) or "nothing else")
    for side in ("LEFT", "RIGHT"):
        near, far = by.get(f"{side}-near"), by.get(f"{side}-far")
        if near and far:
            drop = near["overall_dbfs"] - far["overall_dbfs"]
            check(f"{side} far at least {FAR_DB:g} dB below near", drop >= FAR_DB, f"{drop:.2f} dB down")

    fields = ["label", "plannedAzimuthDeg", "plannedDistance", "azimuthDeg", "distance", "walls", "runStep",
              "l_rms", "r_rms", "ild_db", "peak", "overall_dbfs", "azimuth_guess_deg"]
    with open(os.path.join(out, "grenade_spatial.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.writer(f)
        wr.writerow(fields)
        for r in rows:
            wr.writerow([f"{r[k]:.6g}" if isinstance(r[k], float) else r[k] for k in fields])
    failures = [c["check"] + ": " + c["detail"] for c in checks if not c["pass"]]
    with open(os.path.join(out, "grenade_spatial.json"), "w", encoding="utf-8") as f:
        json.dump({"rate": rate, "listen": sched["listen"], "facing": sched["facing"], "muted": sched["muted"],
                   "thresholds": {"sideIldDb": LR_DB, "farBelowNearDb": FAR_DB, "frontIldDb": FRONT_DB},
                   "windows": rows, "checks": checks, "pass": not failures}, f, indent=1)

    print(f"grenade spatial: listener at {sched['listen']} facing {sched['facing']}, muted {sched['muted']}")
    print(f"  {'window':<11} {'az':>6} {'dist':>6} {'L RMS':>9} {'R RMS':>9} {'ILD dB':>7} {'peak':>4} "
          f"{'dBFS':>7} {'az guess':>8}")
    for r in rows:
        print(f"  {r['label']:<11} {r['azimuthDeg']:>6.1f} {r['distance']:>6.2f} {r['l_rms']:>9.5f} {r['r_rms']:>9.5f} "
              f"{r['ild_db']:>+7.2f} {r['peak']:>4} {r['overall_dbfs']:>7.2f} {r['azimuth_guess_deg']:>+8.1f}")
    for c in checks:
        print(f"  {'PASS' if c['pass'] else 'FAIL'}  {c['check']}  ({c['detail']})")
    print(f"  wrote {os.path.join(out, 'grenade_spatial.csv')} and grenade_spatial.json")
    print(json.dumps({"suite": "grenade_spatial", "passed": len(checks) - len(failures), "failed": len(failures),
                      "failures": failures}))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
