# C3: H.264 chroma drift, max delta 241-252 - methodology, and why the number cannot be what the backlog item assumes

Status (2026-09-21, latest session): **the prior "leading candidate"
(`residual_predict.comp` predicting from source, not reconstructed,
neighbours) is REFUTED** - it is dead code for every frame `lab drift`
tests (see "Session 2026-09-21 (continued)" below). Two follow-up
deblocking-related sub-hypotheses for the LUMA gap were tested
computationally and also refuted as *insufficient* at QP=27 (though both
are real, smaller bugs worth fixing separately). The chroma gap's
direction is still explained by the already-known missing-chroma-deblocking
defect (§14.3); the luma gap's dominant cause is still open. No shader/GPU
code was changed - see that section for the honest state and the ranked
list of what to check next on a board.

Status (prior session): **off-board investigation complete. No board available that session.**
The methodology is fixed and validated (with synthetic data, since the GPU
path cannot run off-board at all). The 241-252 figure itself is **not
re-measured here** - see "What a board session needs to run next" below for
the exact commands.

## The question C3 actually asks, restated precisely

> `lab drift` disables the decoder's loop filter, but this encoder does
> luma-only deblocking, so the comparison is mismatched by construction for
> H.264 and the current number cannot be interpreted.

This reads as: "fix the decode configuration, then trust the number." That
turns out to be necessary but nowhere near sufficient. Two things independent
of *how* the decode is configured make the existing 241-252 figure
uninterpretable as a deblocking artifact at all:

1. **`lab drift` always encodes with `-g 1`** (`tools/bc250_lab.sh`'s
   `drift()`, the `ffmpeg ... -frames:v "$frames" -g 1 ...` line). Every
   frame is an independent IDR. There is no P-frame reference chain for an
   error to compound across. Whatever produced 241-252 happened **within a
   single intra frame**, not via the C9 mechanism (encoder's unfiltered
   reference silently diverging from the decoder's filtered one, frame after
   frame, until the next IDR resets it). C3 cannot be C9's shape, full stop -
   no board run is needed to establish this, it follows from the harness's
   own command line.

2. **The deblocking filter is bounded by spec, and the bound is nowhere near
   241-252.** `deblock_filter.comp`'s `filter_edge()` clamps every pixel
   change to `[-tc, tc]` where `tc = tc0 + (ap?1:0) + (aq?1:0)` (ITU-T
   8.7.2.3/8.7.2.4). Reading the shader's own `TC0_TABLE` (ITU-T Table 8-17),
   the worst case across the *entire* QP range (0-51) is `tc0=25` at QP 51,
   giving `tc <= 27` for luma. The analogous chroma formula (`tc0+1`, no
   p1/q1 update - 8.7.2.4) tops out at `26`, and this shader doesn't even
   implement chroma filtering, so the real chroma ceiling from "filtered vs
   never-filtered" is *at most* that same 26, in the direction the decoder
   filters more than the encoder. A measured max delta of 241-252 is **~9x**
   that ceiling. No decode configuration - `-skip_loop_filter all`, real
   decode, anything in between - can turn a deblocking mismatch into a
   241-252 pixel-value jump. The premise "this is the luma-only-deblocking
   gap, just measured wrong" is itself false; only the *methodology* claim
   ("the current comparison is mismatched by construction") is true.

Both of these are established by reading `tools/bc250_lab.sh`,
`approach1-compute-encoder/shaders/deblock_filter.comp` and
`approach1-compute-encoder/src/gpu_compute.c` - no board access needed - and
neither depends on the other.

## What `-skip_loop_filter all` actually does to each plane, precisely

`gpu_compute.c` (Stage 5 comment, ~line 2415) confirms the luma-rebind fix is
live: `deblock_desc_set` binding 0 is bound to `ctx->recon_image.y_view`, so
the encoder's own luma reference **is** deblocked. `deblock_filter.comp`
declares exactly one image binding (`frameImage`, `r8` - single channel) and
no chroma binding exists anywhere in the shader or its descriptor set
wiring. So, given the H.264 PPS's default `disable_deblocking_filter_idc=0`
(deblocking signalled ON - confirmed in `encoder_h264.c`'s
`h264_encoder_encode_frame()`/`_encode_raw()`, `deblock_idc` is `0` unless
`BC250_FAST_MODE=1`):

