/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_intra.h - real HEVC 4x4 intra prediction, transform, quantization
 *                and reconstruction, implemented independently against the
 *                ITU-T H.265 spec (clauses 8.4.2 intra mode derivation,
 *                8.4.4.2 intra sample prediction, 8.6.3 scaling/dequant,
 *                8.6.4 transform).
 *
 * WHY THIS EXISTS INSTEAD OF REUSING THE GPU PIPELINE: the existing Vulkan
 * compute shaders (dct_transform.comp/quantize.comp/residual_predict.comp)
 * are H.264-shaped in two ways that are NOT just block-size differences:
 *
 *   1. HEVC mandates an alternative 4x4 transform (DST-VII) for luma intra
 *      residuals specifically (ITU-T H.265 8.6.4.1/8.6.4.2) - never DCT for
 *      that one case. dct_transform.comp only ever computes DCT. Signaling
 *      DCT-domain coefficients into an HEVC bitstream as a 4x4 luma intra
 *      TU (which every conformant decoder, ffmpeg included, will inverse-
 *      DST) would decode to a real picture that LOOKS plausible but is
 *      systematically wrong - exactly the "syntactically valid, content
 *      garbage" failure mode DEVLOG.md Sec. 1/6 warns about.
 *   2. HEVC's intra prediction is always performed per-transform-block
 *      using that block's own already-reconstructed neighbors, even when
 *      several TUs share one signaled prediction mode - i.e. the actual
 *      per-pixel prediction has to be recomputed and reconstruction-chained
 *      at 4x4 granularity in coding (z-scan) order. The GPU's
 *      residual_predict.comp only ever computes ONE whole-macroblock I16x16
 *      prediction per dispatch; it has no equivalent to H.264's Intra_4x4
 *      chaining at all.
 *   3. HEVC's dequantization scale (ITU-T H.265 8.6.3: m=16, bdShift =
 *      BitDepth+Log2(nTbS)-5, levelScale={40,45,51,57,64,72}) is a
 *      different function of QP than H.264's, so even reusing the GPU's
 *      already-quantized DCT levels verbatim (setting aside point 1) would
 *      hand a decoder numbers scaled for the wrong standard.
 *
 * None of these are solvable by clever re-addressing of the existing
 * buffers - they require new transform/quant math and a new prediction
 * chaining order, so this module implements the whole per-4x4-block intra
 * codec core in portable C, run on the CPU against real picture data
 * (see encoder_h265.c, which downloads it from the GPU surface via the
 * existing, unmodified gpu_compute_download_nv12() - that part IS reused).
 */
#ifndef BC250_HEVC_INTRA_H
#define BC250_HEVC_INTRA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Named HEVC intra modes. The full set 0..34 (Planar, DC, and the 33
 * angular modes 2..34) is implemented; these four just get names because
 * the surrounding code refers to them by role. */
#define HEVC_MODE_PLANAR      0
#define HEVC_MODE_DC          1
#define HEVC_MODE_HORIZONTAL 10
#define HEVC_MODE_VERTICAL   26
#define HEVC_MODE_COUNT      35

/* The full reference sample set one 4x4 block's prediction can read, after
 * Rec. ITU-T H.265 8.4.4.2.2's substitution scan has filled in every
 * unavailable entry. Angular modes project along a slope and can reach
 * 2*nTbS samples down either edge, so all 4*nTbS+1 = 17 neighbour
 * positions are carried, not just the 11 Planar/DC/10/26 touch.
 *
 *   corner  = p[-1][-1]
 *   left[y] = p[-1][y],  y = 0..7
 *   top[x]  = p[x][-1],  x = 0..7
 *
 * Gathering these is a material share of encode time (it walks 5 z-scan
 * rank comparisons and 17 plane reads), so it is exposed rather than kept
 * internal: a caller evaluating several modes for one block, or
 * predicting a block it has just searched, should gather ONCE and reuse.
 * hevc_predict_4x4() is the convenience wrapper that gathers per call. */
