#!/usr/bin/env bash
# Full generalization ladder: export → train → eval for each level.
# Usage:
#   ./tools/inverse_c1/run_ladder.sh [N] [out_root]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
N="${1:-64}"
OUT="${2:-$ROOT/renders/inverse_c1_ladder}"
CKPT="${ROOT}/tools/inverse_c1/checkpoints"
QUALITY="${QUALITY:-draft}"
EPOCHS="${EPOCHS:-50}"

export QUALITY
chmod +x "$ROOT/tools/inverse_c1/export_ladder.sh" || true

echo "═══ 1) Export ladder (N=$N) ═══"
"$ROOT/tools/inverse_c1/export_ladder.sh" "$N" "$OUT"

echo "═══ 2) Train + eval each level ═══"
SUMMARY="$OUT/ladder_summary.jsonl"
: > "$SUMMARY"

for lv in L0 L1 L2 L2e; do
  data="$OUT/$lv/dataset"
  [[ -f "$data/meta.jsonl" ]] || continue
  count=$(grep -c '"theta"' "$data/meta.jsonl" || echo 0)
  echo "── Train $lv ($count samples) ──"
  out_dir="$CKPT/$lv"
  python3 "$ROOT/tools/inverse_c1/train.py" \
    --data "$data" --out "$out_dir" --epochs "$EPOCHS" --batch 32 --lr 1.2e-3 \
    | tee "$out_dir/train.log"
  echo "── Eval $lv ──"
  set +e
  python3 "$ROOT/tools/inverse_c1/eval.py" \
    --model "$out_dir/best.pt" --data "$data" --out "$out_dir/eval_metrics.json"
  ec=$?
  set -e
  if [[ -f "$out_dir/eval_metrics.json" ]]; then
    python3 - "$out_dir/eval_metrics.json" "$lv" "$SUMMARY" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
m["level"] = sys.argv[2]
with open(sys.argv[3], "a") as f:
    f.write(json.dumps(m) + "\n")
print(f"  SUMMARY {sys.argv[2]}: RGB_MAE={m['rgb_mae']:.4f} RMSE={m['param_rmse']:.4f}")
PY
  fi
done

echo "═══ Ladder complete ═══"
echo "Summary: $SUMMARY"
if [[ -f "$SUMMARY" ]]; then
  python3 - "$SUMMARY" <<'PY'
import json, sys
print(f"{'level':6s} {'n':>5s} {'RGB_MAE':>8s} {'RMSE':>8s} {'gates':>10s}")
for line in open(sys.argv[1]):
    m = json.loads(line)
    g = m.get("gates", {})
    flag = "PASS" if g.get("rgb_ok") and g.get("rmse_ok") else "WEAK"
    print(f"{m.get('level','?'):6s} {m.get('samples',0):5d} {m.get('rgb_mae',0):8.4f} {m.get('param_rmse',0):8.4f} {flag:>10s}")
PY
fi