| plane  | encoder's own reference | `-skip_loop_filter all` decode | real (PPS-honouring) decode |
|---|---|---|---|
| luma   | deblocked                | NOT filtered                    | filtered |
| chroma | **never** deblocked (no shader path touches it) | NOT filtered | filtered |

So under the harness's *current default* (`-skip_loop_filter all`, used
unconditionally before this change):

- **Luma** is the mismatched comparison: encoder filtered vs. decoder
  forced-unfiltered. Expected gap, bounded at 27.
- **Chroma** is *not* mismatched by the flag at all: neither side is
  filtering chroma, so under the current default configuration chroma
  should be **exact**, not drifting. A large chroma delta under
  `-skip_loop_filter all` is therefore *not explained by deblocking in either
  direction* - it has to be something else already present in the
  comparison before the flag question even comes up.

This is the opposite of what C3's own phrasing implies (that fixing the flag
would explain the chroma number). Fixing the flag is still worth doing -
under a *real* decode, chroma legitimately gets the known ~26-bound gap and
that's the only configuration where "luma matches, chroma differs by a known
amount" is a coherent thing to check - but it will not make a 241-252 max
delta go away, because that magnitude was never something deblocking could
produce under either configuration.

## The precedent already in this codebase for exactly this failure mode

`encoder_h264.c` (~line 2431, in the `BC250_DEBUG_MB` diagnostic comment)
documents a case that looks identical in shape to what C3 might be re-hitting:

