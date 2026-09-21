/* bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_intra.c - see hevc_intra.h for the design rationale. Everything in
 * this file is independently written against the published ITU-T H.265
 * spec text (clauses cited per-function below) - unlike hevc_cabac.c, none
 * of it is adapted from x265's source, though the well-known public 4x4
 * DCT-II integer matrix ({64,64,64,64},{83,36,-36,-83},...) and the
 * standard HEVC dequant scale table ({40,45,51,57,64,72}) were
 * cross-checked against x265's source/common/constants.cpp and
 * source/common/scalinglist.cpp purely to catch transcription mistakes -
 * both are the spec's own normative constants, not x265-original values.
 */
#include "hevc_intra.h"
#include <string.h>
#include <stdlib.h>

/* ===================== mode/scan helpers ===================== */

int hevc_scan_idx_for_mode(int mode) {
    if (mode >= 6 && mode <= 14) return 2;  /* SCAN_VER */
    if (mode >= 22 && mode <= 30) return 1; /* SCAN_HOR */
    return 0;                               /* SCAN_DIAG */
}

/* Rec. ITU-T H.265 8.4.2: build the 3-entry candidate mode list from the
 * left/above neighbor PUs' real intra modes. Unavailable neighbors
 * (off-picture, or - not applicable here, one slice per picture - a
 * different slice) are treated as INTRA_DC per the spec's substitution. */
void hevc_derive_mpm(int left_mode, int left_avail, int above_mode, int above_avail,
                      int mpm_out[3]) {
    int cand_a = left_avail ? left_mode : HEVC_MODE_DC;
    int cand_b = above_avail ? above_mode : HEVC_MODE_DC;

    if (cand_a == cand_b) {
        if (cand_a < 2) {
            mpm_out[0] = HEVC_MODE_PLANAR;
            mpm_out[1] = HEVC_MODE_DC;
            mpm_out[2] = HEVC_MODE_VERTICAL;
        } else {
            mpm_out[0] = cand_a;
            mpm_out[1] = 2 + ((cand_a + 29) % 32);
            mpm_out[2] = 2 + ((cand_a - 2 + 1) % 32);
        }
    } else {
        mpm_out[0] = cand_a;
        mpm_out[1] = cand_b;
        if (mpm_out[0] != HEVC_MODE_PLANAR && mpm_out[1] != HEVC_MODE_PLANAR)
            mpm_out[2] = HEVC_MODE_PLANAR;
        else if (mpm_out[0] != HEVC_MODE_DC && mpm_out[1] != HEVC_MODE_DC)
            mpm_out[2] = HEVC_MODE_DC;
        else
            mpm_out[2] = HEVC_MODE_VERTICAL;
    }
}

/* ===================== neighbor gathering (8.4.4.2.2) ===================== */

static inline uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/*
 * Rec. ITU-T H.265 6.4.1's "z-scan order block availability" is NOT the
 * same thing as "is this position within picture bounds" - a neighbor can
 * be positionally inside the picture and still not yet decoded. This bit
 * this encoder got wrong initially: a CU's bottom-right (z-order index 3)
 * PU's "top-right" reference sample can land inside a SIBLING CU that
 * hasn't been coded yet (e.g. the top-left CU's bottom-right PU reaching
 * into the top-right CU of the same CTU), or inside the next CTU to the
 * right (same picture row, but that CTU is later in raster order) - both
 * pass a naive "x0+4 < width && y0 > 0" check while being genuinely
 * undecoded. Confirmed as the actual remaining bug by an exhaustive
 * bit-for-bit cross-check of this encoder's CABAC output against its own
 * recorded per-CU decisions (which matched perfectly - the bitstream was
 * never wrong), followed by comparing this project's intra prediction/
 * transform code line-by-line against ffmpeg's libavcodec/hevc/
 * pred_template.c and dsp_template.c (both matched exactly) - leaving
 * z-scan availability, which ffmpeg's intra_pred() computes via a real
 * MinTbAddrZs table lookup (cand_up_right &&
 * cur_tb_addr > MIN_TB_ADDR_ZS(...)), as the one remaining candidate, and
 * it was.
 *
 * This encoder's coding order is a FIXED, known shape (CTUs in raster
 * order; each CTU always splits into exactly 4 CUs in z-order - TL,TR,
 * BL,BR; each luma CU always splits into exactly 4 PUs the same way;
 * chroma has no PU sub-split, one block per CU) - which makes an exact
 * z-scan rank computable directly, without needing a real MinTbAddrZs
 * table: rank = ((ctuRow*widthInCtus + ctuCol) * 4 + cuZIndex) * 4 +
 * puZIndex (chroma stops one level early, at the CU). A neighbor is
 * available iff it's in-picture AND its rank is strictly less than the
 * current block's.
 */
