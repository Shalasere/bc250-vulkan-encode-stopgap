# Measuring performance

**`tools/lab` is the canonical way to measure this encoder.** If you are
about to write an `ffmpeg` command to find out whether something got
faster, stop and use the harness instead.

That is not a style preference. Roughly a dozen throwaway measurement
scripts drove the optimisation work in `docs/DEVLOG.md` §19–§21, and the
code changes mostly held up — it was the *harness* that kept being wrong,
in ways that each produced a confident, specific, false number. Every
rule below is one of those, encoded so it cannot recur. The most recent
example is the smallest: `tools/benchmark.sh`, removed in this repo's
history, generated its source clip without `-pix_fmt`, wrote a zero-byte
file, and reported two fast timings for two encodes that never ran.

## The short version

```bash
tools/lab setup                       # once per board
tools/lab build work                  # or: build local:<unpushed-ref>  -> prints a <key>
tools/lab noise <key> --repeat=5      # establish the floor BEFORE measuring
tools/lab compare <keyA> <keyB>       # significance-tested A/B
tools/lab scoreboard <key>            # the headline: vs libx264, per load condition
tools/lab drift <key>                 # correctness: recon vs a REAL decoder
tools/lab gate <key> [<baseKey>]      # units + audit + PSNR + drift + byte-exactness
tools/lab deploy <key>                # health-checked, auto-rollback
```

`tools/lab --help` lists every command without touching the board.
`tools/lab help` asks the harness itself and includes every bench option.

## Why it runs the way it does

`tools/lab` runs on your dev machine; `tools/bc250_lab.sh` is the half
that runs on the board. `lab` ships the harness every invocation, so the
board can never run a stale copy.

Two things about it are load-bearing:

**Commands travel as files, never as inline strings.** PowerShell mangles
inline pipes, quotes and `$vars` before WSL ever sees them, and `ssh -n`
is mandatory because ssh inside a pipeline otherwise eats stdin and
swallows the rest of the calling script — while `-n` also nulls stdin, so
heredocs vanish. Two real bugs came from exactly this.

**`build local:<ref>` ships an unpushed commit.** Without it, measuring a
commit means pushing it first, which couples "I want a number" to "the
world sees this". The board content-addresses the tarball, so a given ref
gets a stable, cached key.

## The rules the harness enforces

These are the expensive ones. They are listed here because knowing *why*
the harness refuses something is what stops you from working around it.

**Do not size a task off a small gprof percentage.** gprof on this
codebase has misattributed three times in one session. Its sample bucket
here is 10 ms against a ~0.6 s run, so a *single* sample reads as ~1.7%
and the same bucket on a shorter run inflates to 5.9% — which is exactly
how `bs_rbsp_to_ebsp` got scoped as a 5.9% hotspot when direct
`clock_gettime` instrumentation puts it at 0.93%. It also reported
`hevc_sbac_init_state` at 7.9M calls (reachable ~810 times — symbol
misattribution under `-O2`), and put 17.7% on a function whose most
expensive-looking instruction turned out to be free. Treat gprof as a
pointer to *where to instrument*, then instrument the function directly
before committing to the work.

**Measure at the optimisation level the driver actually ships.** CMake
appends `-O3 -DNDEBUG` via `CMAKE_C_FLAGS_RELEASE` after the `-O2` in
`CMAKE_C_FLAGS`, and auto-detects `-march=znver2 -mtune=znver2` on real
BC-250 hardware. A `-O2` figure — which is what a hand-rolled `gcc -O2`
harness and `tools/hevc_host_drift.sh` produce — can be wrong in *either
direction*, and both have happened here:

| change | `-O2` | `-O3` | `-O3 -march=znver2` |
|---|---|---|---|
| partial-butterfly 4x4 transforms | +15.4% | +5.3% | **+2.1%** |
| zorder shifts + gather hoist + CABAC inline | +29.4% | +40.3% | **+44.5%** |

The pattern is not random. The butterfly is largely what `-O3`'s
vectoriser already derives, so hand-writing it buys little once the
vectoriser is on. Removing integer divisions, call overhead and
redundant work is *not* something the optimiser can do for you, so those
wins survive and can even grow as the surrounding code gets cheaper.
Quote the shipped number; if you cite a `-O2` one, say so.

**Validate the A/B rig before believing the A/B.** A benchmark harness
that cannot report 1.00x when both sides are the same code cannot report
anything else either. One rig here was wrong twice on identical code —
0.74x because GCC cloned and constant-propagated an in-translation-unit
reference while the real call stayed opaque, then 1.30x the other way
from pure hot-loop alignment luck (`-falign-functions=64
-falign-loops=32` settled it). Both errors were ~30%, larger than most
wins you would be trying to measure.

**No delta under ~2.5% of wall time is a result** from a single run.
Measure the floor first — at 1440p it is `p_wall` sd 1.2%, `cavlc` 1.6%,
`shadow` 3.1%, `gpu_total` 0.09%. `lab compare` marks anything inside the
floor as not significant rather than reporting it as a win.

**Byte-exactness is a valid oracle only on `testsrc`, and only all-intra
(`-g 1`).** This encoder is not bit-reproducible on moving content —
three runs of one config give three different valid bitstreams. Using
md5 on `testsrc2` once made a *correct* change look broken and nearly got
it reverted (§19.6). The GPU motion-estimation non-determinism reaches
plain `testsrc` too as soon as P-frames exist, confirmed by running one
unmodified binary against itself and getting four different md5s (§26.5).
`lab exact` keys this to a per-content determinism registry and refuses
invalid comparisons instead of trusting you to remember.