> a per-plane PSNR comparison that motivated adding this (GPU recon_image,
> via `BC250_DUMP_RECON_FRAMES`, vs. real decoded output) initially looked
> chroma-specific (Y~40dB vs U~20dB/V~18.5dB) and pointed straight at this
> code path. That comparison turned out to be comparing against the WRONG
> reference: repeating it against the true source frames
> (`BC250_DUMP_INPUT_FRAMES`) instead of the GPU recon dump showed decoded
> chroma is actually fine (U/V in the high 30s dB, matching
> `quality_test.sh`'s own healthy board-validated numbers) - so this data
> dump did not end up implicating the chroma DC/quant path itself.

That episode (DEVLOG §8/§9, `fix/gradient-boundary-mc-v2`) is a
**recon-vs-decoded** comparison - exactly what `lab drift` runs - showing a
dramatic, chroma-specific gap that turned out to say nothing about the
*decoded* picture's real quality (which was fine against source). The actual
root cause that episode landed on (§9: the P-slice MV predictor used the
wrong ITU-T rule for skip-legality, 8.4.1.3 median instead of 8.4.1.1's
zero-forcing derivation) is long since fixed and validated (avg PSNR vs
source 28.85dB -> 51.01dB). But note that fix was validated **against
source**, never against `lab drift`'s recon-vs-decoded comparison - so it is
possible the *recon-vs-decoded* gap that motivated adding the `BC250_DEBUG_MB`
diagnostic was never independently closed out on its own terms, only
set aside once the decoded-vs-source question got a good answer. C3's
241-252 could be: a residual of that same "recon doesn't match decode, even
though decode matches source" shape, freshly surfaced by a stricter test
(`lab drift` didn't exist as a board-run item until C1-C4).

**One good-news implication of this precedent, worth stating plainly:** a
large `lab drift` recon-vs-decoded gap does not necessarily mean the
*streamed* picture is bad. It has been directly observed once before that
this specific comparison can look catastrophic (Y~40/U~20/V~18.5dB) while
the actually-decoded, actually-streamed picture is fine (high 30s dB) against
ground truth. The right next measurement is not just "how big is the
drift number" but "does `lab qsweep`'s real decoded-vs-source PSNR show
anything wrong on the same content" - if qsweep is clean and only `drift`
is bad, this is very likely the same shape as the fixed §8/§9 finding
(diagnostic dump revealing something visually inconsequential), not a live
streaming defect.

## `h264_encoder_encode_raw()` cannot serve as an off-board oracle here

The task brief for this investigation suggested checking whether
`h264_encoder_encode_raw()` (the CPU-only entry point,
`approach1-compute-encoder/src/encoder_h264.c:3042`) could play the same role
`hevc_encoder_encode_raw()` plays for `tools/hevc_host_drift.sh`. It cannot,
and this is by design, not an oversight:

- `hevc_encoder_encode_raw()` runs the *real* CPU HEVC encode path: real
  intra prediction, real forward transform/quantization, real inverse
  transform/reconstruction, real residual. It is a genuine alternate
  implementation of the codec that happens not to need a GPU.
- `h264_encoder_encode_raw()` is fundamentally different: reading its body
  (`encoder_h264.c:3042-3318`), every IDR macroblock writes
  `cavlc_write_mb_i16x16_header(...)` followed by a **hardcoded all-zero DC
  block** (`static const int16_t zero_dc[16] = { 0 };` - the function's own
  comment: *"All coefficients here are zero (this path codes no residual by
  design - see the function's own note)"*), and every P-frame macroblock is
  either `P_Skip` or a bare MV with **no residual write at all**. This path
  exists to validate bitstream *syntax* (CAVLC header ordering, slice header
  fields, `deblock_idc` presence rules) - `docs/performance-measurement.md`
  and `CLAUDE.md` both already say as much ("h264_encoder_encode_raw() is
  header-only by design and codes no residual"). It never runs
  `reconstruct.comp`'s dequant+IDCT+prediction math, never runs
  `residual_predict.comp`'s real intra/inter prediction, and never touches
  `deblock_filter.comp`. There is no reconstruction here for a drift check
  to compare against - the "reconstruction" is trivially the same
  DC-predicted value the decoder will also compute, by construction, so a
  byte-exactness check against it is guaranteed to pass regardless of any
  real defect in the GPU path.

**Conclusion: there is no way to build an off-board, `hevc_host_drift.sh`-
equivalent oracle for the real H.264 GPU reconstruction path.** The
per-macroblock prediction, forward/inverse transform, quantization and
deblocking that actually matter for this bug all live in Vulkan compute
shaders (`residual_predict.comp`, `dct_transform.comp`, `quantize.comp`,
`reconstruct.comp`, `deblock_filter.comp`), dispatched through
`gpu_compute.c`, which needs a real GPU context. This is exactly what the
backlog item itself already says ("Needs the GPU path, which cannot run
off-board") - this session confirms *why*, concretely, rather than just
restating it, so nobody spends time trying to write that host harness.

## A candidate mechanism worth checking first on the board (not a diagnosis)

Given the above, a max-delta statistic (as opposed to a mean/PSNR figure) of
241-252 is most consistent with a **rare, localized** defect - a handful of
pixels badly wrong, not a systematic per-pixel bias across the frame (which
would show up in the mean too, and 241-252 in the *mean* would mean the
picture is unrecognizable, which nothing in this project's history suggests
for a synthetic `testsrc` all-intra clip). Two documented facts point at the
picture edges/corners specifically as the place to look first:

- `residual_predict.comp`'s own top-of-file comment ("DELIBERATE
  SIMPLIFICATION (neighbor pixel source)") states that H.264 intra
  prediction (both luma and chroma) reads neighbor pixels from the
  **source** image, not from reconstructed neighbors, because the shader
  dispatches all macroblocks of a frame in parallel with no cross-MB
  ordering. This is a real, acknowledged divergence from what a real decoder
  does, and unlike the deblocking gap it applies inside a single intra
  frame with no compounding needed - i.e. it is live in exactly the
  configuration `lab drift` tests (`-g 1`, every frame an IDR).
- Per ITU-T 8.3.4.1/8.3.3, a macroblock at the picture's top row or left
  column has **no** available neighbor for some prediction modes, and the
  spec's fallback (DC mode defaults to 128) only applies if the
  implementation correctly detects unavailability. §8/§9 already found one
  real historical bug in exactly this neighborhood (the old DC-only chroma
  mode "blending a wildly different neighbor row" into prediction, fixed by
  giving chroma the same real SAD-based mode decision luma already had) -
  the general class of bug ("neighbor availability / wrong-neighbor read at
  a picture or slice edge producing a wrong prediction value") has already
  cost this project one real, large, chroma-specific defect once.

This is offered as the **first thing to check on a board**, not as a
diagnosis - it is not verified here, because verifying it requires running
the real GPU path.

## What was built this session: an H.264-aware, interpretable `lab drift`

`tools/bc250_lab.sh drift()` (used by `tools/lab drift`) now:

1. Takes a new **`--real-decode`** flag that drops `-skip_loop_filter all`,
   so the decoder filters exactly what the PPS signals. This is a no-op for
   HEVC (already true since C9's fix); for H.264 it is the only
   configuration in which "luma should match, chroma is expected to differ
   by the known missing-chroma-deblocking gap" is a coherent statement.
2. For `--codec=h264` runs that also pass `--qp=<N>` (CQP mode, so QP is
   known and fixed rather than rate-control-chosen per frame), each frame's
   report line is annotated per plane against the theoretical deblocking
   bound at that QP (`TC0_MAX_Z[qp]+2` luma, `+1` chroma - copied verbatim
   from `deblock_filter.comp`'s own `TC0_TABLE`, see the code comment for
   the derivation):
   - a delta **within** the bound is tagged `expected` (attributable to the
     known gap, whichever decode mode produced it - the tag text says which)
   - a delta **exceeding** the bound is tagged `EXCEEDS ... - a different
     bug` / points at this file
   - under the default (`-skip_loop_filter all`) mode, a nonzero *chroma*
     delta is tagged `unexplained ... not a deblocking gap` outright, since
     neither side is filtering chroma in that configuration (see the table
     above) - there is no bound to check against because deblocking cannot
     be involved at all.
3. This is **purely additive annotation** - the existing byte-exact
   pass/fail counting (`bad`/exit code) is unchanged, so nothing that
   depended on `drift`'s exit status (including `gate()`, which only ever
   calls `drift --codec=hevc`) is affected.

Validated **off-board** with synthetic NV12 fixtures (fabricated recon/decode
buffers feeding the extracted Python comparison logic directly - the
shell/ffmpeg/GPU parts cannot be exercised without a board):

| case | codec | qp | mode | delta | tag |
|---|---|---|---|---|---|
| chroma delta 245 | h264 | 27 | `-skip_loop_filter all` | 245 | `unexplained under -skip_loop_filter all: neither side filters chroma - not a deblocking gap` |
| chroma delta 245 | h264 | 27 | `--real-decode` | 245 | `EXCEEDS deblocking-explicable bound 3` |
| chroma delta 10 | h264 | 45 | `--real-decode` | 10 | `expected: known luma-only-deblocking gap (C3), bound 14` |
| chroma delta 10 | h264 | 27 | `--real-decode` | 10 | `EXCEEDS deblocking-explicable bound 3` |
| luma delta 127 | h264 | 27 | either mode | 127 | `EXCEEDS ... bound 4` |
| (no `--qp`) | h264 | - | `--real-decode` | 245 | no tag + explicit note to pass `--qp` |
| any delta | hevc | 27 | either mode | - | no tag (HEVC already has its own no-op-flag reasoning) |

All seven cases produced exactly the tag the design intends; `tools/
hevc_host_drift.sh` still prints `HOST DRIFT PASS (38 cases byte-exact)`
after this change (re-run this session, unaffected - it does not import
`bc250_lab.sh`).

No shader or `gpu_compute.c` change was made. The mechanism behind 241-252 is
not established well enough to change GPU code for it - see below.

## What a board session needs to run next

```
tools/lab build work
tools/lab drift <key> --codec=h264 --qp=27 --real-decode --frames=5
tools/lab drift <key> --codec=h264 --qp=27 --frames=5            # old default mode, for comparison
tools/lab qsweep <key> --codec=h264                                # is the real decoded-vs-source picture actually bad?
```

Read the annotated per-frame output: if the `--real-decode` chroma tag comes
back `EXCEEDS ... bound N`, that confirms C3's number is a real, separate
defect (not deblocking) and the next step is instrumenting
`residual_predict.comp`'s chroma DC/edge-availability path on the specific
macroblock the max delta lands on (the drift script's dump directory has the
full recon frame; diff it against the decoded frame to locate the
coordinate). If `qsweep`'s decoded-vs-source PSNR is clean while `drift`
alone is bad, treat it with the same caution §8/§9 already recorded: a
recon-vs-decoded gap does not by itself mean the streamed picture is wrong,
and the priority drops from "possible correctness bug" to "diagnostic
worth understanding, not urgent."

If the fix that mechanism eventually turns up is a genuine missing GPU
feature (e.g. chroma deblocking needs to actually be implemented, mirroring
`gpu_compute.c`'s existing luma Stage 5 dispatch plus a real `rg8`
`recon_image.uv_view` binding and ITU-T 8.7.2.4 chroma filtering in
`deblock_filter.comp`), that is real shader work needing board validation to
land safely (per this repo's own `deblock_filter.comp` header: even the
*existing* luma-only filter had to work around a genuine cross-workgroup
race by splitting into two barriered dispatches - a naive chroma add-on
would need the same care) and should not be attempted blind, matching this
task's own scope boundary. But given the magnitude argument above, it is
very unlikely that deblocking is the fix this bug actually needs.

---

## Session 2026-09-21 (continued): the leading candidate is REFUTED by reading
## the actual dispatch code, not just the shader comment that named it

The board review box above (and the backlog C3 entry it's quoted from)
points at `residual_predict.comp` predicting intra chroma and luma from
**source** neighbours instead of **reconstructed** ones as "the leading
candidate," citing that shader's own "DELIBERATE SIMPLIFICATION (neighbor
pixel source)" comment. This session's job was to confirm or refute that by
reading `gpu_compute.c`'s actual Vulkan dispatch/binding code, not by
pattern-matching the shader's name or its comment. Doing that refutes it,
cleanly, and for a boring reason: **that shader is never dispatched for the
frames `lab drift` tests.**

### The refutation

`gpu_compute_dispatch_encode()` (`approach1-compute-encoder/src/gpu_compute.c`,
~line 2477 onward) branches hard on `is_intra`:

```c
if (!is_intra) {
    /* Stage 2.5: real intra/inter prediction ... */
    if (ctx->predict_pipeline && ...) { ... vkCmdDispatch ... }   // residual_predict.comp
    /* Stage 3: DCT, Stage 4: Quantize, Stage 4.5: Reconstruct ... */
} else {
    /* I-slice path - diagonal-wavefront intra reconstruction */
    if (ctx->intra_wavefront_pipeline && ...) {
        for (uint32_t d = 0; d < num_diagonals; d++) { ... vkCmdDispatch ... }  // intra_wavefront.comp
    }
}
```

`residual_predict.comp` (`ctx->predict_pipeline`) is dispatched **only**
inside the `!is_intra` branch. For an I-slice, the `else` branch runs
`intra_wavefront.comp` instead, and does not touch `predict_pipeline`,
`transform_pipeline`, `quantize_pipeline`, or `reconstruct_pipeline` at all -
the comment right above it says so explicitly ("REPLACES, for I-slices only,
the whole-frame-parallel predict->dct->quantize->reconstruct chain above").

`tools/bc250_lab.sh`'s `drift()` always encodes with `-g 1` (every frame an
IDR, `is_intra=1` for every frame, no exceptions - this was already
established in this file's first session, for a different reason). So in
**every** configuration `lab drift` has ever run, `residual_predict.comp`'s
"DELIBERATE SIMPLIFICATION" is dead code for the frames being compared. Its
header comment describes a real historical defect, but not one that is live
in this scenario - the comment is talking about *itself*, not about what
actually executes for an I-slice today.

### `intra_wavefront.comp` already fixed exactly this defect, on 2026-09-08

Reading `intra_wavefront.comp` (its own top-of-file comment, and its actual
neighbour-sample reads) confirms it is not the same "simplification":

- Binding 2/3 (`reconY`/`reconUV`) are `ctx->recon_image` - explicitly
  **read-write**, not the read-only `currentImage`/`currentUV` bindings
  `residual_predict.comp` uses for the same purpose.
- Top/left/corner neighbour samples for both luma (`top_row`/`left_col`/
  `corner`) and chroma (`top_uv`/`left_uv`) are loaded from `reconY`/
  `reconUV`, explicitly, with a comment at each site: *"Top/left/corner
  neighbor samples come from reconY - the TRULY reconstructed pixels of
  earlier-diagonal macroblocks - NOT from currentImage. This is the entire
  point of the fix."*
- `gpu_compute_dispatch_encode()` dispatches this shader once per
  anti-diagonal `d = mbx+mby`, with a real `insert_compute_barrier()`
  (`vkCmdPipelineBarrier`, not just an intra-workgroup `barrier()`) between
  diagonals - so by the time diagonal `d` runs, every macroblock it could
  read as a neighbour (diagonal `d-1` and earlier) has already had its
  reconstructed pixels written and made visible. This is a real,
  spec-correct wavefront dependency, the same technique HEVC/VVC's own WPP
  uses.
- `docs/DEVLOG.md`'s very first correctness table (section 1, bug #5 of 15)
  already documents this as **fixed**: *"Intra prediction read source
  pixels, not reconstructed | ... | Diagonal-wavefront GPU dispatch
  (`intra_wavefront.comp`) - one anti-diagonal at a time with barriers
  between, so intra prediction can only ever see truly-reconstructed
  same-frame neighbors."* `intra_wavefront.comp`'s own header cites an
  on-board validation dated **2026-09-08** - before C3 was even opened,
  and long before this session's board review re-surfaced the same old
  comment as a "leading candidate, not yet checked."

So the hypothesis is not "inconclusive without a board run" - it's refuted
by the dispatch graph itself, which is knowable, and was knowable, off-board.
Two prior investigations (the original off-board pass and the board review)
both cited `residual_predict.comp`'s comment without checking whether that
code path is even reachable for the frames under test. It isn't. Whoever
wrote that comment in `intra_wavefront.comp`'s own header ("residual_predict.
comp - still used, unmodified, for P-slices only") already said as much;
this session just followed that pointer all the way to `gpu_compute.c`
instead of stopping at the shader comment that named the historical defect.

**This does not mean intra prediction is bug-free** - only that
*this specific, named* defect (source vs. reconstructed neighbours) is not
what's live in `lab drift`'s all-intra scenario. Per this project's own rule
("a working fix is not confirmation of the diagnosis that produced it," and
its mirror here - a *plausible* diagnosis is not confirmation either), the
right move is to stop chasing this named bug and look at what actually runs.

### Ruled out by direct comparison: `intra_wavefront.comp`'s inlined math is not a copy-paste drift

`intra_wavefront.comp` reimplements the whole I-slice pipeline (predict +
forward DCT + AC quantize + DC Hadamard + dequant + inverse DCT +
reconstruct) inline, duplicating logic that also lives in
`dct_transform.comp`, `quantize.comp`, and `reconstruct.comp` for the P-path.
Given this project's own history of exactly this failure mode (the V[][]
column-swap bug fixed in `reconstruct.comp`, the DC transpose bug fixed in
`encoder_h264.c`), a transcription drift between the duplicated copy and the
originals is a real, cheap thing to check. Diffed side by side, table for
table and formula for formula:

- `MF[6][3]` (AC quantizer), `get_pos_type`/`get_pos_type_q` - identical to
  `quantize.comp`.
- `V[6][3]` (AC dequantizer, **already carrying the fixed column order**,
  not the old swapped one) - identical to `reconstruct.comp`.
- `forward_4x4()` - identical to `dct_transform.comp`.
- `inverse_4x4()` (with the `d[0]+=32` rounding term) - identical to
  `reconstruct.comp`.
- `DC_LEVEL_SCALE0[6]`, `luma_dc_hadamard_fwd/inv()`, `chroma_dc_hadamard()`,
  `quantize_dc_luma_scalar/chroma_scalar()` (**including the deliberate
  K=5-not-6 asymmetry**), `dequant_dc_luma/chroma()`, `chroma_qp()` -
  all identical to `reconstruct.comp`'s copies, which are themselves
  1:1 ports of `encoder_h264.c`'s already-fixed (transpose bug, K=5
  asymmetry) CPU-side versions.

No divergence found. This rules out "the wavefront shader's inlined
duplicate quietly drifted from the originals" as an explanation for
anything measured here.

### New sub-hypothesis for the LUMA gap, tested and REFUTED at QP=27: the missing strong bS==4 filter

With the leading candidate refuted, the next candidate examined was
`deblock_filter.comp`'s own documented "DELIBERATE SIMPLIFICATION: no
separate 3-tap strong intra filter for bS==4" - it deliberately reuses the
tc0-clamped bS==3 "normal" filter equations for bS==4 (macroblock-boundary
edges of an intra frame) instead of ITU-T 8.7.2.4's real strong filter,
which can write up to 3 samples deep per side with no tc-style clamp at all.
This looked promising: it is luma-only-relevant (matches the board review's
"luma is not exempt" finding), genuinely undercounted by
`tools/bc250_lab.sh`'s `deblock_bound()` (which computes the bound for the
shader's *own*, weak filter, not for what a real decoder's strong filter
would do), and concentrated at macroblock boundaries specifically - which
would explain why only ~1.8% of luma pixels differ (a boundary-concentrated
defect) rather than a frame-wide bias.

Tested with `tools/deblock_strong_vs_weak_check.py` (added this session,
hand-porting both the weak filter deblock_filter.comp actually runs and the
real ITU-T 8.7.2.4 strong filter, then sweeping QP and a battery of
synthetic edge shapes for the max disagreement between them):

```
At QP=27 specifically: max |strong - weak| = 1
At QP=27, crude 2-pass (vertical+horizontal at a MB corner) compounding estimate: 2
Max |strong - weak| over qp 0..51: 35 (at QP=50)
```

**REFUTED at QP=27** (the QP the board review used): the two filters differ
by at most ~1-2 across every synthetic pattern tried, nowhere near the
board's measured max delta of 57. The reason is mechanical, not a
coincidence of my patterns: ITU-T's own alpha/beta edge-detection gate is
small at QP=27 (alpha=17, beta=6), so only near-flat regions pass the
"is this a real blocking edge, not real content" gate at all - and in that
narrow regime the strong and weak filters, both being local smoothing
operators over nearly-equal samples, agree closely almost by construction.
The two filters only diverge by tens of levels at much higher QP (~35 by
QP=50, where alpha/beta widen enough to gate in genuine high-contrast
content). **This is a real, previously-unquantified gap worth fixing on its
own merits at high QP**, but it is not big enough to be *the* explanation
for the QP=27 luma number, and should not be pursued further as the answer
to that specific board measurement.

### A second, previously-unflagged real bug found while investigating this, also not big enough on its own

`deblock_filter.comp`'s own header discusses (and rules out) a
**write-write** race between neighbouring macroblocks' workgroups for the
weak-filter's write footprint (max depth: local column 13 for an internal
edge's `q1'`, local column 14 for a boundary edge's `p1'` - "always
disjoint"). It does not discuss - and there is - a **read-write** race on
the same dispatch: a macroblock's own internal-edge filter (columns 4/8/12)
can *write* into `q1'` at local column 13 (when `aq` is true for that edge),
which is exactly the pixel the RIGHT-neighbour macroblock's own
boundary-edge filter *reads* as `p2` (at `pos-3`) to decide its own `ap` and
compute its `p1'` delta. These are two different, concurrently-scheduled
workgroups within the same `vkCmdDispatch(width_mbs, height_mbs, 1)` call
(pass 0 or pass 1), with no `vkCmdPipelineBarrier` between them - Vulkan
gives no cross-workgroup memory-visibility guarantee within one dispatch
without one. Per ITU-T's required strict raster processing order, a real
decoder always sees the LEFT-neighbour's fully-internal-edge-filtered value
here (the left neighbour is fully processed before the current macroblock,
by construction of raster order); this shader's `p2` read is a genuine race
between "already written by the neighbour's own dispatch" and "not yet" -
non-deterministic across runs, potentially across GPUs/drivers.

This is real and worth fixing (e.g. gate the boundary edge's `ap`/`p1'`
update to not depend on the neighbour's own internal-edge output, or accept
a harmless amount of imprecision there by design and document it the way
the existing "no strong bS4" simplification already is). But its magnitude
is bounded by the same `tc0` clamp as the rest of the weak filter (~2 at
QP=27), so - like the missing-strong-filter gap above - it cannot be the
dominant source of a max-57 delta either.

### Still genuinely open: what actually drives the QP=27 luma number

After this session, every mechanism checked and ruled out (either by
reading the dispatch graph, or by direct table/formula diff, or by
computation) is:

| candidate | verdict |
|---|---|
| `residual_predict.comp` source-vs-reconstructed neighbours (the prior "leading candidate") | **REFUTED** - not dispatched for I-slices at all (see above) |
| `intra_wavefront.comp`'s inlined quant/dequant/transform math vs. the P-path originals | **checked clean** - no transcription drift |
| CPU-side DC Hadamard transpose / K=5 asymmetry vs. GPU | **checked clean** - already-fixed, and GPU/CPU agree |
| missing strong bS==4 filter (deblocking) | **real gap, but too small at QP=27** (max ~1-2 vs. needed ~57) |
| cross-workgroup read-write race in `deblock_filter.comp` | **real bug, but too small at QP=27** (bounded by the same tc0 clamp) |
| missing chroma deblocking entirely (`deblock_filter.comp` has no chroma binding, §14.3) | **still the right explanation for the CHROMA gap's direction**, though the harness's own single-pass `deblock_bound()` likely undercounts the true two-pass-compounded magnitude (chroma bound 3 vs. measured max 8 at QP=27) - a harness tightening, not a new defect, is the most likely next step there |

None of the deblocking-adjacent mechanisms checked here can explain a
57-level luma delta at QP=27 - they are all bounded by the same small
`tc0`-based clamp that also bounds the filter this shader actually runs.
Given intra prediction, the transform/quantize/dequant math, and the
deblocking data-flow wiring (binding `ctx->recon_image`, not source, per
the earlier session's confirmation) are now all checked and clean, the most
likely remaining places for whoever picks this up next, in priority order:

1. **Locate where the differing luma pixels actually are**, not just how
   many. `BC250_DUMP_RECON_FRAMES=1` already writes the encoder's recon
   dump; diffing it against the decoded YUV pixel-by-pixel (the drift
   script already computes this internally but only reports aggregate
   counts) and plotting/printing the coordinates would immediately show
   whether the ~1.8% of wrong pixels cluster at macroblock boundaries
   (deblocking-shaped - in which case the two mechanisms above, even though
   individually too small, might compound with something not modelled in
   the crude 2-pass estimate above), at **slice** boundaries specifically
   (`BC250_SLICES_PER_FRAME=4` by default; `deblock_filter.comp` has no
   slice-awareness at all - it filters every macroblock boundary uniformly,
   including slice boundaries, and this has never been checked against
   what a real decoder does at a slice boundary under this encoder's exact
   slice-partition formula), or scattered through macroblock interiors
   (which would rule out deblocking entirely and point back at
   prediction/transform/entropy coding).
2. **Check the CAVLC/entropy re-derivation path independently of
   deblocking.** `encoder_h264.c` independently re-derives the DC Hadamard
   from `dc_coeff_buffer` (already checked clean here) but reads AC levels
   directly from `quant_levels_buffer` with no re-derivation - if there is
   any position/scan-order mismatch between what `intra_wavefront.comp`
   wrote (raster order into `levels[]`) and what `cavlc_write_4x4_block()`'s
   zigzag gather expects at read time for THIS specific path (as opposed to
   the P-path, which shares the same buffer format but has been running
   longer), a real decoder would reconstruct different values than the GPU
   internally assumed - a defect with nothing to do with deblocking that
   would show up in exactly this kind of recon-vs-decode comparison. Not
   found in the diff done this session, but not exhaustively fuzzed either.
3. Re-run `lab drift --codec=h264 --qp=27 --real-decode` at a **second** QP
   a good distance from 27 (e.g. 12 and 45) - if the delta scales with QP
   the way a deblocking-adjacent mechanism would (bigger tc0/alpha/beta at
   higher QP), that's a strong hint it's still deblocking-family, just via
   a mechanism not modelled above; if it stays roughly constant in absolute
   terms across QP, that points away from deblocking entirely.

`tools/deblock_strong_vs_weak_check.py` (added this session) is the
supporting artifact for the two REFUTED-at-QP27 findings above; re-run it
directly (`python3 tools/deblock_strong_vs_weak_check.py`, no board needed)
if either mechanism needs re-checking at a different QP.

**No shader or `gpu_compute.c` change was made this session either** - same
reasoning as the prior session: the mechanism behind the luma number is not
established well enough to change GPU code for it, and shipping a fix for a
misdiagnosed cause would be worse than shipping no fix, per this project's
own standing rule against acting on a plausible-but-untested mechanism.