/* PERF: both CTU sizes are powers of two (16 luma, 8 chroma), but writing
 * this with `ctu_size` as a runtime variable meant the compiler could not
 * strength-reduce any of the four divisions and two modulos - it has to
 * emit real integer division, because it cannot prove the divisor is 16 or
 * 8. Profiling the 1080p CPU encode put this one function at 21% of total
 * runtime over 25.7 MILLION calls, more than the inverse transform, the
 * prediction and the CABAC bin coder individually, for what is only
 * availability bookkeeping.
 *
 * Splitting the two cases makes every shift and mask a compile-time
 * constant. The arithmetic is unchanged and the output is byte-identical;
 * this is purely the same formula the compiler can now see through.
 * Ranks also fit comfortably in int32 (8K luma tops out near 2.1M), so the
 * 64-bit multiply goes too. */
static inline int zorder_rank(int x, int y, int width, int is_luma) {
    if (is_luma) {
        /* CTU 16 -> CU 8 -> PU 4 */
        int width_ctu = (width + 15) >> 4;
        int rank = ((y >> 4) * width_ctu + (x >> 4)) * 4
                 + (((y >> 3) & 1) * 2 + ((x >> 3) & 1));
        return rank * 4 + (((y >> 2) & 1) * 2 + ((x >> 2) & 1));
    }
    /* chroma: CTU 8 -> CU 4, no PU sub-split */
    int width_ctu = (width + 7) >> 3;
    return ((y >> 3) * width_ctu + (x >> 3)) * 4
         + (((y >> 2) & 1) * 2 + ((x >> 2) & 1));
}

static inline int zorder_available(int nx, int ny, int width, int height, int is_luma, int cur_rank) {
    if (nx < 0 || ny < 0 || nx >= width || ny >= height) return 0;
    return zorder_rank(nx, ny, width, is_luma) < cur_rank;
}

/* Gathers left[0..4] (p[-1][0..4]), top[0..4] (p[0..4][-1]) and the corner
 * (p[-1][-1]), applying the spec's neighbor-substitution scan. Positions
 * past p[-1][4] and p[4][-1] are never referenced - this encoder emits
 * only Planar, DC, Horizontal and Vertical, and none of them reads
 * further - so the scan below covers exactly what those four modes need.
 *
 * NOTE: an earlier version of this comment claimed "below and below-left
 * are always z-scan-unavailable in this encoder's coding order" and the
 * code hardcoded left[4] = left[3] on the strength of it. That was wrong
 * and it was a real decoder-visible bug; p[-1][4] gets a genuine
 * availability test below, and the reasoning is in the comment on it.
 * Every one of left/top/corner/top-right/below-left needs a rank check,
 * because each can be positionally plausible while genuinely undecoded
 * (see zorder_rank()'s comment above). */
