# Work index

Serial-able work items, split by whether they need the BC-250 board. Kept
here rather than in a tracker because every item's verification story
lives in this repo.

**The off-board list is long on purpose.** `hevc_encoder_encode_raw()` is
GPU-free and `tools/hevc_host_drift.sh` gives a byte-exact correctness
oracle on a dev machine, so most HEVC correctness and CPU-side
performance work does *not* queue behind hardware. Only the GPU paths,
throughput figures and anything touching the VA-API surface lifecycle
genuinely need the board.

## Standing rules for anything on this list

- **Byte-identical means byte-identical.** A performance change that
  alters output is a correctness change and needs a different review.
- **`test_hevc_encode` is a valid byte oracle** (deterministic, 3/3).
  **`test_encode` is NOT** — it alternates between two md5s run to run
  (`rate_control.c`'s `CLOCK_MONOTONIC` bucket drain). See B1.
- **Run `tools/hevc_host_drift.sh`** on anything touching prediction,
  transform, quantisation or entropy coding. It is the only oracle here
  that does not share our own code.
- **A measured zero is a result.** Record it and revert; do not keep
  complexity that bought nothing.
- **gprof has misattributed three times** in this codebase:
  `hevc_sbac_init_state` reported at 7.9M calls when it is reachable ~810
  times; a 17.7% attribution whose "obvious" hot instruction turned out
  to be free; and `bs_rbsp_to_ebsp` at 5.9% when direct instrumentation
  says 0.93% (its sample bucket is 10 ms against a ~0.6 s run, so one
  sample reads as ~1.7%). Treat the profile as a pointer to *where to
  instrument*, not as a finding.
- **Measure at the shipped `-O3 -march=znver2`, not `-O2`.** A `-O2`
  figure can be wrong in either direction — the butterfly transforms went
  +15.4% -> +2.1%, while the zorder/gather/CABAC work went +29.4% ->
  +44.5%. See `performance-measurement.md`.

---

## A. Off-board, ready to pick up

**A1 is done and has moved to B6** — non-multiple-of-16 resolution
coverage landed 2026-09-21. Its premise (a padding/crop bug) was wrong,
but it found two real defects anyway. See B6.

**A2. H.264 CAVLC off-board harness — DONE (2026-09-21). The lever is
open; the first obvious optimisation measured zero.**
`tools/cavlc_bench.c` (CMake targets `cavlc_bench` and
`cavlc_bench_prof`) drives `cavlc_write_*` directly with synthetic
quantized coefficients, mirroring `encode_mb_i16x16()`/
`encode_mb_p16x16()`'s syntax order, cbp gating and nC derivation. No
GPU, no board. `verify` runs in CI as `CavlcHarnessRoundTrip`.

Where the time goes, measured by ablation at `-O3 -march=znver2`
(`cavlc_bench profile`, 13 samples/side, A/A floor 0.5%; 1280x720,
`typical` density):

| | share of the timed macroblock loop |
|---|---|
| inside `cavlc.c` | **83%** (the other 17% is the MB-layer glue: cbp, nC, bookkeeping) |
| all bitstream writes combined | **41%** — levels 19%, run_before 7%, coeff_token 5%, mb headers 6%, total_zeros 4.5%, t1 signs 2% |
| **coefficient discovery** (zigzag gather + `cavlc_scan_coeffs`) | **~41%** — `cavlc.c` minus its writes |

So the next lever in this file is *discovery, not bit-writing*: the
zigzag gather into `int scanned[16]` plus the backwards scan cost about
as much as every syntax element put together.

**Measured zero, do not redo off-board:** an all-zero-block fast path
(48% of blocks reaching CAVLC are entirely zero). A 16-element
`acc |= c[i]` reduction was 2-7% *slower*; the cheap four-`uint64_t`
form came in at 0.990x against an A/A control of 0.995-1.011x in the
same session — inside the noise, far inside the 2.5% bar. Reverted;
full reasoning is in `cavlc.c`'s "MEASURED AND REJECTED" comment. It is
still worth one *board* run, since Zen 2 at BC-250 clocks could shift
the balance, but nothing off-board justifies the branch.

> **Board, 2026-09-22: no measurable whole-pipeline win.** `lab compare`
> (H.264, isolates this change - nothing else landed today touches
> `cavlc.c`'s callers on the H.264 path): `p_wall_ms` delta -0.91%,
> within noise (floor 0.61%, threshold ~1.22%). The -41% isolated-stage
> speedup is real (verified twice, see below) but coefficient discovery
> is only part of CAVLC, which is only part of a frame that also pays
> for GPU dispatch/sync and everything else `p_wall_ms` includes - **a
> measured zero at the whole-pipeline level is the honest result here**,
> not a discrepancy to explain away. Kept: still byte-identical, still a
> real off-board mechanism improvement, just not one that moves the
> metric that actually matters for this project's purpose. `err_alloc_
> failed=2`/`err_slice_overflow=41` also showed up in this same `compare`
> output, identically on both the baseline and this key - pre-existing,
> unrelated to this change, and not yet investigated; see the new open
> item at the end of this file.

**DONE off-board (2026-09-22): the bitmask lever, byte-identical,
~-41%.** `cavlc_scan_coeffs` now builds a nonzero bitmask over
`scanned[]` once, then uses `__builtin_clz` to find `last_idx` and to
jump directly between discovered coefficients - turning the old
"walk every zero to count it" run/total_zeros derivation into
O(total_coeff) instead of O(max_coeff). `cavlc_bench scan` (isolated
gather+scan, no writes), 3 interleaved runs/side: baseline median
1527-1540ms, new median 894-906ms. Byte-identical: `emit` md5 matches
old vs new on the same seed, the profile build's sink matches exactly
(631432800) across 6 runs, `cavlc_bench verify` round-trips both
profiles through the reference decoder, full ctest 6/6 relevant suites
pass (the 7th, `VaApiDriverTest`, fails off-board on a pre-existing
llvmpipe/Vulkan memory limitation unrelated to this change).
**Still needs a board run** - this is an off-board timing number and
Zen 2 at BC-250 clocks could shift the balance, per this item's own
standing caveat. The `int16_t scanned[]` half of the original lever is
untried.

**A3. Audit the GPU shader against the spec for more defects of the
chroma-QP class.** `hevc_intra_wavefront.comp` quantised chroma at QpY
for its whole life because nothing compared it against an independent
decoder. Worth a careful read for siblings: transform shift / bdShift
derivation, scan order selection, the `cbf` packing, reference
substitution. Inspection only — the shader cannot run off-board — so
produce a list of *claims to test*, not conclusions.

**First pass done: `docs/hevc-shader-audit.md`.** No confirmed defect;
9 ranked claims with a `lab drift` experiment each, plus a list of what
was checked against the spec and agrees (don't re-derive it). The two
structural findings are that most of the shader has no second
implementation in this tree, and that the forward transform, quantiser
and mode search are invisible to *every* oracle here.

> **Board, 2026-09-21: the "prove coverage first" step, done.** Added
> the GPU-path mode histogram (`BC250_HEVC_DEBUG_MODES`, mirroring the
> CPU path's existing one). On `testsrc2` 1920x1080 QP 27: **exactly 11
> distinct modes appear, and they are exactly the step-4 coarse angular
> grid** (`{0,1,2,6,10,14,18,22,26,30,34}`) — not one refinement or
> off-grid mode was ever selected, with Planar alone at 80.2%. This is
> board-measured confirmation that the GPU shader's mode search has no
> refinement stage at all (unlike the CPU path's post-A6 coarse-then-
> refine search) — a real, measured accuracy ceiling, not just a
> theoretical gap inferred from reading the shader. Adding the same
> refinement stage the CPU path already has is now a board-confirmed
> opportunity, not attempted here. DEVLOG §38.

> **Implemented and board-confirmed, 2026-09-22: refinement fires, and
> it has a real, measured cost.** Added the CPU path's own +/-1/+/-2
> refinement around the coarse winner (angular candidates only), two
> extra barriers total. Mode histogram, same QP27/1920x1080/`testsrc2`
> config as the original board measurement (8160 CTUs): Planar still
> dominates (6549, 80.3%, matching the original 80.2%) but the
> remaining candidates are no longer confined to the 11-entry coarse
> grid - off-grid values (3,4,7,8,9,11,12,13,15,16,17,19,20,21,24,25,
> 27,28,29,32,33 all appear) now make up roughly 4% of all CTUs
> (~310/8160), confirming the refinement stage is genuinely selecting
> modes the old coarse-only search could never reach, not just adding
> dead code. `lab compare` (HEVC GPU-intra path): `p_wall_ms` **+3.35%,
> SIGNIFICANT** - a real, disclosed throughput cost from the 4 extra
> SAD evaluations per angular-winner CTU. **Not yet measured: whether
> this actually improves PSNR/bitrate at a matched setting** - SAD can
> only tie or improve on the coarse winner by construction, but turning
> that into an actual bytes/PSNR delta under real VBR rate control needs
> its own qsweep-vs-qsweep comparison at matching bitrates, which this
> pass did not run. Until that exists, this is a confirmed real cost
> with an unconfirmed (though structurally plausible) benefit - do not
> claim the RD win as proven.

**A4. DONE (2026-09-21).** `hevc_cabac_code_residual_4x4` was really
6.5–11.5% of frame time on detailed content (not the 5.9% a gprof
sample suggested — content-dependent, spans 0.65–41.7% across synthetic
patterns). Residual sign bits batched into one bitmask write instead of
N separate bypass calls: **-14 to -19% on the function, byte-identical**.
Board-priced together with the two items below: -12.4% combined encode
time, significance-tested.

**A5. SUPERSEDED by A6.** The original 4-candidate search this item
described no longer exists — A6 replaced `hevc_choose_luma_mode()`
wholesale with a 35-mode RD-biased search. Before that: gather-once +
hoisted-source-block + per-candidate compile-time unrolling landed
**+12.9% to +14.9%, byte-identical**, and this function turned out to be
~24% of frame time, not the 7.8% a gprof sample suggested (gprof's
fourth misattribution in this codebase, and its first that read low).
A6's board review found the *replacement* search costs +52% frame time
(see A6) — the win recorded here was real at the time but no longer
describes the shipped code.

**A6. PIECE (1) DONE (2026-09-21) — angular modes ported and RD-biased.
Pieces (2)/(3) still open.** All 35 HEVC intra modes now searched (was
4), sourced from the GPU shader's already board-verified angular
formula rather than transcribed from `cavlc-residual-coding`. Byte-exact
(`HOST DRIFT PASS`, 53 cases). Shipped in two passes because the first
one shipped a real, measured RD regression — SAD-only search never
charged a non-MPM mode for the extra bits it costs to signal, so 2 of 5
sampled points came back smaller *and* worse. Second pass biases the
search against the *exact* signalling bit-cost (read off the CABAC
binarization, not estimated) with a QP-dependent lambda; the two
regressive points improved substantially on both axes but were **not
eliminated**, and 3 of 5 flipped to genuine Pareto improvements.

**Do not treat this as a validated compression win.** `k=0.15` (the
lambda scale) is tuned on synthetic content only, a single-QP byte/PSNR
pair is not a BD-rate curve, and the original `-34.8%` figure almost
certainly reflects pieces (2)/(3) together, not this piece alone. Needs
a board `lab qsweep` BD-rate run before any number here is trusted.

> **Board review, 2026-09-21: this has a real, previously-undisclosed
> cost.** `lab compare` (HEVC, gop=120): per-frame encode time
> **+52.42% (SIGNIFICANT)**, bytes within noise. Neither pass measured
> throughput off-board — both checked bytes/PSNR only. Mechanically
> tracks: A5's compile-time-constant unrolled 4-candidate search became
> a 35-candidate runtime loop, an 8.75x increase in full
> predict+SAD work per 4x4 block, losing the unrolled shape's
> vectorisation along the way. Confirmed HEVC-specific (H.264's own
> `compare` in the same run: everything within noise) and consistent
> with the qsweep fps column (10.2–10.5 fps vs this key's predecessor's
> own 15.62/15.64 fps at the same settings). **Undecided**: optimise the
> search (early-exit / coarse-then-refine, the way `cavlc-residual-
> coding`'s own search already does), gate behind a flag, or accept the
> cost — HEVC CPU was already far from real-time and is opt-in-only, so
> practical impact on the one real client is limited, but the trade was
> never surfaced as one until now. DEVLOG §36.

> **Partially addressed (2026-09-21): coarse-then-refine recovers
> ~1.5-1.85x of the mode-search cost, off-board.** Instrumented first:
> angular prediction alone (33/35 candidates) is 33-37% of encode time
> by itself, localizing the cost to candidate *count*. Replaced the
> exhaustive search with Planar+DC, a step-4 angular grid (the same
> coarse grid the GPU shader already uses), ±1/±2 refinement, plus the
> 3 real MPMs always — same RD cost function, at most 18 candidates
> instead of 35. Off-board: 1.45-1.82x speedup over exhaustive,
> content-dependent; independently reproduced before merging
> (183-184ms → 98-100ms at one config, ~1.85x). RD cost: 8/9 sample
> points flat vs the exhaustive search, one disclosed exception
> (+10.9% bytes, -0.81 dB at 256x256/QP27/diagonal-ramp — traced to a
> mode-histogram shift on the same near-zero-SAD regime the RD-bias
> pass already flagged as sensitive). **Explicitly not the board's
> +52.42% number brought down to a new figure** — this off-board
> harness's mode-search share of total time is much larger than a real
> frame's, so only the mechanism and relative recovery transfer.
> `docs/notes/a6-mode-search-perf.md`.

> **Board-confirmed (2026-09-21): -16.46% per-frame encode time
> (SIGNIFICANT), bytes unchanged (within noise).** `lab compare` against
> the immediately-preceding key, same HEVC/gop=120 settings as the
> original board review. Recovers a real fraction of the +52.42%
> regression — not a full return to the pre-A6 4-mode search's speed,
> which was never the goal. Compression unaffected, confirming the
> off-board RD-quality accounting held on real hardware too.

> **Piece (2) DONE (2026-09-21) — all-TU-size transform kernels, wired
> in at 8x8 via `PART_2Nx2N`.** Generic DCT-II transform/quant/dequant
> for log2_size {2,3,4,5} plus 8.4.4.2 prediction with real 8.4.4.2.3
> reference-sample filtering (the 4x4 path has never filtered). Only
> 8x8 is reachable this session — a structural fact, not a scope cut:
> `PART_NxN` (this CU size's only option before this change) forces
> `transform_tree()` to split to 4x4 regardless of anything the encoder
> decides (7.3.8.8, `IntraSplitFlag==1 && trafoDepth==0`); the only way
> to reach an 8x8 transform at 8x8-CU size is `PART_2Nx2N`, which
> necessarily also changes the prediction structure to one 8x8 PU. 16x16
> and 32x32 **cannot** be reached until piece (3) provides a bigger CU —
> the kernels are generic and implemented, just have no caller yet.
> `HOST DRIFT PASS`, all 53 cases, unconditionally (no old path left as
> a fallback). Off-board RD: 5/7 points a real improvement, 2/7 small
> regressions, needs a board qsweep before trusting the direction.
> Piece (3) scoped concretely, not attempted — needs `split_cu_flag`'s
> ctxInc to test neighbour *CtDepth* (9.3.4.2.2), the exact area a
> previously-shipped bug already lived in, now a live risk for the
> first time on this path. `docs/notes/a6-cu-tu-structure.md`.

> **Piece (2) board-checked, 2026-09-22: no real throughput cost.**
> Shipped with zero board timing check, unlike piece (1)'s two rounds of
> board review — closed that gap. `lab noise` floor first (n=5,
> `p_wall_ms` sd 1.45%), then `lab compare` against the immediately-
> preceding key (HEVC, gop=120, 3 runs/side interleaved): `p_wall_ms`
> delta **-1.25%, within noise** (threshold ~2.9%) — no regression, if
> anything a trivial improvement. `p_fps_ceiling` (-0.38%) and `bytes_p`
> (-0.19%) flagged SIGNIFICANT by `lab compare`'s own tighter per-metric
> noise bands, but both are expected: `bytes_p` moves because piece (2)
> changed the actual TU/PU structure (one 8x8 transform instead of four
> 4x4), not because of a performance regression. The separately-observed
> ~10.0-10.8 fps CPU HEVC qsweep figure (2026-09-22 fresh numbers, see
> DEVLOG) is **not** piece (2)'s doing — it's what CPU HEVC costs at
> 1440p/gop=120 regardless.

> **Piece (3) prep, off-board (2026-09-22): the ctxInc landmine is
> defused, no actual CU-size decision added.** `encode_ctu()`'s
> `split_cu_flag` ctxInc was `cond_l+cond_a` - pure left/above CTU
> existence, not 9.3.4.2.2's real test (whether the neighbour is
> *deeper* than the current cqtDepth). It was accidentally correct only
> because this path has exactly one CtDepth value today (every CTU
> always splits to four 8x8 CUs) - the exact bug shape
> `docs/hevc_scope_note.md` already records as real and shipped, once
> depths stop being uniform. Added `enc->ctdepth[]` (one uint8/CTU,
> same lifetime pattern as the existing `gpu_ctu_skip[]`) and switched
> ctxInc to read it. Numerically identical today (CtDepth is always 1
> wherever already coded, so `CtDepth[left]>0` and `cond_l` agree
> everywhere) - `tools/hevc_host_drift.sh` 53/53 byte-exact, ctest 6/6
> relevant suites pass. **No per-CTU split decision exists yet** - this
> only makes the mechanism correct-by-construction for whichever future
> pass adds one, rather than correct by uniform-depth coincidence. The
> actual "should this CTU stay one 16x16 CU" decision, the SPS
> `max_tb_log2` bump, and the rate-normalized RD comparison this needs
> are all still real, separate, unattempted work - see the bullets
> above.
>
> **Board, same day: byte-identical confirmed, but NOT free - a real,
> unexplained +4.96% wall-time cost.** This is a correction to the
> claim two lines above. `lab compare` (HEVC CPU path, isolates this
> change - neither A2 nor A3 touches this path): `bytes_p` delta
> -0.02%, within noise (matches the byte-identity claim). `p_wall_ms`
> delta **+4.96%, SIGNIFICANT** - a real, measured slowdown from a
> change that adds exactly one array read and one comparison per CTU,
> which should not cost anything close to 5% of a ~80ms frame. Not yet
> root-caused; the leading hypothesis is that adding one `uint8_t
> *ctdepth` field to `hevc_encoder_t` shifted every later field's
> offset, moving something else in the same hot struct across a cache
> line - a real, if indirect, mechanism, but unconfirmed. **This should
> have been board-timed before merging, exactly the gap A6 piece (2)
> was called out for and then closed** - it was not, this time, until
> this round's own batch board run caught it after the fact. Filed as
> its own open question rather than reverted outright, since the change
> fixes a real correctness landmine (the ctxInc bug class) that is worth
> keeping even at this cost until a cheaper implementation is found -
> but the earlier "pure infrastructure, no behavior change" framing
> above understated it: no *bitstream* behavior change, but a real
> wall-time one.

Full writeup, including exactly what (2) undivided-CU splitting and (3)
all-TU-size transforms would need (a concrete starting point, read from
`cavlc-residual-coding`): `docs/notes/a6-cavlc-residual-port.md`.

---

## B. Off-board, landed 2026-09-20/21

**B1. DONE.** `test_encode` is deterministic (12/12). Root cause was
`rc_update_stats()`'s `CLOCK_MONOTONIC` bucket drain under `RC_LOW_LATENCY`;
fixed test-side via the existing `BC250_RC_NOMINAL_DRAIN=1` hook, production
rate control untouched. The difference was ONE byte in 350,979.

**B2. REVERTED on board evidence, as the trigger required.**
`bs_rbsp_to_ebsp` was 3.9x faster in situ and byte-identical, but zero
end-to-end on the dev machine and **+0.9% on the board** (9.78 -> 9.87
fps, n=4 each, overlapping ranges, 2.3% run-to-run spread). Not a
result, so the 111 lines of pointer arithmetic went. The differential
fuzz test it brought was **kept** — its `ebsp_reference` oracle now
guards the simple byte loop against any future rewrite, and it checks
every destination capacity from 0 past worst-case expansion.

Also: the 5.9% figure that motivated this task was wrong. gprof's
resolution here is a single 10 ms bucket out of ~0.6 s, so one sample
lands as ~1.7% and a shorter run inflates the same bucket to 5.9%.
Direct instrumentation put the function at 0.93%. **Do not size a task
off a sub-2% gprof number on this codebase** — instrument directly first.

**B3. DONE.** `lab qsweep --codec=h264|hevc`. `scoreboard` deliberately
NOT extended - its reference is libx264, so an HEVC scoreboard would score
our HEVC against x264's H.264 and hand this encoder a win that belongs to
the codec. Needs a libx265 reference verified against Sunshine's own
construction first. **The HEVC figures still need re-taking on the board.**

**B4. DONE, +2.1% shipped.** Division-free quantiser, partial-butterfly
transforms, dead inverse clip removed, dequant folded into inverse stage 1.
Byte-identical under three oracles plus exhaustion over 218M quantiser
pairs. **The headline is +2.1% at `-O3 -march=znver2`, not the +15.4% that
`-O2` shows** - see the optimisation-level rule in
`performance-measurement.md`.

**B5. DONE, and it found a bug in the oracle itself.**
`hevc_host_diff.py` exited 0 on every path, so the drift script printed
per-case failures and still reported PASS with rc=0 - two million wrong
pixels scored as green. Fixed, gate added (+3.3s on a ~40s job), and the
step asserts on the case count so `BC250_DRIFT_CASES` cannot quietly gut it.

**B6 (was A1), landed 2026-09-21. DONE. Its premise was wrong and it
found two real bugs anyway.**
`hevc_host_drift.sh` now runs 21 cases, adding each axis alone and both
together (1918x1080 / 1920x1078 / 1918x1078), 1366x768, 854x480, 100x60,
20x12, 18x18 and sub-CTU 4x4. Padding and the conformance window are
**correct at every size tried** — 600 sweep cases are byte-exact on luma
and chroma. What the coverage gap was hiding:

- **A silently truncated slice, and it is not a resolution bug.**
  `slice_rbsp_cap` was ~1.03 bytes/luma-sample; this encoder emits up to
  1.53 on noise at QP 0 and **1.10 at QP 10**, which is inside the
  shipping `qp_min = 12` range. `bitstream_t` sets `overflow` and stops
  writing, nothing checked it, and the encoder returned the short slice
  as a success — decodes correctly down to one row, garbage below.
  1280x720, fully 16-aligned, failed identically. Fixed by sizing
  (2.0 bytes/luma-sample) *and* by failing the frame on overflow.
- **A heap-buffer-overflow at odd widths**, ASan-confirmed on `HEAD` and
  clean after: `dl_uv` was allocated at a `(width/2)*2` stride while both
  writers use `width`. This is the inherited "buffer boundary overrun on
  non-16-multiple resolutions" the item predicted, except it needs an
  *odd* dimension, not merely an unaligned one.
- Plus a SEGV at 1x1 (`pad_replicate()` wrapping `src_h - 1` on a
  `uint32_t`; UBSan cannot see it, unsigned wraparound is defined), now
  refused at create time.

The oracle itself was also blind to the bug that motivated the item: it
sliced `dec[:W*H]`, so an H.264-style 1080-decodes-as-1088 crop error
scored zero drift. It now asserts the decoded frame size first, and
compares chroma as well as luma. Odd dimensions are excluded with
reasoning (no 4:2:0 representation, and the conformance window is
specified in chroma units so it can only crop an even number of luma
samples) — they stay in the ASan sweep. `test_hevc_encode`'s md5 is
unchanged. CI +1.5 s (~3.3 s -> ~4.8 s). DEVLOG §34.

---

## C. Board items — status as of 2026-09-20 evening

**C1. DONE.** All four HEVC correctness fixes validated on hardware at
1080p: byte-exact on both planes, both paths (GPU and CPU), at QP 27 and
QP 51. Full `lab gate` passes — units 5/5, PSNR 48.49 / SSIM 0.996,
dims, drift 3/3.

**C2. DONE, and much larger than the dev machine suggested.** The CPU
HEVC batch is **+132% on board silicon** (4.25 -> 9.85 fps at 1080p,
noise floor 0.30%), against +44.5% on the dev machine. The board's
slower core pays proportionally more for the integer divisions and call
overhead those changes removed — so a dev-machine figure understates
this class of win, just as it overstated the butterfly.

**C3. RE-OPENED WORSE — the methodology is built, and it rules out
deblocking entirely.** 241–252 is not explainable by the luma-only-
deblocking gap under *any* decode configuration: `deblock_filter.comp`'s
own `tc0` clamp (ITU-T 8.7.2.4) bounds a single deblocking-caused pixel
change to at most 27 across the whole QP range, and `lab drift` always
uses `-g 1` (every frame an independent IDR), so it can't be C9's
compounding-P-frame shape either. Under the harness's own default
(`-skip_loop_filter all`), chroma should already be **exact** — the
encoder never filters chroma and the decoder isn't filtering anything —
so a large chroma delta there is doubly unexplained.

`lab drift --real-decode` (real PPS-signalled filtering) plus per-plane
annotation against the theoretical bound is now in `tools/bc250_lab.sh`.
`h264_encoder_encode_raw()` cannot serve as an off-board oracle here —
it transmits a hardcoded all-zero residual, never touching
`residual_predict.comp`/`reconstruct.comp`/`deblock_filter.comp` where
this actually lives — so **this genuinely needs the board**, confirming
what the item already said.

Leading candidate, not yet checked: `residual_predict.comp` predicts
intra chroma *and luma* from **source** neighbours rather than
**reconstructed** ones — live in exactly the `-g 1` config `drift` uses.
Full analysis + exact next board commands: `docs/notes/c3-h264-chroma-drift.md`.

> **Board review, 2026-09-21: confirmed bigger than deblocking, luma
> included.** `lab drift --codec=h264 --qp=27` (`-skip_loop_filter
> all`): chroma **exact** (0/1036800), confirming the methodology's own
> prediction; luma differs by 103906/2073600, max 7 — just over the
> deblocking-gap bound (4), plausibly still explicable. `--real-decode`
> (the real PPS-signalled filter state — the comparison this item
> actually asked for): luma differs **37405/2073600, max 57** — 14x the
> "should match" bound — and chroma differs **44347/1036800, max 8**.
> Luma should match under real decode (the encoder does deblock luma);
> it doesn't, by a lot. This rules out "chroma-only gap" as the
> complete picture. DEVLOG §36.

> **Leading candidate REFUTED (2026-09-21).** `residual_predict.comp`
> predicting from source neighbours is dead code for this test:
> `gpu_compute_dispatch_encode()` only dispatches it inside
> `if (!is_intra)`, and `lab drift` always encodes with `-g 1` (every
> frame IDR) — independently re-confirmed before merging. The real
> intra path (`intra_wavefront.comp`) reads genuinely-reconstructed
> neighbours and was already fixed for exactly this bug class, board-
> validated 2026-09-08 (DEVLOG §1, bug #5 of 15), well before this item
> was opened. Two more candidates were tested computationally, not just
> read about (`tools/deblock_strong_vs_weak_check.py`): the documented
> missing strong bS=4 deblock filter, and a newly-found cross-workgroup
> race in `deblock_filter.comp`. Both real, both bounded to ~1-2 levels
> at QP 27 — nowhere near the measured max-57 delta. **Still genuinely
> open**; ranked next steps (pixel-coordinate clustering, slice-boundary
> interaction, CAVLC/entropy re-derivation) in
> `docs/notes/c3-h264-chroma-drift.md`, needing a board run.

> **Board, 2026-09-21: pixel-coordinate clustering done, narrowed
> further, still open.** 37347/2,073,600 luma pixels differ (1.80%,
> max|d|=57), consistent with the earlier 37405/2073600. Spatial
> distribution, measured for the first time: every one of 120 MB
> columns and all 68 MB rows has at least one differing pixel somewhere
> - not confined to an edge or corner - but concentration is sharply
> uneven: MB-rows 50-59 (10 consecutive rows, y=800-959) carry roughly
> 3-4x the neighbouring rows' diff-pixel count, and MB-column 15
> (x=240-255) alone carries roughly 3x its neighbours', with a
> secondary cluster at columns 96-111. **Does not show an obvious
> slice-boundary signature** (single slice used; the affected
> rows/columns don't align with any slice-count boundary) - reads as
> content-correlated, not structural. Next step: identify what's
> actually at those coordinates in `testsrc`'s known pattern, or switch
> to flat/synthetic content that separates position from content.
> DEVLOG §38.

> **Off-board, 2026-09-22: identified, and it's a sharp internal edge
> every time.** Dumped raw frame 0 of `testsrc=size=1920x1080` (the
> exact `drift` source) and measured per-MB-row/column gradient energy
> directly - no encoder involved, just the raw pixels. MB-rows 50-59:
> rows 51-58 are a perfectly flat patch (std constant at 45.52,
> gradY=0.00) but rows 50 and 59, the two boundary rows, spike to gradY
> 4.61/4.85 - a solid-color box bounded top and bottom by a hard step
> edge, not a gradient region. MB-col 15: sits immediately after a
> strong vertical-edge pair at MB-cols 13-14 (gradX 3.84/1.74) - on the
> tail of a step edge, not inside smooth content. MB-cols 96-111 (the
> secondary cluster): bracketed by two sharp vertical edges at col 97
> (gradX=4.17) and col 106 (gradX=3.31), same "bounded box" shape as the
> row cluster, column-wise. **Every flagged cluster sits at or
> immediately adjacent to a sharp internal step edge in the test
> pattern - none are in smooth gradient regions, and none are frame or
> slice boundaries.** Sharpens "content-correlated" into a specific,
> testable hypothesis: intra prediction or the transform behaves
> marginally differently from the reference right at a hard edge -
> most plausibly reference-sample filtering/availability at a
> discontinuity (8.4.4.2.3-class behavior) or DCT ringing sensitivity
> to a step. Not yet tested against either specific mechanism - that's
> the next step, now well-scoped instead of open-ended.

**C4. DONE — and it found a live bug.** Re-taking the retracted PSNR
figures through `lab qsweep --codec=hevc` gave HEVC GPU **42.71–42.94
dB** (replacing the ad-hoc 35.26). It also surfaced that H.264 at
**1920x1080 scored 13.66 dB** where aligned heights scored 44.4 — the
SPS crop bug now fixed in `b2b7fef`. 1080p is now **44.30 dB**.

Note the CPU HEVC path scores **24.93 dB** at qsweep's default gop=120,
i.e. with P-frames, versus 42.9 for the all-intra GPU path. ~~That gap is
unexplained and is a **new open item**.~~ **RESOLVED by C9** — it was the
unmodelled in-loop deblocking filter compounding across P-frames. The
CPU path measures **44.56 dB** on the board after the fix, i.e. it now
beats the all-intra GPU path, which is the ordering you would expect.

**C5. DECIDED: keep `BC250_ENABLE_HEVC` opt-in.** The GPU path now
clears every correctness bar (byte-exact, 42.9 dB, ~94 fps at 1080p),
but it is **all-intra only**, so at streaming bitrates it spends them
far less efficiently than H.264 with P-frames. Defaulting HEVC on would
let Sunshine negotiate an all-intra encoder for a live session. Revisit
when C7 lands, not before.

**C6. CONFIRMED NEGATIVE (2026-09-21), now with real root.** Passwordless
`sudo` confirmed present, `cyan-skillfish-governor-smu.service` confirmed
active with its D-Bus name registered, `cyan-skillfish-performance-mode
--fixed-frequency 2000` run via `sudo` directly — the documented,
intended interface. The tool printed "Performance mode enabled with
fixed frequency 2000 MHz" and `pp_dpm_sclk`'s active entry stayed at
~18–100 MHz throughout (before/during/after all near-idle). This is no
longer "needs root" — root was confirmed present and the pin still had
zero visible effect. Whether the fault is the governor tool, the D-Bus
service, or this board's SMU firmware is not established. Still
untested: the actual *light game* load condition this was originally
about (only a synthetic idle probe was run). DEVLOG §38.

> **Board, 2026-09-22: light load DOES move the clock - modestly,
> without the pin.** First attempt at this had a broken ffmpeg filter
> chain (missing `-init_hw_device vulkan`) and measured nothing real;
> fixed and re-run with a genuinely light Vulkan compute load
> (`scale_vulkan` down and back up, not the heavy `nlmeans_vulkan`
> synthetic worst case): `pp_dpm_sclk` BEFORE=13MHz, DURING=29MHz,
> AFTER=19MHz (not yet settled back to baseline when sampled). A real,
> if modest, response - the automatic governor does react to light load
> without the pin being invoked at all, just not by jumping to the
> higher 350/2230MHz DPM states, which presumably need more sustained
> utilization than this synthetic light load provides. This is a
> different question from the original finding (the *pin tool* doing
> nothing) - it confirms the *default automatic* governor is not simply
> broken, just conservative at this load level.

**C7. FIRST CUT IMPLEMENTED (2026-09-21), gated off — board-confirmed
PIXEL-EXACT in every case tested so far.** Zero-motion-SKIP parity with
the CPU path (not real motion compensation — that's a further step,
"still open after this" for *both* paths now per C9's own wording).
Design insight: `recon_image` already persists across frames untouched,
so a SKIP CTU just needs a 6-line shader early-return, not a new
reference image; every merge candidate this path could ever derive is
provably `(0,0)`, so `merge_idx` is always signalled as `0`, exact
rather than a shortcut.

Off-board verification found and fixed a real bug: `split_cu_flag` was
coded after `cu_skip_flag` / omitted entirely for skip CTUs, and ffmpeg
reported **zero decode errors** the whole time — the same "decoder
error names where it noticed, not the fault" shape this project has
hit before. After the fix, a fully-skipped P-frame decodes
byte-identical to its reference at 7 sizes plus a non-CTU-aligned
100x60, via a new GPU-free test entry point
(`hevc_encoder_encode_gpu_raw()`).

> **Board, same day, two passes.** First: does it even survive real
> Vulkan? Two bounded, `timeout`-wrapped encodes (128x128/20 frames/
> gop=5, 256x256/64 frames/gop=8) both encoded rc=0 and decoded cleanly
> through ffmpeg's independent decoder — no crash, no hang, no board
> wedge, driver log confirms the path engaged. That only proves
> syntactic validity, not pixel correctness (a malformed P-slice can
> decode cleanly while wrong, per the bug above).
>
> Second, the real test: a custom `drift`-style comparison (the
> harness's own `drift` command hardcodes `-g 1` and can't reach a
> P-frame at all) — encoder's own recon dump vs a real independent
> decode, frame by frame, real GOP structure. Two cases: static content
> (should skip nearly every CTU after frame 0) and `testsrc2` (real
> motion, forcing a mix of skip and intra-fallback CTUs). **16/16 and
> 24/24 frames byte-exact, both cases, across two full GOPs each.**

> **Board, round 3, same day: scaled up, and the throughput cost is
> real and large.** Pixel-exactness holds at real streaming
> resolutions and longer GOPs with no degradation: **48/48 frames
> (640x480, 3 GOPs) and 90/90 frames (1280x720, 3 GOPs) byte-exact**,
> `testsrc2` motion content both times.
>
> **Throughput, measured for the first time, at 1280x720/gop=30 (3
> reps each, tight clusters, ~2x apart — nowhere near noise):**
>
> | mode | fps |
> |---|---|
> | GPU intra-only (baseline) | 68.6 / 72.0 / 73.1 |
> | GPU intra + P-frame | 35.3 / 34.2 / 34.5 |
>
> **Enabling P-frames costs roughly HALF the throughput** — the
> per-P-frame source+reference download and CPU-side skip decision the
> design doc flagged as "a genuine, acknowledged, unquantified cost" is
> now quantified, and it's substantial, not incidental. Output did get
> smaller too (3,745,841 → 3,178,793 bytes, -15.1%, consistent with
> skip actually saving bits), so the trade isn't nothing — but at these
> settings intra-only is safely real-time at 720p and P-frame mode is
> not (34-35 fps against a 60 fps target), before any GPU dispatch cost
> is even counted against it. **This needs addressing (a cheaper skip
> decision, avoiding the full source download, or accepting the cost
> for the bitrate savings) before C7 is a real candidate for enabling
> by default anywhere near C5's decision.**

> **Board, round 4, same day: fixed, and it overshot — P-frame mode is
> now FASTER than the intra-only baseline it was losing to.** The
> throughput cost traced to two full-frame NV12 host downloads
> (~2.7 MiB combined) plus a CPU SAD loop, every P-frame, all replaced
> by one small compute shader (`hevc_pframe_skip.comp`) that reads the
> source and reference images where they already live on the GPU and
> writes a packed 14.4 KB verdict directly into the same SSBO the
> wavefront shader already reads — the same re-verified pixel-exactness
> test (640x480 and 1280x720, 3 GOPs, `testsrc2`) still passes 48/48
> and 90/90 with the new dispatch path, and `lab gate` PASS confirms the
> default (flag-off) path is untouched. Real fps, same settings as
> round 3:
>
> | mode | fps (round 3, before) | fps (round 4, after) |
> |---|---|---|
> | GPU intra-only (baseline) | 68.6 / 72.0 / 73.1 | 67.0 / 70.7 / 72.9 |
> | GPU intra + P-frame | 35.3 / 34.2 / 34.5 | **80.5 / 80.4 / 77.5** |
>
> Not just recovered — P-frame mode now **beats the all-intra baseline**
> by ~10-15%. This makes sense once the shader's own early-return is
> accounted for: a SKIP CTU costs the wavefront shader nothing (no
> intra prediction, transform, quant or reconstruction at all, just a
> flag check), so on `gop=30` — mostly P-frames — the shader is doing
> real work for a shrinking fraction of CTUs, on top of the compression
> win already measured (output still ~15% smaller than intra-only).
> C7 is no longer a throughput trade at all at these settings — it is a
> straightforward win on both bitrate and fps together.

> **Board, round 5 (2026-09-22): the fps win holds at real streaming
> settings, but there's a real, previously-unmeasured quality cost.**
> Every round above used `-g 5`/`-g 8`/`-g 30` to keep bounded test
> encodes cheap — this pass ran the actual candidate settings for
> streaming (`testsrc2`, 1440p, **gop=120**, `qsweep` across 8-31M for
> the first time with P-frame mode on). Throughput result holds: 59.47-
> 78.25 fps, still comfortably beating a 60 fps target across the
> range. **But PSNR is *lower* than the all-intra GPU path at every
> matched bitrate** (31M: 38.88 dB P-frame vs 40.39 dB intra-only; the
> gap is present and roughly similar in size at every bitrate sampled).
> The zero-motion skip threshold (`BC250_HEVC_SKIP_THRESHOLD`, default
> 1536, see the C6 sweep above) has only ever been validated for
> pixel-exactness and throughput — never tuned against a real quality
> target on real motion — and this is the first time it was measured
> against real motion (`testsrc2`) at a real GOP (120) rather than the
> short bounded configs used everywhere above. Two live hypotheses, not
> yet distinguished: the threshold is skipping CTUs a real
> rate-distortion decision wouldn't, or the intra fallback on
> non-skipped CTUs is coarser than true motion compensation would be
> (which C9 already flagged as the real gap - this path still has none).
> **Net: `BC250_HEVC_GPU_PFRAME` should stay opt-in/default-off** until
> either the threshold is tuned against measured PSNR or real motion
> compensation replaces the intra fallback - the fps win is real, but
> it currently isn't "faster at the same quality," it's "faster at
> lower quality," which was never the deal this item was validated on.
> DEVLOG §39.

> **Board, 2026-09-22: one of the two hypotheses above is refuted -
> the threshold is not the lever.** Real PSNR (fixed 20M/1440p/gop=120/
> `testsrc2`, `qsweep`'s own PSNR machinery, not the earlier broken
> stats-file script) across the same three threshold values C6 already
> byte-swept:
>
> | threshold | fps | PSNR |
> |---|---|---|
> | 768 (stricter) | 69.36 | 35.61 dB |
> | 1536 (default) | 70.69 | 35.59 dB |
> | 3072 (looser) | 70.17 | 35.60 dB |
>
> A 4x range on the threshold moves PSNR by **0.02 dB** - noise, not a
> trend, even though C6 already confirmed the same range moves byte
> count substantially under fixed QP. Under real VBR rate control at a
> fixed bitrate target, the RC absorbs whatever the threshold does to
> raw byte count by adjusting QP, so the aggregate PSNR converges
> regardless of where the threshold sits. **This means tuning
> `BC250_HEVC_SKIP_THRESHOLD` will not close the P-frame-vs-intra-only
> quality gap above** - the remaining hypothesis (intra fallback is a
> ceiling below what real motion compensation would deliver, C9's
> already-flagged gap) is now the only one left standing. Re-tuning the
> threshold is no longer worth attempting for this purpose; real motion
> compensation is the only path to closing the gap.

> **Board, 2026-09-21: threshold knob confirmed to work, longer GOP
> still exact.** Sweep at 1280x720/QP27/gop=30 (default formula gives
> 1536 at this QP): half (768, stricter) → 3,422,701 bytes; default
> (1536) → 3,178,793; double (3072, looser) → 2,269,616. Monotonic in
> the right direction — the knob does what it's documented to do.
> PSNR extraction in this pass had a script bug (stats-file parsing,
> not a hardware issue) and needs re-running before any dB number is
> trusted; the byte trend alone confirms the mechanism, not the right
> operating point. Pixel-exactness re-confirmed at a longer GOP than
> previously tested: **135/135 frames exact at gop=45** (3 full GOPs,
> up from gop=30). DEVLOG §38.

**Still needed**: the skip threshold's real rate/quality tradeoff in dB
(byte trend confirmed, PSNR extraction needs re-running), the
CPU-fallback interaction, genuinely non-synthetic content, GOPs longer
than 45 frames, and resolutions above 1280x720. `docs/notes/c7-gpu-pframes.md`
and `docs/notes/c7-pframe-throughput.md` have the full design.

**C8. DECIDED (2026-09-22): NO-GO for now.** 854x480 encodes as 856x480; not
fixable as the driver stands, since ffmpeg rounds up before
`vaCreateContext` and `VAEncSequenceParameterBufferHEVC` has no
conformance-window fields, so the true size never arrives through the
normal path at all. `lab dims` reports these as LIMIT rather than
failing. H.264 is exact at every resolution tested (its own SPS crop
fields already carry the true size, `b2b7fef`).

The only channel the true size could ever reach this driver through is
ffmpeg's own packed SPS (`VAEncPackedHeaderDataBufferType`), which
computes a correct conformance window from `avctx->width/height` — but
accepting it is a real behavioural change, not a bug fix: advertising
`VA_ENC_PACKED_HEADER_SEQUENCE` (even to parse two numbers out of it,
never splicing ffmpeg's actual bytes into the bitstream) makes ffmpeg
build container extradata from *its own* SPS while the in-band
bitstream stays this driver's SPS — a narrow but real spec-conformance
risk, scoped specifically to file-based/muxed output (`ffmpeg -c:v
hevc_vaapi ... output.mp4`, a use case this project's own README
documents), not the RTP/Sunshine streaming path this driver actually
exists for. Full design (the narrow fix: parse only `pic_width/height_
in_luma_samples` + the conformance-window fields out of ffmpeg's SPS,
change nothing else) and the exact risk mechanism, read from the
existing `VAConfigAttribEncPackedHeaders` comment rather than assumed:
`docs/notes/c8-packed-headers.md`.

**The decision: don't build it now.** The risk is scoped to file/muxed
output, which is real but not this project's reason to exist (Sunshine/
RTP streaming, where no container extradata is ever built from either
SPS). Against that: the only user-visible symptom is `lab dims`'
already-documented LIMIT case, three specific sizes, cosmetic (the
decoded picture is correct, just not the exact odd size requested).
Spending a `VAConfigAttribEncPackedHeaders` advertisement + SPS-parsing
path to fix three cosmetic sizes, with a real spec-conformance risk on
a use case this driver doesn't target, isn't worth it today. Revisit if
a real user hits the LIMIT case on the file-output path specifically
(not streaming) and asks for it — the design in this note is ready to
implement at that point without re-deriving anything.

**C9. DONE — board-validated 2026-09-21. +19.6 dB.** The gap was not
tuning. See `docs/hevc_scope_note.md` for the full writeup and DEVLOG §35
for the board run.

> **Board result, and it was a real risk.** This same deblocking edit is
> recorded as collapsing PSNR to 4.85–9.78 dB on hardware on 2026-09-19
> (a genuine syntax error then — two bits missing). The prediction
> written down *before* the run was: CPU HEVC at qsweep's gop=120 was
> **24.93 dB**; if the fix is real it lands in the low 40s, and if the
> September failure has recurred it comes back under 10. Measured on the
> board at 2560x1440, `testsrc2`, 150 frames, gop 120:
>
> | path | before | after |
> |---|---|---|
> | HEVC **CPU** | 24.93 dB | **44.56 / 44.59 dB** (15M / 31M) |
> | HEVC **GPU** intra | 42.71–42.94 dB | **42.81 / 43.27 dB** |
>
> The CPU path now *exceeds* the GPU intra path, which is the expected
> ordering: the CPU path has P-frames and the GPU path is all-intra, so
> at a fixed bitrate the CPU path should win once its reference chain
> stops diverging from the decoder's. It was losing by 18 dB purely
> because of the unmodelled loop filter. The GPU path's slice header
> changed symmetrically and is unharmed — that had had no hardware run
> at all before this.
>
> Full `lab gate` PASS alongside: units 5/5, mask EXACT on 400 audited
> frames, H.264 quality 48.48 dB / SSIM 0.9963, `dims` 11/14 exact with
> the 3 documented HEVC alignment LIMITs, drift 3/3 byte-exact on both
> planes. HEVC drift byte-exact on **both** the CPU and GPU paths.

1. **The PPS left deblocking enabled and the encoder never modelled it.**
   Harmless all-intra (0.02 dB), compounding on P-frames: the encoder's
   reference is its own unfiltered reconstruction, the decoder's is the
   filtered one, and they diverge further with every P-frame until the
   next IDR. `testsrc2` 640x480 CQP 27 gop 120: the encoder reconstructs
   43.64 dB and the decoder delivers 38.07. Now signalled off — same
   bytes, **38.07 → 43.64 dB**. `tools/hevc_host_drift.sh` was blind to
   this by construction because it decoded with `-skip_loop_filter all`;
   it no longer does, and is byte-exact without it, which is what proves
   the new PPS bits parse correctly (the 2026-09-19 attempt at this edit
   really was a syntax error — two bits missing).
2. **`derive_merge_candidates()` appended the GPU's motion vector to the
   merge list.** It is not a merge candidate; with TMVP off the decoder
   fills that slot with zero motion, so encoder and decoder built
   different blocks with no residual to correct it. On-board only, which
   is why nothing caught it: off-board every candidate is (0,0), so every
   `merge_idx` picks the same vector. `BC250_HEVC_FAKE_GPU_MV` now
   supplies a non-zero one — (2,0) failed 17271/24576 luma samples before
   the fix, 0 after, and six such cases are permanent oracle cases.

**Consequence, and it's DONE (2026-09-21), not still-open weight.** With
the GPU vector gone, the merge list is a fixpoint at zero, so
`hevc_motion_search_diamond_8x8()` had no effect on the bitstream — but
verified, not assumed, first: its SAD *did* still reach `cu_skip_flag`
via a shortcut, so the claim as originally written was half wrong.
Removed, with the skip decision now computed directly and shared with
rate control. **-20.7% at 720p gop10, -7.6% at two other configs, -0.6%
on an all-intra control**, 37/38 bitstreams byte-identical (one VBR case
moves by 103 bytes — rate control now gets the honest zero-MV SAD
instead of a search result the bitstream could never ask for). The GPU
ME dispatch itself was left in place (shared code path with H.264;
removing it is separate, larger work) — see `docs/notes/dead-motion-search.md`.

**Still open after this:** real inter coding. The P path is zero-motion
SKIP plus intra fallback, with no motion compensation and no inter
residual. Doing it properly means `merge_flag`=0 + AMVP + `mvd_coding` +
`rqt_root_cbf`. That is the item that would make HEVC a real streaming
alternative, alongside C7.

**Re-measured on hardware 2026-09-21 — confirmed.** See the box at the
top of this item.

---

## D. Opened by the 2026-09-21 board run

**D1. BOARD-CONFIRMED (2026-09-21) — the flatness is fixed, H.264's
response shape is now close to libx264's.** `lab qsweep --codec=h264
--bitrates=8M,15M,20M,25M,31M`: qp_avg falls **45.3 → 27.6** across the
range (was pinned ~27-28 regardless of bitrate) and 15M→31M PSNR moves
**+5.55 dB** — against libx264's own **+6.31 dB** over the identical
range, and against this project's own pre-fix **+0.48 dB** for the same
comparison. Still trails libx264 in absolute terms (no B-frames/sub-pel
ME/RDO — disclosed, expected), but the bug D1 targeted (the shape, not
the absolute level) is gone. HEVC CPU shows a real response low in the
range (8M→15M: +5.15 dB) then plateaus 15M→31M (+0.11 dB) — consistent
with `qp_min=12`'s already-documented 1440p floor, not a recurrence,
though not directly confirmed (HEVC's `[BC250_PERF_FRAME]` qp column is
H.264-only; a `BC250_PERF_STATS=1` HEVC run would confirm QP is actually
pinned at 12 by 15M). DEVLOG §36.

**ROOT-CAUSED AND FIXED OFF-BOARD (2026-09-21), before the above —
the original diagnosis.** `VAEncPictureParameterBufferH264/HEVC.pic_init_qp`
and `VAEncMiscParameterRateControl.initial_qp` are mandatory **per-frame**
VA-API fields (DEVLOG §10.7: Sunshine really does resend one every frame),
wired straight into `*_encoder_set_qp()`. That function stomped
`rc.base_qp`/`rc.current_qp` on *every* call with no "did this actually
change" guard — unlike `*_encoder_set_bitrate()`, which has always had
one. The feedback loop kept running every frame, but around an anchor
reset back to the resent hint before it ever accumulated more than one
frame's `±2`/`±3` step, which is why mean QP tracked the hint almost
exactly and barely moved between bitrates on any path.

Fixed the same way `set_bitrate()` already was. Reproduced off-board
*before* touching the fix: `tools/rc_bench.c` drives the raw encode path
directly (no VA-API, no GPU, no board) and simulating the stomp alone
reproduces the board's flat pattern almost exactly; post-fix, a resent
unchanged hint is a no-op and the real controller responds to a 2x
bitrate change with an 11.5 QP swing — matched independently by rebuilding
and re-running before merging. HEVC CPU PSNR at 15M→31M: **44.39→44.49 dB
(+0.10, matches the board's flat +0.03) before the fix, 35.39→42.40 dB
(+7.00, libx264-shaped) after.**

**Read that last number carefully — quality at 15M *drops* (44.39→35.39
dB) after the fix.** That's the controller correctly spending the lower
budget the caller actually asked for instead of ignoring it, not a
regression, but it's a real operating-point change with real
client-perceived-quality implications and needs a board re-measure
against a real session before this is called fully done, not just
root-caused. `docs/notes/d1-rate-control.md` has the full writeup.
H.264's raw path codes no residual by construction, so only the QP-walk
*mechanism* is validated for H.264 off-board — the quality claim there
still needs the GPU path or the board.

**Board re-measure, closing this note (2026-09-22).** This session's own
fresh release-candidate `qsweep` (`testsrc2`, 2560x1440, gop=120, real
GPU H.264 path, not the off-board raw-residual simulation above) gives
the full response curve on the actual shipped path:

| bitrate | 8M | 15M | 20M | 25M | 31M |
|---|---|---|---|---|---|
| ours PSNR | 35.44 | 36.74 | 38.31 | 39.81 | 41.84 |

+6.40 dB from 8M to 31M, monotonic and libx264-shaped (libx264 over the
same range: +9.00 dB, 38.91→47.91) — the flat-response bug this item
fixed does not reappear on the real GPU path, and the "quality legitimately
drops at a given bitrate once the controller stops ignoring it" effect
predicted above is the correct read: this curve is the controller
actually spending each budget, not the old ~flat curve pinned near the
top of the range regardless of what was asked for. GPU-path quality
claim now confirmed on the board, not just off-board — this closes the
"needs a board re-measure" gap. The remaining gap versus libx264
(3.5-6.1 dB, widening with bitrate) is real and tracked as its own
finding in DEVLOG §39, not a rate-control defect — no B-frames/sub-pel
ME/RDO here, disclosed and expected.

Original finding, in the same board run, across 15M → 31M at 1440p
`testsrc2`:

| encoder | 15M | 31M | delta |
|---|---|---|---|
| ours, H.264 | 42.61 dB (qp_avg 27.9) | 43.09 dB (qp_avg 27.5) | **+0.48** |
| libx264 | 41.60 dB | 47.91 dB | **+6.31** |
| ours, HEVC CPU | 44.56 dB | 44.59 dB | **+0.03** |
| ours, HEVC GPU | 42.81 dB | 43.27 dB | **+0.46** |

So we **beat libx264 by 1.0 dB at 15M and lose to it by 4.8 dB at 31M**,
entirely because doubling the budget moves our QP by 0.4 while libx264
converts it into 6.3 dB. Every one of this encoder's paths shows the
same flat response, which points at rate control rather than at any
codec-specific issue.

Do NOT reflexively blame `qp_min = 12` — lowering that was measured as
+14% bits for −22% throughput and no visible change (§18), and qp_avg
here is ~27, nowhere near the clamp. The question is why the controller
does not *drive* QP down when the bucket has room. Related in shape, but
not the same finding, as the HEVC "when frame size barely responds to QP
the lever is block count" result.

This matters for the only real client: Sunshine sessions commonly run
15–50 Mbps, so the upper half of that range is where we are weakest.

**D2. STILL OPEN — an ambient figure recorded, deliberately not compared
against the old idle baseline.** The board was running a live Steam/
gamescope session during this run (`gamescope`, `Xwayland`, two
`steamwebhelper` at ~25% and ~14% CPU). Both our encoder and libx264
came in below their recorded idle figures, and *both* fell by a similar
proportion — which is the signature of CPU contention, not the GPU
contention the synthetic `nlmeans_vulkan` generator produces.

> **Board, 2026-09-21: a fresh ambient figure, with a new confound
> disclosed rather than papered over.** H.264 2560x1440/gop=120/31M,
> Steam confirmed running (`steamwebhelper` 26.0%/15.2%/3.0% CPU):
> **87.2 / 88.7 / 89.0 fps**, tight (~2%) spread. **Deliberately not
> compared against the recorded 75.0 fps idle figure** — this session's
> own D1 fix changed H.264 rate control's QP response, which changes
> encode speed independently of any contention effect, so the two
> numbers now differ for at least two reasons, not one, and a clean
> attribution to Steam/gamescope needs a fresh idle baseline on
> *today's* build, not the old one. Did not stop `gamescope-session-
> plus` for a true paired comparison — no physical console access to
> this board, and a session that failed to restart cleanly would need
> someone at the machine to recover it. That remains the real next
> step, on record rather than attempted blind. DEVLOG §38.

Worth one deliberate paired run (same content, same bitrate, Steam up
vs Steam down, on the *current* build) to replace the unsourced
"60 → 11 fps" claim §24.6 has been carrying.

> **Board, 2026-09-22: still genuinely blocked on the paired comparison,
> but one new corroborating data point.** Checked board session state
> before assuming idle: `gamescope-session-plus`, both `Xwayland`
> instances and `steamwebhelper` (25.7%/15.0% CPU on two processes) have
> all been running continuously for **1 day 5h29m** - there is no
> current idle window to baseline against, and stopping a session with
> no physical console access remains the same real risk this item
> already declined to take blind. Not attempted.
>
> What this does add: today's own release-candidate `scoreboard` run
> (load=none, same 2560x1440/testsrc2 settings) landed at **67.29 fps**
> - matching this session's separately-run `qsweep` figure (67.22 fps)
> to within run-to-run noise, both taken with Steam/gamescope confirmed
> live throughout. That stability across two independent runs, same
> build, both with Steam up, is evidence (not proof) that an *idle
> desktop* Steam/gamescope session (UI up, no game launched) is not a
> strong perturber of this metric at these settings - unlike the
> documented ~45x hit from an actual heavy GPU compute load. This
> narrows, but does not close, the open question: the unsourced
> "60 → 11 fps real game" claim this item exists to replace is
> specifically about a *running game* (real GPU+CPU load), a materially
> different condition from "Steam client idle, nothing launched" - which
> is all today's data (or last session's 87.2/88.7/89.0 fps figure)
> actually represents. The paired idle-vs-Steam-UI run stays the
> concrete next step if a true idle window ever exists; a paired
> Steam-UI-vs-real-game run is arguably the more useful one for what
> this item was actually trying to answer, and needs someone at the
> physical console to launch and hold open a real game.

---

## E. Opened by the 2026-09-22 board round

**E1. NEW, not yet investigated.** `lab compare`'s H.264 run this round
(the A2 board check) surfaced two nonzero error counters it flags on
sight: `err_alloc_failed=2` and `err_slice_overflow=41`, summed across
3 runs of 300 frames each (`bench()`'s default config: `testsrc`,
2560x1440, gop=120, 31M) - roughly **4.6% of frames hitting a real
slice-buffer overflow** (`encoder_h264.c`'s `slice_overflow` path,
which B6 fixed to fail the frame cleanly rather than silently truncate
- so this is 41 *failed* frames, not 41 corrupted ones, but still 41
frames of real content this bench config could not encode at all).
Identical count on both the baseline and today's key, so nothing
landed today caused it - it was already there, just never surfaced
before because nothing previously printed these counters this
plainly. Not chased this session (out of scope for what this round's
board job was launched to check) - the counter values, the 31M/`testsrc`
combination B6 already flagged as capable of exceeding even the
resized slice buffer on noise-like content, and the exact repro
command (`tools/lab bench <key>` or any `compare`) are enough to pick
this up as its own item.
