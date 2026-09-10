#!/usr/bin/env bash
#
# bc250_lab.sh - closed-loop measurement harness for the BC-250 encoder.
#
# WHY THIS EXISTS
# ---------------
# The optimization work in docs/DEVLOG.md §19-§21 was driven by roughly a
# dozen throwaway scripts. The code changes held up; the *harness* is what
# repeatedly went wrong, in four distinct ways worth naming, because each is
# now a design constraint here:
#
#   1. A health check gated on a SEGV count in a time window and rolled back
#      a working build. Sunshine SEGVs in its own teardown path on nearly
#      every stop on this box (libevdev_uinput_destroy, _dl_fini), so the
#      signal fired on the good and bad case alike. (§20.5)
#   2. Byte-exactness was used as a correctness oracle on content this
#      encoder does not reproduce run-to-run, which made a correct change
#      look broken. (§19.6)
#   3. Deltas were reported with no noise floor, so "faster" and "within
#      variance" were indistinguishable.
#   4. Twelve copies of the same log parse, one of which sorted its input and
#      destroyed chronological order.
#
# So this harness: keys byte-exactness to a per-content determinism registry
# and refuses invalid comparisons; measures the noise floor and marks deltas
# inside it as not significant; delegates all parsing to
# bc250_lab_parse.py; and health-checks the live pid rather than counting
# strings in a window.
#
# It also adds the capability that was missing entirely: measuring the
# encoder under GPU or CPU contention. Every throughput figure published
# before this was taken on an idle machine, and real use turned out to be
# 11 fps against 60 when a game was running (§21.4).
#
# USAGE
#   ./bc250_lab.sh setup                     one-time: clone repo, make dirs
#   ./bc250_lab.sh build <ref|work>          build (cached by commit) -> key
#   ./bc250_lab.sh bench <key> [opts]        measure; TSV to stdout
#   ./bc250_lab.sh noise <key> [opts]        establish the run-to-run floor
#   ./bc250_lab.sh compare <keyA> <keyB>     A/B with significance testing
#   ./bc250_lab.sh exact <keyA> <keyB>       byte-exactness, deterministic only
#   ./bc250_lab.sh audit <key>               nonzero-mask exactness audit
#   ./bc250_lab.sh quality <key> [-r N]      PSNR/SSIM via quality_test.sh
#   ./bc250_lab.sh units <key>               unit-test binaries
#   ./bc250_lab.sh gate <key> [<baseKey>]    audit + units + quality + exact
#   ./bc250_lab.sh health                    is the live Sunshine healthy?
#   ./bc250_lab.sh deploy <key>              install + health-check + rollback
#   ./bc250_lab.sh rollback                  restore the previous driver
#
# BENCH/NOISE OPTIONS
#   --content=testsrc|testsrc2   default testsrc
#   --res=WxH                    default 2560x1440
#   --frames=N                   default 300
#   --gop=N                      default 120  (use 1 for all-intra)
#   --bitrate=X                  default 31M
#   --repeat=N                   default 1    (>1 gives real sd)
#   --load=none|gpu|cpu|both     default none
#   --env=K=V,K=V                extra driver env (e.g. BC250_NZ_MASK=0)
#   --audit                      also enable BC250_NZ_AUDIT=1
#
set -uo pipefail

LAB="${BC250_LAB_DIR:-/var/home/user/bc250-lab}"
REPO_URL="${BC250_LAB_REPO:-https://github.com/Shalasere/bc250-vulkan-encode-stopgap.git}"
REPO="$LAB/repo"
ART="$LAB/artifacts"
WORK="$LAB/work"
RUNS="$LAB/runs"
DRV_DIR="${BC250_DRV_DIR:-/opt/bc250-driver}"
BOX="${BC250_DISTROBOX:-driver-build}"
RENDER="${BC250_RENDER:-/dev/dri/renderD128}"
SUNSHINE_UNIT=app-dev.lizardbyte.app.Sunshine.service
PARSE="$LAB/bc250_lab_parse.py"

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/1000/bus}"

