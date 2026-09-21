# A6 mode-search-perf — coarse-then-refine for `hevc_choose_luma_mode()`

Status: **done.** Replaces A6's 35-candidate exhaustive RD-biased search with
a coarse-then-refine search (Planar, DC, a step-4 angular grid, +/-1/+/-2
refinement, always the 3 real MPMs). Measured off-board: **1.45x-1.82x
speedup** over the exhaustive search (content-dependent), byte-exactness
preserved (`tools/hevc_host_drift.sh`: PASS, 53/53), RD-quality cost small
and isolated to one of nine sample points. Recommendation: **ship it** — see
"Verdict" at the end.

## 1. Instrumentation first

Per `docs/performance-measurement.md` and `CLAUDE.md`, this used
`tools/hevc_bench.c`'s existing `hevc_bench_prof profile` ablation
infrastructure, extended rather than replaced with a hand-rolled script.

**What was added** (`approach1-compute-encoder/src/hevc_intra.c`,
`tools/hevc_bench.c`, both guarded by `-DHEVC_INTRA_PROFILE` so the shipped
build is untouched — confirmed by checksum: `hevc_bench`'s output checksum
on `640x480 qp=27 pattern=3 frames=8` was `7f46abef1fc8cffa` both before and
after adding the probes): `HEVC_PROF_BRACKET()`, an rdtsc bracket around one
call site, with its own accumulator pair (cycles, calls). Three probes were
added inside the mode-search loop: `predict_block4()` for Planar/DC,
`predict_block4()` for angular modes, and `sad_4x4()`. `hevc_bench.c`'s
`profile` command now prints all three as a percentage of the run, alongside
the existing whole-function ablation and rdtsc estimate.

**Caveats stated up front, same discipline the existing rdtsc estimate
already uses**: every probe includes its own overhead (an upper bound, not
exact), and the three rows are independently bracketed so they need not sum
to the whole-function total. The point of the breakdown is not an absolute
figure — it's the *ratio* between the same call sites on two different
builds, since both builds bracket identical code.

**A/A floor, established before trusting anything**: the `profile` command's
built-in A/A control (same code, same shape, ABBA-ordered) came back
`-1.16%` to `+1.41%` across four baseline configurations (640x480,
pattern 2/3, QP 12/27) — well under `docs/performance-measurement.md`'s
~2.5%-of-wall-time floor. The rig is trustworthy.

### Baseline breakdown (exhaustive-35, before this change)

`640x480`, `frames=8`, `gop=1`, `samples=9`, two content patterns, two QPs:

| pattern | qp | whole search = % of encode | planar/dc predict | angular predict | sad_4x4 | remainder (RD arith + loop + one-time gather/hoist) |
|---|---|---|---|---|---|---|
| 2 (diag ramp) | 12 | 78.4% | 1.13% | **36.57%** | 13.98% | 33.02% |
| 2 (diag ramp) | 27 | 81.4% | 1.09% | **36.87%** | 14.50% | 33.98% |
| 3 (pseudo-random) | 12 | 67.2% | 0.96% | **33.79%** | 12.28% | 29.51% |
| 3 (pseudo-random) | 27 | 69.2% | 0.97% | **33.40%** | 12.76% | 30.09% |

**Angular prediction alone — 33 of the 35 candidates, each 16 calls into
`predict_angular_sample()`'s per-sample table lookups and branches — is
consistently the single largest share, ~33-37% of total encode time by
itself.** `sad_4x4` is ~12-15%. Planar/DC prediction is under 1.5%. This
directly confirms the backlog's own mechanical hypothesis (an 8.75x increase
in full `predict_block4()`+`sad_4x4()` work per block, having lost the old
4-candidate shape's unrolling/vectorisation) and localizes it: the fix needs
to cut angular *candidate count*, not touch the RD arithmetic or `sad_4x4`
itself.

## 2. The search: coarse-then-refine + always-cost-MPM

`hevc_choose_luma_mode()` (`approach1-compute-encoder/src/hevc_intra.c`) now
evaluates, in order, with a `tried[HEVC_MODE_COUNT]` de-dup table shared
across all stages:

1. Planar, DC.
2. A step-4 angular grid: modes 2, 6, 10, 14, 18, 22, 26, 30, 34 (9
   candidates) — the same 11-of-35 coarse grid (2 + these 9)
   `hevc_intra_wavefront.comp`'s own GPU shader search already uses; this
   file's old exhaustive-search comment used to cite that shader's "mode
   refinement... not implemented here yet" as the reason this path stayed
   exhaustive. This change is that missing refinement, on the CPU path.
