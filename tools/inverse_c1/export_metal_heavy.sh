#!/usr/bin/env bash
# L2 metal-heavy export: more mirror/spheres samples for BRDF prior quality.
# Usage: ./tools/inverse_c1/export_metal_heavy.sh [out_root]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/renders/inverse_c1_metal}"
BIN="${ROOT}/build/inverse_fit"
QUALITY="${QUALITY:-draft}"
MERGED="$OUT/dataset"
mkdir -p "$MERGED"

if [[ ! -x "$BIN" ]]; then
  echo "Build inverse_fit first"; exit 1
fi

# preset → sample count (metal-heavy)
declare -A COUNTS=(
  [mirror]=120
  [spheres]=120
  [lantern]=60
  [outdoor]=60
  [helmet]=60
)

merge_into() {
  local src="$1" merged="$2" start_i="$3"
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

rm -rf "$MERGED"
mkdir -p "$MERGED"
global=0
first=1
seed_base=7000

for p in mirror spheres lantern outdoor helmet; do
  n=${COUNTS[$p]}
  tmp="$OUT/_tmp_$p"
  rm -rf "$tmp"
  echo "════ metal-heavy preset=$p N=$n ════"
  "$BIN" --export-dataset "$n" --preset "$p" --quality "$QUALITY" --views 1 \
    --domain-rand --export-views random \
    --seed "$((seed_base + global))" --out-dir "$tmp"
  src="$tmp/dataset"
  if [[ $first -eq 1 ]]; then
    cp -f "$src/config.json" "$MERGED/config.json"
    python3 - "$MERGED/config.json" <<'PY'
import json, sys
p = sys.argv[1]
with open(p) as f: c = json.load(f)
c["level"] = "L2_metal"
c["unified_theta"] = True
c["metal_heavy"] = True
with open(p, "w") as f: json.dump(c, f, indent=2)
PY
    head -n 1 "$src/meta.jsonl" > "$MERGED/meta.jsonl"
    first=0
  fi
  global=$(merge_into "$src" "$MERGED" "$global")
  rm -rf "$tmp"
done

count=$(grep -c '"theta"' "$MERGED/meta.jsonl" || echo 0)
echo "→ metal-heavy L2 done: $count samples in $MERGED"
echo "Train: python3 tools/inverse_c1/train.py --data $MERGED --out tools/inverse_c1/checkpoints/L2_metal --epochs 100"