static void gather_neighbors(const uint8_t *plane, int stride, int width, int height,
                              int x0, int y0, int is_luma, uint8_t left[5], uint8_t top[5], uint8_t *corner) {
    int cur_rank = zorder_rank(x0, y0, width, is_luma);
    int avail_left = zorder_available(x0 - 1, y0, width, height, is_luma, cur_rank);
    int avail_top = zorder_available(x0, y0 - 1, width, height, is_luma, cur_rank);
    int avail_corner = zorder_available(x0 - 1, y0 - 1, width, height, is_luma, cur_rank);
    int avail_top_right = zorder_available(x0 + 4, y0 - 1, width, height, is_luma, cur_rank);

    /* p[-1][4], the below-left sample. Planar reads it (as p[-1][nTbS]),
     * and it is NOT always unavailable - an earlier version of this
     * function asserted that it was and hardcoded left[4] = left[3].
     *
     * Counter-example, which is what the off-board 16x16 reproduction
     * narrowed to: for the first 4x4 of the CU at (8,8) in a CTU, p[-1][4]
     * is the sample at (7,12), which lies in the CU at (0,8). CU order
     * within a CTU is (0,0), (8,0), (0,8), (8,8), so that CU is already
     * fully reconstructed and the sample IS available. A decoder uses it;
     * this encoder was substituting left[3] instead, and the two
     * reconstructions diverged by +-1 and then propagated.
     *
     * Only p[-1][4] is added rather than the full p[-1][4..7] the spec's
     * substitution scan starts from: this encoder emits Planar, DC,
     * Horizontal and Vertical only, and none of them reads past
     * p[-1][nTbS]. When p[-1][4] is unavailable the scan below still
     * reproduces the spec's answer for it, because p[-1][5..7] would each
     * copy from the previous entry and end at the same source. */
    int avail_below_left = zorder_available(x0 - 1, y0 + 4, width, height, is_luma, cur_rank);

    uint8_t sv[11];
    uint8_t sa[11];

    /* Scan order is the spec's: bottom-left upward, then the corner, then
     * left-to-right along the top. sv[0] is p[-1][4]. */
    sa[0] = (uint8_t)avail_below_left;
    if (avail_below_left) sv[0] = plane[(y0 + 4) * stride + (x0 - 1)];

    sa[1] = sa[2] = sa[3] = sa[4] = (uint8_t)avail_left;
    if (avail_left) {
        sv[1] = plane[(y0 + 3) * stride + (x0 - 1)];
        sv[2] = plane[(y0 + 2) * stride + (x0 - 1)];
        sv[3] = plane[(y0 + 1) * stride + (x0 - 1)];
        sv[4] = plane[(y0 + 0) * stride + (x0 - 1)];
    }
    sa[5] = (uint8_t)avail_corner;
    if (avail_corner) sv[5] = plane[(y0 - 1) * stride + (x0 - 1)];

    sa[6] = sa[7] = sa[8] = sa[9] = (uint8_t)avail_top;
    if (avail_top) {
        sv[6] = plane[(y0 - 1) * stride + (x0 + 0)];
        sv[7] = plane[(y0 - 1) * stride + (x0 + 1)];
        sv[8] = plane[(y0 - 1) * stride + (x0 + 2)];
        sv[9] = plane[(y0 - 1) * stride + (x0 + 3)];
    }
    sa[10] = (uint8_t)avail_top_right;
    if (avail_top_right) sv[10] = plane[(y0 - 1) * stride + (x0 + 4)];

    int first = -1;
    for (int i = 0; i < 11; i++) { if (sa[i]) { first = i; break; } }

    if (first < 0) {
        for (int i = 0; i < 11; i++) sv[i] = 128;
    } else {
        for (int i = 0; i < first; i++) sv[i] = sv[first];
        for (int i = first + 1; i < 11; i++) if (!sa[i]) sv[i] = sv[i - 1];
    }

    left[4] = sv[0];
    left[3] = sv[1]; left[2] = sv[2]; left[1] = sv[3]; left[0] = sv[4];
    *corner = sv[5];
    top[0] = sv[6]; top[1] = sv[7]; top[2] = sv[8]; top[3] = sv[9];
    top[4] = sv[10];
}

/* ===================== prediction (8.4.4.2.5-8.4.4.2.7) ===================== */