3. +/-1 and +/-2 around whichever angular candidate had the lowest RD cost
   in step 2 (up to 4 more candidates, clamped to [2,34]).
4. The PU's 3 real MPM candidates, always — per the task's rationale, they
   are "nearly free to signal" (1-2 bypass bits vs. a 5-bit escape) and are
   not guaranteed to already sit in the coarse grid or its refinement
   window.

At most 2 + 9 + 4 + 3 = 18 candidates, fewer after de-duplication (matching
`docs/backlog.md`'s own phrasing when it flagged this as future work). The
RD cost function (`sad + lambda * hevc_mode_rate_bits(mode, mpm)`, the
QP-dependent `k=0.15` lambda) is **unchanged** — this is a search-*shape*
change only, not a re-tuning of the RD bias the previous pass established.
Tie-break convention is preserved too: first-evaluated candidate wins an
exact tie (strict `<`), now over the smaller candidate set.

**Read `cavlc-residual-coding`'s own search as a lead, not fact**, per the
task brief: `/mnt/c/Users/apple/bc250-work`'s `hevc_pick_mode_from_costs()`
(read-only) does the same coarse(step-4)+refine(+/-1/+/-2)+MPM shape. It was
*not* imported — this port reuses this tree's own already-established RD
cost function, tie-break convention, and profiling harness, and computes
angular predictions with this tree's already-verified `predict_block4()`
rather than that tree's `hevc_predict_4x4_refs()`. One thing from that tree
is explicitly **not** carried over: its own comment reports that pruning to
the coarse grid (rather than a full argmin over pre-computed exhaustive
costs) matters there because two things in *that* encoder reward mode
agreement between neighbouring blocks — an MPM hit, and a CU that collapses
from `PART_NxN` to `PART_2Nx2N` when all four blocks agree. This tree has no
such CU-partition-mode collapse (backlog A6 pieces (2)/(3), the CU/TU
structure work, were never attempted here — see
`docs/notes/a6-cavlc-residual-port.md`), so that specific mechanism doesn't
transfer; the coarse grid here is adopted purely for the throughput reason
measured in section 1, not for a partition-mode side effect this codebase
doesn't have.

### Spec verification: does a narrower search silently break anything mode-dependent?

Checked by reading the actual call sites, not assumed:

- **Mode 10/26's edge filter** (`predict_block4()`'s `if (mode ==
  HEVC_MODE_VERTICAL)` / `HORIZONTAL` branches) and **`hevc_scan_idx_for_mode()`**
  both key off the **final winning mode value**, passed at their own call
  sites (`encoder_h265.c`: `pu_modes[pu]` and the value
  `hevc_choose_luma_mode()` returns) — never the search path that found it.
  A narrower search changes *which* mode can win, never what a won mode
  means once chosen. Confirmed by reading `encoder_h265.c` lines 1212
  (`hevc_choose_luma_mode()` call), 1319 (`hevc_scan_idx_for_mode(pu_modes[pu])`),
  and `predict_block4()`'s own edge-filter branches in `hevc_intra.c`.
