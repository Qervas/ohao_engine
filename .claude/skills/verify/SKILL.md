---
name: verify
description: Run OHAO's gates (validation, the renderer against closed forms, the goldens, the replay test) and judge pass/fail. Use before every commit that changes code, and whenever a change touches shared code.
---

# Verify OHAO

The gates serve five requirements (AGENTS.md, "Gates"): never hurt the machine, the renderer right
against closed forms, a handful of goldens, gameplay replays exactly, it builds. One command:

```
python tools/check.py --changed      # while iterating: only what the change reaches
python tools/check.py                # to close a step: build, ohao doctor, quick + gpu (~2 minutes)
python tools/check.py --all          # adds the goldens and the slow caustics checks
```

Exit 0 = pass, 1 = a test failed or the kernel logged a GPU fault during the run (`gpuFaults` in
`build/check/check.json`), 2 = the environment is wrong. A failing test's entry carries the last 40
lines of its output.

## Judging a failure

- **A golden** fails: look at `build/golden_diff/<name>.heat.png` and `<name>.crop.png` before
  anything else. Never widen a tolerance; `--update` only after looking.
- **Validation** reports messages: `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` shows every report;
  `OHAO_VALIDATION=sync` for a missing barrier.
- **A GPU fault (Xid)** in `gpuFaults`: the GPU, not a test, failed. Read `journalctl -k`; after a
  firmware fault (Xid 119/120/62) the laptop needs a full power-off, not a warm reboot.
- **A test hangs** past its time limit: CTest kills it. Never loop a window-opening program on the
  desktop to chase it; reproduce headless.
- **Device lost (-4) everywhere** after a crash: `ohao doctor` says whether the GPU is wedged.
