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

echo "═══ Compare sheets ═══"
python3 "$ROOT/tools/inverse_c1/make_compare.py" "$BASE" "$NN" 2>/dev/null || true

echo "═══ Compare metrics ═══"
python3 - "$BASE/run.log" "$NN/run.log" "$OUT/${PRESET}_summary.json" "$PRESET" <<'PY'
import re, sys, json
def parse(path):
    t = open(path, errors="ignore").read()
    # Prefer the diagnostics line (primary |Δ| RGB=(..) rough=.. metal=..)
    prim = re.search(
        r'primary \|Δ\| RGB=\(([^)]+)\)\s*rough=([0-9.eE+-]+)\s*metal=([0-9.eE+-]+)',
        t,
    )
    show = re.search(r'SHOW RMSE \(primary\) = ([0-9.eE+-]+)', t)
    prmse = re.search(r'param RMSE = ([0-9.eE+-]+)', t)
    st = 'PASS' if 'SELFTEST PASS' in t else ('FAIL' if 'SELFTEST FAIL' in t else '?')
    def f(m, g=1):
        return float(m.group(g)) if m else None
    rgb_t = None
    rgb_mean = None
    rough = metal = None
    if prim:
        rgb_t = [float(x) for x in prim.group(1).split(',')]
        rgb_mean = sum(rgb_t) / max(1, len(rgb_t))
        rough = float(prim.group(2))
        metal = float(prim.group(3))
    return {
        'rgb_delta': rgb_t,
        'rgb_mean_abs': rgb_mean,
        'rough_abs': rough,
        'metal_abs': metal,
        'show_rmse': f(show),
        'param_rmse': f(prmse),
        'selftest': st,
    }
b, n = parse(sys.argv[1]), parse(sys.argv[2])
summary = {'preset': sys.argv[4], 'baseline': b, 'nn_hybrid': n}
with open(sys.argv[3], 'w') as f:
    json.dump(summary, f, indent=2)
print(f"{'metric':14s} {'baseline FD':22s} {'NN→FD':22s}")
def fmt(v):
    if v is None: return '?'
    if isinstance(v, float): return f'{v:.4f}'
    if isinstance(v, list): return ','.join(f'{x:.3f}' for x in v)
    return str(v)
for k in ('rgb_delta','rgb_mean_abs','rough_abs','metal_abs','show_rmse','param_rmse','selftest'):
    print(f"{k:14s} {fmt(b.get(k)):22s} {fmt(n.get(k)):22s}")
print(f"wrote {sys.argv[3]}")
PY
echo "Outputs: $BASE  vs  $NN"
