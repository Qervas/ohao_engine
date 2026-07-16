#!/usr/bin/env bash
# One-command C1 pipeline: (optional export) → train → hybrid gallery → index.
#
# Usage:
#   ./tools/inverse_c1/pipeline.sh                  # train if needed + gallery
#   ./tools/inverse_c1/pipeline.sh --export          # chunked metal export first
#   ./tools/inverse_c1/pipeline.sh --presets lantern,mirror,outdoor
#   SKIP_TRAIN=1 ./tools/inverse_c1/pipeline.sh     # reuse existing weights
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${ROOT}/build/inverse_fit"
CKPT="${ROOT}/tools/inverse_c1/checkpoints/L2_metal"
MODEL="${CKPT}/best.pt"
DATA="${ROOT}/renders/inverse_c1_metal/dataset"
GALLERY="${ROOT}/renders/inverse_c1_gallery"
DO_EXPORT=0
DO_TRAIN=1
PRESETS=(lantern mirror outdoor)
EPOCHS="${EPOCHS:-80}"
QUALITY="${QUALITY:-draft}"
ITERS="${ITERS:-32}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --export) DO_EXPORT=1; shift ;;
    --skip-train) DO_TRAIN=0; shift ;;
    --presets) IFS=',' read -r -a PRESETS <<< "$2"; shift 2 ;;
    --epochs) EPOCHS="$2"; shift 2 ;;
    --quality) QUALITY="$2"; shift 2 ;;
    --out) GALLERY="$2"; shift 2 ;;
    *) echo "Unknown arg: $1"; exit 2 ;;
  esac
done
[[ "${SKIP_TRAIN:-0}" == "1" ]] && DO_TRAIN=0

if [[ ! -x "$BIN" ]]; then
  echo "Building inverse_fit..."
  cmake --build "${ROOT}/build" --target inverse_fit -j"$(nproc)"
fi

if [[ $DO_EXPORT -eq 1 ]]; then
  echo "═══ Export metal-heavy L2 (chunked) ═══"
  # Prefer chunked path for stability (domain-rand can OOM on long single runs)
  OUT="${ROOT}/renders/inverse_c1_metal"
  MERGED="$OUT/dataset"
  mkdir -p "$MERGED"
  global=0
  first=1
  export_chunk() {
    local p=$1 n=$2 seed=$3
    local tmp=$OUT/_tmp_${p}_${seed}
    rm -rf "$tmp"
    echo "chunk preset=$p n=$n"
    "$BIN" --export-dataset "$n" --preset "$p" --quality "$QUALITY" --views 1 \
      --domain-rand --export-views random --seed "$seed" --out-dir "$tmp" \
      >/dev/null || { echo "WARN fail $p"; return 1; }
    local src=$tmp/dataset
    if [[ $first -eq 1 ]]; then
      cp -f "$src/config.json" "$MERGED/config.json"
      python3 - "$MERGED/config.json" <<'PY'
import json,sys
c=json.load(open(sys.argv[1])); c.update(level="L2_metal", metal_heavy=True, unified_theta=True)
json.dump(c, open(sys.argv[1],"w"), indent=2)
PY
      head -n1 "$src/meta.jsonl" > "$MERGED/meta.jsonl"
      first=0
    fi
    while IFS= read -r line; do
      [[ "$line" == *'"type":"header"'* ]] && continue
      file=$(echo "$line" | sed -n 's/.*"file":"\([^"]*\)".*/\1/p')
      [[ -z "$file" || ! -f "$src/$file" ]] && continue
      new=$(printf '%05d.png' "$global")
      cp -f "$src/$file" "$MERGED/$new"
      echo "$line" | sed "s/\"file\":\"$file\"/\"file\":\"$new\"/; s/\"i\":[0-9]*/\"i\":$global/" >> "$MERGED/meta.jsonl"
      global=$((global + 1))
    done < "$src/meta.jsonl"
    rm -rf "$tmp"
    echo "  total=$global"
  }
  rm -rf "$MERGED"; mkdir -p "$MERGED"
  for i in 0 1 2; do export_chunk mirror 40 $((9000+i*17)) || true; done
  for i in 0 1 2; do export_chunk spheres 40 $((9100+i*19)) || true; done
  for i in 0 1; do export_chunk lantern 40 $((9200+i*23)) || true; done
  export_chunk outdoor 40 9300 || true
  export_chunk helmet 40 9400 || true
  DATA="$MERGED"
  echo "Exported $global samples → $DATA"
fi

if [[ $DO_TRAIN -eq 1 ]]; then
  if [[ ! -f "$DATA/meta.jsonl" ]]; then
    echo "No dataset at $DATA — run with --export or point DATA=..."
    exit 1
  fi
  echo "═══ Train L2_metal ($EPOCHS epochs) ═══"
  python3 "$ROOT/tools/inverse_c1/train.py" \
    --data "$DATA" --out "$CKPT" --epochs "$EPOCHS" --batch 32 --lr 1e-3 \
    --width 128 --height 72
  MODEL="$CKPT/best.pt"
  mkdir -p "${ROOT}/tools/inverse_c1/checkpoints/L2"
  cp -f "$MODEL" "${ROOT}/tools/inverse_c1/checkpoints/L2/best.pt"
fi

if [[ ! -f "$MODEL" ]]; then
  echo "Missing model $MODEL"
  exit 1
fi

echo "═══ Hybrid gallery: ${PRESETS[*]} ═══"
mkdir -p "$GALLERY"
SUMMARY="$GALLERY/quality_sprint_summary.jsonl"
: > "$SUMMARY"
for p in "${PRESETS[@]}"; do
  echo "---- $p ----"
  "$ROOT/tools/inverse_c1/hybrid_compare.sh" \
    --preset "$p" --quality "$QUALITY" --iters "$ITERS" \
    --model "$MODEL" --out-dir "$GALLERY" \
    2>&1 | tee "$GALLERY/${p}_compare.log" | tail -25 || true
  if [[ -f "$GALLERY/${p}_summary.json" ]]; then
    cat "$GALLERY/${p}_summary.json" >> "$SUMMARY"
    echo >> "$SUMMARY"
  fi
done

echo "═══ Build gallery index ═══"
python3 "$ROOT/tools/inverse_c1/make_gallery.py" --root "$GALLERY"

echo ""
echo "Pipeline complete."
echo "  Model:   $MODEL"
echo "  Gallery: $GALLERY/index.html"
echo "  Open:    xdg-open $GALLERY/index.html   # or open in browser"
