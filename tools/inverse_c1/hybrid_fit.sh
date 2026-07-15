#!/usr/bin/env bash
# Hybrid C1: NN θ prior → physical FD refine via inverse_fit.
#
# Usage:
#   ./tools/inverse_c1/hybrid_fit.sh --preset lantern --quality draft
#   ./tools/inverse_c1/hybrid_fit.sh --target-image photo.png --model ckpt.pt
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${ROOT}/build/inverse_fit"
INFER="${ROOT}/tools/inverse_c1/infer.py"
MODEL="${ROOT}/tools/inverse_c1/checkpoints/best.pt"
OUT="${ROOT}/renders/inverse_c1_hybrid"
PRESET="lantern"
QUALITY="draft"
TARGET=""
EXTRA=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --model) MODEL="$2"; shift 2 ;;
    --out-dir) OUT="$2"; shift 2 ;;
    --preset) PRESET="$2"; shift 2 ;;
    --quality) QUALITY="$2"; shift 2 ;;
    --target-image) TARGET="$2"; shift 2 ;;
    *) EXTRA+=("$1"); shift ;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "Missing $BIN — build inverse_fit first"
  exit 1
fi
if [[ ! -f "$MODEL" ]]; then
  echo "Missing model $MODEL — train first:"
  echo "  python3 tools/inverse_c1/train.py --data renders/inverse_c1_data/dataset --out tools/inverse_c1/checkpoints"
  exit 1
fi

mkdir -p "$OUT"

# 1) Render synthetic target (or use photo) into OUT
if [[ -n "$TARGET" ]]; then
  IMG="$TARGET"
  echo "Using external target $IMG"
  # Still need a fit pass with target-image; prior from same image
  python3 "$INFER" --model "$MODEL" --image "$IMG" --out "$OUT/theta_prior.json"
  "$BIN" --target-image "$IMG" --theta-init "$OUT/theta_prior.json" \
    --quality "$QUALITY" --out-dir "$OUT" --no-multi-start "${EXTRA[@]}"
else
  # Render truth targets first by running a dry path: use inverse_fit once to emit target,
  # then infer, then fit with theta-init.
  # Step A: export target only via short selftest-less path — full selftest with wrong init is wasteful.
  # We run fit with NN: first need a target image. Generate with export of truth by selftest stop?
  # Practical: run inverse_fit --selftest which writes target_*, then infer on target_fit or target_show,
  # then re-run with theta-init. Two-pass.
  PASS1="$OUT/pass1_targets"
  mkdir -p "$PASS1"
  echo "Pass 1: render targets (preset=$PRESET)…"
  # Use export of 0? Not supported. Quick selftest with 1 iter is still heavy.
  # Instead: run full selftest without NN to write targets is too long.
  # Minimal: use --export-dataset 1 with fixed seed? That samples random θ not truth.
  # Best: run inverse_fit normally; inject NN after targets via two-phase binary later.
  # For now two-pass: short run with --iters 1 writes targets then early… actually still runs all stages.
  #
  # Workaround: call inverse_fit with high multi-start skip and theta from random mid — just for targets.
  # We add env var skip… Not available.
  #
  # Use existing target if present, else run fit once.
  if [[ ! -f "$OUT/target_show.png" && ! -f "$OUT/target_front.png" ]]; then
    echo "Rendering targets via inverse_fit (one-time)…"
    "$BIN" --selftest --preset "$PRESET" --quality "$QUALITY" --out-dir "$OUT" \
      --no-multi-start --iters 2 "${EXTRA[@]}" || true
  fi
  IMG=""
  for cand in "$OUT/target_fit.png" "$OUT/target_show.png" "$OUT/target_front.png"; do
    if [[ -f "$cand" ]]; then IMG="$cand"; break; fi
  done
  if [[ -z "$IMG" ]]; then
    echo "ERROR: no target image in $OUT"
    exit 1
  fi
  echo "Pass 2: NN prior on $IMG"
  python3 "$INFER" --model "$MODEL" --image "$IMG" --out "$OUT/theta_prior.json"
  echo "Pass 3: FD refine with NN init"
  "$BIN" --selftest --preset "$PRESET" --quality "$QUALITY" --out-dir "$OUT" \
    --theta-init "$OUT/theta_prior.json" --no-multi-start "${EXTRA[@]}"
fi

echo "Hybrid fit done → $OUT"
