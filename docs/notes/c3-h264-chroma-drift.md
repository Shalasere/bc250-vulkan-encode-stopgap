# C3: H.264 chroma drift, max delta 241-252 - methodology, and why the number cannot be what the backlog item assumes

Status: **off-board investigation complete. No board available this session.**
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
