# A6 — porting the compression work from `cavlc-residual-coding`

Status: **piece (1), the 33 angular intra modes, done and byte-exact; the
RD/MPM-cost gap this file's first pass flagged is now closed with an
RD-biased mode search (see "Follow-up" below) — genuinely improved, but
still not a clean, unconditional win.** Pieces (2) (all-TU-size
transforms) and (3) (undivided 16x16 CUs) **not attempted** — see "Why
(2)/(3) were not attempted" below. This is a deliberate stop, per the
task's own prioritization rule ("only attempt (2) if (1) is solid, only
(3) if (1) and (2) are solid").

**2026-09-21 follow-up, read this first if you're deciding whether to
merge piece (1):** the original pass below (byte-exact, done) shipped a
SAD-only mode search that this file's own measurement section caught
making a genuine RD regression on 2 of 5 sample points (smaller AND
lower-PSNR at fixed QP - a worse operating point, not a Pareto
improvement). `hevc_choose_luma_mode()` now minimizes `sad + lambda *
rate_bits(mode)` instead of raw SAD, using the real bypass-bit cost of
MPM-hit vs escape signaling and a QP-dependent lambda. Re-measuring the
same 5 points plus the pseudo-random set: **the same 2 of 5 points remain
a "smaller and worse" trade (not zero), but measurably less severe on
both, and the other 3 of 5 flip from "bigger, better" to genuine Pareto
improvements (smaller AND better).** See "Follow-up: RD-biased mode
search" below for the full numbers, why "zero smaller-and-worse cases" may
not be an achievable bar for a real lambda-weighted RD search on this
content, and what's still open.

## What backlog A6 actually asked for, and what this delivers

`docs/backlog.md`'s A6: undivided 16x16 CUs (measured -34.8% BD-rate on
`cavlc-residual-coding`), all-TU-size transforms, the full 33 angular
modes — "a port, not a merge," because the two trees implemented HEVC
independently (58 conflicts, add/add on every core HEVC source).

This delivers the **angular modes** piece: `hevc_choose_luma_mode()` and
`hevc_predict_4x4()` in `approach1-compute-encoder/src/hevc_intra.c` now
search and predict all 35 HEVC intra modes (0=Planar, 1=DC, 2-34=angular)
at the encoder's existing 4x4-TU / 8x8-CU / NxN-PU granularity, replacing
the old 4-candidate (Planar/DC/Horizontal/Vertical) search. CU/TU
structure, transform sizes, and everything else in `encoder_h265.c`'s
`encode_cu()`/`encode_ctu()` is **unchanged**.

## Why this was a port from the GPU shader, not from `cavlc-residual-coding`

