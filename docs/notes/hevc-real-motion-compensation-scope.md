# Giving HEVC P-frames real motion compensation — scope, not a patch

Backlog H3 / DEVLOG §43 measured the cost of not having this: at the
identical QP on real content, HEVC costs **2.09x** what H.264 costs, and
that entire gap disappears (1.09x) with P-frames removed. This note is
the scoping pass for closing it - reading what's actually in the tree
today before proposing what to add, not a specification written from
memory of the standard.

**Not started this session.** Comparable in size to backlog G17 (HEVC
multi-slice threading) - a real feature project with its own board
verification at every step, not something to bolt onto an RC-negotiation
fix.

## 1. What's actually there today

`encode_cu()` (`encoder_h265.c`) has exactly two P-frame outcomes for a CU,
and nothing between them:

1. **Skip** (`cu_skip_flag=1`): the co-located zero-motion-vector SAD
   passes a threshold, so the CU is a straight copy from `prev_recon_*`
   at offset (0,0) - no residual, ever, by construction. Every merge
   candidate `derive_merge_candidates()` produces is (0,0) today (it's
   spatial-neighbor-only, and every neighbor is itself always (0,0)), so
   the "which candidate" machinery (`merge_idx`) is fully wired but
   structurally can't select anything but the zero vector.
2. **Full intra** (`pred_mode_flag=1`, `MODE_INTRA`): anything that
   doesn't pass the skip threshold falls all the way to intra prediction
   - the same transform/quant/residual pipeline as an I-frame CU, paying
   the full intra cost regardless of how small the actual motion was.

There is no third path: no CU is ever inter-predicted *with* a residual.
That gap is exactly what the 1.09x/2.09x measurement isolates.

**The GPU motion search already runs and already produces real vectors -
they're just discarded.** `motion_estimation.comp` is dispatched
unconditionally as Stage 2 of `gpu_compute_dispatch_encode()` (shared with
the H.264 path; `is_intra` picks the prediction path, not whether ME
runs), producing one motion vector per 16x16 CTU. `hevc_encoder_encode_
frame()` reads it back via `gpu_compute_get_mv_staging_data()` into
`enc->gpu_mvs[]` - and nothing consumes that array. `docs/notes/dead-
motion-search.md` (which removed the CPU-side diamond search as
provably-dead code, since nothing could signal a non-zero vector) says
this outright: the GPU MV readback and `BC250_HEVC_FAKE_GPU_MV` (the
off-board test hook that injects a fake non-zero vector into
`enc->gpu_mvs[]`) are "the seam a real MVD/AMVP path would reconnect to."
This means the *hardest* part of a motion-compensation feature - actually
finding motion - has a real, running, GPU-accelerated implementation
already. What's missing is entirely on the CPU/bitstream side: nothing
exists that can *say* a CU moved.

**A previous attempt at consuming this got the mechanics wrong.**
Backlog G6 (`4118dcc` fork review) found and rejected a version of "inject
the GPU MV as a merge candidate" with an ordering/pruning bug. The
*concept* - using the already-computed CTU vector as a real merge
candidate - is sound; that specific implementation wasn't.

## 2. What HEVC's own syntax needs that isn't coded yet

Checked against `hevc_cabac.h`: `hevc_cabac_code_pred_mode_flag()` and
`hevc_cabac_code_merge_idx()` already exist (used today, always with
fixed arguments - `pred_mode_flag` is only ever called with `1`). Nothing
else inter-specific exists. A real inter CU (ITU-T H.265 7.3.8.5/7.3.8.6)
needs, in ascending order of how much new machinery each one requires:

- `merge_flag` - new CABAC context, doesn't exist.
- `rqt_root_cbf` - new CABAC context; gates whether the CU's transform
  tree is coded at all when in merge mode with a matching prediction.
  Also doesn't exist (intra CUs don't need it - they always go through
  the transform tree).
- Real per-CU residual coding *for an inter-predicted block* - the
  transform/quant/CABAC-residual pipeline already exists and is codec-
  agnostic (it operates on `residual[]` = source − prediction, regardless
  of where the prediction came from), so this is generalizing an existing
  path to accept a motion-compensated prediction block instead of only
  ever an intra-predicted one - not new pipeline.