typedef struct {
    uint8_t left[2 * 32];   /* p[-1][0 .. 2*nTbS-1] */
    uint8_t top[2 * 32];    /* p[0 .. 2*nTbS-1][-1] */
    uint8_t corner;
} hevc_refs_t;

void hevc_gather_refs_sz(const uint8_t *recon_plane, int stride, int width, int height,
                          int x0, int y0, int log2_size, int is_luma, hevc_refs_t *refs_out);

/* nTbS == 4 shorthand. */
void hevc_gather_refs(const uint8_t *recon_plane, int stride, int width, int height,
                       int x0, int y0, int is_luma, hevc_refs_t *refs_out);

/* Predict one nTbS x nTbS block from already-gathered references. `mode`
 * may be any of 0..34. Writes nTbS*nTbS samples, row-major.
 *
 * Applies 8.4.4.2.3's reference smoothing internally where the spec calls
 * for it (luma, nTbS >= 8, mode-dependent) - which is why the references
 * are passed unfiltered and each mode filters its own copy: one gathered
 * set is shared across a whole mode search, and different candidates
 * disagree about whether they are filtered. */
void hevc_predict_refs(const hevc_refs_t *refs, int log2_size, int mode, int is_luma,
                        uint8_t *pred_out);

/* nTbS == 4 shorthand. */
void hevc_predict_4x4_refs(const hevc_refs_t *refs, int mode, int is_luma,
                            uint8_t pred_out[16]);

/* Rec. ITU-T H.265 Table 8-10 (scan derivation for intra 4x4/8x8 luma, and
 * 4:4:4 chroma - not applicable to our 4:2:0 chroma, which always scans
 * diagonally): dirMode in [6,14] -> SCAN_VER(2), [22,30] -> SCAN_HOR(1),
 * else SCAN_DIAG(0). Matches hevc_cabac.c's scan_idx numbering. */
int hevc_scan_idx_for_mode(int mode);

/* Real intra mode decision for one 4x4 luma block at pixel position
 * (x0,y0). Each candidate mode's prediction is built from `recon_y` (real
 * already-reconstructed neighbor pixels, or the substituted default where
 * unavailable per 8.4.4.2.2 - the same values a real decoder's own
 * prediction will see), then compared against the real SOURCE pixels at
 * (x0,y0) in `src_y` (both planes share `stride`/`width`/`height`) - the
 * comparison target for "which mode is best" is always the true picture
 * content, not the neighbor data used to build the candidate.
 *
 * `mpm` (the 3 most-probable modes from hevc_derive_mpm(), which the
 * caller must derive BEFORE deciding, not after) and `qp` turn this into a
 * real rate-aware decision rather than pure SAD: an MPM costs 2-3 bits to
 * signal and anything else costs 6, and at 4x4 granularity that side
 * information is a large fraction of an intra frame's total bits, so
 * picking a marginally-better-SAD non-MPM angular mode can easily cost
 * more than it saves. Cost is SAD + lambda(qp) * estimated mode bits.
 *
 * Pass mpm == NULL to get the pure-SAD decision with no rate term. */
int hevc_choose_luma_mode(const uint8_t *src_y, const uint8_t *recon_y, int stride,
                           int width, int height, int x0, int y0,
                           const int mpm[3], int qp);

/* Mode decision for a block of any size, against already-gathered
 * references. Same coarse-then-refine candidate set and same
 * SAD + lambda*bits criterion as the 4x4 form. */
int hevc_choose_mode_sz(const hevc_refs_t *refs, const uint8_t *src, int stride,
                         int x0, int y0, int log2_size, const int mpm[3], int qp);

/* Same decision, but against references the caller has already gathered -
 * so a caller that will go on to predict the winning mode does not pay
 * for the 17-sample substitution scan twice. */
int hevc_choose_luma_mode_refs(const hevc_refs_t *refs, const uint8_t *src_y, int stride,
                                int x0, int y0, const int mpm[3], int qp);

