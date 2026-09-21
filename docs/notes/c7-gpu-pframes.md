# C7: P-frames for the GPU intra path (zero-motion skip)

Status: **first cut implemented, off-board verified where possible, found
and fixed one real bug in the process, gated off by default.** No board run
has happened. Read this whole file before enabling `BC250_HEVC_GPU_PFRAME=1`
anywhere near real hardware.

## Scope, restated

Per the task that produced this: bring the GPU intra path
(`BC250_HEVC_GPU=1`, `hevc_intra_wavefront.comp`) to the *same*
zero-motion-SKIP-plus-intra-fallback parity the CPU path
(`encoder_h265.c`'s non-GPU path) already has and has board-validated - not
real motion compensation, not AMVP/`mvd_coding`. That remains "still open
after this" for both paths (`docs/backlog.md` C9's own wording), and is a
separate, larger piece of work.

## Design, before any code

### The hard part is not the skip decision. It's same-frame neighbour availability.

The obvious framing - "decide skip per CTU on the host, upload a mask, let
the shader read a reference image for skipped CTUs, keep the shader
otherwise unchanged" - runs into a real problem the moment you ask what a
*non-skip* CTU next to a *skip* CTU is supposed to read as its left/above
intra-prediction neighbour.

`hevc_intra_wavefront.comp`'s whole 2:1 wavefront schedule (see
`docs/hevc-gpu-intra.md`) exists because HEVC intra prediction reads
already-reconstructed neighbour SAMPLES, not neighbour macroblocks - and
those samples have to be genuinely correct at the moment a later CTU reads
them, because the residual a later CTU signals is relative to whatever
prediction it made from those samples. If CTU A is a P-frame SKIP (its
reconstruction is, by definition, the co-located block of the reference
picture) and CTU B (to its right) is coded intra, B's left-edge reference
samples must be A's *skip* reconstruction, not some placeholder or A's own
would-have-been intra guess - because that is what a real decoder will
actually have sitting in its picture buffer when it decodes B. Get this
wrong and the encoder's residual for B corrects against the wrong
prediction; the decoder reconstructs a different, wrong B. This is exactly
the class of bug this project's own history is full of (the `left[4]`
below-left bug, the chroma QP bug, the merge-candidate injection bug) -
structurally valid bitstream, wrong picture, and often invisible to a
silent-decode check.

A host-side "encode intra for everything, then overwrite the skip CTUs'
bytes afterward" approach cannot fix this: by the time the host sees
anything, the *entire* frame's wavefront has already run on the GPU, using
whatever pixels were in `recon_image` at each step - which, for a CTU the
host meant to mark skip, would be that CTU's freshly (and wrongly, from the
decoder's point of view) intra-predicted pixels, already consumed as a
neighbour by anything scheduled after it.

### The insight that makes this cheap: `recon_image` already IS the reference, for free

`gpu_compute.c`'s `ctx->recon_image` is not recreated every frame - it
persists in device memory across `gpu_compute_hevc_dispatch_intra()` calls,
and nothing clears it between frames. At the moment frame N+1's dispatch
begins, `recon_image` still holds frame N's finished reconstruction,
untouched, because the wavefront shader only ever writes to a CTU's own
cells once per frame, at that CTU's own step.

So: for zero-motion skip specifically (never any other displacement), the
co-located reference block a decoder would use for a SKIP CU is *already
sitting in `recon_image` at that CTU's coordinates*, right up until the
moment this frame's dispatch would otherwise have overwritten it. The
entire "give the shader a reference to copy from" problem disappears if the
shader simply **does not write** to a flagged CTU's cells at all. Its
existing content - last frame's real reconstruction - is left in place,
and any later-scheduled CTU (skip or not) that reads it as a neighbour sees
exactly what a real decoder's neighbour would be, because that is what a
real decoder's reference picture and this encoder's reference picture are,
by construction, the same object.

