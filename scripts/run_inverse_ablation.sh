#!/usr/bin/env bash
# M3b ablation table: views / map-res / resolution / quality-plate on one hard preset.
# Baseline is quality-oriented (not the 256×144 toy path).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BIN:-./build/inverse_fit}"
OUT_ROOT="${OUT_ROOT:-renders/inverse_ablation}"
PRESET="${PRESET:-spheres}"

if [[ ! -x "$BIN" ]]; then
  cmake --build build --target inverse_fit -j"$(nproc)"
fi
mkdir -p "$OUT_ROOT"
echo -e "case\targs\tout_dir\tpass\tinit_psnr\ttrain_psnr\tmap_mse_rec\tshow_wh" >"$OUT_ROOT/ablation.tsv"

run_case() {
  local name="$1"; shift
  local args="$*"
  local out="${OUT_ROOT}/${PRESET}_${name}"
  mkdir -p "$out"
  echo ""
  echo "=== ablation: $name ==="
  set +e
  # shellcheck disable=SC2086
  "$BIN" --backend diff --dense-orm --preset "$PRESET" $args --out-dir "$out" \
    2>&1 | tee "$out/run.log" | rg -n "Dense-|MAPTEST|wrong-init|final loss|QUALITY|SHOW plate" || true
  local rc=${PIPESTATUS[0]}
  set -e
  local pass=0
  if [[ $rc -eq 0 ]] && python3 tools/inverse_lab/test_dense_orm.py "$out" >"$out/gate.log" 2>&1; then
    pass=1
  fi
  python3 -c "
import json
from pathlib import Path
p=Path('$out/dense_orm_metrics.json')
m=json.loads(p.read_text()) if p.is_file() else {}
line=f\"$name\t$args\t$out\t$pass\t{m.get('init_psnr','')}\t{m.get('train_psnr','')}\t{m.get('rough_mse_recovered','')}\t{m.get('show_wh','')}\"
print(line)
open('$OUT_ROOT/ablation.tsv','a').write(line+'\n')
"
}

# A0 baseline quality plate (publish bar)
run_case qplate --quality-plate --dense-map-res 128 --dense-grid 4
# A1 lower map res
run_case map64 --quality-plate --dense-map-res 64 --dense-grid 4
# A2 fewer views
run_case views1 --quality-plate --dense-map-res 128 --dense-grid 4 --dense-views 1
# A3 daily 720 (not full quality plate)
run_case hd720 --hd 720 --dense-map-res 64 --dense-grid 4
# A4 lab-fast toy (honest lower bar)
run_case lab_fast --fit-width 256 --fit-height 144 --show-width 256 --show-height 144 --dense-map-res 64 --dense-grid 4

python3 tools/inverse_lab/test_ablation.py "$OUT_ROOT"
echo "ABLATION DONE  table=$OUT_ROOT/ablation.tsv"
