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
# The decode side no longer passes -skip_loop_filter all. It used to have
# to: the encoder does not simulate deblocking, and the PPS left deblocking
# enabled, so an unfiltered comparison was guaranteed to differ. The PPS now
# signals deblocking off (write_pps in encoder_h265.c) and SAO was always
# off in our SPS, so there is no in-loop filter left to skip - and dropping
# the flag makes this a check on the REAL decode path. It is also the only
# thing that proves the new PPS bits parse the way the encoder believes:
# if they did not, the decoder would still be filtering and every case here
# would fail.
#
# usage: tools/hevc_host_drift.sh [build-dir]
#   BC250_DRIFT_CASES="w h qp pattern [frames] [gop] [gpu_mv]; ..."  cases
#   pattern: 0=flat 1=vertical bars 2=diagonal ramp 3=pseudo-random
#   frames:  default 1
#   gop:     default 1 (= IDR every frame). gop > 1 encodes one IDR then
#            P-frames, so this oracle covers the INTER path - cu_skip/merge
#            signalling, the reference-picture chain and the P-slice header.
#            Until that existed every case here was all-intra and the
#            encoder's entire inter branch was invisible to the strongest
#            check in the project.
#   gpu_mv:  "dx,dy" in INTEGER pel, or - for none (default). Stands in for
#            motion_estimation.comp's per-CTU output via
#            BC250_HEVC_FAKE_GPU_MV, because that readback is the inter
#            path's ONLY source of a non-zero motion vector and it does not
#            exist on a machine with no GPU. Without it every merge
#            candidate is (0,0), every merge_idx selects the same vector,
#            and a wrong merge list is indistinguishable from a right one.
#            A real bug hid there: see derive_merge_candidates().
#            HONEST STATUS: since the motion search was removed
#            (docs/notes/dead-motion-search.md) nothing in the encoder
#            reads the GPU MV, so these six cases are currently
#            byte-identical to the same cases without it - that was
#            measured, not assumed. They are kept as the regression guard
#            for the day an MV consumer is reconnected; if the GPU MV path
#            is ever removed, remove them with it.
#   kbps:    0 (default) = constant QP. >0 = VBR rate control, so QP moves
#            between frames - which is how the driver is actually driven
#            (`lab qsweep` passes a bitrate, never a QP) and the only way
#            to reach a non-zero slice_qp_delta on a P-frame.
#
# BC250_DRIFT_ONLY=intra|inter restricts the built-in list.
#
# BC250_DRIFT_BS_MD5=<absolute path> additionally records "<md5>  <label>"
# for every emitted BITSTREAM. That is a different question from the one
# this oracle answers: the diff below compares the encoder's reconstruction
# against the decoder's, so it passes for ANY self-consistent bitstream and
# is blind to a change that alters the bytes while staying decodable. Run it
# before and after a change that is claimed to be output-neutral and diff the
# two lists. Recording implies BC250_RC_NOMINAL_DRAIN=1, because the VBR
# cases below otherwise drain their leaky bucket by real elapsed time and are
# not byte-reproducible against themselves (docs/performance-measurement.md,
# "a second, CPU-only way to lose byte-exactness").
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$REPO/approach1-compute-encoder/src"
WORK="${1:-/tmp/bc250_host_drift}"
rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"

command -v ffmpeg >/dev/null || { echo "ffmpeg required"; exit 2; }

