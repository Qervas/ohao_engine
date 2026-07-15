#!/usr/bin/env bash
# Multi-preset synthetic dataset export for C1 training.
# Usage:
#   ./tools/inverse_c1/export_dataset.sh [N_per_preset] [out_root]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
N="${1:-128}"
OUT="${2:-$ROOT/renders/inverse_c1_data}"
BIN="${ROOT}/build/inverse_fit"
QUALITY="${QUALITY:-draft}"
VIEWS="${VIEWS:-1}"

if [[ ! -x "$BIN" ]]; then
  echo "Build inverse_fit first: cmake --build build --target inverse_fit -j"
  exit 1
fi

# Keep θ layout consistent: same flags for all presets (default studio 12D).
PRESETS=(lantern outdoor mirror helmet spheres)

mkdir -p "$OUT"
echo "Exporting C1 dataset: N=$N/preset  quality=$QUALITY  → $OUT"

# Merge into one dataset dir with offset indices.
MERGED="$OUT/dataset"
mkdir -p "$MERGED"
: > "$MERGED/meta.jsonl"
FIRST=1
GLOBAL=0

for p in "${PRESETS[@]}"; do
  TMP="$OUT/_tmp_$p"
  rm -rf "$TMP"
  echo "── preset=$p ──"
  "$BIN" --export-dataset "$N" --preset "$p" --quality "$QUALITY" --views "$VIEWS" \
    --seed "$((1000 + GLOBAL))" --out-dir "$TMP" || {
      echo "WARN: export failed for $p, skipping"
      continue
    }
  SRC="$TMP/dataset"
  if [[ ! -f "$SRC/meta.jsonl" ]]; then
    echo "WARN: no meta for $p"
    continue
  fi
  if [[ $FIRST -eq 1 ]]; then
    cp -f "$SRC/config.json" "$MERGED/config.json" 2>/dev/null || true
    # header
    head -n 1 "$SRC/meta.jsonl" >> "$MERGED/meta.jsonl"
    FIRST=0
  fi
  # remap files with global index
  while IFS= read -r line; do
    [[ "$line" == *'"type":"header"'* ]] && continue
    file=$(echo "$line" | sed -n 's/.*"file":"\([^"]*\)".*/\1/p')
    [[ -z "$file" ]] && continue
    new=$(printf '%05d.png' "$GLOBAL")
    cp -f "$SRC/$file" "$MERGED/$new"
    # rewrite file field
    echo "$line" | sed "s/\"file\":\"$file\"/\"file\":\"$new\"/; s/\"i\":[0-9]*/\"i\":$GLOBAL/" >> "$MERGED/meta.jsonl"
    GLOBAL=$((GLOBAL + 1))
  done < "$SRC/meta.jsonl"
  rm -rf "$TMP"
done

# Patch dims/names already in config from first preset
COUNT=$(grep -c '"theta"' "$MERGED/meta.jsonl" || true)
echo "Merged $COUNT samples → $MERGED"
echo "Train: python3 tools/inverse_c1/train.py --data $MERGED --out tools/inverse_c1/checkpoints --epochs 40"