/* PERF: prediction split into "gather the references" and "apply a mode to
 * already-gathered references". The reference set for a block does not
 * depend on which mode is being tried, but hevc_choose_luma_mode() tries
 * four candidates and the old single-entry-point shape re-gathered for
 * every one of them, then a fifth time for the chosen mode - five
 * identical gathers per 4x4 block, each running five z-scan availability
 * tests. Profiling 1080p put gather_neighbors()'s zorder_rank alone at 21%
 * of total runtime across 25.7M calls.
 *
 * The GPU shader already had this shape ("build both reference sets once",
 * hevc_intra_wavefront.comp); this brings the CPU path in line. Output is
 * unchanged - it is the same gather feeding the same mode arithmetic,
 * just not repeated. */
static void predict_from_refs(const uint8_t left[5], const uint8_t top[5], uint8_t corner,
                              int mode, int is_luma, uint8_t pred_out[16]);

void hevc_predict_4x4(const uint8_t *recon_plane, int stride, int width, int height,
                      int x0, int y0, int mode, int is_luma, uint8_t pred_out[16]) {
    uint8_t left[5], top[5], corner;
    gather_neighbors(recon_plane, stride, width, height, x0, y0, is_luma, left, top, &corner);
    predict_from_refs(left, top, corner, mode, is_luma, pred_out);
}

/* is_luma is still needed here, not just for the gather: DC and the
 * horizontal/vertical modes apply their edge filtering only for cIdx == 0
 * (8.4.4.2.5-8.4.4.2.6). */
static void predict_from_refs(const uint8_t left[5], const uint8_t top[5], uint8_t corner,
                              int mode, int is_luma, uint8_t pred_out[16]) {
    switch (mode) {
    case HEVC_MODE_PLANAR:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int v = (3 - x) * left[y] + (x + 1) * top[4] +
                        (3 - y) * top[x] + (y + 1) * left[4] + 4;
                pred_out[y * 4 + x] = (uint8_t)(v >> 3);
            }
        break;

    case HEVC_MODE_DC: {
        int dc = (left[0] + left[1] + left[2] + left[3] + top[0] + top[1] + top[2] + top[3] + 4) >> 3;
        for (int i = 0; i < 16; i++) pred_out[i] = (uint8_t)dc;
        if (is_luma) {
            pred_out[0] = (uint8_t)((left[0] + 2 * dc + top[0] + 2) >> 2);
            for (int x = 1; x < 4; x++) pred_out[x] = (uint8_t)((top[x] + 3 * dc + 2) >> 2);
            for (int y = 1; y < 4; y++) pred_out[y * 4] = (uint8_t)((left[y] + 3 * dc + 2) >> 2);
        }
        break;
    }

    case HEVC_MODE_HORIZONTAL:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                pred_out[y * 4 + x] = left[y];
        if (is_luma) {
            for (int x = 0; x < 4; x++)
                pred_out[x] = clip8(left[0] + ((top[x] - corner) >> 1));
        }
        break;

    case HEVC_MODE_VERTICAL:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                pred_out[y * 4 + x] = top[x];
        if (is_luma) {
            for (int y = 0; y < 4; y++)
                pred_out[y * 4] = clip8(top[0] + ((left[y] - corner) >> 1));
        }
        break;

    default:
        for (int i = 0; i < 16; i++) pred_out[i] = 128;
        break;
    }
}

int hevc_choose_luma_mode(const uint8_t *src_y, const uint8_t *recon_y, int stride,
                           int width, int height, int x0, int y0) {
    static const int candidates[4] = { HEVC_MODE_PLANAR, HEVC_MODE_DC, HEVC_MODE_HORIZONTAL, HEVC_MODE_VERTICAL };
    int best_mode = HEVC_MODE_DC;
    long best_sad = -1;

    /* Gather ONCE for all four candidates - the reference set does not
     * depend on the mode. See predict_from_refs()'s comment. */
    uint8_t left[5], top[5], corner;
    gather_neighbors(recon_y, stride, width, height, x0, y0, 1, left, top, &corner);

    for (int c = 0; c < 4; c++) {
        uint8_t pred[16];
        predict_from_refs(left, top, corner, candidates[c], 1 /* luma */, pred);
        long sad = 0;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int src = src_y[(y0 + y) * stride + (x0 + x)];
                int p = pred[y * 4 + x];
                int d = src - p;
                sad += d < 0 ? -d : d;
            }
        if (best_sad < 0 || sad < best_sad) { best_sad = sad; best_mode = candidates[c]; }
    }
    return best_mode;
}