/* ---- Split mode decision (the form that can be offloaded) ----
 *
 * hevc_choose_luma_mode() cannot be parallelized across blocks: it reads
 * `recon_y`, and a block's reconstruction depends on the mode chosen for
 * its z-scan predecessors. That serial chain is ~64% of encode time by
 * profile, all of it in prediction and SAD.
 *
 * These two functions break that chain into a parallel half and a serial
 * half. hevc_block_mode_costs() scores ALL 35 modes for one block against
 * neighbours taken from `ref_plane` - pass the SOURCE picture and every
 * block becomes independent, so the whole frame's costs can be computed
 * at once (on the GPU, or in any order). hevc_pick_mode_from_costs() then
 * applies the rate term serially, where the true MPM list is known.
 *
 * The split keeps the rate-aware decision EXACT: only the distortion term
 * is approximated, by measuring prediction against source rather than
 * reconstructed neighbours. At sane QP those differ by the quantization
 * error alone. It also makes the distortion search exhaustive over all 35
 * modes instead of hevc_choose_luma_mode()'s coarse-then-refine subset,
 * because a parallel evaluator has no reason to prune. */
void hevc_block_mode_costs(const uint8_t *ref_plane, const uint8_t *src_y, int stride,
                            int width, int height, int x0, int y0,
                            uint16_t costs_out[HEVC_MODE_COUNT]);

int hevc_pick_mode_from_costs(const uint16_t costs[HEVC_MODE_COUNT],
                               const int mpm[3], int qp);

/* 🚨 Deciding the mode outright from source-neighbour costs (i.e. using
 * hevc_pick_mode_from_costs() as the final answer) is MEASURED BAD: it
 * costs +48% BD-rate, and it loses on BOTH axes at once - more bits AND
 * 3-12 dB lower PSNR - because the encoder picks whatever predicts the
 * source best from ideal neighbours, while the decoder must predict from
 * quantized reconstruction. Flat/gradient content is hit hardest (+59%
 * desktop, +81% synthetic) and dense detail barely at all (+4.9%). This
 * is the same trap already recorded for the H.264 I-slice path.
 *
 * So the parallel pass is used as a SHORTLIST, not a decision. This
 * returns the `n` cheapest modes by source cost; the caller then
 * re-evaluates just those against the real reconstructed neighbours and
 * picks among them. The shortlist only has to CONTAIN the true winner,
 * which a source-based score is good at, and the decision itself stays
 * exact. Writes n modes to out[], returns how many were written. */
void hevc_rank_modes_by_cost(const uint16_t costs[HEVC_MODE_COUNT], int n, uint8_t *out);

/* Evaluate an explicit candidate list against already-gathered (real)
 * references and return the best by SAD + lambda*bits. */
int hevc_choose_among(const hevc_refs_t *refs, const uint8_t *src_y, int stride,
                       int x0, int y0, const int mpm[3], int qp,
                       const int *cands, int ncands);

/* The Lagrangian multiplier hevc_choose_luma_mode() weighs signalling bits
 * against SAD with, in 1/256ths. Exposed so callers making higher-level
 * rate decisions (e.g. encoder_h265.c's PART_2Nx2N vs PART_NxN choice)
 * weigh bits on exactly the same scale rather than inventing a second
 * one. */
int hevc_lambda_sad_q8(int qp);

/* Bits needed to signal `mode` for a PU whose MPM list is `mpm`:
 * prev_intra_luma_pred_flag plus either mpm_idx or a 5-bit
 * rem_intra_luma_pred_mode. Matches what hevc_cabac_code_intra_luma_
 * flag()/_data() actually emit. Returns 0 if mpm is NULL. */
int hevc_mode_signal_bits(int mode, const int mpm[3]);

/* Derive the 3 most-probable-mode candidates for a 4x4 luma PU at (x0,y0)
 * from its already-decided left/above neighbor block modes, per 8.4.2.
 * left_mode/above_mode are ignored (treated as unavailable) when the
 * neighbor is off-picture OR outside the current slice - this encoder uses
 * one slice per picture, so only the frame edges apply. Pass -1 for an
 * unavailable neighbor's mode (any value works, it is masked out by the
 * avail flags). */
