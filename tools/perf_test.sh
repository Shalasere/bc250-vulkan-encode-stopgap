#!/usr/bin/env bash
# bc250-vcn-driver - feature/realtime-perf
#
# perf_test.sh - Honest wall-clock throughput + per-stage GPU/CPU timing
# harness for the BC-250 Vulkan-compute VA-API H.264 encoder.
#
# WHY THIS EXISTS
# ----------------
# tools/quality_test.sh proves the encoder is *correct* (PSNR/SSIM against a
# ground-truth reference); it says nothing about *speed*, and its own ffmpeg
# invocation is not timed for real-time throughput at all. Nobody had ever
# measured whether this encoder can keep up with real-time capture (30-60fps
# at 720p/1080p) until this script. This is a measurement/diagnosis tool
# only - it does not change any encoding behavior.
#
# WHAT IT MEASURES
#   1. Wall-clock throughput: encodes a real N-frame clip through the exact
#      same ffmpeg + VA-API + this driver pipeline quality_test.sh uses
#      (-vaapi_device ... -vf format=nv12,hwupload -c:v h264_vaapi), timed
#      end-to-end with the shell's own `time` builtin (real wall clock, not
#      a synthetic microbenchmark), at one or more requested resolutions.
#   2. Per-stage GPU time: if the driver is built with the BC250_PERF_STATS
#      instrumentation (see gpu_compute.c's BC250_PERF_NUM_TIMESTAMPS
#      comment and encoder_h264.c's matching CPU-side CAVLC/frame-wall
#      brackets), setting BC250_PERF_STATS=1 makes it print one
#      "[BC250_PERF_GPU] ..." / "[BC250_PERF_CPU] ..." / "[BC250_PERF_FRAME] ..."
#      line per frame to stderr with real Vulkan-timestamp-query-derived
#      GPU stage durations (motion estimation, prediction, DCT, quantize,
#      reconstruct, diagonal-wavefront intra reconstruction, deblock,
#      entropy) plus CPU-side CAVLC/bitstream time and total per-frame wall
#      time. This script captures ffmpeg's stderr (where those lines land,
#      since the driver is a shared library loaded into the ffmpeg process)
#      to a log and summarizes it with awk - no code changes, no guessing.
#   3. Per-frame variance: the summary reports min/max/mean/stddev of
#      per-frame wall time (from the [BC250_PERF_FRAME] lines), and I-frame
#      vs P-frame timing separately, since the diagonal-wavefront intra path
#      is architecturally a plausible (but here empirically checked, not
#      assumed) source of slower/spikier I-frames.
#   4. Optional reference point: if RUN_LIBX264_REFERENCE=1, also encodes
#      the identical source through plain software libx264 (no VA-API, no
#      this driver) at a comparable preset, purely to calibrate what this
#      CPU/board can plausibly do - not to imply libx264 is the target.
#
# USAGE
#   ./tools/perf_test.sh
#   RESOLUTIONS="1280x720 1920x1080" FRAMES=300 ./tools/perf_test.sh
#
# ENVIRONMENT OVERRIDES (all optional)
#   BUILD_DIR              cmake build directory
#                            (default: <repo>/approach1-compute-encoder/build)
#   BC250_ENV_SCRIPT       board build-toolchain env script to source if present
#                            (default: $HOME/build-deps/env.sh)
#   WORK_DIR               scratch directory for streams/logs
#                            (default: /tmp/bc250_perf_test)
#   RESOLUTIONS            space-separated list of WIDTHxHEIGHT to test
#                            (default: "1280x720 1920x1080")
#   FRAMERATE              source framerate fed to ffmpeg (default: 30)
#   FRAMES                 frame count to encode per resolution (default: 300)
#   BITRATE                encoder target bitrate (default: 4M)
#   RENDER_DEVICE          VA-API render node (default: /dev/dri/renderD128)
#   SKIP_BUILD             if set to 1, don't (re)build; require an existing
#                            bc250_drv_video.so in BUILD_DIR
#   BC250_PERF_STATS_MODE  1 (default) to enable BC250_PERF_STATS=1 during the
#                            timed run for the per-stage breakdown; 0 to skip
#                            (the instrumentation itself is lightweight, but
#                            set to 0 to double-check it isn't skewing the
#                            headline fps number)
#   RUN_LIBX264_REFERENCE  1 to also run a plain software libx264 encode at
#                            each resolution for context (default: 0)
#   LIBX264_PRESET         libx264 preset for the reference encode
#                            (default: veryfast - a realistic "real-time
#                            streaming" preset, not a max-quality one)
#
# This script never touches a system-wide driver install path - it always
# runs the driver straight out of BUILD_DIR via LIBVA_DRIVERS_PATH.
#
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/approach1-compute-encoder/build}"
BC250_ENV_SCRIPT="${BC250_ENV_SCRIPT:-$HOME/build-deps/env.sh}"
WORK_DIR="${WORK_DIR:-/tmp/bc250_perf_test}"
RESOLUTIONS="${RESOLUTIONS:-1280x720 1920x1080}"
FRAMERATE="${FRAMERATE:-30}"
FRAMES="${FRAMES:-300}"
BITRATE="${BITRATE:-4M}"
RENDER_DEVICE="${RENDER_DEVICE:-/dev/dri/renderD128}"
SKIP_BUILD="${SKIP_BUILD:-0}"
BC250_PERF_STATS_MODE="${BC250_PERF_STATS_MODE:-1}"
RUN_LIBX264_REFERENCE="${RUN_LIBX264_REFERENCE:-0}"
LIBX264_PRESET="${LIBX264_PRESET:-veryfast}"