/* ===================== transform (8.6.4) ===================== */

/* Public/standard 4x4 integer DCT-II matrix (ITU-T H.265 8.6.4.1's
 * transMatrix for nTbS=4; identical to H.264's and every other MPEG-family
 * codec's 4-point integer DCT approximation). Cross-checked against
 * x265's source/common/constants.cpp g_t4[][] - same standard values. */
static const int16_t DCT4[4][4] = {
    { 64,  64,  64,  64 },
    { 83,  36, -36, -83 },
    { 64, -64, -64,  64 },
    { 36, -83,  83, -36 }
};

/* Public/standard 4x4 DST-VII "alternative transform" matrix, used ONLY
 * for 4x4 luma intra residuals (ITU-T H.265 8.6.4.1: "if cIdx is equal to
 * 0 and predMode is equal to MODE_INTRA and nTbS is equal to 4, the
 * alternative transform... is used"). Cross-checked against x265's
 * primitives.dst4x4 call site (quant.cpp) for when it fires - the matrix
 * values themselves are the spec's own public constant, reproduced in
 * essentially every independent HEVC implementation (HM, libde265, ffmpeg,
 * etc), not x265-original. */
static const int16_t DST4[4][4] = {
    { 29,  55,  74,  84 },
    { 74,  74,   0, -74 },
    { 84, -29, -74,  55 },
    { 55, -84,  74, -29 }
};

static inline int32_t clip_coeff(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

/* Forward 2D separable transform, matrix M applied directly (not
 * transposed) along both axes. Shift split (1, 8) - chosen so that, paired
 * with the spec-mandated inverse shifts below (7, 12 for 8-bit), a
 * forward-then-inverse round trip with no quantization in between
 * reproduces the original residual exactly for a constant (DC-only) input:
 * forward stage1 shift=1 add=1, stage2 shift=8 add=128 -> for a constant
 * input v, coeff[0][0] = 128*v (verified by hand: row0 of M is {64,64,64,64}
 * so a per-axis DC gain of 4*64=256=2^8; after forward's own /2^1 then
 * /2^8 net divide of 2^9, combined per-axis, the DC coefficient comes out
 * to 128*v); the inverse below then recovers exactly v from that (see its
 * own comment). Non-DC content is NOT expected to be bit-exact through
 * this round trip (that's inherent to any integer DCT/DST approximation,
 * including the real x265/HM ones - see this file's header comment), only
 * well-scaled - forward quantization error is what's supposed to make the
 * picture lossy, not a transform bug. */
static void forward_transform_4x4(const int16_t residual[16], const int16_t M[4][4], int32_t out[16]) {
    int32_t tmp[4][4];
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i < 4; i++) {
            int32_t sum = 0;
            for (int r = 0; r < 4; r++) sum += (int32_t)M[i][r] * residual[r * 4 + c];
            tmp[i][c] = (sum + 1) >> 1;
        }
    }
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int32_t sum = 0;
            for (int c = 0; c < 4; c++) sum += (int32_t)M[j][c] * tmp[i][c];
            out[i * 4 + j] = (sum + 128) >> 8;
        }
    }
}

/* Inverse 2D separable transform, matrix M applied in TRANSPOSED form
 * (M[k][idx], k summed) along both axes, with the spec-mandated shifts for
 * 8-bit content (ITU-T H.265 8.6.4.2): stage1 shift=7/add=64 (fixed,
 * independent of bit depth), stage2 shift = 20-BitDepth = 12/add=2048.
 * This exact process is what a real HEVC decoder performs, and this
 * encoder uses the SAME code for its own reconstruction chaining, so the
 * two are trivially identical by construction. */
