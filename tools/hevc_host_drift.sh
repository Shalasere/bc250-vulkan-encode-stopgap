#!/usr/bin/env bash
# bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap
# SPDX-License-Identifier: GPL-3.0-only
#
# hevc_host_drift.sh - the drift oracle for the CPU HEVC path, with NO GPU
# and NO board. Encodes, dumps the encoder's own reconstruction, decodes
# the resulting bitstream with a real decoder, and compares byte for byte.
#
# This is the same check `tools/lab drift` runs on the board, restricted to
# the path that does not need hardware: hevc_encoder_encode_raw() is
# GPU-free, so the whole loop runs on a dev machine. It is what root-caused
# the below-left reference bug (docs/hevc_scope_note.md) - a 16x16 case
# with 8 wrong pixels, rather than 1080p with two million.
#
# -skip_loop_filter all is required on the decode side: this encoder does
# not simulate deblocking while the PPS leaves it enabled, and SAO is off
# in our SPS. Without that flag the comparison is guaranteed to differ and
# tells you nothing.
#
# usage: tools/hevc_host_drift.sh [build-dir]
#   BC250_DRIFT_CASES="w h qp pattern; ..."  override the case list
#   pattern: 0=flat 1=vertical bars 2=diagonal ramp 3=pseudo-random
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO/approach1-compute-encoder/src"
WORK="${1:-/tmp/bc250_host_drift}"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

command -v ffmpeg >/dev/null || { echo "ffmpeg required"; exit 2; }

echo "building host repro..."
gcc -std=c11 -O2 -D_GNU_SOURCE -I"$SRC" -o hostrepro "$REPO/tools/hevc_host_repro.c" \
    "$SRC/encoder_h265.c" "$SRC/hevc_intra.c" "$SRC/hevc_cabac.c" \
    "$SRC/bitstream.c" "$SRC/rate_control.c" "$SRC/gpu_compute.c" \
    -lvulkan -lm 2>&1 | grep -E '\berror\b' | head
# A stale binary silently "passing" is a real failure mode - refuse to run.
[ -x hostrepro ] || { echo "BUILD FAILED"; exit 1; }

CASES="${BC250_DRIFT_CASES:-16 16 4 2; 32 32 4 2; 64 64 4 0; 64 64 4 1; 64 64 4 2; 64 64 4 3; 64 64 30 1; 64 64 30 2; 128 128 20 3; 256 256 27 3; 1920 1080 27 3}"

fail=0; n=0
IFS=';' read -ra LIST <<< "$CASES"
for c in "${LIST[@]}"; do
    set -- $c
    [ $# -ge 4 ] || continue
    w=$1 h=$2 qp=$3 pat=$4
    cw=$(( (w + 15) / 16 * 16 )); ch=$(( (h + 15) / 16 * 16 ))
    lbl="c_${w}x${h}_q${qp}_p${pat}"
    rm -f bc250_hevc_debug_recon_i420.raw "$lbl".hevc "$lbl".yuv
    BC250_HEVC_DEBUG_RECON=1 ./hostrepro "$w" "$h" "$qp" 1 "$lbl" "$pat" >/dev/null 2>&1 \
        || { echo "  $lbl: ENCODE FAILED"; fail=1; continue; }
    ffmpeg -v error -y -skip_loop_filter all -i "$lbl".hevc \
           -f rawvideo -pix_fmt yuv420p "$lbl".yuv 2>/dev/null
    python3 "$REPO/tools/hevc_host_diff.py" "$lbl" "$w" "$h" "$cw" "$ch" || fail=1
    n=$((n+1))
done

echo
if [ "$fail" = 0 ]; then echo "HOST DRIFT PASS ($n cases byte-exact)"; else echo "HOST DRIFT FAIL"; fi
exit $fail
