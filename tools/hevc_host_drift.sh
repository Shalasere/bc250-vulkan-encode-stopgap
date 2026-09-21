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
# hevc_host_diff.py compares LUMA AND CHROMA, and first asserts that the
# decoded frame is the size the case asked for - see its header for why the
# size assert is not redundant with the pixel comparison.
#
# Note this harness calls hevc_encoder_encode_raw() directly, so unlike the
# VA-API path it gets the true dimensions: ffmpeg's round-up-to-8 before
# vaCreateContext (docs/backlog.md C8, which makes 854x480 arrive as
# 856x480 on the board) does not apply here. Confirmed with ffprobe on the
# generated streams - they report exactly the requested display size.
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

# Cases 1-11 are the original set: 16-multiple squares plus 1920x1080.
#
# Cases 12-21 cover the padding and conformance-window crop path, which the
# original set barely touched - every one of those except 1080p has both
# dimensions divisible by 16, and 1080p only exercises the HEIGHT axis (1080
# -> 1088). The width axis had no coverage at all, and no case anywhere had
# a dimension that was not a multiple of 8. That gap is a known bug class:
# the same class produced the live H.264 SPS-crop bug fixed in b2b7fef,
# where 1920x1080 decoded as 1920x1088 and scored 13.66 dB.
#
# The set below picks, deliberately:
#   - each axis alone, then both at once (1918x1080 / 1920x1078 / 1918x1078)
#   - dimensions that are not multiples of 8 OR of 4. 1918, 1366, 854, 1078
#     and 358 are all 2 mod 4 and 6 mod 8, which is the most misaligned an
#     even dimension can be
#   - a different pad AMOUNT per case (2, 10, 14, 6, 12), not just "not 16"
#   - real sizes a user would actually ask for (854x480, 1366x768)
#   - small awkward ones, including sub-CTU (4x4) and a picture only one CTU
#     row tall (20x12), where the bottom CTU row is also the top one
#   - one low-QP/high-bitrate case (640x358 q0 p3). That is not about
#     resolution: it is the regression guard for the slice-buffer capacity
#     bug this case set found - see below.
#
# ODD dimensions are excluded on purpose, not overlooked. 4:2:0 has one
# chroma sample per 2x2 luma block, so an odd width or height has no
# representation in the format; HEVC's conformance window is also specified
# in chroma units (SubWidthC/SubHeightC = 2), so the crop can only remove an
# even number of luma samples and an odd size is unsignallable. ffmpeg never
# produces one either. Sizes below one CTU are NOT excluded - they work,
# because the picture is coded at 16x16 and the whole thing is cropped away
# by the conformance window; 4x4 and even 2x2 are byte-exact.
#
# What this found, on its first run:
#   1. Drift at QP 4 on pattern 3 at every size from 640x360 up, correct
#      down to one row and garbage below it. NOT a resolution bug -
#      1280x720 (both axes 16-aligned) failed identically. The slice buffer
#      was sized at coded_luma + 64 KiB ~= 1.03 bytes per luma sample, and
#      this encoder emits up to 1.53 on noise at QP 0; bitstream_t sets its
#      `overflow` flag and silently stops writing, and nothing checked it,
#      so the encoder returned a *truncated* slice as a success. Fixed in
#      encoder_h265.c (2.0 bytes/luma-sample, plus a hard failure if that is
#      ever exceeded). QP 0 on noise is now case 17.
#   2. 4x4 "ENCODE FAILED", which was tools/hevc_host_repro.c's own output
#      buffer (w*h*3 = 48 bytes, against a ~140-byte access unit), not an
#      encoder limit at small sizes.
# No padding or crop defect was found: with those two fixed, 600 sweep cases
# across 30 sizes x 5 QPs x 4 patterns are byte-exact on luma AND chroma.
CASES="${BC250_DRIFT_CASES:-16 16 4 2; 32 32 4 2; 64 64 4 0; 64 64 4 1; 64 64 4 2; 64 64 4 3; 64 64 30 1; 64 64 30 2; 128 128 20 3; 256 256 27 3; 1920 1080 27 3; 1918 1080 27 3; 1920 1078 27 3; 1918 1078 27 3; 1366 768 27 2; 854 480 30 1; 640 358 0 3; 100 60 27 3; 20 12 27 1; 18 18 27 2; 4 4 27 3}"

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
