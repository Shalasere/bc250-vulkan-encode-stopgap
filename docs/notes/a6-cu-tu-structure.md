# A6 piece (2) — all-TU-size transforms

Status: **piece (2), all-TU-size transforms, DONE for the size this CU
structure can actually reach (8x8), byte-exact-verified.** The
transform/quantization *kernels* are implemented generically for
log2_size in {2,3,4,5} (4x8x16x32), but only the 8x8 one is wired into
any live encoder call site this session — see "Why only 8x8 is reachable
this session" below for why that is a structural fact about this CU
size, not an arbitrary scope cut. Piece (3) (undivided 16x16 CUs /
variable CU splitting) was **not attempted**, per the task's own
ordering rule and this project's standing practice of stopping cleanly
rather than stacking an unverified structural change on top of another.

## What was asked, and what this delivers

`docs/backlog.md`'s A6 piece (2): "all-TU-size transforms (currently
fixed 4x4 DST-VII/DCT-II only; HEVC supports 4/8/16/32)". The task
brief's own scoping (echoing `docs/notes/a6-cavlc-residual-port.md`)
said the missing piece on top of the already-generic CABAC/quant
infrastructure was "the CPU-side transform/quant kernels themselves for
8x8 and above, and the encode-side depth decision."

This delivers:

- Generic forward/inverse DCT-II transform, quantization and
  dequantization for log2_size in {2,3,4,5} (`hevc_transform_quant()`,
  `hevc_dequant_itransform()` in `hevc_intra.c`/`.h` — log2_size==2
  dispatches straight to the existing, unmodified 4x4 functions, so this
  is a strict superset of what already shipped, not a parallel
  implementation).
- Generic 8.4.4.2 intra prediction for a square block of any of those
  sizes ≥ 8, INCLUDING real 8.4.4.2.3 reference-sample filtering
  (`hevc_predict_nxn()`, `hevc_choose_luma_mode_nxn()`) — genuinely new
  machinery relative to the existing 4x4 path, which never filters at
  all (Table 8-3 has no row for nTbS==4).
- `encoder_h265.c`'s `encode_cu()` now emits **PART_2Nx2N with one
  undivided 8x8 luma TU** per CU (was: PART_NxN, four 4x4 PU/TUs) —
  real, live use of the new size-8 kernels in the shipped bitstream, not
  just a standalone function nobody calls.
- The encode-side transform-depth decision the task asked for, applied
  at the only granularity this CU size makes available: "always use the
  largest transform the CU allows, never split" (correctness-only first
  cut, RD-aware splitting explicitly out of scope — see "Future work").

`tools/hevc_host_drift.sh`: **`HOST DRIFT PASS`, all 53 cases**, including
every inter/P-frame and VBR case — see "Verification" below.

## Why only 8x8 is reachable this session — read this before assuming (2) is half-done

This is the single most important design finding of this pass, and it
is a structural fact about HEVC's syntax, not a scope decision:

