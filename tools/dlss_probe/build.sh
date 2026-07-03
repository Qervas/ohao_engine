#!/usr/bin/env bash
# Build + run the DLSS-RR availability probe (Phase 0 feasibility gate).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DLSS="$ROOT/external/DLSS"
INC="$DLSS/include"
NGX_STATIC="$DLSS/lib/Linux_x86_64/libnvsdk_ngx.a"
SNIPPET_DIR="$DLSS/lib/Linux_x86_64/rel"
OUT="$HERE/dlss_probe"

echo "== compiling =="
g++ -std=c++17 -O2 -g \
    -I"$INC" \
    "$HERE/dlss_probe.cpp" \
    "$NGX_STATIC" \
    -lvulkan -ldl -lpthread \
    -o "$OUT"
echo "built: $OUT"

echo "== running =="
# NGX loads the dlssd snippet from the PathListInfo dir (passed in the probe), but we
# also add it to LD_LIBRARY_PATH as belt-and-suspenders.
export LD_LIBRARY_PATH="$SNIPPET_DIR:${LD_LIBRARY_PATH:-}"
# Make sure the NVIDIA ICD is visible on this hybrid Intel+NVIDIA laptop (probe still
# selects the device explicitly by vendorID 0x10DE).
export __NV_PRIME_RENDER_OFFLOAD=1
export __VK_LAYER_NV_optimus=NVIDIA_only

"$OUT" "$SNIPPET_DIR" "$HERE/ngx_appdata"