echo -e "${BLUE}======================================================${NC}"
echo -e "${BLUE}${BOLD}   BC-250 Encoder Real-Time Throughput Measurement    ${NC}"
echo -e "${BLUE}======================================================${NC}"
echo -e "  Resolutions    : ${RESOLUTIONS}"
echo -e "  Framerate      : ${FRAMERATE} fps (source)"
echo -e "  Frame count    : ${FRAMES}"
echo -e "  Build dir      : ${BUILD_DIR}"
echo -e "  Work dir       : ${WORK_DIR}"
echo -e "  Per-stage stats: ${BC250_PERF_STATS_MODE}"
echo -e "  libx264 ref    : ${RUN_LIBX264_REFERENCE}"

# ------------------------------------------------------------------
# [1/3] Build (or verify) a private driver build - same pattern as
# quality_test.sh, so this always exercises exactly what's on this branch.
# ------------------------------------------------------------------
echo -e "\n${BOLD}[1/3] Preparing private driver build...${NC}"
if [ -f "$BC250_ENV_SCRIPT" ]; then
    echo -e "  -> Sourcing build toolchain: $BC250_ENV_SCRIPT"
    set +u
    # shellcheck disable=SC1090
    source "$BC250_ENV_SCRIPT"
    set -u
fi

DRIVER_SO="$BUILD_DIR/bc250_drv_video.so"
if [ "$SKIP_BUILD" != "1" ] || [ ! -f "$DRIVER_SO" ]; then
    mkdir -p "$BUILD_DIR"
    (
        cd "$BUILD_DIR"
        cmake "$REPO_ROOT/approach1-compute-encoder" \
              -DCMAKE_BUILD_TYPE=Release \
              -DBUILD_TESTS=OFF \
              -DCMAKE_INSTALL_PREFIX="$WORK_DIR/fake_prefix" \
              -DLIBVA_DRIVERS_PATH="$BUILD_DIR"
        make -j"$(nproc)"
    )
fi

if [ ! -f "$DRIVER_SO" ]; then
    echo -e "${RED}✗ bc250_drv_video.so not found at $DRIVER_SO after build. Aborting.${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓ Driver ready: $DRIVER_SO${NC}"

export LIBVA_DRIVER_NAME=bc250
export LIBVA_DRIVERS_PATH="$BUILD_DIR"
export BC250_SHADER_DIR="$BUILD_DIR"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