static void inverse_transform_4x4(const int16_t coeff[16], const int16_t M[4][4], int16_t out[16]) {
    int32_t tmp[4][4];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            int32_t sum = 0;
            for (int k = 0; k < 4; k++) sum += (int32_t)M[k][r] * coeff[k * 4 + c];
            tmp[r][c] = clip_coeff((sum + 64) >> 7);
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int32_t sum = 0;
            for (int k = 0; k < 4; k++) sum += (int32_t)M[k][c] * tmp[r][k];
            out[r * 4 + c] = (int16_t)clip_coeff((sum + 2048) >> 12);
        }
    }
}

/* ===================== quantization (8.6.3) ===================== */

/* levelScale[qp%6] - Rec. ITU-T H.265 Table (8.6.3), the standard HEVC
 * dequant scale, identical to x265's ScalingList::s_invQuantScales. Public
 * spec constant. */
static const int levelScale[6] = { 40, 45, 51, 57, 64, 72 };

/* bdShift = BitDepth(8) + Log2(nTbS=4) - 5 = 5, m = 16 (flat default
 * scaling list, since this encoder's SPS sets scaling_list_enabled_flag=0
 * - ITU-T H.265 7.4.3.2.1's default). Forward quantization is defined here
 * as the algebraic inverse of the (normative) dequant formula below, which
 * guarantees the two are exactly self-consistent regardless of the
 * transform's own internal scale - see hevc_intra.h's top comment. */
#define HEVC_BDSHIFT 5
#define HEVC_FLAT_M  16

int hevc_chroma_qp_from_luma(int qp_luma) {
    /* qPiCb = Clip3(-QpBdOffsetC, 57, QpY + pps_cb_qp_offset +
     * slice_cb_qp_offset). Both PPS chroma offsets are written as 0 and
     * pps_slice_chroma_qp_offsets_present_flag is 0 (see write_pps()), and
     * QpBdOffsetC is 0 at 8-bit, so qPi is just QpY clamped - and Cb and Cr
     * therefore share one value. */
    static const int qpc_30_43[14] = { 29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37 };
    int qpi = qp_luma < 0 ? 0 : (qp_luma > 57 ? 57 : qp_luma);
    if (qpi < 30) return qpi;
    if (qpi > 43) return qpi - 6;
    return qpc_30_43[qpi - 30];
}

void hevc_transform_quant_4x4(const int16_t residual[16], int qp, int use_dst,
                               int16_t coeff_out[16]) {
    int32_t raw[16];
    forward_transform_4x4(residual, use_dst ? DST4 : DCT4, raw);
    int per = qp / 6, rem = qp % 6;
    int64_t denom = (int64_t)HEVC_FLAT_M * levelScale[rem] << per;
    int64_t half_denom = denom / 2;
    for (int i = 0; i < 16; i++) {
        int32_t coeff_raw = raw[i];
        int sign = coeff_raw < 0 ? -1 : 1;
        int64_t mag = coeff_raw < 0 ? -(int64_t)coeff_raw : (int64_t)coeff_raw;
        int64_t num = mag << HEVC_BDSHIFT;
        int64_t level = (num + half_denom) / denom;
        int32_t res = (int32_t)(sign * level);
        if (res > 32767) res = 32767;
        if (res < -32768) res = -32768;
        coeff_out[i] = (int16_t)res;
    }
}

void hevc_dequant_itransform_4x4(const int16_t coeff[16], int qp, int use_dst,
                                  int16_t residual_out[16]) {
    int16_t dq[16];
    int per = qp / 6, rem = qp % 6;
    int64_t scale = ((int64_t)HEVC_FLAT_M * levelScale[rem]) << per;
    int64_t half_scale = 1 << (HEVC_BDSHIFT - 1);
    for (int i = 0; i < 16; i++) {
        int64_t val = (int64_t)coeff[i] * scale;
        val = (val + half_scale) >> HEVC_BDSHIFT;
        dq[i] = (int16_t)clip_coeff((int32_t)val);
    }
    inverse_transform_4x4(dq, use_dst ? DST4 : DCT4, residual_out);
}
