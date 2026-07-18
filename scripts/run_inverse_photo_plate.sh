#!/usr/bin/env bash
# H3 photo plate: multi-view export → photo_proxy domain shift → lab fit → PHOTOTEST.
# Hard preset default (spheres). Honest gates — no fake ≥28 on proxy photos.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BIN="${BIN:-./build/inverse_fit}"
# Default lantern multi-view photo-proxy. 640×360 is the shippable plate;
# set SHOW_W=1280 SHOW_H=720 for daily-res (much slower under PT FD).
# Harder: PRESET=helmet (slower). Avoid PRESET=spheres unless overnight.
PRESET="${PRESET:-lantern}"
OUT_ROOT="${OUT_ROOT:-renders/photo_lab/${PRESET}}"
VIEWS="${VIEWS:-4}"
SHOW_W="${SHOW_W:-640}"
SHOW_H="${SHOW_H:-360}"
SHOW_SPP="${SHOW_SPP:-128}"
ITERS="${ITERS:-16}"
FIT_SPP="${FIT_SPP:-32}"

if [[ ! -x "$BIN" ]]; then
  cmake --build build --target inverse_fit -j"$(nproc)"
fi

EXPORT_DIR="${OUT_ROOT}_export"
PROXY_DIR="${OUT_ROOT}_proxy"
FIT_DIR="${OUT_ROOT}_photo_fit"
mkdir -p "$(dirname "$OUT_ROOT")" "$EXPORT_DIR" "$PROXY_DIR" "$FIT_DIR"

echo "=== 1) Multi-view synthetic export (preset=$PRESET) ==="
"$BIN" --export-capture --preset "$PRESET" --views "$VIEWS" \
  --show-width "$SHOW_W" --show-height "$SHOW_H" --show-spp "$SHOW_SPP" \
  --map-res 2 --out-dir "$EXPORT_DIR" 2>&1 | tee "${OUT_ROOT}_export.log" | tail -30

CAP_SRC="${EXPORT_DIR}/capture"
if [[ ! -f "$CAP_SRC/capture.json" ]]; then
  echo "FATAL: export missing $CAP_SRC/capture.json"
  exit 1
fi

echo "=== 2) Photo proxy domain shift ==="
python3 tools/inverse_lab/make_photo_proxy.py \
  "$CAP_SRC" "${PROXY_DIR}/capture" \
  --exposure 0.88 --noise 0.015 --jpeg-q 88

echo "=== 3) Fit from photo_proxy (train-only) ==="
# Fit may exit non-zero if synthetic LABTEST fails — expected for photo_proxy.
# PHOTOTEST is the authoritative gate for this plate.
set +e
# No visual polish: polish@720p is multi-hour for little PHOTOTEST signal.
"$BIN" --lab-bundle "${PROXY_DIR}/capture" --preset "$PRESET" --quality draft \
  --show-width "$SHOW_W" --show-height "$SHOW_H" --show-spp 96 --fit-spp "$FIT_SPP" \
  --iters "$ITERS" --multi-start 3 --no-visual-polish \
  --out-dir "$FIT_DIR" 2>&1 | tee "${OUT_ROOT}_fit.log" | rg -n "inverse_fit|SHOW RMSE|holdout|relight|PASS|FAIL|result|wrong-init" || true
fit_rc=${PIPESTATUS[0]}
set -e
echo "  inverse_fit exit=$fit_rc (LABTEST may fail under domain shift; PHOTOTEST decides)"

if [[ ! -f "$FIT_DIR/lab_metrics.json" ]]; then
  echo "FATAL: fit produced no lab_metrics.json"
  exit 1
fi

echo "=== 4) PHOTOTEST (honest) ==="
python3 tools/inverse_lab/test_photo_plate.py "$FIT_DIR" --capture "${PROXY_DIR}/capture"
echo "PHOTO PLATE DONE"
echo "  export=$CAP_SRC"
echo "  proxy=${PROXY_DIR}/capture"
echo "  fit=$FIT_DIR"
echo "  strip=$FIT_DIR/photo_vs_rerender.png"
echo "  metrics=$FIT_DIR/photo_plate_metrics.json"