# ------------------------------------------------------------------
# [2/3] For each resolution: timed VA-API encode through this driver,
# optionally with BC250_PERF_STATS=1 for the per-stage breakdown, plus an
# optional plain-libx264 reference encode for context.
# ------------------------------------------------------------------
echo -e "\n${BOLD}[2/3] Timed encodes...${NC}"

SUMMARY_FILE="$WORK_DIR/perf_summary.txt"
: > "$SUMMARY_FILE"

for RES in $RESOLUTIONS; do
    WIDTH="${RES%x*}"
    HEIGHT="${RES#*x}"
    TAG="${WIDTH}x${HEIGHT}"
    ENCODED="$WORK_DIR/encoded_${TAG}.mp4"
    LOG="$WORK_DIR/ffmpeg_${TAG}.log"
    TIME_LOG="$WORK_DIR/time_${TAG}.log"

    echo -e "\n${BOLD}--- ${TAG} @ ${FRAMERATE}fps, ${FRAMES} frames (VA-API / this driver) ---${NC}"

    PERF_ENV=()
    if [ "$BC250_PERF_STATS_MODE" = "1" ]; then
        PERF_ENV+=(BC250_PERF_STATS=1)
    fi

    set +e
    # `time -p` (POSIX format) on the whole ffmpeg invocation gives a
    # trustworthy wall-clock number for the entire encode, independent of
    # ffmpeg's own -benchmark/-progress output (which can undercount setup/
    # teardown). stderr is captured to LOG - that's also where this driver's
    # opt-in "[BC250_PERF_GPU]"/"[BC250_PERF_CPU]"/"[BC250_PERF_FRAME]" lines
    # land, since it's a shared library loaded into the ffmpeg process.
    { time -p env "${PERF_ENV[@]}" ffmpeg -y -v info -f lavfi \
        -i "testsrc=size=${WIDTH}x${HEIGHT}:rate=${FRAMERATE}" -frames:v "$FRAMES" \
        -vaapi_device "$RENDER_DEVICE" \
        -vf 'format=nv12,hwupload' \
        -c:v h264_vaapi -b:v "$BITRATE" \
        "$ENCODED" ; } > "$LOG" 2> "$TIME_LOG"
    ENCODE_RC=$?
    set -e
    # `time -p`'s own output goes to stderr, mixed with ffmpeg's stderr in
    # TIME_LOG (ffmpeg logs to stderr by default) - pull the `real` line
    # back out.
    cat "$TIME_LOG" >> "$LOG"

    if [ $ENCODE_RC -ne 0 ] || [ ! -s "$ENCODED" ]; then
        echo -e "  ${RED}✗ Encode failed (exit $ENCODE_RC) or produced an empty file at ${TAG}. Log tail:${NC}"
        tail -n 40 "$LOG" | sed 's/^/    /'
        echo "resolution=${TAG} status=FAILED" >> "$SUMMARY_FILE"
        continue
    fi

    REAL_SECONDS=$(grep -m1 '^real ' "$TIME_LOG" | awk '{print $2}')
    ENCODED_BYTES=$(wc -c < "$ENCODED")
    FPS=$(awk -v f="$FRAMES" -v t="$REAL_SECONDS" 'BEGIN{ if (t>0) printf "%.3f", f/t; else print "nan" }')
    REALTIME_MARGIN=$(awk -v fps="$FPS" -v target="$FRAMERATE" 'BEGIN{ printf "%.3f", fps/target }')

    echo -e "  ${GREEN}✓ Encoded ${FRAMES} frames -> ${ENCODED_BYTES} bytes in ${REAL_SECONDS}s${NC}"
    echo -e "  ${BOLD}Throughput: ${FPS} fps${NC} (target ${FRAMERATE} fps => ${REALTIME_MARGIN}x real-time)"
    if awk -v m="$REALTIME_MARGIN" 'BEGIN{exit !(m>=1.0)}'; then
        echo -e "  ${GREEN}${BOLD}=> keeps up with real-time at ${FRAMERATE}fps${NC}"
    else
        echo -e "  ${RED}${BOLD}=> DOES NOT keep up with real-time at ${FRAMERATE}fps (${REALTIME_MARGIN}x)${NC}"
    fi

    {
        echo "resolution=${TAG}"
        echo "framerate_target=${FRAMERATE}"
        echo "frames=${FRAMES}"
        echo "wall_seconds=${REAL_SECONDS}"
        echo "fps=${FPS}"
        echo "realtime_margin=${REALTIME_MARGIN}"
        echo "encoded_bytes=${ENCODED_BYTES}"
    } >> "$SUMMARY_FILE"

    # ------------------------------------------------------------------
    # Per-stage GPU/CPU breakdown + per-frame variance, parsed straight out
    # of the driver's own stderr lines (no re-encoding, no re-running).
    # ------------------------------------------------------------------
    if [ "$BC250_PERF_STATS_MODE" = "1" ]; then
        GPU_LINES=$(grep -c '^\[BC250_PERF_GPU\]' "$LOG" || true)
        if [ "$GPU_LINES" -gt 0 ]; then
            echo -e "\n  ${BOLD}Per-stage GPU time (mean ms/frame, ${GPU_LINES} frames captured):${NC}"
            grep '^\[BC250_PERF_GPU\]' "$LOG" | \
                sed -E 's/.*type=([IP]) /type=\1 /' | \
                awk '
                {
                    ftype=""
                    for (i=1;i<=NF;i++) {
                        split($i,kv,"=")
                        if (kv[1]=="type") ftype=kv[2]
                        else if (kv[1] ~ /_ms$/) { sum[kv[1]]+=kv[2]; sum_by_type[kv[1] SUBSEP ftype]+=kv[2]; cnt_by_type[ftype]++ }
                    }
                    n++
                }
                END {
                    order["me_ms"]=1; order["predict_ms"]=2; order["dct_ms"]=3; order["quant_ms"]=4
                    order["reconstruct_ms"]=5; order["wavefront_ms"]=6; order["deblock_ms"]=7
                    order["entropy_ms"]=8; order["copy_ms"]=9; order["total_ms"]=10
                    for (k in order) keys[order[k]]=k
                    total_mean = (n>0 && sum["total_ms"]>0) ? sum["total_ms"]/n : 0
                    for (i=1;i<=10;i++) {
                        k=keys[i]
                        if (!(k in sum)) continue
                        mean = sum[k]/n
                        pct = (total_mean>0) ? (mean/total_mean*100) : 0
                        printf "    %-16s mean=%8.3f ms  (%5.1f%% of total_ms)\n", k, mean, pct
                    }
                    printf "    -- by frame type --\n"
                    for (ft in cnt_by_type) {
                        tot = sum_by_type["total_ms" SUBSEP ft]
                        c = cnt_by_type[ft]
                        if (c>0) printf "    type=%s frames=%d mean_total_ms=%.3f\n", ft, c, tot/c
                    }
                }'
        else
            echo -e "  ${YELLOW}! No [BC250_PERF_GPU] lines found in log - BC250_PERF_STATS instrumentation did not fire${NC}"
        fi

        FRAME_LINES=$(grep -c '^\[BC250_PERF_FRAME\]' "$LOG" || true)
        if [ "$FRAME_LINES" -gt 0 ]; then
            echo -e "\n  ${BOLD}Per-frame wall-time variance (encoder_h264.c's own clock, ${FRAME_LINES} frames):${NC}"
            grep '^\[BC250_PERF_FRAME\]' "$LOG" | grep -oP 'wall_ms=\K[0-9.]+' > "$WORK_DIR/wall_ms_${TAG}.txt"
            awk '
                { sum+=$1; n++; if(NR==1||$1<min)min=$1; if(NR==1||$1>max)max=$1; v[n]=$1 }
                END {
                    if (n==0) { print "    (no samples)"; exit }
                    mean=sum/n
                    for (i=1;i<=n;i++) sq+=(v[i]-mean)^2
                    sd=sqrt(sq/n)
                    printf "    n=%d mean_ms=%.3f min_ms=%.3f max_ms=%.3f stddev_ms=%.3f  (mean fps=%.2f)\n", n, mean, min, max, sd, (mean>0?1000.0/mean:0)
                }' "$WORK_DIR/wall_ms_${TAG}.txt"

            echo -e "    -- I-frame vs P-frame wall_ms --"
            grep '^\[BC250_PERF_FRAME\]' "$LOG" | awk '
                {
                    ftype=""; wall=0
                    for (i=1;i<=NF;i++) {
                        split($i,kv,"=")
                        if (kv[1]=="type") ftype=kv[2]
                        if (kv[1]=="wall_ms") wall=kv[2]
                    }
                    sum[ftype]+=wall; cnt[ftype]++
                    if (!(ftype in mn) || wall<mn[ftype]) mn[ftype]=wall
                    if (!(ftype in mx) || wall>mx[ftype]) mx[ftype]=wall
                }
                END {
                    for (t in cnt) printf "    type=%s n=%d mean_ms=%.3f min_ms=%.3f max_ms=%.3f\n", t, cnt[t], sum[t]/cnt[t], mn[t], mx[t]
                }'
        fi

        CPU_LINES=$(grep -c '^\[BC250_PERF_CPU\]' "$LOG" || true)
        if [ "$CPU_LINES" -gt 0 ]; then
            echo -e "\n  ${BOLD}CPU-side CAVLC+bitstream time (${CPU_LINES} frames):${NC}"
            grep '^\[BC250_PERF_CPU\]' "$LOG" | grep -oP 'cavlc_ms=\K[0-9.]+' | awk '
                { sum+=$1; n++ } END { if(n>0) printf "    mean=%.3f ms/frame\n", sum/n }'
        fi
    fi

    # ------------------------------------------------------------------
    # Optional software libx264 reference, same resolution/framerate/frame
    # count/bitrate, for calibration only.
    # ------------------------------------------------------------------
    if [ "$RUN_LIBX264_REFERENCE" = "1" ]; then
        echo -e "\n  ${BOLD}--- ${TAG} libx264 (preset=${LIBX264_PRESET}) software reference ---${NC}"
        REF_ENCODED="$WORK_DIR/ref_x264_${TAG}.mp4"
        REF_TIME_LOG="$WORK_DIR/ref_time_${TAG}.log"
        set +e
        { time -p ffmpeg -y -v error -f lavfi \
            -i "testsrc=size=${WIDTH}x${HEIGHT}:rate=${FRAMERATE}" -frames:v "$FRAMES" \
            -c:v libx264 -preset "$LIBX264_PRESET" -b:v "$BITRATE" \
            "$REF_ENCODED" ; } 2> "$REF_TIME_LOG"
        REF_RC=$?
        set -e
        if [ $REF_RC -eq 0 ] && [ -s "$REF_ENCODED" ]; then
            REF_SECONDS=$(grep -m1 '^real ' "$REF_TIME_LOG" | awk '{print $2}')
            REF_FPS=$(awk -v f="$FRAMES" -v t="$REF_SECONDS" 'BEGIN{ if (t>0) printf "%.3f", f/t; else print "nan" }')
            echo -e "  libx264 ${LIBX264_PRESET}: ${REF_FPS} fps (${REF_SECONDS}s for ${FRAMES} frames)"
            echo "libx264_${TAG}_fps=${REF_FPS}" >> "$SUMMARY_FILE"
        else
            echo -e "  ${YELLOW}! libx264 reference encode failed (exit $REF_RC)${NC}"
        fi
    fi
done

echo -e "\n${BLUE}======================================================${NC}"
echo -e "${BOLD}Summary (also in $SUMMARY_FILE):${NC}"
cat "$SUMMARY_FILE"
echo -e "${BLUE}======================================================${NC}"
echo -e "\nFull per-resolution ffmpeg+driver logs: $WORK_DIR/ffmpeg_<res>.log"
