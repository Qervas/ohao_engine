#!/usr/bin/env python3
"""
check-validation -- run real scenes under VK_LAYER_KHRONOS_validation and fail on
any validation message (warning or error).

Unit tests exercise passes in isolation; this exercises whole frames the way the
engine records them -- scene upload, G-buffer, CSM, RT shadows, RT GI, lighting,
post -- which is where object-lifetime bugs live. One object destroyed while a
command buffer still references it invalidates the whole buffer and the layer
then reports every later command, so the FIRST message is the one to read; this
script prints it.

The engine installs its own debug messenger (OHAO_VALIDATION=1), which replaces
the layer's default printing. Every message arrives on stderr as
    [validation] <VUID> | <message>
and the engine confirms the layer loaded with "Validation layers ENABLED". A run
without that confirmation did not check anything and is reported as such.

Usage (from repo root):
  python tests/validation/check_validation.py        # all scenes
  python tests/validation/check_validation.py -v     # also list every distinct VUID

Exit 0 when every scene is clean, 1 otherwise, 2 if validation did not run.
"""
import os
import re
import subprocess
import sys
import tempfile

SCENES = [
    ("cornell_box deferred",
     ["build/Release/cornell_box.exe", "{out}", "4", "deferred", "--denoise=none"]),
    ("damaged_helmet deferred",
     ["build/Release/model_viewer.exe", "assets/test_models/DamagedHelmet.glb", "{out}", "4",
      "deferred", "--denoise=none"]),
]

MSG = re.compile(r"^\[validation\] ([^|]*?) \| (.*)$", re.M)


def run(name, argv, verbose):
    with tempfile.TemporaryDirectory() as tmp:
        argv = [a.replace("{out}", os.path.join(tmp, "out.png")) for a in argv]
        argv[0] = os.path.abspath(os.path.normpath(argv[0]))   # CreateProcess wants a real path
        env = dict(os.environ, OHAO_VALIDATION="1")
        r = subprocess.run(argv, env=env, capture_output=True, text=True, errors="replace")
    log = r.stdout + r.stderr
    if "Validation layers ENABLED" not in log:
        return None, f"{name}: validation layer did not load -- check NOT run"
    msgs = MSG.findall(log)
    if r.returncode != 0:
        return False, f"{name}: exited {r.returncode} ({len(msgs)} validation messages)"
    if not msgs:
        return True, f"{name}: clean"
    vuid, text = msgs[0]
    out = (f"{name}: {len(msgs)} validation messages, {len({v for v, _ in msgs})} distinct\n"
           f"      first: {vuid}\n             {text.strip()[:260]}")
    if verbose:
        counts = {}
        for v, _ in msgs:
            counts[v] = counts.get(v, 0) + 1
        out += "\n" + "\n".join(f"      {n:4d}  {v}" for v, n in sorted(counts.items(), key=lambda kv: -kv[1]))
    return False, out


def main():
    verbose = "-v" in sys.argv
    results = [run(n, a, verbose) for n, a in SCENES]
    for ok, msg in results:
        tag = "PASS" if ok else ("SKIP" if ok is None else "FAIL")
        print(f"[{tag}]  {msg}")
    if any(ok is None for ok, _ in results):
        return 2
    return 0 if all(ok for ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