- **`hevc_derive_mpm()` / CABAC mode signalling** (`hevc_cabac_code_intra_luma_flag/_data`,
  `hevc_cabac_code_intra_chroma_pred_mode`) are unchanged and were already
  general over the full 0-34 range (backlog A6's own earlier note) —
  unaffected by which subset of modes the search itself tries.
- **Chroma** is unaffected: it's still DC-only (`hevc_predict_4x4(...,
  HEVC_MODE_DC, ...)`), never routed through the new search.

## 3. Byte-exactness (the bar here is decode correctness, not bitstream identity)

Per the task: the search *shape* changed, so a different mode can legitimately
win on identical content — bitstreams **are** expected to differ from the
35-exhaustive build, and do (see checksums throughout this file). The actual
bar, `tools/hevc_host_drift.sh`, still requires that whichever mode wins
still decodes byte-exact against ffmpeg's real decoder (real loop filter on,
not `-skip_loop_filter all` — see `docs/hevc_scope_note.md`'s discipline).

```
$ tools/hevc_host_drift.sh /tmp/some-scratch-dir
...
HOST DRIFT PASS (53 cases byte-exact)
```

53/53, unchanged from A6's own count. Re-run twice (including once after a
full clean rebuild) with the same result.

**Footgun encountered and recorded so it isn't repeated**: `hevc_host_drift.sh`'s
first argument is a **scratch work directory that the script `rm -rf`s**,
not a cmake build directory to reuse — running `tools/hevc_host_drift.sh build`
from the repo root deletes `build/` (it did, here; recovered by
re-running `cmake -B build -S approach1-compute-encoder`, source untouched).
Always pass an actual scratch path (e.g. `/tmp/bc250_host_drift_scratch`),
never the cmake build directory's name.

## 4. `ctest -R HevcEncodeBitstreamTest`

Re-confirmed the previous A6 agent's finding: **this test has no hardcoded
bitstream md5 and no per-mode-decision expectation.** Read
`approach1-compute-encoder/tests/test_hevc_encode.c` in full — every
`assert(memcmp(...))` in it compares the partial-butterfly transform kernels
against the literal matrix-product reference (`test_transform_butterfly_equivalence`)
and the quantizer against its literal-division reference, both over
synthetic/random coefficient blocks that never touch `hevc_choose_luma_mode()`.
Nothing needed updating. Full `ctest`: **6/7**, the one failure
(`VaApiDriverTest`, `Vulkan error -2` under WSL's software `llvmpipe`) is the
same pre-existing WSL/no-real-GPU limitation `docs/notes/a6-cavlc-residual-port.md`
already recorded — reproduced on the unmodified baseline too, not a
regression from this change.

## 5. Throughput measurement

**Rig**: `tools/hevc_bench bench` (CMake target, shipped `-O3 -DNDEBUG
-march=znver2 -mtune=znver2`, `-falign-functions=64 -falign-loops=32`
pinned). Three binaries compared side by side:

- `pre_a6` — commit `dc516b8`, the original 4-mode (Planar/DC/H/V) search,
  built in a throwaway `git worktree` at that commit (removed after use).
- `old` — this branch's `HEAD` (`e97807a`) before this change: the
  35-candidate exhaustive RD-biased search.
- `new` — this change: coarse-then-refine.

Each config run twice per binary (an A/A check on that binary, not just an
A/B against the other) with 11 samples/run, median reported.

| size/qp/pattern | pre_a6 (4-mode) | old (exhaustive-35) | new (coarse+refine) | old/new speedup | recovery vs (old-pre_a6) |
|---|---|---|---|---|---|
| 640x480 q12 pat2 (diag) | 47.5 ms | 192.2 ms | 106.0 ms | **1.81x** | 59.6% |
| 640x480 q27 pat2 (diag) | 46.1 ms | 181.8 ms | 101.6 ms | **1.79x** | 59.1% |
| 640x480 q12 pat3 (rand) | 79.6 ms | 223.9 ms | 151.5 ms | **1.48x** | 50.2% |
| 640x480 q27 pat3 (rand) | 70.5 ms | 225.9 ms | 149.2 ms | **1.51x** | 49.4% |
| 1280x720 q27 pat3 (rand) | 163.8 ms | 502.8 ms | 338.4 ms | **1.48x** | 48.5% |

("recovery" = `(old - new) / (old - pre_a6)`, i.e. what fraction of the gap
this change closes back toward the pre-A6 shape, on this harness.)

**A/A floor for this comparison**: repeat runs of the *same* binary at the
same config varied by 0.5-4.1% (median vs median across the two runs); every
old-vs-new delta above is 45-82%, far above that floor. This is a result.

**Speedup is content-dependent**: diagonal-ramp content (pattern 2) recovers
more (1.79-1.81x) than pseudo-random (pattern 3, 1.48-1.51x) — pseudo-random
content has more scattered per-block RD winners, so the refine+MPM stages
add relatively more candidates back on top of the coarse grid than on
smoother content.

**Honest scope caveat — this is NOT directly the board's +52.42% number.**
This off-board harness runs `hevc_encoder_encode_raw()` alone: no GPU
dispatch, no motion estimation, no CABAC-adjacent board overhead diluting
the percentage. That is *why* `pre_a6`→`old` here is a 3.8x-4.3x blowup on
this harness while the board's whole-pipeline `lab compare` measured
"only" +52.42% — the mode search is a much larger fraction of *this specific
measurement's* denominator than of a real frame's total wall time. What
carries over is the *mechanism* (angular candidate count dominates, see
section 1) and the *relative* recovery this change buys against that
mechanism specifically (48-60%, five points above) — not a claim that the
board's +52.42% drops to a specific new number. **A board `lab compare` run
is the only way to get that real number, and none is available in this
task** (no board access, off-board only per the task brief).

## 6. RD-quality measurement

Same byte+PSNR-at-matched-QP methodology `docs/notes/a6-cavlc-residual-port.md`
used: decode the encoded bitstream AND an independently-generated true
synthetic source frame to raw YUV with identical forced framing (never PSNR
a raw bitstream against a fresh `-f lavfi` source directly, per `CLAUDE.md`),
then `ffmpeg -lavfi psnr`. Same 5 primary (diagonal-ramp) points as that
file's follow-up table, plus its 4 secondary (pseudo-random) points, all
single-frame/all-intra/CQP:

| size/qp/pattern | pre_a6: bytes/PSNR-Y | old (exhaustive-35): bytes/PSNR-Y | new (coarse+refine): bytes/PSNR-Y | new vs old |
|---|---|---|---|---|
| 256x256 q12 diag | 9368 / 53.28 dB | 7408 / 52.64 dB | 7462 / 52.56 dB | +0.73% / -0.07 dB (negligible) |
| 256x256 q27 diag | 4881 / 44.96 dB | 3364 / 44.54 dB | 3732 / 43.74 dB | **+10.9% / -0.81 dB (real regression)** |
| 128x128 q20 diag | 1078 / 50.57 dB | 1039 / 50.97 dB | 1047 / 50.95 dB | +0.77% / -0.02 dB (negligible) |
| 64x64 q20 diag | 355 / 50.39 dB | 339 / 50.83 dB | 341 / 50.81 dB | +0.59% / -0.02 dB (negligible) |
| 128x128 q34 diag | 778 / 39.30 dB | 764 / 39.49 dB | 765 / 39.42 dB | +0.13% / -0.07 dB (negligible) |
| 256x256 q12 rand | 68210 / 49.80 dB | 68806 / 49.80 dB | 68969 / 49.80 dB | +0.24% / -0.00 dB (flat) |
| 256x256 q20 rand | 54208 / 42.40 dB | 54593 / 42.70 dB | 54804 / 42.73 dB | +0.39% / **+0.03 dB** (flat, marginally better) |
| 256x256 q27 rand | 41921 / 36.04 dB | 42572 / 35.72 dB | 42742 / 35.74 dB | +0.40% / **+0.02 dB** (flat, marginally better) |
| 256x256 q34 rand | 31072 / 28.99 dB | 31914 / 29.10 dB | 31963 / 29.06 dB | +0.15% / -0.04 dB (flat) |

(`pre_a6`/`old`'s numbers reproduce `docs/notes/a6-cavlc-residual-port.md`'s
follow-up table exactly at every shared point — e.g. 256x256/q12/diag:
9368 B/53.28 dB and 7408 B/52.64 dB both match to the decimal reported
there — which cross-validates this measurement's methodology.)

**Reading**: 8 of 9 points are effectively flat versus the full-35-exhaustive
build — deltas under 1% bytes and under 0.1 dB, the same order as this
harness's own rounding/measurement noise, and two points are even
marginally *better*. **One point, 256x256/QP27/diagonal-ramp, shows a real,
non-trivial regression**: +10.9% bytes *and* -0.81 dB versus the
full-exhaustive search — both worse, not a rate/distortion trade.

**Mechanism, checked, not guessed** (`BC250_HEVC_DEBUG_MODES=1` mode
histograms for exactly this config):

```
OLD (exhaustive-35):  mode=34: 3156  mode=0: 237  mode=2: 218  mode=33: 142  mode=10: 123  mode=3: 97 ...
NEW (coarse+refine):   mode=34: 2558  mode=2: 405  mode=10: 298  mode=0: 292  mode=1: 127  mode=26: 125 ...
```

Real mode selection shifted, not a bug: ~600 fewer blocks land on mode 34
and instead spread across 2/10/0/1/26/etc. Mode 34 and its refine neighbours
(32, 33) are all reachable by the new search (34 is itself a coarse grid
point), so this isn't a "the true winner was unreachable" case in the
simple sense — it's the combined effect of evaluating fewer total
candidates changing which one wins ties/near-ties under the RD cost, on
exactly the near-zero-SAD diagonal-gradient content
`docs/notes/a6-cavlc-residual-port.md`'s own lambda-tuning section already
flagged as this search's most sensitive regime ("even a 'small' absolute
lambda swamps a genuine, large *relative* SAD gap"). This is the same
known-sensitive operating point, not a new failure mode.

**Against the original (pre-A6) baseline, not just old**: at this same
regressed point, `new` (3732 B / 43.74 dB) is still *smaller* than `pre_a6`
(4881 B / 44.96 dB) — fewer bytes at lower PSNR is an ordinary point on a
rate-distortion curve, not a double (Pareto) regression below the original
4-mode baseline. No point in either table regresses *both* bytes and PSNR
simultaneously versus `pre_a6`.

**Standing caveat, same as the prior file's**: this is single-QP byte/PSNR,
not a BD-rate curve — it answers "did narrowing the search meaningfully hurt
compression on these samples," not "what's the real BD-rate delta." A board
`qsweep` session against natural content would be needed for that, and
remains future work (already true of the RD-bias pass this change sits on
top of).

## 7. Two new compiler warnings, fixed without behaviour change

Restructuring the search into `hevc_eval_mode_candidate()` (a small
`static inline` helper called from several distinct sites: Planar, DC, the
coarse loop, the refine loop, the MPM loop) changed how aggressively GCC
specializes `predict_angular_sample()` per call site, and one specialization
made `-Warray-bounds` flag `HEVC_ANGLE_TABLE[mode]` and
`HEVC_INVANGLE_TABLE[mode-11]` as theoretically reachable with an
out-of-range `mode` — a path that was always mathematically unreachable for
any real caller (every call site bounds `mode` to `[0,34]` or narrower
before it reaches the angular branch; the `k<0` branch computing
`mode-11` is, per its own long-standing comment, only reachable when
`mode` is in `[11,25]`) but was no longer provable to the compiler across
the new inlining shape. Fixed with defensive, non-behaviour-changing clamps
at the top of `predict_angular_sample()` and around the `INVANGLE_TABLE`
index — dead code for every legitimate input, verified by the unchanged
`hevc_bench` checksum and the unchanged drift-oracle result. Two more
`-Wcomment` warnings ("/*" inside a comment, from `*best_mode/*best_cost` in
this file's own new doc comments) were fixed by adding a space. Final build:
zero new warnings.

## 8. Verdict

**Ship it.** Recovers 48-60% of the mode-search-specific regression on this
off-board harness (content-dependent), preserves byte-exact decode
correctness (`HOST DRIFT PASS`, 53/53), and the RD-quality cost is small and
localized: 8 of 9 sample points are flat (within ~1% bytes / ~0.1 dB) versus
the full exhaustive search, with one disclosed exception (256x256/QP27/
diagonal-ramp: +10.9% bytes, -0.81 dB) that is still not a double regression
below the original 4-mode baseline. `HevcEncodeBitstreamTest` needed no
changes (confirmed, again, to have no mode-decision-sensitive assertion).

This is not a full recovery to the pre-A6 4-mode search's speed (the task's
own framing said that isn't the goal — it would mean abandoning the mode
coverage A6 was for) — `new` still runs at roughly 1.9x-2.2x `pre_a6`'s time
on this harness, versus `old`'s 3.8x-4.3x. That remaining gap is the accepted
cost of keeping all 35 modes reachable via 18-or-fewer candidates instead of
4 fixed ones.

**What I'd try next, in order, if the one regressed point needs closing
further**: (1) a board `qsweep`/BD-rate run against real content, since nine
single-QP synthetic points can't rule in or out whether that one point
matters at all in practice; (2) if it does, widen the refine window (+/-3)
specifically at high QP on smooth/gradient content, or reconsider whether the
coarse grid should also always evaluate one candidate adjacent to each MPM
(not just the MPM itself) so a near-tie between an MPM-adjacent angle and the
coarse-angular winner isn't decided by which one the grid happened to sample.
Neither is attempted here — narrower, better-targeted follow-on work than
re-opening the whole search shape.

## Files changed

- `approach1-compute-encoder/src/hevc_intra.c` — `hevc_choose_luma_mode()`
  replaced with the coarse-then-refine search (`hevc_eval_mode_candidate()`,
  `HEVC_COARSE_ANGULAR_MODES`); per-candidate profiling probes
  (`HEVC_PROF_BRACKET`, guarded by `HEVC_INTRA_PROFILE`, verified byte-identical
  in the shipped build); defensive bounds clamps in
  `predict_angular_sample()` (no behaviour change for any real input).
- `tools/hevc_bench.c` — `profile` command extended to print the
  per-candidate breakdown (predict planar/dc, predict angular, sad_4x4)
  alongside the existing whole-function ablation.
- `docs/notes/a6-mode-search-perf.md` — this file.

No changes to `docs/DEVLOG.md` or `docs/backlog.md` (parallel-agent
constraint, per the task brief) or to any other worktree. Scratch tools used
for the throughput/RD measurements above (`gen_source.c`, `measure_rd.sh`,
`ab_compare.sh`, `pre_a6_compare.sh`, `mode_histogram.sh`, plus a throwaway
`git worktree` at `dc516b8` for the pre-A6 binaries) were dev-machine-only
and not committed — reproducible from this file's description
(`tools/hevc_host_repro.c`'s `fill()` for the synthetic patterns, `git show
dc516b8:...` for the pre-A6 source, `ffmpeg -lavfi psnr` between two
forced-framing raw YUV decodes per `CLAUDE.md`'s PSNR rule).