# ---------------------------------------------------------------------------
# CONTENT DETERMINISM REGISTRY
#
# This encoder is NOT bit-reproducible on moving content: three runs of an
# identical configuration produce three different (valid) bitstreams, ~0.02%
# apart in size, most likely from GPU-side tie-breaking in motion estimation
# (§19.6). It IS reproducible on content that pins at qp_min.
#
# Byte-exactness is therefore only a valid oracle for the first list. `exact`
# refuses anything else rather than producing a meaningless verdict - which
# is what happened when this was left to judgement.
# ---------------------------------------------------------------------------
DETERMINISTIC_CONTENT="testsrc"
NONDETERMINISTIC_CONTENT="testsrc2"

is_deterministic() {
    case " $DETERMINISTIC_CONTENT " in *" $1 "*) return 0;; esac
    return 1
}

die()  { echo "lab: $*" >&2; exit 1; }
note() { echo "# $*" >&2; }

# ---------------------------------------------------------------------------
setup() {
    mkdir -p "$LAB" "$ART" "$WORK" "$RUNS"
    if [ ! -d "$REPO/.git" ]; then
        note "cloning $REPO_URL"
        git clone --quiet "$REPO_URL" "$REPO" || die "clone failed"
    fi
    # The parser lives next to this script in-tree; copy it where the board
    # can always find it regardless of which build tree is being tested.
    local here; here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [ -f "$here/bc250_lab_parse.py" ] && [ "$here/bc250_lab_parse.py" != "$PARSE" ]; then
        cp -f "$here/bc250_lab_parse.py" "$PARSE"
    fi
    [ -f "$PARSE" ] || die "bc250_lab_parse.py not found (looked in $here and $PARSE)"
    chmod +x "$PARSE"
    note "lab ready at $LAB"
    note "  repo:      $REPO"
    note "  artifacts: $ART"
    note "  parser:    $PARSE"
}

