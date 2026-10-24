"""Waits until the machine is quiet enough for a clean bench run (sub-project 2b spec,
"Machine quiet"): every other process together under half a core, the benches' own noisy
threshold, for a whole minute (6 consecutive 10 s samples).

    python tools/bench/wait_quiet.py [--deadline 7200] [--threshold 50] [--samples 6] [--interval 10]

Prints one line per sample (total, the busiest processes) and, last, one JSON line:
{"quiet": bool, "waitedS": s, "samples": n, "lastTotalCorePct": pct, "busy": [[name, pct], ...]}.
Exit 0 when quiet, 1 at the deadline, 2 when psutil is missing. Run it before a bench whose
verdict needs a clean run, and record the load it saw beside the bench's result.
"""
import argparse
import json
import os
import sys
import time

IGNORE = {"system idle process", "idle"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--deadline", type=float, default=7200, help="give up after this many seconds")
    ap.add_argument("--threshold", type=float, default=50, help="percent of one core, all other processes together")
    ap.add_argument("--samples", type=int, default=6, help="consecutive quiet samples needed")
    ap.add_argument("--interval", type=float, default=10, help="seconds per sample")
    a = ap.parse_args()
    try:
        import psutil
    except ImportError:
        print("wait_quiet: needs psutil (pip install psutil)", file=sys.stderr)
        return 2

    me = os.getpid()
    start = time.time()
    quiet = 0
    procs = {}
    total, top = 0.0, []
    while time.time() - start < a.deadline:
        for p in psutil.process_iter(["pid", "name"]):
            if p.info["pid"] not in procs:
                procs[p.info["pid"]] = p
                try:
                    p.cpu_percent(None)   # the first call starts the process's interval
                except psutil.Error:
                    pass
        time.sleep(a.interval)
        total, top = 0.0, []
        for pid, p in list(procs.items()):
            try:
                if pid == me or (p.name() or "").lower() in IGNORE:
                    continue
                c = p.cpu_percent(None)
            except psutil.Error:
                procs.pop(pid, None)
                continue
            total += c
            if c >= 5:
                top.append((c, p.name()))
        top.sort(reverse=True)
        quiet = quiet + 1 if total < a.threshold else 0
        print("%s total %.0f%% quiet %d/%d %s" % (time.strftime("%H:%M:%S"), total, quiet, a.samples,
                                                ", ".join("%s %.0f" % (n, c) for c, n in top[:5])), flush=True)
        if quiet >= a.samples:
            break
    ok = quiet >= a.samples
    print(json.dumps({"quiet": ok, "waitedS": round(time.time() - start), "samples": quiet,
                      "lastTotalCorePct": round(total, 1), "busy": [[n, round(c, 1)] for c, n in top[:8]]}))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
