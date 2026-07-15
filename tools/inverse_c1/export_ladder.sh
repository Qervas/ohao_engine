#!/usr/bin/env bash
# Export generalization ladder datasets (L0 / L1 / L2 / L2+L4-lite).
# Same 12D studio θ layout everywhere → one unified trainer.
#
# Usage:
#   ./tools/inverse_c1/export_ladder.sh [N_per_preset] [out_root]
# Env:
#   QUALITY=draft|high   (default draft)
#   LEVELS="L0 L1 L2 L2e"  which levels to export
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
N="${1:-80}"
OUT="${2:-$ROOT/renders/inverse_c1_ladder}"
BIN="${ROOT}/build/inverse_fit"
QUALITY="${QUALITY:-draft}"
LEVELS="${LEVELS:-L0 L1 L2 L2e}"
PRESETS=(lantern outdoor mirror helmet spheres)

if [[ ! -x "$BIN" ]]; then
  echo "Build inverse_fit first: cmake --build build --target inverse_fit -j"
  exit 1
fi

merge_into() {
  local src="$1"
  local merged="$2"
  local start_i="$3"
  local i="$start_i"
  mkdir -p "$merged"
  if [[ ! -f "$merged/config.json" && -f "$src/config.json" ]]; then
    cp -f "$src/config.json" "$merged/config.json"
  fi
  if [[ ! -f "$merged/meta.jsonl" ]]; then
    head -n 1 "$src/meta.jsonl" > "$merged/meta.jsonl" || true
  fi
  while IFS= read -r line; do
    [[ "$line" == *'"type":"header"'* ]] && continue
    file=$(echo "$line" | sed -n 's/.*"file":"\([^"]*\)".*/\1/p')
    [[ -z "$file" || ! -f "$src/$file" ]] && continue
    new=$(printf '%05d.png' "$i")
    cp -f "$src/$file" "$merged/$new"
    echo "$line" | sed "s/\"file\":\"$file\"/\"file\":\"$new\"/; s/\"i\":[0-9]*/\"i\":$i/" >> "$merged/meta.jsonl"
    i=$((i + 1))
  done < "$src/meta.jsonl"
  echo "$i"
}

export_one() {
  local tag="$1"; shift
  local dest="$OUT/$tag"
  rm -rf "$dest"
  mkdir -p "$dest"
  echo "════ Export $tag  N=$N/preset  quality=$QUALITY  args: $* ════"
  local merged="$dest/dataset"
  mkdir -p "$merged"
  local global=0
  local first=1
  for p in "${PRESETS[@]}"; do
    # L0 is lantern-only
    if [[ "$tag" == "L0" && "$p" != "lantern" ]]; then
      continue
    fi
    local tmp="$dest/_tmp_$p"
    rm -rf "$tmp"
    echo "── $tag preset=$p ──"
    if ! "$BIN" --export-dataset "$N" --preset "$p" --quality "$QUALITY" --views 1 \
        --seed "$((2000 + global * 17))" --out-dir "$tmp" "$@"; then
      echo "WARN: export failed for $p"
      continue
    fi
    local src="$tmp/dataset"
    [[ -f "$src/meta.jsonl" ]] || continue
    if [[ $first -eq 1 ]]; then
      cp -f "$src/config.json" "$merged/config.json" 2>/dev/null || true
      # stamp level into config
      if [[ -f "$merged/config.json" ]]; then
        python3 - "$merged/config.json" "$tag" <<'PY'
import json, sys
p, level = sys.argv[1], sys.argv[2]
with open(p) as f: c = json.load(f)
c["level"] = level
c["unified_theta"] = True
c["version"] = max(2, int(c.get("version", 1)))
with open(p, "w") as f: json.dump(c, f, indent=2)
PY
      fi
      head -n 1 "$src/meta.jsonl" > "$merged/meta.jsonl"
      first=0
    fi
    global=$(merge_into "$src" "$merged" "$global")
    rm -rf "$tmp"
  done
  local count
  count=$(grep -c '"theta"' "$merged/meta.jsonl" || echo 0)
  echo "→ $tag done: $count samples in $merged"
}

mkdir -p "$OUT"
for lv in $LEVELS; do
  case "$lv" in
    L0)
      # Single preset, fixed camera/lights (baseline)
      PRESETS=(lantern)
      export_one L0
      PRESETS=(lantern outdoor mirror helmet spheres)
      ;;
    L1)
      # Multi-preset, fixed geometry (unified 12D studio)
      PRESETS=(lantern outdoor mirror helmet spheres)
      export_one L1
      ;;
    L2)
      # Multi-preset + domain randomization (cam/lights/hero)
      PRESETS=(lantern outdoor mirror helmet spheres)
      export_one L2 --domain-rand --export-views random
      ;;
    L2e|L2E|L4|L2L4)
      # L2 + exposure jitter (photo-ish domain gap lite)
      PRESETS=(lantern outdoor mirror helmet spheres)
      export_one L2e --domain-rand --export-views random --export-exposure-jitter
      ;;
    *)
      echo "Unknown level $lv (use L0 L1 L2 L2e)"
      ;;
  esac
done

echo ""
echo "Ladder export complete → $OUT"
echo "Train example:"
echo "  python3 tools/inverse_c1/train.py --data $OUT/L2/dataset --out tools/inverse_c1/checkpoints/L2 --epochs 80"
echo "Eval:"
echo "  python3 tools/inverse_c1/eval.py --model tools/inverse_c1/checkpoints/L2/best.pt --data $OUT/L2/dataset"