**With the CU size fixed at 8x8 (`HEVC_CU_SIZE`, unchanged — piece (3),
variable CU splitting, is what would ever make it bigger), there is no
bitstream-legal way to reach a transform larger than 4x4 while keeping
the OLD `PART_NxN` structure.** Rec. ITU-T H.265 7.3.8.8's
`transform_tree()` forces `split_transform_flag` to 1 (split) whenever
`IntraSplitFlag == 1 && trafoDepth == 0`, and `IntraSplitFlag` is 1
exactly when `PartMode == PART_NxN`. With `MinCbLog2SizeY ==
log2CbSize == 3` (an 8x8 CU is the SPS's minimum CU size in this
tree — `write_sps()`'s `log2_min_luma_coding_block_size_minus3 = 0`),
`PART_NxN` is legal here, but it forces the transform tree to split down
to 4x4-PU-aligned leaves regardless of anything the encoder decides.
There is no `split_transform_flag` value or depth parameter that
escapes this — it's an *inferred*, not *signaled*, split at this exact
depth/PartMode combination.

The only way to make an 8x8 *transform* reachable at this CU size is
`PART_2Nx2N` (`IntraSplitFlag == 0`), which removes that forced split.
That is what this change does: `encode_cu()` now emits
`hevc_cabac_code_part_mode_intra(cab, 1 /* PART_2Nx2N */)`
unconditionally, replacing the old unconditional `PART_NxN`. This
necessarily also changes the **prediction** structure — one 8x8 luma PU
(one intra mode covering the whole CU) instead of four independent 4x4
PUs — not only the transform. That is a real, structural consequence of
what "reach an 8x8 transform" requires here, not scope creep: getting a
non-4x4 transform live in the bitstream at this CU size is inseparable
from changing `part_mode`.

This means **16x16 and 32x32 transforms cannot be reached by any change
confined to "transform/quant kernels" at this CU size, full stop** — a
TU can never exceed its containing CU, and the CU is 8x8 until piece (3)
makes it bigger. The kernels for those sizes are implemented and
described as "generic, not yet reachable" throughout this file precisely
so that claim isn't overstated. This is also why the task brief's own
phrasing ("given a CU size, decide whether to code one larger transform
or split into smaller ones") only cashes out as one real binary choice
this session (`PART_2Nx2N`/one 8x8 TU vs the old `PART_NxN`/four 4x4
TUs) rather than a genuine multi-way depth search — a real multi-depth
transform tree needs a CU big enough to have more than one non-trivial
depth to choose between, which piece (3) is what provides.

**`split_transform_flag` itself is still not explicitly coded** for this
CU size either way (same as before this change, and same as the GPU
path's own existing 2Nx2N/16x16-CU case): `max_transform_hierarchy_depth
_intra` stays 0 in the SPS (`write_sps()`'s own comment on that
parameter), so `MaxTrafoDepth = 0 + IntraSplitFlag`. For `PART_2Nx2N`
that's 0, and `transform_tree()`'s own condition for actually *signaling*
the flag (`trafoDepth < MaxTrafoDepth`) is never true at `trafoDepth==0`
when `MaxTrafoDepth==0` — so the "don't split" decision is inferred by
the SPS's own transform-hierarchy-depth field, not by a coded bit. This
mirrors the GPU path's existing 16x16-CU precedent exactly (also
`MaxTrafoDepth==0`, also no coded `split_transform_flag`). Making the
flag itself an explicit, coded, per-CU choice (which the CABAC-side
infrastructure, `hevc_cabac_code_split_transform_flag()`, already
supports and did not need to change) would need
`max_transform_hierarchy_depth_intra >= 1` and a real split-vs-no-split
decision function — deliberately not done this session (see "Future
work"): it only becomes a meaningful choice once there's a real
RD reason to prefer 4x4 over 8x8 within the SAME CU size, which the
next bullet explains this pass does not attempt to find.

## What already existed vs what's new — verified by reading, not assumed

Per the task's instruction to confirm this by reading the actual bodies:

- **`hevc_cabac_code_residual()`** (the non-`_4x4` one) was already fully
  generic over `log2_size` — last-sig-coeff prefix/suffix, `sig_ctx_large()`,
  the coefficient-group scan, `ctxSet` derivation, everything. It was
  already exercised at `log2_size==3` for the GPU path's *chroma* (an 8x8
  Cb/Cr block for its 16x16 CU) and at `log2_size==4` for the GPU path's
  *luma*, so this change's `log2_size==3, is_luma=1` call is a new
  parameter combination on an already-live, already-generic function —
  not new entropy-coding code.
- **`hevc_cabac_code_split_transform_flag()`** already existed, generic
  over `log2_size` via its context-bank indexing
  (`HEVC_CTX_TRANS_SUBDIV + (5 - log2_size)`), but had **zero callers**
  anywhere in the tree before or after this change (see the point above
  for why — `MaxTrafoDepth==0` on every live path means it's never
  actually signaled). It remains unchanged and still uncalled.
- **Genuinely new**: the CPU-side 8x8/16x16/32x32 forward/inverse
  transform and quantization kernels (`hevc_intra.c`'s new section), and
  8x8+ intra prediction WITH 8.4.4.2.3 reference-sample filtering
  (`hevc_predict_nxn()`) — the 4x4 path has no filtering at all
  (unconditionally `filterFlag=0`, since Table 8-3 has no nTbS==4 row),
  so this is not a reparameterization of anything that already existed
  in this tree's CPU path.

## Where the new kernels' formulas came from, and how they were checked

Per the task's instruction to verify a lead against the spec rather than
trust the tree that suggested it: `hevc_intra_wavefront.comp` (the GPU
shader, `approach1-compute-encoder/shaders/`) is the ONLY other place in
this tree that already implements HEVC prediction/transform/quant above
4x4 — it does 16x16 luma and 8x8 chroma for its own undivided-16x16-CU
design, and per `docs/hevc-gpu-intra.md` its RECONSTRUCTION path (intra
prediction + dequant + inverse transform) is board-verified byte-exact
against ffmpeg's decode via `lab drift`, at the sizes it reaches. That
shader's own header is explicit that its FORWARD transform and quantizer
are *not* independently verified by anything running today (drift
compares reconstructions, which never exercises the forward-only half of
the pipeline) — the same caveat applies to this file's new forward-path
code, and is not overstated below.

Read from that shader as leads, then independently checked against Rec.
ITU-T H.265's own text before use here, not copied on trust:

- **The 32-point transform matrix and its per-size derivation**
  (`tmat(log2n,i,j)`: row decimation `i << (5-log2n)`, right-half mirror
  `M[i][N-1-j] == (-1)^i * M[i][j]`) — this is normative, not a
  per-implementation choice; spot-checked row 0 (flat 64s), row 8 (the
  familiar 4-point `{83,36,-36,-83}` pattern) and row 16 (alternating
  ±64) against the values every independent HEVC codec (HM, x265,
  ffmpeg, libde265) reproduces identically for this constant. Reused
  verbatim as `DCT32_LEFT[32][16]` in `hevc_intra.c`.
- **The forward-transform shift pair**: `shift1 = log2_size - 1,
  shift2 = log2_size + 6`. Confirmed this is ONE formula covering the
  existing 4x4 path too, not a coincidence between two hand-picked
  constants: at `log2_size==2` it gives `(1, 8)`, exactly the CPU 4x4
  path's hardcoded shift pair (`FWD_BODY`'s `>>1` then `>>8`, established
  independently before this change and re-derived from the DC round-trip
  identity in that code's own comment).
- **The inverse-transform shifts (7/64, then 12/2048)**: confirmed
  size-INDEPENDENT (same pair the 4x4 path's `INV_BODY` already uses),
  matching the shader's identical constants at both its N=16 and N=8
  sizes — this one is also directly a normative constant of Rec. ITU-T
  H.265 8.6.4.2 for 8-bit content (stage-1 shift 7, stage-2 shift
  `20-BitDepth`), not implementation-specific.
- **The quantizer's `bdShift = log2_size + 3`** — matches the existing
  4x4 path's own `HEVC_BDSHIFT = 8 + Log2(4) - 5 = 5` for `log2_size==2`,
  and is Rec. ITU-T H.265 8.6.3's normative
  `bdShift = BitDepth + Log2(nTbS) - 5` formula directly.
- **8.4.4.2.3's reference-sample filter**: the 3-tap `[1,2,1]/4`
  smoothing (with the corner-adjacent special cases at each array's near
  end, and the far end left unfiltered) and the `filterFlag` derivation
  (Planar always filters, DC and nTbS==4 never do, everything else
  compares `min(|mode-26|, |mode-10|)` against Table 8-3's
  `intraHorVerDistThres[nTbS]` — 7/1/0 for nTbS 8/16/32) were read off
  `gather()`/`refset_for()` in the shader, then independently checked
  against 8.4.4.2.3's own text before being written into
  `hevc_intra_filter_flag()`/`gather_wide_n()` in this file's own
  `uint8_t[]` convention (not copied verbatim — the shader uses shared
  GPU memory and per-thread indexing that doesn't translate directly).

Nothing was imported from `cavlc-residual-coding` for this piece — that
tree was read (again) as background per the task brief, but every
formula actually used here traces to this tree's own already-verified
GPU shader plus the public spec text, per this project's established
"read a lead, verify against the spec, reimplement in this file's own
idiom" practice (the same discipline piece (1) applied to the angular
mode tables).

## Deliberate simplifications, and why they're the right first cut

- **Plain matrix-product transform, not a partial-butterfly
  factorization.** The 4x4 path's `FWD_DCT4`/`FWD_DST4`/`INV_DCT4`/
  `INV_DST4` macros are hand-derived butterfly reassociations of the
  4-point matrix product, verified bit-identical against the literal
  product by `tests/test_hevc_encode.c`. Deriving and verifying NEW
  butterfly factorizations for 8/16/32-point DCT-II (each a genuinely
  different, size-specific derivation) on top of everything else this
  pass touches would be exactly the kind of compounded, hard-to-verify
  change this project's `CLAUDE.md` warns against. The plain matrix
  product is what `hevc_intra_wavefront.comp` already uses for its own
  8x8/16x16 case, with its own comment noting it is "bit-identical - the
  same integer sums in a different association order" as a butterfly —
  i.e. this is not a slower-and-different implementation, it's the same
  established convention this tree already uses at these sizes, just on
  the CPU. A partial-butterfly version, if ever needed for speed, is
  future work (see below) — this pass prioritized the consistency the
  task brief asked for over raw speed.
- **Literal-division quantizer, not the 4x4 path's reciprocal-multiply
  trick.** `hevc_quantize_4x4()`'s `QUANT_MAGIC[]` reciprocal was proven
  correct by exhaustion specifically for a 4x4 transform's `|raw| <=
  2^22` bound (that function's own comment derives the bound from the
  4-point matrix's largest row sum, 256). An 8/16/32-point matrix has a
  materially larger row sum (up to 90 per coefficient, times up to 32
  terms), so that bound does not carry over as-is, and re-deriving +
  re-proving a new one per size was real, unforced work this
  correctness-only pass didn't need to take on. `quantize_nxn()`/
  `dequant_nxn()` use the same literal-division shape as the existing
  `hevc_quantize_4x4_ref()` — proven-simple, not proven-fast.
- **SAD-only mode search at 8x8, no rate term.** Piece (1)'s RD-biased
  search (`sad + lambda * rate_bits(mode)`, `k=0.15`) was tuned
  specifically for a single 4x4 block's SAD magnitude — see
  `hevc_luma_mode_lambda()`'s own comment on why the textbook `k=0.85`
  was already wrong by nearly an order of magnitude for THAT domain
  change (4x4 aggregate vs a single 4x4 block). Reusing `k=0.15`
  unmeasured for an 8x8 SAD (4x the samples, different absolute
  magnitudes) would repeat exactly the mistake piece (1)'s first pass
  made — inventing an untested constant instead of measuring one. This
  is intentionally the same "first cut, ship SAD-only, then measure and
  RD-bias in a follow-up if warranted" shape piece (1) itself used.
  `hevc_choose_luma_mode_nxn()` accepts `mpm` in its signature for
  interface symmetry and future use, but does not read it yet.

## Verification

**Byte-exactness (`tools/hevc_host_drift.sh`): `HOST DRIFT PASS`, 53/53
cases**, unchanged case count from piece (1) — no new cases were needed
because this change is NOT conditionally selected: `PART_2Nx2N`/one 8x8
TU is now the *only* luma structure `encode_cu()` ever emits for an
intra CU, so every existing intra case (all-intra and inter/P-frame,
CQP and VBR, sizes from 4x4 up to 1920x1080, all 7 synthetic patterns)
already exercises it unconditionally. This is a meaningfully stronger
regression check than "found a new case that happens to select the new
path" — there is no old path left to fall back to.

```
$ tools/hevc_host_drift.sh
...
HOST DRIFT PASS (53 cases byte-exact)
```

This required one build-time fix beyond the encoder logic itself:
`write_sps()`'s `max_tb_log2` argument at the CPU path's call site
changed from `2` (`MaxTb=4`) to `3` (`MaxTb=8`) — without it, an 8x8
transform is not SPS-legal and a real decoder (ffmpeg) would be
justified in treating the bitstream as non-conformant, decoding
something, or refusing it, none of which "byte-exact" should be allowed
to paper over. Confirmed necessary by first trying the encoder change
without this and getting real per-case luma mismatches; confirmed
sufficient by then getting a clean 53/53 pass.

**`ctest`: 6/7 pass**, same result and same single pre-existing failure
as piece (1) reported (`VaApiDriverTest`, `Vulkan error -2` under WSL's
software `llvmpipe` Vulkan — a WSL/no-GPU environment limitation,
unrelated to this change; nothing here touches `gpu_compute.c` or
`va_backend.c`). `HevcEncodeBitstreamTest` passes, including the
existing transform-butterfly-equivalence checks in
`tests/test_hevc_encode.c` for the unmodified 4x4 path.

**Compiler**: a clean `-O3 -march=znver2` build (the project's shipped
flags) produced a batch of GCC `-Wstringop-overflow` warnings the first
time through, all inside the new `gather_wide_n()`/`predict_angular_
sample_n()`/`predict_blockN()` functions. These were a known class of
GCC false-positive (the parameter declarations used `uint8_t
left[HEVC_NXN_MAX_SIDE]` array-of-named-size syntax, which is only ever
a pointer in C — GCC's interprocedural `-Wstringop-overflow` at `-O3`
sometimes mis-derives an object's real size across multiple non-static
call sites of the same static helper with that spelling). Rewritten as
plain `uint8_t *left` parameters (zero semantic change — array-in-
parameter-position decays to a pointer regardless of the spelling used);
the warnings are gone and the drift/ctest results above are from that
clean build.

## Honest compression/quality measurement (off-board, dev-machine)

Same procedure and the same honesty standard as piece (1)'s own
measurement section: pre-change (`HEAD`, `PART_NxN`/four 4x4 TUs) and
post-change (`PART_2Nx2N`/one 8x8 TU) encoders built side by side
(`git archive HEAD` into a scratch directory for "old", so the live
worktree's git state was never touched), same synthetic content
(`tools/hevc_host_repro.c` patterns 2/3, several sizes/QPs), decoded to
raw YUV via ffmpeg, compared against an independently-regenerated true
source frame (never a raw stream against a fresh `-f lavfi` source
directly, per `CLAUDE.md`'s PSNR rule) with `ffmpeg -lavfi psnr`,
reading `psnr_y` specifically (not the U/V-diluted `average`, since
piece (2) does not touch chroma at all — chroma stays 4x4/DC-only,
unconditionally, on both sides of this change):

| size/QP/pattern | old (4x4 NxN): bytes / PSNR-Y | new (8x8 2Nx2N): bytes / PSNR-Y | delta | reading |
|---|---|---|---|---|
| 256x256 q12 diag | 7462 B / 52.56 dB | 7733 B / 54.53 dB | +3.6% / +1.97 dB | bigger, better |
| 256x256 q27 diag | 3732 B / 43.74 dB | 3618 B / 45.85 dB | -3.1% / +2.12 dB | **smaller AND better** |
| 128x128 q20 diag | 1047 B / 50.95 dB | 1064 B / 50.70 dB | +1.6% / -0.25 dB | bigger, slightly worse |
| 64x64 q20 diag | 341 B / 50.81 dB | 328 B / 50.72 dB | -3.8% / -0.09 dB | smaller, slightly worse |
| 128x128 q34 diag | 765 B / 39.42 dB | 548 B / 41.35 dB | **-28.4% / +1.93 dB** | **smaller AND better** |
| 256x256 q12 rand | 68969 B / 49.80 dB | 66618 B / 50.04 dB | -3.4% / +0.24 dB | **smaller AND better** |
| 256x256 q27 rand | 42742 B / 35.74 dB | 40642 B / 35.97 dB | -4.9% / +0.23 dB | **smaller AND better** |

**Reading this**: 5 of 7 points are a Pareto improvement or a real
quality gain paid for honestly in more bytes; the remaining 2 (128x128
q20 diag, 64x64 q20 diag) are a small regression on both axes, but both
deltas are tiny (-0.25 dB / +1.6% and -0.09 dB / -3.8%) compared to
piece (1)'s own flagged regressions (which ran to -1.74 dB / -5.3% and
worse before its RD-bias follow-up). Mechanically this is unsurprising
in the SAME direction piece (1) already documented: an 8x8 undivided
transform removes 3 of the 4 old MPM/escape signaling costs and the
per-4x4-PU overhead, but trades away the four independent 4x4
predictions' finer spatial adaptivity — on a diagonal ramp with a
genuinely different gradient across a 128x128 or 64x64 CU's four
former sub-blocks, one 8x8 prediction fits the content slightly less
well than four locally-adapted 4x4 ones, exactly at the point where the
byte savings and the prediction-quality cost are both small. Pattern 3
(pseudo-random, no exploitable local structure for four separate 4x4
predictions to fit better than one 8x8 one) shows no such regression at
either QP tested.

**This is suggestive, not a BD-rate curve** — same caveat piece (1)'s
own measurement carried: two QP points per pattern, off-board, on
synthetic content only. A real `lab qsweep`/BD-rate run against natural
video, on a board, is what would be needed before trusting this
direction for anything beyond this dev-machine measurement, exactly as
piece (1)'s notes said and for the same reasons (no board available
this session either).

## What was preserved (re-verified, not assumed)

All re-checked via the same 53-case drift suite, which specifically
includes chroma, inter/P-frame and VBR-rate-control cases:

- **Chroma QP (Table 8-10)**: untouched. `encode_cu()`'s chroma block —
  DC-only 4x4 prediction, `hevc_transform_quant_4x4()`/
  `hevc_dequant_itransform_4x4()` at `hevc_chroma_qp_from_luma(qp)` —
  is not touched by this change at all; it runs exactly as it did
  before, on the same 4x4 chroma block a CU's chroma always was
  regardless of luma `part_mode` (4:2:0 + `MinCbLog2SizeY`-sized CU
  always gives one merged 4x4 chroma TU either way — confirmed by
  re-reading the code, not assumed).
- **Below-left reference-sample fix**: the LUMA path this fix lives in
  (`gather_neighbors_wide()`, used by the now-dead-from-`encode_cu()`
  4x4 mode search) is untouched; the NEW `gather_wide_n()` re-derives
  every reference position's availability from the same
  `zorder_rank()`/`zorder_available()` functions that fix introduced,
  called unchanged (see "Why the z-order rank formula needed no
  change" below).
- **Deblocking-off PPS signalling**: `write_pps()` not modified;
  confirmed still correct because `hevc_host_drift.sh` runs the real
  decoder loop filter (not `-skip_loop_filter all`) and still passes.
- **RD-biased 4x4 mode search (piece 1)**: `hevc_choose_luma_mode()`,
  `hevc_mode_rate_bits()`, `hevc_luma_mode_lambda()` are all byte-for-
  byte unmodified. They are simply no longer called from `encode_cu()`
  (that call site now uses the new `hevc_choose_luma_mode_nxn()`
  instead) — the function remains exported and correct for any future
  caller (e.g. a future 4x4-vs-8x8 RD comparison), it's just not on the
  live path any more. Same for `hevc_predict_4x4()`, still called
  unchanged for chroma.
- **Dead motion search removal (C9)**: untouched; inter-path cases in
  the drift suite (including the six `BC250_HEVC_FAKE_GPU_MV`
  regression cases) still pass.

### Why the z-order rank formula needed no change

`zorder_rank()`/`zorder_available()` (`hevc_intra.c`) encode this
encoder's FIXED coding order (raster CTUs; each CTU always splits into 4
CUs in z-order; luma used to sub-split into 4 PUs in z-order, chroma
never did) as a pure function of pixel position. Piece (2) does not
change the CTU-or-CU axis of that order at all (still raster CTUs, still
`split_cu_flag=1` forcing 4 CUs per CTU, completely unchanged) — it
changes what happens to the LUMA PU axis, from 4 sub-ranks per CU down
to 1. Every reference `gather_wide_n()` looks up is strictly outside the
current 8x8 CU (`x0-1`/`y0-1`/`y0+n..2n-1`, never a position inside the
CU's own 8x8 span), so it never needs the old within-CU 4-sub-rank
resolution the four-PU case used for referencing an earlier PU of the
SAME CU — there is no earlier PU of the same CU any more. Computing
`cur_rank` once at the CU's own top-left corner (exactly the position
`zorder_rank()` already accepts) correctly represents "this CU's coding
instant" for every possible external neighbor, and was confirmed by the
53-case drift pass rather than by this argument alone — including the
non-CTU-aligned cases (100x60, 18x18, 4x4) that stress the availability
substitution hardest.

## Files changed

- `approach1-compute-encoder/src/hevc_intra.h` — new declarations:
  `HEVC_NXN_MAX_N`, `hevc_intra_filter_flag()`, `hevc_predict_nxn()`,
  `hevc_choose_luma_mode_nxn()`, `hevc_transform_quant()`,
  `hevc_dequant_itransform()`. No existing declaration changed.
- `approach1-compute-encoder/src/hevc_intra.c` — the new "A6 piece (2)"
  section: `DCT32_LEFT[]`/`hevc_tmat()`, `gather_wide_n()`,
  `hevc_intra_filter_flag()`, `predict_angular_sample_n()`,
  `predict_blockN()`, `hevc_predict_nxn()`, `sad_nxn()`/
  `hevc_choose_luma_mode_nxn()`, `forward_transform_nxn()`/
  `inverse_transform_nxn()`, `quantize_nxn()`/`dequant_nxn()`,
  `hevc_transform_quant()`/`hevc_dequant_itransform()`. Nothing above
  this new section was modified.
- `approach1-compute-encoder/src/encoder_h265.c` — `encode_cu()`'s luma
  path rewritten for `PART_2Nx2N`/one 8x8 TU (see "Why only 8x8 is
  reachable" above for the full reasoning); the CPU path's `write_sps()`
  call site's `max_tb_log2` argument changed `2` → `3`. Chroma handling,
  the skip-CU path, `encode_ctu()`'s `split_cu_flag`, and
  `encode_core_gpu()` (the GPU path) are all unmodified — confirmed by
  re-reading the diff, not assumed.
- `docs/notes/a6-cu-tu-structure.md` — this file.

No changes to `docs/DEVLOG.md` or `docs/backlog.md` (parallel-agent
constraint), no changes to `hevc_intra_wavefront.comp` or any other GPU
file, and no changes to the `bc250-work` read-only reference tree.
Scratch measurement scripts (`measure_rd.sh`/`measure_rd2.sh`/
`measure_rd3.sh`, a `git archive`-based old/new build harness) were
dev-machine-only and not committed — reproducible from this file's
description (`git archive HEAD` for the "old" baseline binary,
`tools/hevc_host_repro.c` unmodified for the encode, `ffmpeg -lavfi
psnr` between two forced-framing raw YUV decodes for the PSNR-Y numbers
above).

## Piece (3): scoped, not attempted — what it needs

Per the task's own ordering rule (only attempt (3) if (1) and (2) are
solid) and this project's standing practice of stopping cleanly at a
real wall rather than stacking further structural change on an
already-substantial one in the same pass. From having now actually
implemented (2), the concrete starting point for (3) is more specific
than `a6-cavlc-residual-port.md`'s original description:

- **A real per-CTU `split_cu_flag` decision.** `encode_ctu()` currently
  hardcodes `hevc_cabac_code_split_cu_flag(cab, 1, cond_l + cond_a)` —
  always split the 16x16 CTU into four 8x8 CUs. Piece (3) needs this to
  sometimes be 0 (an undivided 16x16 CU), which needs:
  - `write_sps()`'s `max_tb_log2` bumped again (this session leaves it
    at 3/MaxTb=8; a 16x16 undivided CU wants MaxTb=16, exactly what the
    GPU path already passes) — but now for the CPU path too, and
    conditionally correct for whichever CUs actually go undivided.
  - **`split_cu_flag`'s ctxInc must test neighbour CtDepth, not a
    boolean.** This is the exact area `docs/hevc-gpu-intra.md` records a
    real, previously-shipped bug in: coding it as `(col>0)+(row>0)`
    instead of testing whether each neighbour CU is *deeper* (9.3.4.2.2)
    silently zeroed the context every time on an all-depth-0 path,
    costing a near-black 5.0 dB picture that decoded without a single
    parse error. Piece (3) is the first time this CPU path would have
    more than one possible CtDepth per CTU, so this is a real, live risk
    here, not a historical footnote — a per-CU `CtDepth` (0 for an
    undivided 16x16 CU, 1 for a split-to-8x8 one) needs to be tracked
    and the neighbour lookup needs to compare depths, not presence.
  - A real per-CTU size decision (16x16 whole vs split-to-4x8x8) that
    this session's "8x8 kernels exist and are wired in" makes at least
    possible to compare against — but that comparison itself (which is
    smaller, which is better) is new work, not something this pass's
    SAD-only, single-size search does.
  - `part_mode`'s choice becomes meaningful at the 16x16 level too (a
    16x16 CU could be `PART_2Nx2N` with one 16x16 PU/TU, or `PART_NxN`
    with four 8x8 PU/TUs) — this session's 8x8-kernel work is exactly
    the piece that choice would need on its `PART_NxN` side, so it is a
    genuine head start, not a coincidence.
- **Wiring the already-generic 16x16/32x32 transform/quant kernels into
  a real call site** falls out of the above once a CU can actually be
  that large — the kernels in this pass do not need to change, only
  gain a caller.
- **The 8x8 mode search's SAD-only shortcut and piece (1)'s RD-bias
  lambda would both need re-examination once real CU-size competition
  exists** — deciding "should this CTU be one 16x16 CU or four 8x8 CUs"
  is exactly the kind of RD question a raw SAD comparison across
  different total sample counts answers badly; some rate-normalized or
  bits-per-sample comparison would be needed, which is real, separate
  design work.

## Further piece-(2) work not attempted this session (scoped, not started)

Independent of piece (3), and listed here so a future pass has a
concrete list rather than a vague "make it faster/better":

1. **A real RD-aware mode search at 8x8** (this pass is SAD-only — see
   "Deliberate simplifications" above) — needs its own measured lambda,
   not a reused 4x4 constant.
2. **Partial-butterfly transform kernels for 8x8/16x16/32x32**, if a
   real workload ever shows the plain matrix product costing something —
   not measured this session (no board), so not attempted per this
   project's "measure before optimizing" rule.
3. **A reciprocal-multiply quantizer for sizes > 4x4** — needs its own
   overflow-bound derivation and exhaustive proof per size, the same
   rigor `hevc_quantize_4x4()`'s `QUANT_MAGIC[]` comment documents for
   the 4x4 case.
4. **An actual coded, per-CU `split_transform_flag` choice within a
   single fixed CU size** (e.g. an 8x8 CU sometimes choosing to split
   into four 4x4 TUs even under `PART_2Nx2N`, which needs
   `max_transform_hierarchy_depth_intra >= 1`) — deliberately not done
   this session (see "Why only 8x8 is reachable"); only meaningful once
   there's a measured RD reason to prefer it, which piece (1)'s own
   4x4-vs-8x8 numbers above suggest exists on SOME content (the two
   small regressions) but not enough to justify inventing the decision
   function without a board to validate it against.