void hevc_derive_mpm(int left_mode, int left_avail, int above_mode, int above_avail,
                      int mpm_out[3]);

/* Predict one 4x4 block (luma if is_luma, else one of Cb/Cr) at pixel
 * position (x0,y0) in a `stride`-wide plane of size width x height, using
 * already-reconstructed neighbor samples (recon_plane) and Rec. ITU-T
 * H.265 8.4.4.2.2's neighbor-substitution + 8.4.4.2.4-6's DC/Planar/
 * angular sample derivation (including the DC/mode-10/mode-26 luma
 * edge-filtering steps - chroma never gets edge-filtered, per the spec's
 * own cIdx==0 conditions). `mode` may be any of 0..34. Writes 16
 * predicted samples, row-major (pred[y*4+x]). */
void hevc_predict_4x4(const uint8_t *recon_plane, int stride, int width, int height,
                      int x0, int y0, int mode, int is_luma, uint8_t pred_out[16]);

/* Largest transform block this module supports, and the log2 range of
 * nTbS: 2 (4x4) through 5 (32x32), which is the whole range HEVC allows. */
#define HEVC_MAX_TB_SIZE  32
#define HEVC_MIN_LOG2_TB   2
#define HEVC_MAX_LOG2_TB   5

/* Forward transform + real HEVC quantization (8.6.3) of an nTbS x nTbS
 * pixel-domain residual, and its exact inverse, for any nTbS in 4..32.
 *
 * `use_dst` selects the alternative DST-VII transform, which HEVC permits
 * for exactly one case (8.6.4.1): 4x4 luma intra. It is ignored - and
 * must be - for every other size, where DCT-II is mandatory.
 *
 * Both the transform shifts and the quantizer scale are size-dependent,
 * which is why these cannot just be the 4x4 routines run over a bigger
 * block: the forward shifts are (log2n + BitDepth - 9, log2n + 6) and the
 * quantizer's bdShift is BitDepth + log2n - 5. Getting either wrong
 * produces a picture that decodes cleanly at the wrong amplitude.
 *
 * Buffers are row-major, nTbS*nTbS entries. */
/* transMatrix entry for an nTbS = (1<<log2n) transform, row i, column j
 * (8.6.4.2). Exposed so the table's structural properties can be asserted
 * in tests rather than only exercised end to end. */
int hevc_transform_matrix(int log2n, int i, int j);

/* The written-out 4x4 matrix the fast path actually uses (row-major, 16
 * entries). Exposed so a test can assert it against the derivation above
 * rather than the two silently diverging. */
const int16_t *hevc_transform_matrix4(int use_dst);

void hevc_transform_quant(const int16_t *residual, int log2_size, int qp, int use_dst,
                           int16_t *coeff_out);
void hevc_dequant_itransform(const int16_t *coeff, int log2_size, int qp, int use_dst,
                              int16_t *residual_out);

/* Forward transform (DST-VII if use_dst, else DCT-II) + real HEVC
 * quantization (8.6.3) of a 4x4 pixel-domain residual (row-major,
 * residual[y*4+x] = source-prediction, may be negative). Writes 16
 * quantized signed coefficient levels, row-major, clipped to int16. */
void hevc_transform_quant_4x4(const int16_t residual[16], int qp, int use_dst,
                               int16_t coeff_out[16]);

/* Real HEVC dequantization (8.6.3) + inverse transform (8.6.4) of 16
 * quantized levels (row-major) back to a pixel-domain residual - the exact
 * same computation a real decoder performs, used here for this encoder's
 * own reconstruction chaining (see hevc_intra.h's top comment, point 2). */
void hevc_dequant_itransform_4x4(const int16_t coeff[16], int qp, int use_dst,
                                  int16_t residual_out[16]);

#ifdef __cplusplus
}
#endif
#endif /* BC250_HEVC_INTRA_H */