BS_MD5="${BC250_DRIFT_BS_MD5:-}"
if [ -n "$BS_MD5" ]; then
    case "$BS_MD5" in /*) ;; *) echo "BC250_DRIFT_BS_MD5 must be an absolute path"; exit 2 ;; esac
    export BC250_RC_NOMINAL_DRAIN=1
    : > "$BS_MD5"
fi

echo "building host repro..."
gcc -std=c11 -O2 -D_GNU_SOURCE -I"$SRC" -o hostrepro "$REPO/tools/hevc_host_repro.c" \
    "$SRC/encoder_h265.c" "$SRC/hevc_intra.c" "$SRC/hevc_cabac.c" \
    "$SRC/bitstream.c" "$SRC/rate_control.c" "$SRC/gpu_compute.c" \
    -lvulkan -lm 2>&1 | grep -E '\berror\b' | head
# A stale binary silently "passing" is a real failure mode - refuse to run.
[ -x hostrepro ] || { echo "BUILD FAILED"; exit 1; }

# Intra cases 1-11 are the original set: 16-multiple squares plus 1920x1080.
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
INTRA_CASES="16 16 4 2; 32 32 4 2; 64 64 4 0; 64 64 4 1; 64 64 4 2; 64 64 4 3; 64 64 30 1; 64 64 30 2; 128 128 20 3; 256 256 27 3; 1920 1080 27 3"
INTRA_CASES="$INTRA_CASES; 1918 1080 27 3; 1920 1078 27 3; 1918 1078 27 3; 1366 768 27 2; 854 480 30 1; 640 358 0 3; 100 60 27 3; 20 12 27 1; 18 18 27 2; 4 4 27 3"
# Inter cases: same patterns, but a real GOP. Patterns 2/3 advance with the
# frame index, so these are moving content and every frame after the first
# is a P-frame whose reference is the encoder's own previous reconstruction.
INTER_CASES="64 64 4 0 4 4; 64 64 4 2 4 4; 64 64 27 2 4 4; 64 64 27 3 4 4; 128 128 27 2 4 4; 128 128 27 3 8 8; 256 256 27 3 4 4; 64 64 4 2 8 8"
# Same again with a non-zero stand-in for the GPU's motion vector, which is
# the only thing that makes the merge list contain more than one distinct
# vector. (2,0) and (0,2) each failed 17k+/24k luma samples before
# derive_merge_candidates() stopped appending a candidate the decoder does
# not have; they are kept as the regression test for that.
INTER_CASES="$INTER_CASES; 64 64 27 2 6 6 2,0; 64 64 27 2 6 6 0,2; 64 64 27 2 6 6 4,4; 64 64 27 3 6 6 -4,2; 128 128 27 3 6 6 2,2; 128 128 27 2 6 6 -2,0"
# Rate-controlled: QP moves per frame, so slice_qp_delta is non-zero on
# P-frames and the CABAC contexts re-init at a QP the PPS never announced.
INTER_CASES="$INTER_CASES; 128 128 27 3 8 8 - 400; 128 128 27 2 8 8 - 4000; 256 256 27 3 8 8 2,0 2000"

case "${BC250_DRIFT_ONLY:-all}" in
  intra) DEFAULT_CASES="$INTRA_CASES" ;;
  inter) DEFAULT_CASES="$INTER_CASES" ;;
  *)     DEFAULT_CASES="$INTRA_CASES; $INTER_CASES" ;;
esac
CASES="${BC250_DRIFT_CASES:-$DEFAULT_CASES}"

fail=0; n=0
IFS=';' read -ra LIST <<< "$CASES"
for c in "${LIST[@]}"; do
    set -- $c
    [ $# -ge 4 ] || continue
    w=$1 h=$2 qp=$3 pat=$4
    nf=${5:-1} gop=${6:-1} gmv=${7:--} kbps=${8:-0}
    cw=$(( (w + 15) / 16 * 16 )); ch=$(( (h + 15) / 16 * 16 ))
    lbl="c_${w}x${h}_q${qp}_p${pat}"
    [ "$gop" != 1 ] && lbl="${lbl}_n${nf}g${gop}"
    [ "$gmv" != - ] && lbl="${lbl}_mv${gmv/,/_}"
    [ "$kbps" != 0 ] && lbl="${lbl}_vbr${kbps}"
    fakemv=(); [ "$gmv" != - ] && fakemv=(env "BC250_HEVC_FAKE_GPU_MV=$gmv")
    rm -f bc250_hevc_debug_recon_i420.raw "$lbl".hevc "$lbl".yuv
    BC250_HEVC_DEBUG_RECON=1 ${fakemv[@]+"${fakemv[@]}"} \
        ./hostrepro "$w" "$h" "$qp" "$nf" "$lbl" "$pat" "$gop" "$kbps" >/dev/null 2>&1 \
        || { echo "  $lbl: ENCODE FAILED"; fail=1; continue; }
    [ -n "$BS_MD5" ] && md5sum "$lbl".hevc | sed "s|$lbl.hevc|$lbl|" >> "$BS_MD5"
    # No -skip_loop_filter: the PPS now disables deblocking outright (see
    # write_pps), so this is the real decode path, not a filtered-off one.
    ffmpeg -v error -y -i "$lbl".hevc \
           -f rawvideo -pix_fmt yuv420p "$lbl".yuv 2>/dev/null
    python3 "$REPO/tools/hevc_host_diff.py" "$lbl" "$w" "$h" "$cw" "$ch" "$nf" || fail=1
    n=$((n+1))
done

echo
if [ "$fail" = 0 ]; then echo "HOST DRIFT PASS ($n cases byte-exact)"; else echo "HOST DRIFT FAIL"; fi
exit $fail