**There is a second, CPU-only way to lose byte-exactness, and it has
nothing to do with the GPU.** In every non-CQP rate-control mode,
`rc_update_stats()` drains its leaky bucket by `target_bitrate × real
elapsed seconds` from `CLOCK_MONOTONIC`, so the bitstream becomes a
function of how fast the machine ran. `tests/test_encode` was silently
non-deterministic for this reason — `h264_encoder_create()` uses
`RC_LOW_LATENCY`, while `hevc_encoder_create()` uses `RC_CQP` and hits
the early return, which is the whole reason `test_hevc_encode` was
reproducible and `test_encode` was not.

The scale is worth internalising: **one byte in 350,979**, a
`slice_qp_delta` whose `se(v)` kept the same code length, so the file
size, the per-frame byte counts and the stdout logs were all identical.
Only a full md5 showed it. It is fixed in the test (via the existing
`BC250_RC_NOMINAL_DRAIN=1` hook), but the mechanism is still live in any
harness that encodes at a real bitrate target — if you build one, pin
CQP or set that hook before trusting a byte comparison.

**Name the load condition, every time.** Every throughput figure
published before the harness existed was taken on an idle GPU. Under
`--load=gpu` (ffmpeg `nlmeans_vulkan`) 1440p goes **66.2 → 1.48 fps** —
but that generator is a pathologically heavy compute filter, so it is a
synthetic worst case, not "what a game does". There is **no trustworthy
real-game figure**; the often-repeated "a game took 1440p from 60 to 11
fps" is unsourced and collides with a number §12.4 retracted as a
debug-I/O artifact. State the condition *and* what produced it.

**The bar is libx264, not the previous commit.** Software encoding barely
notices a game, while GPU contention costs this encoder up to ~45×. A
change that beats last week's build and still loses to x264 under load
has not achieved anything. `lab scoreboard` is the scoreboard.

**Never gate health on a SEGV count.** Sunshine SEGVs in its own teardown
path on nearly every stop on this box, so that signal fires identically
for healthy and broken builds — it once rolled back a working driver
(§20.5). `lab health` keys on the live pid.

**Never PSNR-compare a raw `.h264` against a fresh `-f lavfi` source.** A
container-less stream has no reliable timing for `-lavfi psnr`'s frame
alignment; the drift compounds every frame and is indifferent to IDR
boundaries, which produced a false "catastrophic 21 dB quality gap"
(§22). The tell was that a fresh IDR should reset a real quality problem
and this one didn't. Decode both streams to raw YUV first, with identical
forced `-f rawvideo -s WxH -r N` framing. `lab qsweep` and `scoreboard
--quality` already do this correctly.

**Design the audit before depending on a new GPU→CPU data path.** The
per-block nonzero mask was silently wrong on every I-frame because
`intra_wavefront.comp` bypasses `quantize.comp`. `BC250_NZ_AUDIT=1`
recomputes it on the CPU and caught it (§19.4); `lab audit` runs it.

**Correctness needs an oracle that does not share our code.** PSNR
against the source says the picture is plausible. Byte-exactness against
a previous build says nothing changed. Comparing the GPU path against
our own CPU path says only that our two implementations agree — and they
can agree while both are wrong, which is exactly what happened: the GPU
chroma was quantized at QpY instead of QpC for its whole life, and a
"bit-exact" check against a CPU recomputation carrying the same omission
passed the entire time. `lab drift` compares the encoder's own
reconstruction against **ffmpeg's**, with the in-loop filter disabled
(`-skip_loop_filter all`; SAO is off in our SPS). In two days it found a
near-black picture at 5 dB, a QP-ordering bug, the chroma QP defect, and
a ~33 dB luma divergence in the CPU HEVC path that is still open. Run it
on anything that touches prediction, transform, quantization or entropy
coding.

And one that no harness can enforce for you: **an exact oracle is only
exact about what it compares.** The mask audit reported EXACT across 401
frames of genuinely corrupt output, because a one-frame slot swap meant
both buffers were read from the same wrong slot — they matched each other
perfectly while both being wrong. A performance counter caught it, not a
correctness check. Ask what a check actually compares, not how strict it
sounds.

## Tool inventory

| tool | status |
|---|---|
| `tools/lab` | **canonical.** Dev-machine entry point. |
| `tools/bc250_lab.sh` | **canonical.** The on-board half; `lab` ships it. |
| `tools/bc250_lab_parse.py` | component. All log parsing, so there is one parser rather than twelve. |
| `tools/quality_test.sh` | component. PSNR/SSIM, invoked by `lab quality`. |
| `tools/perf_test.sh` | **superseded.** `lab bench` does the same per-stage breakdown and adds a noise floor, load conditions and significance testing. Retained only because DEVLOG, `hevc_scope_note.md` and a CMakeLists comment cite it as the provenance of published numbers. |
| `tools/bc250_diagnose.sh` | user-facing probe, not a benchmark. This is what the issue and PR templates ask people to run. |
| `tools/benchmark.sh` | **deleted.** See this repo's history for why; it is the cautionary tale at the top of this page. |

## If the harness is genuinely missing something

Extend `bc250_lab.sh`. A new one-off script starts with none of the rules
above and, on this project's record, has roughly even odds of producing a
number that is confidently wrong — which is worse than no number, because
someone will act on it.
