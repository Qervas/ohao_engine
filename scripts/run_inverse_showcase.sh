#!/usr/bin/env bash
# One-command inverse showcase: Diff-IR selftest + PT capture-gated lab plate.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BIN:-./build/inverse_fit}"
if [[ ! -x "$BIN" ]]; then
  cmake --build build --target inverse_fit -j"$(nproc)"
fi

echo "=== Diff-IR (DIFFTEST) ==="
"$BIN" --backend diff --preset lantern --quality draft \
  --show-width 320 --show-height 180 --iters 40 --map-res 2 \
  --out-dir renders/diff_demo

CAP=renders/inverse_lab/lantern_frontier/capture
if [[ ! -f "$CAP/capture.json" ]]; then
  echo "=== Export lab capture ==="
  "$BIN" --export-capture --preset lantern --quality draft --views 3 \
    --show-width 640 --show-height 360 --show-spp 128 --map-res 2 \
    --out-dir renders/inverse_lab/lantern_frontier
fi

echo "=== PT lab plate (LABTEST capture-gated) ==="
"$BIN" --backend pt --lab-bundle "$CAP" --preset lantern --quality draft \
  --show-width 640 --show-height 360 --show-spp 128 --fit-spp 64 \
  --iters 28 --multi-start 5 --visual-polish --polish-iters 6 \
  --out-dir renders/inverse_lab/lantern_frontier_fit

echo "=== metrics ==="
cat renders/inverse_lab/lantern_frontier_fit/lab_metrics.json
python3 tools/inverse_lab/test_metrics_and_maps.py renders/inverse_lab/lantern_frontier_fit
echo "SHOWCASE DONE"
