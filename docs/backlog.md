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

Still open here: a cheaper `cavlc_scan_coeffs` (e.g. deriving `last_idx`
and the zero runs from a nonzero bitmask instead of walking every scan
position, or narrowing `scanned[]` to `int16_t` so the gather is a
16-bit shuffle). Untried. Byte-identity is easy to check - `cavlc_bench
emit` md5, the reference decoder, and ffmpeg.

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
and mode search are invisible to *every* oracle here. The items still
need running on the board — start with the mode histogram plus
`drift --content=testsrc2`.

**A4. `hevc_cabac_code_residual_4x4` (~5.9% of profile).** Entropy coding
of the CPU path.

**A5. `hevc_choose_luma_mode` (~7.8%).** Tries all four candidates with a
full prediction + SAD each. A cheaper first-pass metric or early
termination may cut it, but it changes mode decisions unless done
carefully — if output changes, this becomes a compression change and
needs BD-rate on the board, not a byte comparison.

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
> complete picture and points sharper at the leading candidate above —
> whoever picks this up next should start with why LUMA drifts under
> real decode, not with the missing chroma binding. DEVLOG §36.

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

**C6. STILL OPEN.** The performance-mode governor question is
inconclusive, not negative: `pp_dpm_sclk` still read 7 MHz after a
`--fixed-frequency 2000` pin, so the request never visibly took (the
script drives the governor over D-Bus and wants root; it was run
unprivileged and reported success anyway). Needs root, and needs the
*light game* load it is actually about. `lab bench` records `sclk_mhz`
now, so the next attempt can confirm the clock moved first.

**C7. STILL OPEN, and now the highest-value HEVC item.** P-frames for
the GPU intra path. It is all-intra only, which is what blocks C5 and
what keeps HEVC from being a real alternative to H.264 for streaming.

**C8. NEW — HEVC rounds odd frame sizes up to a multiple of 8.**
854x480 encodes as 856x480. Not fixable as the driver stands: ffmpeg
rounds up before `vaCreateContext` and
`VAEncSequenceParameterBufferHEVC` has no conformance-window fields, so
the true size never arrives. Fixing it means **accepting packed
headers**, which would let ffmpeg's own SPS through — a real change with
its own risks. `lab dims` reports these as LIMIT rather than failing.
H.264 is exact at every resolution tested.

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

**Consequence to be aware of:** with the GPU vector gone, the merge list
is a fixpoint at zero, so every P-frame MV is (0,0) and both
`hevc_motion_search_diamond_8x8()` and the P-frame GPU ME dispatch are
now pure cost with no effect on the bitstream (the search still feeds
`last_frame_sad` for rate control). Not a regression — the encoder could
never legally signal a non-zero vector — but it is dead weight, and
removing it is a free CPU win for whoever picks up C2's thread.

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

**D2. Publish a load-condition figure that is not synthetic.** The board
was running a live Steam/gamescope session during this run (`gamescope`,
`Xwayland`, two `steamwebhelper` at ~25% and ~14% CPU). Both our encoder
and libx264 came in below their recorded idle figures, and *both* fell
by a similar proportion — which is the signature of CPU contention, not
the GPU contention the synthetic `nlmeans_vulkan` generator produces.
This is the closest thing to a real-session number this project has, and
it is still not a controlled measurement: the content and bitrate differ
from the idle runs it would be compared against. Worth one deliberate
paired run (same content, same bitrate, Steam up vs Steam down) to
replace the unsourced "60 → 11 fps" claim §24.6 has been carrying.
