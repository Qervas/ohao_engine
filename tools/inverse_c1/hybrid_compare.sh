#!/usr/bin/env bash
# Compare baseline FD vs NN-prior FD on the same scene (color recovery).
#
# Usage:
#   ./tools/inverse_c1/hybrid_compare.sh --preset lantern --quality draft
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${ROOT}/build/inverse_fit"
MODEL="${ROOT}/tools/inverse_c1/checkpoints/L2/best.pt"
PRESET="lantern"
QUALITY="draft"
OUT="${ROOT}/renders/inverse_c1_compare"
ITERS=36

while [[ $# -gt 0 ]]; do
  case "$1" in
    --model) MODEL="$2"; shift 2 ;;
    --preset) PRESET="$2"; shift 2 ;;
    --quality) QUALITY="$2"; shift 2 ;;
    --out-dir) OUT="$2"; shift 2 ;;
    --iters) ITERS="$2"; shift 2 ;;
    *) shift ;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "Missing $BIN"; exit 1
fi
if [[ ! -f "$MODEL" ]]; then
  echo "Missing $MODEL — train L2 first"; exit 1
fi

BASE="$OUT/${PRESET}_baseline"
NN="$OUT/${PRESET}_nn"
mkdir -p "$BASE" "$NN"

echo "═══ 1) Baseline FD (wrong init, no multi-start) ═══"
"$BIN" --selftest --preset "$PRESET" --quality "$QUALITY" --iters "$ITERS" \
  --no-multi-start --out-dir "$BASE" 2>&1 | tee "$BASE/run.log" | grep -E 'truth|recovered θ|primary \|Δ|SHOW RMSE|SELFTEST|param RMSE' || true

# Prefer FIT target for NN (matches training domain)
IMG=""
for c in "$BASE/target_fit.png" "$BASE/target_show.png" "$BASE/target_front.png"; do
  [[ -f "$c" ]] && IMG="$c" && break
done
if [[ -z "$IMG" ]]; then
  echo "No target image in $BASE"; exit 1
fi

echo "═══ 2) NN prior on $IMG ═══"
python3 "$ROOT/tools/inverse_c1/infer.py" --model "$MODEL" --image "$IMG" --out "$NN/theta_prior.json" \
  2>&1 | tee "$NN/infer.log"

echo "═══ 3) FD refine from NN prior ═══"
"$BIN" --selftest --preset "$PRESET" --quality "$QUALITY" --iters "$ITERS" \
  --theta-init "$NN/theta_prior.json" --no-multi-start --out-dir "$NN" 2>&1 | tee "$NN/run.log" \
  | grep -E 'C1 NN|truth|recovered θ|primary \|Δ|SHOW RMSE|SELFTEST|param RMSE' || true

echo "═══ Compare ═══"
python3 - "$BASE/run.log" "$NN/run.log" <<'PY'
import re, sys
def parse(path):
    t = open(path, errors="ignore").read()
    def g(pat, default=None):
        m = re.search(pat, t)
        return m.group(1) if m else default
    rgb = re.search(r'primary \|Δ\| RGB=\(([^)]+)\)', t)
    rough = re.search(r'rough=([0-9.eE+-]+)', t)
    metal = re.search(r'metal=([0-9.eE+-]+)', t)
    show = re.search(r'SHOW RMSE \(primary\) = ([0-9.eE+-]+)', t)
    prmse = re.search(r'param RMSE = ([0-9.eE+-]+)', t)
    st = 'PASS' if 'SELFTEST PASS' in t else ('FAIL' if 'SELFTEST FAIL' in t else '?')
    return {
        'rgb': rgb.group(1) if rgb else '?',
        'rough': rough.group(1) if rough else '?',
        'metal': metal.group(1) if metal else '?',
        'show': show.group(1) if show else '?',
        'prmse': prmse.group(1) if prmse else '?',
        'selftest': st,
    }
b, n = parse(sys.argv[1]), parse(sys.argv[2])
print(f"{'metric':12s} {'baseline FD':22s} {'NN→FD':22s}")
for k in ('rgb','rough','metal','show','prmse','selftest'):
    print(f"{k:12s} {b[k]:22s} {n[k]:22s}")
PY
echo "Outputs: $BASE  vs  $NN"