This is why the actual shader diff (`shaders/hevc_intra_wavefront.comp`) is
six lines: a `skip_mask` SSBO (binding 7) and an early `return` right after
the existing `if (s_ctby < 0) return;` guard, before `gather()` or any
image read/write. No new reference image, no copy shader, no change to any
of the already-board-validated intra/transform/quant/reconstruction code.
When `skip_mask` is all-zero (every caller before this feature, and every
caller with `BC250_HEVC_GPU_PFRAME` unset), the shader's behaviour is
provably identical to before - the branch is never taken.

### The insight that makes signalling cheap: `merge_idx` is always 0

The GPU path's CTU *is* the CU (one undivided 16x16, no split - see
`docs/hevc-gpu-intra.md`'s coding-structure table), so this path never
needs the CPU path's `derive_merge_candidates()` (which is specific to four
8x8 `PART_NxN` CUs per CTU and their z-scan sub-positions - see that
function's own comment). But more than that: **this path never signals a
non-zero motion vector, anywhere, by construction** (matching the CPU
path's own post-C9 state - `docs/notes/dead-motion-search.md`). Given that:

- Every spatial candidate this path can ever mark "available and inter" is
  itself a zero-motion SKIP, so its value is (0,0).
- `sps_temporal_mvp_enabled_flag = 0` (`write_sps()`), so there is no
  temporal candidate.
- A P-slice's zero-candidate padding (8.5.3.2.1) is (0,0)/refIdx 0 by
  construction, and `num_ref_idx_l0_default_active_minus1 = 0` means
  refIdx can only ever be 0 anyway.

Every one of the (up to five) entries a real decoder would ever derive for
this CU's merge list is (0,0). `merge_idx`'s *value* therefore cannot
change the reconstructed picture no matter which of the five slots a
decoder resolves it to - so `encode_core_gpu()` always signals `merge_idx =
0`, the cheapest legal truncated-unary code (one context-coded bin), rather
than deriving a list whose content could never have mattered. This is
exact, not a shortcut with an asterisk - see the comment at the call site
in `encoder_h265.c` for the full argument.

What still needed real, per-CTU derivation (because it DOES change decoder
state): `cu_skip_flag`'s ctxInc (9.3.4.2.2, `condL + condA` where `condX`
is "that neighbour exists and was itself skip" - same formula as the CPU
path's `encode_cu()`, reused at CTU instead of 8x8-CU granularity since
this path has exactly one CU per CTU) and the MPM DC-for-skip-neighbour
rule (8.4.2: `CuPredMode != MODE_INTRA` forces `candIntraPredModeX =
INTRA_DC` for a skip neighbour).

## What changed, by file

- `shaders/hevc_intra_wavefront.comp`: binding 7 (`skip_mask`, one `uint`
  per CTU), a `shared bool s_skip`, and an early return. Compiled with
  `glslangValidator -V` and validated with `spirv-val` - both clean. No
  other line changed.
- `src/gpu_compute.h` / `src/gpu_compute.c`: a second, host-visible,
  double-buffered buffer pair (`hevc_skip_buffers[2]`) alongside the
  existing HEVC mode/coeff/cbf ones, bound to binding 7 and rewritten every
  dispatch from whatever `skip_mask` the caller passes (`NULL` -> all-zero,
  unchanged behaviour). `gpu_compute_hevc_dispatch_intra()` gained
  `skip_mask` and `out_recon_was_reset` parameters - the latter is a
  defence-in-depth signal: if `recon_image` had to be (re)created this call
  (first frame ever, or a coded-dimension change), its content is
  undefined, not "last frame's reconstruction", so the dispatch ignores
  whatever mask it was given regardless of what the caller asked for, and
  tells the caller so it can treat the frame as an IDR. A second new
  function, `gpu_compute_hevc_download_recon_nv12()`, is the exact
  readback `gpu_compute_debug_dump_recon()` already did for a file dump,
  exposed as a real (always-on) API - the skip decision needs the
  reference on the host to compare against source.