The task brief said to read `cavlc-residual-coding`'s intra code as leads,
not facts, since it's a genuinely independent implementation. Before
touching that tree, `main` turned out to already contain a **second,
independently-written, board-verified** implementation of the exact
math this piece needed: `hevc_intra_wavefront.comp`, the GPU shader that
does the full 35-mode angular search over an undivided 16x16 CU
(`docs/hevc_scope_note.md`'s "v0.3.0" section; `lab drift` on the board
confirms it byte-exact against ffmpeg's decode at QP 4/27/51 on both
planes — see that shader's own header comment).

That shader's `ANGLE[35]`/`INVANG[15]` tables and its `predict()`
function's angular branch (8.4.4.2.6) are exactly Rec. ITU-T H.265's
public Table 8-5/8-6 and formula — nTbS-agnostic, board-validated, and
already living in this same repo. Porting *that* down to a 4x4 TU is a
straight reparameterization (2*nTbS goes from 32 to 8 samples per side;
8.4.4.2.3 reference-sample smoothing drops out entirely, because
Table 8-3's `intraHorVerDistThres` has no row for nTbS=4 — `filterFlag`
is unconditionally 0 there, one whole subsystem the shader has that this
port doesn't need). This is a lower-risk source than transcribing
`cavlc-residual-coding`'s own angular code (which I did read — see
below — and which does the identical spec math, unsurprisingly, since
both are transcriptions of the same public formula), because the shader
version already has an independent, hardware-verified oracle behind it
in *this* tree.

`hevc_intra_wavefront.comp`'s own SCOPE comment is explicit that CPU/GPU
mode-decision parity was never a design goal ("the two paths have never
agreed on mode decisions and are not required to"), so widening the CPU
path's mode range does not create a new parity obligation — consistent
with the task brief's instruction on this point. The shader's build and
tests are untouched; nothing in this change touches
`hevc_intra_wavefront.comp`, `gpu_compute.c`, or the GPU dispatch path.

## What I read in `cavlc-residual-coding`, and what's actually valuable there

Read `approach1-compute-encoder/src/hevc_intra.c`, `hevc_intra.h`,
`hevc_cabac.c`, and `encoder_h265.c` on that branch
(`/mnt/c/Users/apple/bc250-work`, branch `cavlc-residual-coding`,
read-only). Findings, treated as leads and checked, not imported wholesale:

- **Its angular math is the same public spec formula.** `predict_angular()`
  takes a `log2_size` parameter (so it's already generalized over TU size,
  unlike this port's fixed nTbS=4) but the core arithmetic matches Table
  8-5/8-6 the same way the GPU shader's does. No surprise, no divergence
  worth importing line-for-line.
- **Its mode search is coarse-then-refine, not exhaustive, and — this is
  the important part — it is NOT SAD-only.** Its own comment
  (`hevc_intra.c` ~line 509-524): "a step-4 sweep across the angular range
  plus Planar/DC, then a +/-1 and +/-2 refinement... plus the three MPMs
  (which are nearly free to signal and therefore worth always costing)."
  It explicitly folds an approximate **rate** term for MPM-vs-non-MPM
  signalling cost into which mode wins, not just prediction SAD.
- **It really does implement variable CU splitting and multiple TU
  sizes**, not just more intra modes: `HEVC_CU_SIZE 8` as the minimum,
  `MaxTbLog2SizeY` raised so an undivided 16x16 CU can carry one 16x16
  luma TU, a real per-CTU `split` decision feeding
  `hevc_cabac_code_split_cu_flag(cab, split, cl + ca)` (encoder_h265.c
  ~line 1082), and CtDepth-aware MPM/context derivation. This is a
  genuinely larger piece of work than the angular-mode piece, matching
  the task brief's warning that (2)/(3) are "the deepest structural
  change."
- The claimed -34.8% BD-rate almost certainly reflects **all three**
  pieces together, likely including the RD-aware mode cost above — not
  the angular-modes piece in isolation. See the measurement below for why
  that distinction matters a lot in practice.

I did not import any of its code. Everything shipped here is either
already-existing `main` infrastructure (see next section) or a
reparameterization of the GPU shader's board-verified formula.

## What was actually changed

`approach1-compute-encoder/src/hevc_intra.c`:

- Added `gather_neighbors_wide()`: the full Rec. ITU-T H.265 8.4.4.2.2
  neighbor-gather/substitution scan out to `2*nTbS-1` samples per side
  (17-entry scan, nTbS=4), replacing the old `gather_neighbors()`'s
  truncated `nTbS+1`-sample version for every live caller. The old
  narrow gather is kept, `__attribute__((unused))`, as the historical
  record next to the below-left-reference comment it's tied to — it has
  no more callers.
- Added `HEVC_ANGLE_TABLE[35]` / `HEVC_INVANGLE_TABLE[15]` (Table 8-5/8-6,
  verbatim from the GPU shader) and `predict_angular_sample()` (8.4.4.2.6).
- Added `predict_block4()`, replacing the old 4-case `predict_from_refs()`
  switch: Planar/DC as before, everything else through the angular
  formula (modes 10/26 are not special-cased for the base prediction —
  `ANGLE_TABLE[10]==ANGLE_TABLE[26]==0` collapses the general formula to
  the same result — only their edge filter still is, since that really is
  mode-specific per 8.4.4.2.6).
- `hevc_choose_luma_mode()`: exhaustive SAD-only search over all 35 modes
  (was: 4, unrolled). Gather-once, hoisted-source-block optimisations from
  A5 are preserved; the per-candidate unrolling is not (35 candidates
  doesn't fit that shape), so this is a runtime loop.
- `hevc_predict_4x4()`: now calls the wide gather + `predict_block4()`
  for any mode — used unchanged by the chroma DC-only call site in
  `encoder_h265.c`.

`approach1-compute-encoder/src/hevc_intra.h`: added `HEVC_MODE_COUNT`
(35) — `encode_core_gpu()` in `encoder_h265.c` had a literal 34 with a
comment noting "this tree has no HEVC_MODE_COUNT constant"; it now uses
the constant. Updated stale doc comments claiming only 4 modes are ever
chosen.

`approach1-compute-encoder/src/encoder_h265.c`: only the GPU path's mode
clamp (`mode > 34` → `mode >= HEVC_MODE_COUNT`) changed. **No changes**
to `encode_cu()`, `encode_ctu()`, CU/PU/TU structure, chroma handling, or
anything else — confirmed by re-reading the diff.

**Nothing changed** in `hevc_cabac.c`. `hevc_derive_mpm()`,
`hevc_cabac_code_intra_luma_flag()`/`_data()` (MPM index + 5-bit escape),
`hevc_scan_idx_for_mode()`, and `hevc_cabac_code_intra_chroma_pred_mode()`
were already written generically against the spec's real mode ranges —
they needed zero changes to support 35 modes instead of 4. This was
checked by re-deriving each against the spec section it claims (8.4.2 MPM
derivation, the scan-index thresholds), not assumed from the old
comments, because the old comments' "this only ever sees 4 modes" framing
was exactly the kind of stale claim this project's CLAUDE.md warns about.

## Why not extend the OLD narrow gather instead of writing a new one

`gather_neighbors()`'s substitution scan starts at `p[-1][nTbS]`
(index `nTbS+1` samples from the corner), not at the spec's real
`p[-1][2*nTbS-1]`. That was safe when nothing read past `p[-1][nTbS]`,
which is exactly the below-left bug this project already found once
(`docs/hevc_scope_note.md`) — a positionally-plausible sample that is
genuinely available and genuinely different from its neighbour. Layering
more samples onto that truncated scan would substitute unavailable deep
positions from the wrong "first available" point in the full 8.4.4.2.2
process. `gather_neighbors_wide()` is therefore an independent, from-
scratch application of the real scan over the real extent, not an
extension.

## Verification

**Byte-exactness (`tools/hevc_host_drift.sh`): PASS, 53/53 cases.**
The original 38 cases (21 intra dimension-sweep + 17 inter) all still
pass. Added 15 new cases using three new synthetic patterns
(`tools/hevc_host_repro.c` patterns 4-6: horizontal bars, shallow
diagonal ramp 1:2, steep diagonal ramp 2:1), because the original 4
patterns barely reach the new mode range — see the mode-histogram
measurement below. All new cases are byte-exact against ffmpeg's decode,
on both intra and the inter/P-frame path, with the deblocking-off PPS
fix's real decoder loop filter still on (not `-skip_loop_filter all` —
same oracle discipline `docs/hevc_scope_note.md` established).

```
$ tools/hevc_host_drift.sh
...
HOST DRIFT PASS (53 cases byte-exact)
```

**`ctest -R HevcEncodeBitstreamTest`: PASS.** This test has no hardcoded
bitstream md5 to update (checked: it verifies NAL sequencing and
structural properties, not a literal checksum) — the task brief
anticipated needing to update one; there isn't one in this codebase for
this test, so nothing needed updating.

**Full `ctest`: 6/7 pass.** `VaApiDriverTest` fails in this WSL
environment with repeated `Vulkan error -2` (image allocation) against
`llvmpipe (LLVM 20.1.2, software rasterizer)` — this is a Vulkan/GPU
environment limitation (no real GPU under WSL2's software Vulkan), not a
regression from this change. Nothing in this port touches
`gpu_compute.c`, `va_backend.c`, or any Vulkan call; the failure is in
image-allocation retries inside `gpu_compute.c:1623`, code this change
never touches, for the H.264 encode path this change has nothing to do
with. Flagged honestly rather than silently ignored, but not something
this port could have caused or can fix off-board.

**Mode coverage** (`BC250_HEVC_DEBUG_MODES=1`, 256x256, one frame, all 7
patterns): 31 of 35 modes reached across the pattern set (missing 11, 18,
20, 25 — modes right at the invAngle-table boundary that no synthetic
pattern here happens to select as the SAD-minimizer; the invAngle branch
itself IS exercised, by modes 13, 15, 21-24). Per-pattern histograms:

| pattern | dominant modes | distinct modes |
|---|---|---|
| 0 flat | 0 (all) | 1 |
| 1 vertical bars | 26, 0, 1 (+ a few angular near-vertical) | 11 |
| 2 diagonal ramp (1:1) | 34, 2 (extreme diagonal), 0, 33 | 18 |
| 3 pseudo-random | 21, 22, 31, 30, 23, 13 | 24 |
| 4 horizontal bars | 2, 0, 1 | 8 |
| 5 shallow diagonal (1:2) | 5, 2, 3, 7, 4, 6, 8, 10 | 16 |
| 6 steep diagonal (2:1) | 31, 30, 32, 29, 28, 33 | 13 |

This confirms the new modes are genuinely selected on real content, not
just theoretically reachable.

## The honest, measured, off-board compression picture — and why it's NOT a clean win

Built the pre-change (`HEAD`, 4-mode) and post-change (35-mode) encoders
side by side and compared bitstream bytes at matched QP on identical
synthetic content (`tools/hevc_host_repro.c`, patterns 2/3, sizes 64-256,
QP 12-34):

| size | QP | pattern | old bytes | new bytes | delta |
|---|---|---|---|---|---|
| 256x256 | 12 | diag ramp | 9368 | 8287 | **-11.5%** |
| 256x256 | 27 | diag ramp | 4881 | 4621 | **-5.3%** |
| 128x128 | 20 | diag ramp | 1078 | 1520 | **+41.0%** |
| 64x64 | 20 | diag ramp | 355 | 450 | **+26.8%** |
| 128x128 | 34 | diag ramp | 778 | 1015 | **+30.5%** |
| 256x256 | 12/20/27/34 | pseudo-random | +0.86/+0.68/+1.59/+2.80% | | (all slightly bigger) |

Mixed, and NOT reliably smaller — several cases are noticeably *bigger*.
That alone would be a fair "measured zero, maybe negative" verdict by
this project's own standard (`docs/backlog.md`: "a measured zero is a
result"). But bytes alone don't tell the real story here, and the full
story is worse than "mixed":

**PSNR at the same QP, against the true synthetic source** (decode via
ffmpeg, compare against the exact source frame, not against the
encoder's own reconstruction):

| size/QP/pattern | old: bytes / PSNR-Y | new: bytes / PSNR-Y | reading |
|---|---|---|---|
| 256x256 q12 diag | 9368 B / 53.28 dB | 8287 B / 52.46 dB | **smaller AND worse** |
| 256x256 q27 diag | 4881 B / 44.96 dB | 4621 B / 43.22 dB | **smaller AND worse** |
| 64x64 q20 diag | 355 B / 50.39 dB | 450 B / 50.89 dB | bigger, better |
| 128x128 q34 diag | 778 B / 39.30 dB | 1015 B / 40.22 dB | bigger, better |

The two "byte reduction" cases in this sample are **not** free wins —
they're a real quality *loss* alongside the size drop, i.e. a worse
operating point on the rate-distortion curve at this exact QP, not a
Pareto improvement. The two "byte increase" cases *are* a real quality
gain, but paid for in full or more, not a favorable trade obviously
either.

**Why**: `hevc_choose_luma_mode()`, before and after this change, is
SAD-only with no rate term — same convention as A5, just over more
candidates. A mode with lower prediction SAD is not guaranteed to survive
transform+quantization+CABAC with a proportionally lower bit cost at a
*given* QP, and a mode outside the 3-entry MPM list costs 5 extra bypass
bits (`rem_intra_luma_pred_mode`) that a SAD-only criterion never counts
against it. This is exactly the gap `cavlc-residual-coding`'s own mode
search closes by explicitly costing the three MPMs as "nearly free" and
biasing toward them — a technique this port does not include, because it
was out of scope for "port the angular modes" as literally asked, and
because it's a real behavioural design decision (a second thing to get
right, verify, and be honest about) that deserved its own pass rather
than being folded in silently.

**This means the -34.8% BD-rate claim from `cavlc-residual-coding` should
NOT be assumed to transfer to this port as shipped.** It almost certainly
depends on the RD-aware mode cost and/or the undivided-CU/bigger-transform
pieces this port does not include. What ships here is spec-correct and
byte-exact-verified, genuinely finds better local predictions (the PSNR
numbers prove that part), but is not shown to compress better, and two of
five sampled points show it compressing worse AND lower-quality
simultaneously at fixed QP.

**What would need to happen before trusting any compression number:**
1. Add an RD-aware (or at minimum MPM-cost-aware) term to
   `hevc_choose_luma_mode()`'s candidate scoring — this alone might flip
   several of the above from "smaller and worse" to genuinely better,
   since it directly targets the mechanism identified above.
2. Real BD-rate needs multiple QP points and the interpolation `lab
   qsweep`/the board's BD-rate machinery does — a two-point byte/PSNR
   table like the one above is suggestive, not a BD-rate figure, exactly
   as the task brief anticipated.
3. A board session, not a dev-machine one — off-board `-O2` timing and
   dev-CPU-only execution are fine for correctness, not for anything
   claiming a real number (`docs/performance-measurement.md`'s standing
   rule).

## Follow-up (2026-09-21): RD-biased mode search

Item 1 above, done in this pass. `hevc_choose_luma_mode()`
(`approach1-compute-encoder/src/hevc_intra.c`) no longer minimizes raw
SAD; it minimizes `sad + lambda * rate_bits(mode)`.

**`rate_bits(mode)`** is not an estimate — it's the exact number of
bypass bits `hevc_cabac_code_intra_luma_data()` emits for that candidate,
read directly off that function's binarization: 1 bit if `mode` is the
first MPM candidate (mpm_idx TR(cMax=2), pred_idx 0 → one bypass bin),
2 bits if it's the second or third (pred_idx 1 or 2 → two bypass bins),
5 bits for the fixed-length `rem_intra_luma_pred_mode` escape otherwise.
The one context-coded bin (`prev_intra_luma_pred_flag`) is deliberately
excluded from the model — it's coded for every candidate regardless of
hit/miss, and its real arithmetic-coded length depends on CABAC's
probability state, which a fast SAD-domain search has no way to know.
Bypass bins have no such ambiguity: they cost exactly 1 raw bit each by
construction, so this term is exact about what it counts.

**`lambda`** (`hevc_luma_mode_lambda()`) is QP-dependent: shape from the
field's standard SSD-domain lambda (`lambda_SSD = k * 2^((QP-12)/3)`,
because HEVC's quantization step doubles every 6 QP and SSD scales as
step²), square-rooted because this search's distortion metric is SAD
(scales as step¹, not step²) — same relationship x264 uses between its
SATD/SAD-domain lambda table and its SSD-domain one.

**The scale constant `k` needed real tuning, and the textbook value was
wrong for this search.** `k=0.85` (the commonly-cited x264/HM constant)
was tried first and is calibrated for SATD/SSD costs aggregated over a
whole 8x8–32x32 transform block. Applied to a single 4x4 SAD (16
samples), it over-committed to the cheap-to-signal candidate on
near-perfectly-predictable content — exactly this test's diagonal-ramp
pattern, where the true best angular mode's SAD can be near zero, so
even a "small" absolute lambda swamps a large *relative* SAD gap. Measured
effect at `k=0.85`: 4 of the 5 primary sample points became a
smaller-and-worse trade (up from 2), even though the two original bad
points did get a better byte/dB ratio. A sweep over
`k ∈ {0.05, 0.10, 0.15, 0.20, 0.30, 0.85}` against the same 5 points
found **`k=0.15`** gives the best result of those tried — see the table
below. This is an empirically-tuned constant, not a purely analytic one;
it is tuned against this project's synthetic diagonal-ramp/pseudo-random
test content specifically, and a real board/`qsweep` session against
natural video could call for a different value. That further tuning is
explicitly not done here — see "still open" below.

### Measurement: old (pre-A6, `47c74c9`) vs new (RD-biased, `k=0.15`)

Same procedure as the first pass's table: `tools/hevc_host_repro.c`
(patterns 2/3), decode both bitstreams to raw YUV, compare against an
independently-generated true source frame (never a raw stream against a
fresh `-f lavfi` source directly, per `CLAUDE.md`), `ffmpeg -lavfi psnr`.
"old" here is the actual pre-A6 4-mode baseline (`47c74c9`, `HEAD~1`
before A6's own commit `f484edb`) — not the intermediate SAD-only 35-mode
build the first pass compared against — so this table answers "is piece
(1) plus this follow-up better than what shipped before A6 touched this
code at all," which is the question that actually matters for merging.

| size/QP/pattern | old: bytes / PSNR-Y | new (RD, k=0.15): bytes / PSNR-Y | delta | reading |
|---|---|---|---|---|
| 256x256 q12 diag | 9368 B / 53.28 dB | 7408 B / 52.64 dB | -20.9% / -0.64 dB | smaller, worse |
| 256x256 q27 diag | 4881 B / 44.96 dB | 3364 B / 44.54 dB | -31.1% / -0.42 dB | smaller, worse |
| 128x128 q20 diag | 1078 B / 50.57 dB | 1039 B / 50.97 dB | -3.6% / **+0.40 dB** | **smaller AND better** |
| 64x64 q20 diag | 355 B / 50.39 dB | 339 B / 50.83 dB | -4.5% / **+0.44 dB** | **smaller AND better** |
| 128x128 q34 diag | 778 B / 39.30 dB | 764 B / 39.49 dB | -1.8% / **+0.20 dB** | **smaller AND better** |
| 256x256 q12 rand | 68210 B / 49.80 dB | 68806 B / 49.80 dB | +0.87% / +0.00 dB | ~flat |
| 256x256 q20 rand | 54208 B / 42.40 dB | 54593 B / 42.70 dB | +0.71% / +0.30 dB | bigger, slightly better |
| 256x256 q27 rand | 41921 B / 36.04 dB | 42572 B / 35.72 dB | +1.55% / -0.32 dB | bigger, slightly worse |
| 256x256 q34 rand | 31072 B / 28.99 dB | 31914 B / 29.10 dB | +2.71% / +0.11 dB | bigger, ~flat |

**Reading this against the bar this follow-up was asked to clear
("fewer/zero smaller-and-worse cases"):** the count of smaller-and-worse
points did NOT drop to zero — it's still 2 of 5 primary points, exactly
the same 2 (256x256, QP 12 and 27, diagonal ramp) the first pass flagged.
What changed is severity and the other 3 points:

- Both remaining bad points got a **better** byte/quality ratio than the
  SAD-only version: QP 12 went from -11.5%/-0.82 dB (SAD-only 35-mode vs
  old) to -20.9%/-0.64 dB (more bytes saved, less quality lost); QP 27
  went from -5.3%/-1.74 dB to -31.1%/-0.42 dB (a much better trade on both
  axes at once).
- The other 3 of 5 points, which were "bigger, better" under the
  SAD-only version (a real quality gain, but not obviously a good
  trade — paid for in full or more, per the first pass's own words), are
  now genuine Pareto improvements: smaller AND better, not a trade at
  all.
- Pseudo-random content (no clean angular structure, no near-zero-SAD
  ties to be swayed by a rate term) is close to unaffected by this
  follow-up either way — consistent with the mechanism above: lambda
  only matters when the SAD gap between candidates is already small.

**Why "zero smaller-and-worse cases" may not be the right bar for a real
RD search, and this is not a case of aiming low:** a lambda-weighted RD
search is *supposed* to sometimes trade distortion for rate when that
trade is favorable — that's the entire point of adding a rate term. Two
points remaining "smaller and worse" at a *fixed* QP does not by itself
mean the trade is bad; it would only be a genuine regression if the same
byte count could buy better PSNR some other way, or the same PSNR could
be had in fewer bytes — a question only a real BD-rate curve (multiple QP
points, interpolated) can answer, not a single-QP byte/PSNR pair. That
caveat was true of the first pass's table and is equally true of this
one; it is not resolved by this follow-up and needs its own pass (see
below).

### Is piece (1) + this follow-up now a real, mergeable win?

**Better than the first pass shipped, still not a clean, unconditional
win — this is the same kind of honest "not there yet" result the first
pass reported, not a false "fixed."**

What's solid:
- Byte-exactness is unchanged and re-verified: `tools/hevc_host_drift.sh`
  still prints `HOST DRIFT PASS` on all 53 cases, and
  `ctest -R HevcEncodeBitstreamTest` still passes, both at the final
  `k=0.15` build. This change is mode-decision-only (which mode SAD+rate
  picks), so the drift oracle's invariant here is "does whatever mode
  gets picked still decode byte-exact," not "does the same mode get
  picked as before" — confirmed understood, and it is what was checked.
- Full `ctest`: still 6/7, same single pre-existing failure
  (`VaApiDriverTest`, `Vulkan error -2` under WSL's software `llvmpipe`
  Vulkan) — reproduced on the unmodified pre-A6 baseline too in this same
  environment, so it is not a regression from this change or from A6.
- The two `docs/backlog.md`-flagged-worthy regressions this follow-up
  targeted did shrink in severity on both axes, and the mechanism
  (`rate_bits()` costed the actual bypass-bin counts) and the fix
  direction (QP-dependent lambda, larger bias at higher QP) both work as
  designed — case-by-case behavior matches the intended shape, not
  coincidence: the highest-QP primary point (q34) is the cleanest Pareto
  win among the five.
- Chroma QP, below-left-reference availability, deblocking-off PPS
  signalling, and the dead-motion-search inter path are all unaffected —
  this follow-up touches only `hevc_choose_luma_mode()`'s cost function
  and hoists (does not change the logic of) the MPM derivation in
  `encoder_h265.c`'s `encode_cu()` from Step 2 to Step 1; nothing else in
  either file changed. Re-verified via the same 53-case drift suite,
  which includes chroma, inter/P-frame, and VBR-rate-control cases.

What's still open, honestly:
1. **The remaining 2 of 5 "smaller and worse" points are not resolved,
   only improved.** Whether they represent a real BD-rate regression or
   a favorable trade this single-QP table can't see is still an open
   question — needs a real BD-rate curve (see below).
2. **`k=0.15` is empirically tuned on this project's synthetic test
   content** (diagonal ramps and pseudo-random noise), not derived from
   first principles or validated against natural video. It is very
   plausibly not the right constant for real content, where SAD values
   are rarely as close to zero as a perfect gradient's. A board/`qsweep`
   session against real footage is the next step before trusting this
   constant for anything beyond this dev-machine measurement.
3. **Real BD-rate** (multiple QP points, interpolated, per `lab
   qsweep`/the board's BD-rate machinery) is still not done — this
   remains a two-point byte/PSNR table, suggestive but not a BD-rate
   figure, exactly as before.
4. Matching this file's own earlier hypothesis: the claimed -34.8%
   BD-rate from `cavlc-residual-coding` almost certainly still depends on
   pieces (2) (all-TU-size transforms) and (3) (undivided 16x16 CUs)
   alongside RD-aware mode search, not on any one piece alone. This
   follow-up closes the specific "SAD-only ignores signaling cost" gap
   piece (1) had, but does not by itself establish that piece (1) is
   worth shipping in isolation for a compression win — only that it is no
   longer *actively* undermined by an uncounted rate term the way it was.

**What I'd try next, in order:** (1) a real board `qsweep`/BD-rate run
against natural content to see whether `k=0.15` (or some other value)
actually improves BD-rate over the pre-A6 baseline, since this dev-
machine synthetic-pattern table cannot settle that; (2) if BD-rate is
still not clean, revisit whether the lambda shape itself (not just the
scale) is right for a single 4x4 SAD rather than an aggregated
transform-block cost — the mismatch identified above (near-zero-SAD ties
on smooth content) may need a different-shaped term, not just a smaller
constant; (3) attempting pieces (2)/(3) alongside this, per this file's
own earlier hypothesis that they may be where most of the real
compression win actually lives, with RD-aware mode search as a
supporting piece rather than the main lever.

## What was preserved (re-verified, not assumed)

- **Chroma QP (Table 8-10)**: untouched. Chroma stays DC-only
  (`hevc_predict_4x4(..., HEVC_MODE_DC, 0, ...)` in `encoder_h265.c`,
  unchanged); `hevc_cabac_code_intra_chroma_pred_mode()`'s
  luma-mode-dependent DM/index-3 logic was re-checked against the full
  0-34 luma range (not just the old 4 modes) and is still correct: it
  only ever compares the candidate DC value (1) against `luma_mode_pu0`,
  which is valid for any luma mode value.
- **Below-left reference-sample fix**: superseded, not silently dropped.
  `gather_neighbors_wide()` re-derives below-left (and every other
  position) availability from `zorder_available()`, the same function
  the original fix introduced, now called over the full 8-sample extent
  instead of the fixed single below-left sample. Confirmed by the drift
  PASS, which is exactly the oracle that caught the original bug.
- **Deblocking-off PPS signalling**: untouched (`write_pps()` not
  modified); confirmed still correct because `hevc_host_drift.sh` runs
  WITHOUT `-skip_loop_filter all` (per `docs/hevc_scope_note.md`'s
  discipline) and still passes.
- **Dead motion search removal (C9)**: untouched; inter-path cases in
  the drift suite still pass, including the six `BC250_HEVC_FAKE_GPU_MV`
  regression cases.

## Why (2) all-TU-size transforms and (3) undivided 16x16 CUs were not attempted

Per the task's own ordering, each is gated on the previous being solid.
(1) is byte-exact, but the RD-criterion finding above means "solid" here
means "spec-correct," not "known to compress better" — and stacking (2)
and (3) on top of an unresolved RD-search question would make it
strictly harder to attribute any future measurement to the right cause.
Concretely, from reading `cavlc-residual-coding`'s implementation:

- **(2) needs real `split_transform_flag` signalling and 8x8/16x16/32x32
  luma kernels** on top of the existing 4x4 DST-VII/DCT-II — the
  transform/quant infrastructure in `hevc_cabac.c` for arbitrary
  `log2_size` (`hevc_cabac_code_residual()`,
  `hevc_cabac_code_split_transform_flag()`) already exists (built for the
  GPU path's 16x16/8x8 case), which is a genuine head start; the missing
  piece is the CPU-side transform/quant kernels themselves for 8x8 and
  above, and the encode-side depth decision.
- **(3) needs a real per-CTU `split_cu_flag` decision** (not the
  constant-1-then-constant-0 the two existing CPU/GPU paths use today),
  which changes `part_mode` (2Nx2N vs NxN), CtDepth-dependent MPM/
  split-context derivation (9.3.4.2.2 — the exact area a previous ctxInc
  bug already lived in, per `encode_core_gpu()`'s comment), and interacts
  with (2) (TU size follows CU size). This is the "deepest structural
  change" the task brief called it.

Both are real, tractable follow-on work with a concrete starting point
now on record (this file, plus the `cavlc-residual-coding` line
references above) — but attempting them in the same pass as an unresolved
"is the mode search itself sound" question would risk exactly the kind
of compounded, hard-to-attribute change this project's `CLAUDE.md`/
`performance-measurement.md` repeatedly warn about.

## Files changed

First pass (commit `f484edb`):
- `approach1-compute-encoder/src/hevc_intra.c` — the port itself.
- `approach1-compute-encoder/src/hevc_intra.h` — `HEVC_MODE_COUNT`, doc
  updates.
- `approach1-compute-encoder/src/encoder_h265.c` — one clamp updated to
  use the new constant; no structural change.
- `tools/hevc_host_repro.c` — patterns 4-6 for angular-mode coverage.
- `tools/hevc_host_drift.sh` — 15 new cases using patterns 4-6, comment
  updates.
- `docs/notes/a6-cavlc-residual-port.md` — this file.

RD-biased mode search follow-up (this commit, 2026-09-21):
- `approach1-compute-encoder/src/hevc_intra.c` — `hevc_choose_luma_mode()`
  now takes `qp` and the PU's already-derived `mpm[3]`, and minimizes
  `sad + lambda * rate_bits(mode)` instead of raw SAD; added
  `hevc_mode_rate_bits()` and `hevc_luma_mode_lambda()`.
- `approach1-compute-encoder/src/hevc_intra.h` — signature/doc update for
  the above.
- `approach1-compute-encoder/src/encoder_h265.c` — `encode_cu()`'s MPM
  derivation moved from Step 2 (CABAC signaling) up into Step 1 (mode
  decision), since the search now needs it; logic unchanged, only
  relocated (see the comment at its new call site for why that's
  neighbor-data-safe). No other structural change.
- `docs/notes/a6-cavlc-residual-port.md` — this file, the "Follow-up"
  section.

No changes to `docs/DEVLOG.md` or `docs/backlog.md` (parallel-agent
constraint) or to any other worktree. Scratch measurement scripts used to
produce the follow-up's table (`gen_source.c`, `measure_rd.sh`,
`build_old_new.sh`, `extract_old.sh`) were dev-machine-only and removed
before committing — they are reproducible from this file's description if
needed again (`git show 47c74c9:<path>` for the old-baseline sources,
`tools/hevc_host_repro.c` unmodified for `hostrepro`, `ffmpeg -lavfi psnr`
between two forced-framing raw YUV decodes per `CLAUDE.md`'s PSNR rule).