# build <ref|work> -> prints the artifact key
# Cached: a ref that resolves to an already-built commit is not rebuilt, so
# baselines in an A/B cost nothing after the first time.
build() {
    local ref="${1:?build <ref|work>}"
    local key src
    if [ "$ref" = work ]; then
        # Working tree shipped by the dev side as $LAB/work-src.tar.gz.
        [ -f "$LAB/work-src.tar.gz" ] || die "no $LAB/work-src.tar.gz (ship the working tree first)"
        key="work-$(md5sum "$LAB/work-src.tar.gz" | cut -c1-12)"
        src="$WORK/$key"
        if [ ! -d "$ART/$key" ]; then
            rm -rf "$src"; mkdir -p "$src"
            tar xzf "$LAB/work-src.tar.gz" -C "$src" || die "extract failed"
        fi
    else
        [ -d "$REPO/.git" ] || die "run 'setup' first"
        git -C "$REPO" fetch --quiet --all --tags 2>/dev/null
        local sha; sha=$(git -C "$REPO" rev-parse --short=12 "$ref^{commit}" 2>/dev/null) \
            || die "cannot resolve ref '$ref'"
        key="$sha"
        src="$WORK/$key"
        if [ ! -d "$ART/$key" ]; then
            rm -rf "$src"; mkdir -p "$src"
            git -C "$REPO" archive "$sha" | tar x -C "$src" || die "archive failed"
        fi
    fi

    if [ -d "$ART/$key" ] && [ -f "$ART/$key/bc250_drv_video.so" ]; then
        echo "$key"; return 0
    fi

    local bdir="$src/approach1-compute-encoder/build"
    mkdir -p "$bdir"
    note "building $key (this is not cached yet)"
    distrobox enter "$BOX" -- bash -lc \
        "cd '$bdir' && cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON .. >/dev/null 2>&1 && make -j12 >/dev/null 2>&1" \
        >/dev/null 2>&1
    [ -f "$bdir/bc250_drv_video.so" ] || die "build failed for $key"

    mkdir -p "$ART/$key"
    cp -f "$bdir/bc250_drv_video.so" "$ART/$key/"
    cp -f "$bdir"/*.comp.spv "$ART/$key/" 2>/dev/null
    mkdir -p "$ART/$key/tests"
    cp -f "$bdir"/tests/test_* "$ART/$key/tests/" 2>/dev/null
    echo "$key" > "$ART/$key/key"
    md5sum "$ART/$key/bc250_drv_video.so" | awk '{print $1}' > "$ART/$key/md5"
    # A build whose shaders did not land silently runs new C against old
    # SPIR-V; refuse to cache that.
    local nspv; nspv=$(ls "$ART/$key"/*.comp.spv 2>/dev/null | wc -l)
    [ "$nspv" -ge 9 ] || die "only $nspv shaders in $key - refusing to cache a partial build"
    echo "$key"
}

art_dir() {
    local key="${1:?}"
    [ -d "$ART/$key" ] || die "unknown build key '$key' (run build first)"
    echo "$ART/$key"
}

# ---------------------------------------------------------------------------
# LOAD GENERATORS
#
# gpu:  ffmpeg's nlmeans_vulkan, a heavy Vulkan compute denoiser. Chosen
#       because it loads the shader cores and memory system WITHOUT touching
#       this VA-API driver and without doing any CPU entropy coding, so GPU
#       contention can be separated from CPU/bandwidth contention. Nothing
#       like vkmark/glmark2 is installed on this board.
# cpu:  busy loops, one per core-ish, no dependencies.
#
# Note gpu_busy_percent is NOT supported on this device, so there is no way
# to confirm a target utilisation - the load is characterised by its own
# achieved throughput instead, reported as load_fps.
# ---------------------------------------------------------------------------
LOAD_PIDS=()

start_load() {
    local kind="$1" outdir="$2"
    LOAD_PIDS=()
    case "$kind" in
        none) return 0;;
        gpu|both)
            ( while :; do
                ffmpeg -hide_banner -v error -init_hw_device vulkan=vk:0 -filter_hw_device vk \
                  -f lavfi -i testsrc2=size=1920x1080:rate=60 -frames:v 600 \
                  -vf 'format=nv12,hwupload,nlmeans_vulkan,hwdownload,format=nv12' \
                  -f null - 2>/dev/null
              done ) & LOAD_PIDS+=($!)
            ;;
    esac
    case "$kind" in
        cpu|both)
            local n; n=$(( $(nproc) / 2 )); [ "$n" -lt 1 ] && n=1
            for _ in $(seq 1 "$n"); do
                ( while :; do :; done ) & LOAD_PIDS+=($!)
            done
            ;;
    esac
    [ "$kind" = none ] || { note "load=$kind started (pids ${LOAD_PIDS[*]})"; sleep 5; }
}

stop_load() {
    [ "${#LOAD_PIDS[@]}" -eq 0 ] && return 0
    for p in "${LOAD_PIDS[@]}"; do
        pkill -P "$p" 2>/dev/null
        kill "$p" 2>/dev/null
    done
    pkill -f 'nlmeans_vulkan' 2>/dev/null
    LOAD_PIDS=()
    sleep 2
}
trap stop_load EXIT INT TERM

# ---------------------------------------------------------------------------
# one encode run -> log file
run_encode() {
    local key="$1" content="$2" res="$3" frames="$4" gop="$5" bitrate="$6" \
          envs="$7" audit="$8" out="$9"
    local bd; bd=$(art_dir "$key")
    local -a envv=(BC250_PERF_STATS=1)
    [ "$audit" = 1 ] && envv+=(BC250_NZ_AUDIT=1)
    if [ -n "$envs" ]; then
        local IFS=,; for kv in $envs; do [ -n "$kv" ] && envv+=("$kv"); done
    fi
    env LIBVA_DRIVER_NAME=bc250 LIBVA_DRIVERS_PATH="$bd" BC250_SHADER_DIR="$bd" \
        "${envv[@]}" \
        ffmpeg -y -v info -f lavfi -i "${content}=size=${res}:rate=60" \
        -frames:v "$frames" -g "$gop" -vaapi_device "$RENDER" \
        -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v "$bitrate" \
        -f h264 "${out}.h264" > "${out}.log" 2>&1
    echo $?
}

BENCH_FIELDS="tag,p_wall_ms,p_wall_ms_sd,p_fps_ceiling,cavlc_ms,shadow_ms,gpu_total_ms,gpu_me_ms,gpu_copy_ms,unaccounted_ms,unaccounted_pct,qp,bytes_p,frames_p,err_vk_oom,err_alloc_failed,err_slice_overflow"

bench() {
    local key="${1:?bench <key> [opts]}"; shift
    local content=testsrc res=2560x1440 frames=300 gop=120 bitrate=31M
    local repeat=1 load=none envs="" audit=0 quiet=0
    for a in "$@"; do
        case "$a" in
            --content=*) content="${a#*=}";;
            --res=*)     res="${a#*=}";;
            --frames=*)  frames="${a#*=}";;
            --gop=*)     gop="${a#*=}";;
            --bitrate=*) bitrate="${a#*=}";;
            --repeat=*)  repeat="${a#*=}";;
            --load=*)    load="${a#*=}";;
            --env=*)     envs="${a#*=}";;
            --audit)     audit=1;;
            --quiet)     quiet=1;;
            *) die "bench: unknown option '$a'";;
        esac
    done
    local stamp; stamp=$(date +%Y%m%d-%H%M%S)
    local outdir="$RUNS/$stamp-$key-$content-$res-$load"
    mkdir -p "$outdir"

    start_load "$load" "$outdir"
    local first=1
    for i in $(seq 1 "$repeat"); do
        local base="$outdir/run$i"
        local rc; rc=$(run_encode "$key" "$content" "$res" "$frames" "$gop" \
                                  "$bitrate" "$envs" "$audit" "$base")
        if [ "$rc" != 0 ]; then
            note "ENCODE FAILED (rc=$rc) run$i - see $base.log"; tail -5 "$base.log" >&2
            continue
        fi
        local tag="$key/$content/$res/load=$load/r$i"
        if [ "$first" = 1 ] && [ "$quiet" = 0 ]; then
            python3 "$PARSE" "$base.log" --tsv --header --tag "$tag" --fields "$BENCH_FIELDS"
            first=0
        else
            python3 "$PARSE" "$base.log" --tsv --tag "$tag" --fields "$BENCH_FIELDS"
        fi
        python3 "$PARSE" "$base.log" --tag "$tag" > "$base.json"
    done
    stop_load
    echo "$outdir" > "$LAB/.last_run"
    note "artifacts: $outdir"
}

# noise <key> - the floor any claimed delta must clear.
noise() {
    local key="${1:?noise <key> [opts]}"; shift
    local n=5
    local -a passthru=()
    for a in "$@"; do
        case "$a" in --repeat=*) n="${a#*=}";; *) passthru+=("$a");; esac
    done
    note "noise floor: $n identical runs of $key"
    local tmp; tmp=$(mktemp)
    bench "$key" --repeat="$n" --quiet "${passthru[@]}" > "$tmp"
    python3 - "$tmp" "$BENCH_FIELDS" <<'PY'
import sys, statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().strip().splitlines() if l.strip()]
if not rows: print("no rows"); sys.exit(1)
F=sys.argv[-1].split(",")
idx={k:i for i,k in enumerate(F)}
print(f"{'metric':<16}{'mean':>12}{'sd':>10}{'sd%':>8}{'min':>12}{'max':>12}  n={len(rows)}")
for m in ("p_wall_ms","p_fps_ceiling","cavlc_ms","shadow_ms","gpu_total_ms","bytes_p"):
    try: v=[float(r[idx[m]]) for r in rows if r[idx[m]] not in ('','None')]
    except Exception: continue
    if not v: continue
    mean=st.mean(v); sd=st.stdev(v) if len(v)>1 else 0.0
    pct=(100*sd/mean) if mean else 0.0
    print(f"{m:<16}{mean:>12.4f}{sd:>10.4f}{pct:>7.2f}%{min(v):>12.4f}{max(v):>12.4f}")
print()
print("Any A/B delta smaller than ~2x the sd% above is not a result.")
PY
    rm -f "$tmp"
}

# compare <keyA> <keyB> - A/B with a significance verdict.
compare() {
    local a="${1:?compare <keyA> <keyB> [opts]}" b="${2:?}"; shift 2
    local reps=3
    local -a passthru=()
    for x in "$@"; do
        case "$x" in --repeat=*) reps="${x#*=}";; *) passthru+=("$x");; esac
    done
    local ta tb; ta=$(mktemp); tb=$(mktemp)
    note "A=$a  B=$b  (${reps} runs each, interleaved to spread thermal drift)"
    # Interleaved rather than all-A-then-all-B: the board warms up, and a
    # block design would charge that entirely to B.
    for i in $(seq 1 "$reps"); do
        bench "$a" --repeat=1 --quiet "${passthru[@]}" >> "$ta"
        bench "$b" --repeat=1 --quiet "${passthru[@]}" >> "$tb"
    done
    python3 - "$ta" "$tb" "$a" "$b" "$BENCH_FIELDS" <<'PY'
import sys, statistics as st
F=sys.argv[-1].split(",")
idx={k:i for i,k in enumerate(F)}
def load(p):
    return [l.split('\t') for l in open(p).read().strip().splitlines() if l.strip()]
A,B=load(sys.argv[1]),load(sys.argv[2])
na,nb=sys.argv[3],sys.argv[4]
def col(rows,m):
    out=[]
    for r in rows:
        try: out.append(float(r[idx[m]]))
        except Exception: pass
    return out
for m in ("err_vk_oom","err_alloc_failed","err_slice_overflow"):
    for rows,nm in ((A,na),(B,nb)):
        v=col(rows,m)
        if v and sum(v)>0: print(f"!! {nm}: {m} = {int(sum(v))} - investigate before trusting anything below")
print(f"{'metric':<16}{na[:10]:>12}{nb[:10]:>12}{'delta':>11}{'delta%':>9}   verdict")
for m in ("p_wall_ms","p_fps_ceiling","cavlc_ms","shadow_ms","gpu_total_ms","bytes_p"):
    va,vb=col(A,m),col(B,m)
    if not va or not vb: continue
    ma,mb=st.mean(va),st.mean(vb)
    sa=st.stdev(va) if len(va)>1 else 0.0
    sb=st.stdev(vb) if len(vb)>1 else 0.0
    d=mb-ma; dp=(100*d/ma) if ma else 0.0
    noise=2*max(sa,sb)
    verdict="SIGNIFICANT" if abs(d)>noise and noise>0 else ("within noise" if noise>0 else "no spread (repeat>1 needed)")
    print(f"{m:<16}{ma:>12.4f}{mb:>12.4f}{d:>+11.4f}{dp:>+8.2f}%   {verdict}")
print()
print("delta = B - A. 'within noise' means |delta| <= 2x the larger sd; do not report it as a win.")
PY
    rm -f "$ta" "$tb"
}

# exact <keyA> <keyB> - byte-exactness, deterministic content only.
exact() {
    local a="${1:?exact <keyA> <keyB>}" b="${2:?}"; shift 2
    local res=2560x1440 frames=200
    for x in "$@"; do case "$x" in --res=*) res="${x#*=}";; --frames=*) frames="${x#*=}";; esac; done
    local pass=0 fail=0
    printf "%-34s %s\n" "case" "result"
    for content in $DETERMINISTIC_CONTENT; do
        for gop in 120 1 10; do
            local base="$RUNS/exact-$(date +%s)-$gop"
            local ra rb; ra=$(run_encode "$a" "$content" "$res" "$frames" "$gop" 31M "" 0 "${base}A")
            rb=$(run_encode "$b" "$content" "$res" "$frames" "$gop" 31M "" 0 "${base}B")
            local label="$content/$res/gop=$gop"
            if [ "$ra" != 0 ] || [ "$rb" != 0 ]; then
                printf "%-34s ENCODE FAILED (A=%s B=%s)\n" "$label" "$ra" "$rb"; fail=$((fail+1))
            elif cmp -s "${base}A.h264" "${base}B.h264"; then
                printf "%-34s byte-identical\n" "$label"; pass=$((pass+1))
            else
                printf "%-34s *** DIFFERS *** (%s vs %s bytes)\n" "$label" \
                    "$(wc -c < "${base}A.h264")" "$(wc -c < "${base}B.h264")"; fail=$((fail+1))
            fi
            rm -f "${base}A.h264" "${base}B.h264"
        done
    done
    echo
    echo "pass=$pass fail=$fail"
    for c in $NONDETERMINISTIC_CONTENT; do
        echo "note: '$c' deliberately NOT compared - this encoder is not bit-reproducible on"
        echo "      moving content (DEVLOG §19.6), so a byte diff there means nothing."
    done
    [ "$fail" -eq 0 ]
}

audit() {
    local key="${1:?audit <key>}"; shift
    local res=2560x1440 frames=200
    for x in "$@"; do case "$x" in --res=*) res="${x#*=}";; --frames=*) frames="${x#*=}";; esac; done
    local ok=0
    for content in $DETERMINISTIC_CONTENT $NONDETERMINISTIC_CONTENT; do
        local base="$RUNS/audit-$(date +%s)-$content"
        local rc; rc=$(run_encode "$key" "$content" "$res" "$frames" 10 31M "" 1 "$base")
        [ "$rc" != 0 ] && { echo "$content: ENCODE FAILED rc=$rc"; ok=1; continue; }
        python3 "$PARSE" "$base.log" \
          | python3 -c 'import json,sys; d=json.load(sys.stdin); print("%-10s frames=%s mismatched_frames=%s total_mismatches=%s all_zero=%.1f%% ac_zero=%.1f%%" % (sys.argv[1], d.get("audit_frames"), d.get("audit_mismatch_frames"), d.get("audit_mismatches_total"), d.get("audit_all_zero_pct",0), d.get("audit_ac_zero_pct",0)))' "$content"
        local mm; mm=$(python3 "$PARSE" "$base.log" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("audit_mismatch_frames",1))')
        [ "$mm" = 0 ] || ok=1
        rm -f "$base.h264"
    done
    [ "$ok" -eq 0 ] && echo "=> mask EXACT on all audited frames" || echo "=> MASK MISMATCH - do not ship"
    return $ok
}

units() {
    local key="${1:?units <key>}"
    local bd; bd=$(art_dir "$key")
    local ok=0
    for t in test_bitstream test_cavlc test_encode test_va_api; do
        if [ ! -x "$bd/tests/$t" ]; then echo "  MISSING $t"; ok=1; continue; fi
        if ( cd "$bd" && LIBVA_DRIVER_NAME=bc250 LIBVA_DRIVERS_PATH="$bd" \
             BC250_SHADER_DIR="$bd" timeout 300 "./tests/$t" >/tmp/lab_$t.out 2>&1 ); then
            echo "  PASS $t"
        else
            echo "  FAIL $t"; tail -8 /tmp/lab_$t.out | sed 's/^/       /'; ok=1
        fi
    done
    return $ok
}

quality() {
    local key="${1:?quality <key>}"; shift
    local reps=2
    for x in "$@"; do case "$x" in -r|--repeat=*) reps="${x#*=}";; esac; done
    local bd; bd=$(art_dir "$key")
    [ -f "$REPO/tools/quality_test.sh" ] || die "quality_test.sh not in $REPO (run setup)"
    local ok=1
    for i in $(seq 1 "$reps"); do
        local out; out=$(cd "$REPO" && SKIP_BUILD=1 BUILD_DIR="$bd" \
            LIBVA_DRIVER_NAME=bc250 LIBVA_DRIVERS_PATH="$bd" BC250_SHADER_DIR="$bd" \
            timeout 900 bash tools/quality_test.sh 2>&1)
        local psnr ssim verdict
        psnr=$(echo "$out" | grep -a -m1 'PSNR y:' | grep -oP 'average:\K[0-9.]+')
        ssim=$(echo "$out" | grep -a -m1 'SSIM Y:' | grep -oP 'All:\K[0-9.]+')
        verdict=$(echo "$out" | grep -aoE 'PASS|FAIL' | head -1)
        printf "  run%d PSNR=%-11s SSIM=%-10s %s\n" "$i" "${psnr:-na}" "${ssim:-na}" "${verdict:-?}"
        [ "$verdict" = PASS ] && ok=0
    done
    return $ok
}

gate() {
    local key="${1:?gate <key> [<baselineKey>]}" base="${2:-}"
    local rc=0
    echo "=== units ==="   ; units "$key"   || rc=1
    echo "=== audit ==="   ; audit "$key"   || rc=1
    echo "=== quality ===" ; quality "$key" || rc=1
    if [ -n "$base" ]; then
        echo "=== byte-exactness vs $base ==="; exact "$base" "$key" || rc=1
    else
        echo "=== byte-exactness: skipped (no baseline key given) ==="
    fi
    echo
    [ "$rc" -eq 0 ] && echo "GATE PASS" || echo "GATE FAIL"
    return $rc
}

# ---------------------------------------------------------------------------
# HEALTH
#
# Keyed to the LIVE pid, not to a count of strings in a time window. Sunshine
# SEGVs in its own teardown path (libevdev_uinput_destroy / inputtino, and
# _dl_fini via exit) on nearly every stop on this box - unrelated to the
# VA-API driver - so a SEGV count fires identically for healthy and broken
# builds and rolled back a working one (§20.5).
# ---------------------------------------------------------------------------
health() {
    local pid; pid=$(pgrep -f '/usr/bin/sunshine' | head -1)
    if [ -z "$pid" ]; then echo "health: FAIL - no sunshine process"; return 1; fi
    local j; j=$(journalctl --user -u "$SUNSHINE_UNIT" -n 600 --no-pager 2>&1 \
                 | grep -F "sunshine[$pid]")
    local enc oom af act up
    enc=$(echo "$j" | grep -ac 'Found H.264 encoder')
    oom=$(echo "$j" | grep -ac 'Vulkan error -2')
    af=$(echo "$j" | grep -ac 'allocate_encoding_buffers')
    act=$(systemctl --user is-active "$SUNSHINE_UNIT")
    up=$(ps -o etimes= -p "$pid" 2>/dev/null | tr -d ' ')
    echo "health: pid=$pid uptime=${up}s unit=$act encoder_found=$enc vk_oom=$oom alloc_failed=$af"
    [ "$enc" -ge 1 ] && [ "$oom" -eq 0 ] && [ "$af" -eq 0 ] && [ "$act" = active ] \
        && { echo "health: PASS"; return 0; }
    echo "health: FAIL"; return 1
}

wake_display() {
    # Sunshine reads a slept output as 0x0 and returns Error 503 to the client
    # on every app launch, not just at startup (§14.4). Nudge before restart.
    pgrep -x ydotoold >/dev/null || { nohup ydotoold >/tmp/ydotoold.log 2>&1 & sleep 1; }
    for _ in 1 2 3; do
        ydotool mousemove -x 20 -y 12 2>/dev/null; sleep 0.2
        ydotool mousemove -x -20 -y -12 2>/dev/null; sleep 0.2
    done
}

deploy() {
    local key="${1:?deploy <key>}"
    local bd; bd=$(art_dir "$key")
    sudo cp -f "$DRV_DIR/bc250_drv_video.so" "$DRV_DIR/bc250_drv_video.so.prev" 2>/dev/null
    sudo cp -f "$bd/bc250_drv_video.so" "$DRV_DIR/bc250_drv_video.so"
    sudo chmod 755 "$DRV_DIR/bc250_drv_video.so"
    # Shaders MUST go with the .so - new C against old SPIR-V is a silent
    # wrong-results failure, not a load error.
    for s in "$bd"/*.comp.spv; do sudo cp -f "$s" "$DRV_DIR/"; done
    sudo chmod 644 "$DRV_DIR"/*.comp.spv
    echo "deployed $key md5=$(md5sum "$DRV_DIR/bc250_drv_video.so" | awk '{print $1}')"
    wake_display
    systemctl --user reset-failed "$SUNSHINE_UNIT" 2>/dev/null
    systemctl --user stop "$SUNSHINE_UNIT"; sleep 1
    pkill -f '/usr/bin/sunshine' 2>/dev/null; sleep 3
    systemctl --user start "$SUNSHINE_UNIT"; sleep 15
    if health; then echo "DEPLOY OK ($key)"; return 0; fi
    echo "DEPLOY FAILED - rolling back"; rollback; return 1
}

rollback() {
    [ -f "$DRV_DIR/bc250_drv_video.so.prev" ] || die "no .prev driver to roll back to"
    sudo cp -f "$DRV_DIR/bc250_drv_video.so.prev" "$DRV_DIR/bc250_drv_video.so"
    echo "rolled back to md5=$(md5sum "$DRV_DIR/bc250_drv_video.so" | awk '{print $1}')"
    echo "NOTE: shaders were NOT rolled back - redeploy a known-good key if they changed."
    wake_display
    systemctl --user reset-failed "$SUNSHINE_UNIT" 2>/dev/null
    systemctl --user restart "$SUNSHINE_UNIT"; sleep 15
    health
}

usage() { sed -n '2,60p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; }

cmd="${1:-}"; [ $# -gt 0 ] && shift
case "$cmd" in
    setup)    setup "$@";;
    build)    build "$@";;
    bench)    bench "$@";;
    noise)    noise "$@";;
    compare)  compare "$@";;
    exact)    exact "$@";;
    audit)    audit "$@";;
    units)    units "$@";;
    quality)  quality "$@";;
    gate)     gate "$@";;
    health)   health "$@";;
    deploy)   deploy "$@";;
    rollback) rollback "$@";;
    ""|-h|--help|help) usage;;
    *) die "unknown command '$cmd' (try --help)";;
esac