- `src/encoder_h265.c` / `.h`: the bulk of the change.
  - `hevc_encoder_t` gained `use_gpu_pframe` (the `BC250_HEVC_GPU_PFRAME=1`
    opt-in, latched at create time next to `use_gpu` for the same
    SPS-latching reason), `gpu_ctu_skip` (one `uint32_t` per CTU, doubling
    as the shader's `skip_mask` upload and the host's own ctxInc/MPM
    bookkeeping - `cu_is_inter == cu_is_skip` on this path, so one array
    is enough, unlike the CPU path's separate `cu_skip_map`/`cu_is_inter`),
    and `gpu_recon_uv_scratch` (de-interleaving scratch for the NV12
    reference readback).
  - `decide_gpu_ctu_skips()` (new): the per-CTU zero-motion SAD decision.
    Reuses `compute_sad_8x8_luma()`/`compute_sad_4x4_chroma()` verbatim,
    four times each, to cover a 16x16 luma / 8x8 chroma CTU instead of an
    8x8/4x4 CU - no new SAD code. Threshold is `4 * 96 * (1 + qp/8)`, i.e.
    the CPU path's own per-CU threshold scaled by the 4x area ratio - a
    heuristic starting point, explicitly not a derived constant (see
    `BC250_HEVC_GPU_SKIP_THRESHOLD` override), because **no value of this
    threshold can make the bitstream non-conforming** - a CTU marked skip
    always reconstructs as an exact reference copy by construction. Wrong
    only trades bitrate/quality, never correctness.
  - `encode_core_gpu()` gained an `is_idr` parameter (previously hardcoded
    true) and, for `!is_idr`, the P-slice header (byte-identical syntax to
    `encode_core()`'s - same RPS, same PPS), the `cu_skip_flag`/`merge_idx`
    signalling above, and `has_ref`/`poc` bookkeeping so a GPU-path frame
    (I or P) becomes a valid reference for the next one - previously
    `has_ref` stayed false forever on this path "deliberately", which was
    correct only because the path had never had anything to reference.
  - `hevc_encoder_encode_frame()`'s GPU branch now computes `is_idr` the
    CPU path's way (gated by `use_gpu_pframe` - unset, it is unconditionally
    true, reproducing prior behaviour exactly), and for a P-frame candidate
    downloads the source NV12 and the reference (`recon_image`) to host
    memory, runs the skip decision, and passes the resulting mask into the
    (now GPU-side-checked) dispatch.
  - `hevc_encoder_encode_gpu_raw()` (new, alongside the existing
    `hevc_encoder_encode_raw()`): the off-board test entry point - see
    Verification below.

## Why this is gated off by default

`BC250_HEVC_GPU=1` alone reproduces the exact board-validated all-intra
behaviour with **zero** code-path change - `is_idr` is forced true, the
skip mask is always `NULL`/all-zero, the shader's early return is never
taken. `BC250_HEVC_GPU_PFRAME=1` is a second, independent opt-in, because
this feature has had **no board run at all**. That is the same precedent
`docs/backlog.md` C5 already set for `BC250_HEVC_GPU` itself: don't change
validated default behaviour for something unvalidated, however carefully
reasoned.

## Verification

### What actually ran, off-board

There is no board and no working Vulkan device in this environment.
`libvulkan_lvp.so` (lavapipe/llvmpipe) IS present in this WSL environment,
confirmed by checking `/usr/lib/x86_64-linux-gnu/` and
`/usr/share/vulkan/icd.d/lvp_icd.json` directly - so the premise that no
device exists at all was worth checking rather than assuming, and it holds:
there is a software Vulkan implementation available, but this session did
not attempt to drive a real `vaCreateContext`/surface-lifecycle test
through it (the existing `test_va_api`/`VaApiDriverTest` OOM the prompt
that started this investigation already described, and reproducing or
debugging that OOM was out of scope for this pass - see "What is NOT
verified" below for exactly what that leaves unchecked).

What DID run, all real, all off-board:

1. **The full project builds clean.** `cmake` + `make -j4` on every target
   (`bc250_drv_video`, all `tests/`, `hevc_bench`/`hevc_cabac_bench`/
   `hevc_host_repro`/`rc_bench`/`cavlc_bench`) with the project's own
   `-Wall -Wextra`, zero new warnings, after touching `gpu_compute.h/.c`,
   `encoder_h265.h/.c` and the shader. This is real signal: it means every
   caller of the two changed function signatures
   (`gpu_compute_hevc_dispatch_intra()`, `encode_core_gpu()`) - there is
   exactly one call site of each - was actually updated, and no type
   mismatch slipped through.
2. **The shader compiles and validates.** `glslangValidator -V
   hevc_intra_wavefront.comp` succeeds; `spirv-val` on the resulting module
   reports nothing. This is real static verification of the GLSL/SPIR-V
   correctness of the one shader change, short of running it.
3. **The CPU path's existing off-board oracle still passes.**
   `tools/hevc_host_drift.sh` (53/53 cases, byte-exact against ffmpeg's own
   decode, both planes) and `tests/test_hevc_encode`/`test_encode` all pass
   unchanged after this work. This matters because several new fields
   (`gpu_ctu_skip`, `use_gpu_pframe`, etc.) live on the *shared*
   `hevc_encoder_t` struct and `hevc_encoder_create()`/`_destroy()` - this
   confirms the CPU path (which this task was explicitly told not to
   touch) is unaffected.
4. **A new off-board entry point, `hevc_encoder_encode_gpu_raw()`, drives
   the new GPU-path P-slice code with no GPU at all** - the same "stand in
   for the missing GPU signal" role `BC250_HEVC_FAKE_GPU_MV` already plays
   for the CPU path (see `hevc_encoder_encode_raw()`'s existing comment).
   It takes the source and an explicit reference plane as host pointers,
   and synthetic per-CTU mode/coeff/cbf arrays (default: flat DC, no
   residual) standing in for whatever the shader would have produced for
   non-skip CTUs. **This is the only way `encode_core_gpu()`'s new P-slice
   code has run at all, ever, in this environment or before it.**

   Driving it (scratch harness, not committed - `c7_pframe_test.c`,
   described here for reproducibility) at 32x32 through 128x128 and at a
   non-CTU-aligned 100x60, one IDR frame then one P frame with the
   reference set equal to the IDR frame's own (uniform) source:

   - **Found a real bug, immediately, at every size tested.** The decoded
     P-frame differed from the decoded reference by up to ~10% of pixels
     (never catastrophic - values drifted a handful of steps around 128,
     never "black" - and ffmpeg reported **zero** decode errors the whole
     time, exactly the "a decoder error names where it noticed, not the
     fault" failure mode this project's own conventions warn about).
     Root cause: `split_cu_flag` - a `coding_quadtree()` element that must
     be coded *before* `coding_unit()` is ever entered, for every CTU,
     regardless of slice type - was only being written on the intra
     (non-skip) branch, and written *after* `cu_skip_flag`/
     `pred_mode_flag` even there. Every skip CTU was missing the bin
     entirely; every non-skip P-slice CTU had it in the wrong position.
     All-intra frames never hit this, which is why the already-board-
     validated all-intra path was never at risk: `is_idr` never took the
     skip branch, and `pred_mode_flag` is never coded there either, so
     `split_cu_flag` already came first on that path. It only broke once a
     P-slice existed to code something ahead of it. Fixed by moving
     `split_cu_flag(0, 0)` to the top of the per-CTU body, before the
     `cu_skip_flag` check, unconditionally - see the comment at that call
     site in `encoder_h265.c`.
   - **After the fix, re-run at every size (32x32, 48x48, 64x64, 80x80,
     96x96, 112x112, 128x128, and 100x60): the decoded P-frame is
     byte-identical to the decoded reference frame, in every case.** This
     is the strongest off-board claim this feature can make: a real,
     independent decoder (ffmpeg) reconstructs a fully-skipped P-frame as
     an exact copy of its reference, which is exactly what HEVC's spec
     says a zero-motion SKIP CU must do, and is not something the encoder
     could fake by agreeing with itself.
   - **A mixed skip/non-skip case (left half of a 128x128 frame identical
     to the reference, right half different) decodes silently** - real
     variation in `cu_skip_flag`'s ctxInc (some CTUs have a skip left/above
     neighbour, some don't, some are the picture's first CTU with neither)
     with no CABAC desync. Confirmed 32/64 CTUs chosen skip, matching the
     exact 50/50 spatial split the test constructed.
   - **`ffmpeg -bsf:v trace_headers` confirms the P-slice header parses
     exactly as intended**: `slice_type=1`, `slice_pic_order_cnt_lsb=1`,
     `short_term_ref_pic_set_sps_flag=1`, `num_ref_idx_active_override_
     flag=0`, `five_minus_max_num_merge_cand=0` - byte-for-byte the values
     `encode_core_gpu()` writes, with no bit drift from the preceding IDR's
     VPS/SPS/PPS.

### What is NOT verified, and needs a board

- **Nothing about real GPU execution.** The shader's `skip_mask`
  early-return, the `recon_image` reuse-across-frames argument, and the
  `gpu_compute.c` resource/descriptor plumbing (the new `hevc_skip_buffers`
  pair, the binding-7 rewrite every dispatch, `out_recon_was_reset`) have
  been reviewed carefully and are designed to be a no-op when unused, but
  **none of it has executed on any Vulkan implementation, software or
  real, in this pass.** The static shader validation (compiles, SPIR-V
  valid) is real but is not execution.
- **The `decide_gpu_ctu_skips()` threshold is unmeasured.** It cannot be
  wrong in a way that breaks conformance (see above), but whether `4 * 96 *
  (1 + qp/8)` is anywhere near the right rate/quality tradeoff for this
  path's 16x16 CTUs - as opposed to the CPU path's 8x8 CUs it was scaled
  from - is a real, open, board-measurement question. `lab qsweep
  --codec=hevc --env="BC250_HEVC_GPU=1 BC250_HEVC_GPU_PFRAME=1"` is the
  right tool once a board is available.
- **No throughput number of any kind.** The new P-frame candidate path adds
  a real NV12 download (source) and a real `recon_image` readback
  (reference) to the host, per P-frame, that the existing all-intra fast
  path never did - `docs/performance-measurement.md`'s rules about idle
  numbers, load conditions and `tools/lab bench` all apply, and none of
  them have been run. This is a genuine, acknowledged, *unquantified* cost,
  not assumed to be free.
- **Cross-path fallback interaction, reasoned about but not tested.** If a
  GPU P-frame candidate's dispatch or readback fails and `hevc_encoder_
  encode_frame()` falls through to the CPU path (`encode_core()`) for that
  same frame, the CPU path will find `has_ref` true (set by an earlier
  successful GPU frame) and `prev_recon_y/cb/cr` already populated - by
  this same feature's own pre-dispatch reference download, which holds the
  correct "previous frame's reconstruction" regardless of which path
  handles the current frame. This should be consistent by construction
  (both paths mean the same thing by `prev_recon_*`), but it is an
  interaction between two previously-independent code paths that did not
  exist before this feature and has not been exercised, board or
  off-board.
- **Multi-P-frame GOPs beyond one P after one I.** The off-board harness
  above tests exactly one P-frame; nothing here builds a longer chain (P
  referencing P referencing P...) or exercises `hevc_cabac_reset_contexts`'
  P-slice re-init across more than one transition. The underlying mechanism
  (recon_image persisting, one reference, DeltaPoc -1) does not obviously
  care how many P-frames precede it, but "does not obviously care" is a
  claim, not a measurement.

### Bottom line

The signalling layer - the part this task specifically flagged as needing
real design work (RPS, P-slice headers, `cu_skip_flag`, the DPB-lite
single-reference model) - is now implemented, verified correct by the
strongest off-board check available (independent-decoder byte-exactness on
the one property that's spec-guaranteed regardless of threshold tuning),
and already caught and fixed one real bug that a
silent-decode check alone would have missed, consistent with this
project's entire prior history on this file. The GPU-side plumbing (shader,
descriptor/buffer wiring) is implemented to the same standard of care as
the already-shipped shader, is backward-compatible by construction with
every existing caller, and is honestly unverified beyond static shader
validation - it needs a board, and is gated behind `BC250_HEVC_GPU_PFRAME=1`
(default off) specifically so that landing this cannot regress the
already-validated `BC250_HEVC_GPU=1` all-intra baseline while that board
run is pending.
