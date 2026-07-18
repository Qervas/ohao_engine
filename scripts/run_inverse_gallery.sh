#!/usr/bin/env bash
# M3a multi-preset inverse gallery wall.
# Runs dense ORM (H2) across ≥3 studio presets, writes metrics + stills,
# builds a simple HTML gallery wall, and gates with test_gallery.py.
#
# Usage:
#   ./scripts/run_inverse_gallery.sh              # default: lantern helmet spheres @ --hd 720
#   ./scripts/run_inverse_gallery.sh --fast       # FIT 256×144, no SHOW upscale
#   PRESETS="lantern helmet spheres outdoor" ./scripts/run_inverse_gallery.sh
#   MODES="orm metal" ./scripts/run_inverse_gallery.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BIN:-./build/inverse_fit}"
OUT_ROOT="${OUT_ROOT:-renders/inverse_gallery}"
PRESETS="${PRESETS:-lantern helmet spheres}"
MODES="${MODES:-orm}"   # orm | metal | "orm metal"
HD="${HD:-720}"         # 720 | 1080 | none (fast)
FAST=0

for arg in "$@"; do
  case "$arg" in
    --fast) FAST=1; HD=none ;;
    --hd=*) HD="${arg#--hd=}" ;;
    --presets=*) PRESETS="${arg#--presets=}" ;;
    --modes=*) MODES="${arg#--modes=}" ;;
    -h|--help)
      sed -n '2,14p' "$0"
      exit 0
      ;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "Building inverse_fit..."
  cmake --build build --target inverse_fit -j"$(nproc)"
fi

mkdir -p "$OUT_ROOT"
SUMMARY_JSON="$OUT_ROOT/gallery_summary.json"
SUMMARY_MD="$OUT_ROOT/GALLERY.md"
WALL_HTML="$OUT_ROOT/gallery_wall.html"
: >"$OUT_ROOT/gallery_runs.tsv"
echo -e "preset\tmode\tout_dir\tpass\tnote" >"$OUT_ROOT/gallery_runs.tsv"

hd_args=()
if [[ "$HD" == "none" || "$FAST" -eq 1 ]]; then
  hd_args=(--fit-width 256 --fit-height 144 --show-width 256 --show-height 144)
  HD_LABEL="256x144"
else
  hd_args=(--hd "$HD")
  HD_LABEL="hd${HD}"
fi

run_one() {
  local preset="$1" mode="$2"
  local out="${OUT_ROOT}/${preset}_${mode}_${HD_LABEL}"
  mkdir -p "$out"
  local extra=()
  local gate=""
  case "$mode" in
    orm)
      extra=(--dense-orm --dense-map-res 64 --dense-grid 4)
      gate=tools/inverse_lab/test_dense_orm.py
      ;;
    metal)
      extra=(--dense-metal --dense-map-res 64 --dense-grid 2)
      gate=tools/inverse_lab/test_dense_metal.py
      ;;
    map)
      extra=(--dense-map --dense-map-res 64 --dense-grid 8)
      gate=tools/inverse_lab/test_dense_map.py
      ;;
    *)
      echo "unknown mode: $mode" >&2
      return 2
      ;;
  esac

  echo ""
  echo "=== gallery: preset=$preset mode=$mode out=$out ==="
  set +e
  "$BIN" --backend diff --preset "$preset" "${extra[@]}" "${hd_args[@]}" \
    --out-dir "$out" 2>&1 | tee "$out/run.log" | rg -n "Dense-|MAPTEST|A/B|FIT |SHOW |wrong-init|final loss|gates" || true
  local rc=${PIPESTATUS[0]}
  set -e

  local pass=0 note="binary_exit_$rc"
  if [[ $rc -eq 0 ]]; then
    if python3 "$gate" "$out" >"$out/gate.log" 2>&1; then
      pass=1
      note="gate_pass"
    else
      note="gate_fail"
      cat "$out/gate.log" || true
    fi
  else
    note="maptest_fail"
  fi
  echo -e "${preset}\t${mode}\t${out}\t${pass}\t${note}" >>"$OUT_ROOT/gallery_runs.tsv"
  echo "  -> pass=$pass ($note)"
  return 0
}

for mode in $MODES; do
  for preset in $PRESETS; do
    run_one "$preset" "$mode"
  done
done

python3 tools/inverse_lab/build_gallery_wall.py "$OUT_ROOT" \
  --summary-json "$SUMMARY_JSON" \
  --summary-md "$SUMMARY_MD" \
  --wall-html "$WALL_HTML"

echo ""
echo "=== gallery gate ==="
python3 tools/inverse_lab/test_gallery.py "$OUT_ROOT"
echo "GALLERY DONE  wall=$WALL_HTML  summary=$SUMMARY_MD"