- `inter_pred_idc`, `ref_idx_l0`, `mvp_l0_flag`, `mvd_coding()`
  (`abs_mvd_greater0_flag`, `abs_mvd_greater1_flag`, `abs_mvd_minus2`
  exp-Golomb, `mvd_sign_flag`) - none of this exists. This is the full
  explicit-AMVP path, and it's genuinely the biggest single piece: new
  CABAC syntax elements, a new AMVP candidate derivation (8.5.3.2.5-
  8.5.3.2.7 - related to but distinct from the existing merge-candidate
  derivation in `derive_merge_candidates()`), and a real per-CU motion
  search refinement (the shape of thing `docs/notes/dead-motion-search.md`
  removed, now with somewhere legal to put its answer).

## 3. A phased plan, cheapest-and-most-certain first

**Phase 1 - merge with real residual, using the vector that's already
computed.** Reuse `enc->gpu_mvs[]`'s per-CTU vector as a genuine
additional merge candidate (fixing G6's ordering bug this time), and
remove the implicit "merge chosen ⇒ must be zero-residual skip" coupling:
let a CU take `merge_flag=1` with a *non-zero* vector, predict from that
offset in `prev_recon_*`, and run the existing transform/quant/residual
pipeline against the resulting (small) residual instead of falling to
intra. Needs: `merge_flag` + `rqt_root_cbf` CABAC contexts (new but
small), generalizing the CU-encode path to accept a motion-compensated
prediction (mechanical - the transform/residual code doesn't care where
`residual[]` came from), and revisiting `derive_merge_candidates()`'s 5-
candidate list construction. Does **not** need AMVP, MVD coding, or a new
motion search - the GPU already supplies the vector.

This targets the measured mechanism directly: every CU that fails the
zero-motion skip test today goes straight to full intra. A block moving
with the CTU's real (small, coherent) motion - the common case for
camera pans and character motion, which is most of what a real film
looks like - would get a cheap motion-compensated residual instead.
Expect this to close a meaningful fraction of the 2.09x gap, not
necessarily all of it: one CTU-wide vector is coarser than a per-CU
search, and it says nothing about content whose real motion isn't well
described by a single 16x16-CTU-granularity vector.

**Phase 2 - full AMVP + MVD signalling.** The "real x265-grade P-frame"
tier: a real per-CU motion search (reviving the shape of what `dead-
motion-search.md` removed, now with `mvd_coding()` to put its answer
into), a genuine AMVP candidate list, and the full new CABAC syntax set
above. Comparable in scope to G17, likely larger given the new entropy-
coding surface. Whether it's worth doing at all should be decided *after*
Phase 1 lands and gets board-measured - if Phase 1 closes most of the
gap, Phase 2's marginal return may not justify its cost.

## 4. Verification this would need, matching how everything else in this
   tree gets landed

- `hevc_host_drift.sh`'s existing 53-case byte-exactness oracle already
  exercises P-frames (`_n*g*_` cases) and the `BC250_HEVC_FAKE_GPU_MV`
  hook - both are the regression guard for "did this change the
  bitstream on cases that shouldn't move," and for "does a real non-zero
  vector actually reach the bitstream now" respectively.
- Locking discipline (top-of-repo CLAUDE.md): `dead-motion-search.md`
  already flags that `gpu_compute_get_mv_staging_data()` calls
  `gpu_compute_submitted_slot()` internally, so consuming `enc->gpu_mvs[]`
  for real is one of the `DRIVER_LOCK`-guarded accessors that rule is
  about - re-read that rule before touching this call site, not after.
- The real payoff number is the same H2/H3 board measurement already on
  file: H.264-vs-HEVC size ratio at matched QP, all-intra vs normal GOP,
  on real content (Big Buck Bunny). Phase 1 should be judged against
  whether it moves 2.09x meaningfully toward 1.09x, not against a
  synthetic PSNR number.
