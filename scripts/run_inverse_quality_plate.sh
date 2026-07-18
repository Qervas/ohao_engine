#!/usr/bin/env bash
# Publish-quality dense inverse plate (persuasion bar).
#
# dB gains only count for the product face when stills are clean 1080p,
# maps are dense, accumulation is high, and presets are hard (not toy-only).
#
# Usage:
#   ./scripts/run_inverse_quality_plate.sh
#   PRESETS="spheres outdoor helmet" ./scripts/run_inverse_quality_plate.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BIN:-./build/inverse_fit}"
OUT_ROOT="${OUT_ROOT:-renders/inverse_quality_plate}"
# Hard scenes: metal chart + outdoor HDRI + textured hero (not lantern-only).
PRESETS="${PRESETS:-spheres outdoor helmet}"
MODE="${MODE:-orm}"

if [[ ! -x "$BIN" ]]; then
  cmake --build build --target inverse_fit -j"$(nproc)"
fi
mkdir -p "$OUT_ROOT"
echo -e "preset\tmode\tout_dir\tpass\tquality_plate" >"$OUT_ROOT/quality_runs.tsv"

for preset in $PRESETS; do
  out="${OUT_ROOT}/${preset}_${MODE}_qplate"
  mkdir -p "$out"
  echo ""
  echo "=== QUALITY PLATE: preset=$preset mode=$MODE ==="
  extra=(--dense-orm --dense-map-res 128 --dense-grid 4)
  gate=tools/inverse_lab/test_dense_orm.py
  if [[ "$MODE" == "metal" ]]; then
    extra=(--dense-metal --dense-map-res 128 --dense-grid 2)
    gate=tools/inverse_lab/test_dense_metal.py
  fi
  set +e
  "$BIN" --backend diff --preset "$preset" "${extra[@]}" --quality-plate \
    --out-dir "$out" 2>&1 | tee "$out/run.log" | rg -n "Dense-|QUALITY|MAPTEST|A/B|wrong-init|final loss|SHOW plate|frames|preset=" || true
  rc=${PIPESTATUS[0]}
  set -e
  pass=0
  if [[ $rc -eq 0 ]] && python3 "$gate" "$out" >"$out/gate.log" 2>&1; then
    pass=1
  else
    cat "$out/gate.log" 2>/dev/null || true
  fi
  # quality_plate must be true in metrics
  qp=$(python3 -c "import json;print(json.load(open('${out}/dense_orm_metrics.json' if '${MODE}'=='orm' else '${out}/dense_metal_metrics.json')).get('quality_plate',False))" 2>/dev/null || echo false)
  echo -e "${preset}\t${MODE}\t${out}\t${pass}\t${qp}" >>"$OUT_ROOT/quality_runs.tsv"
  echo "  -> pass=$pass quality_plate=$qp"
done

python3 tools/inverse_lab/build_gallery_wall.py "$OUT_ROOT" \
  --summary-json "$OUT_ROOT/gallery_summary.json" \
  --summary-md "$OUT_ROOT/GALLERY.md" \
  --wall-html "$OUT_ROOT/gallery_wall.html" || true

python3 tools/inverse_lab/test_quality_plate.py "$OUT_ROOT"
echo "QUALITY PLATE DONE  wall=$OUT_ROOT/gallery_wall.html"
