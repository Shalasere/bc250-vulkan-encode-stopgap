/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
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
/* Every divisor here is a power of two (CTU 16 luma / 8 chroma, halved per
 * nesting level), so this is written with shifts and masks rather than /
 * and %: gather_refs() below calls it 17 times for every 4x4 block of
 * every candidate-free prediction, which at 1080p is ~2.2M calls per
 * frame, and integer division is the single most expensive operation it
 * would otherwise contain. Same values, no division. */
static long long zorder_rank(int x, int y, int width, int is_luma) {
    int lg_ctu = is_luma ? 4 : 3;               /* log2 CTU size in this plane */
    int width_ctu = (width + (1 << lg_ctu) - 1) >> lg_ctu;
    int ctu_col = x >> lg_ctu, ctu_row = y >> lg_ctu;
    int rx = x & ((1 << lg_ctu) - 1), ry = y & ((1 << lg_ctu) - 1);
    int lg_half = lg_ctu - 1;                   /* log2 CU size */
    int cu_col = rx >> lg_half, cu_row = ry >> lg_half;
    long long rank = ((long long)ctu_row * width_ctu + ctu_col) * 4 + (cu_row * 2 + cu_col);
    if (is_luma) {
        int rx2 = rx & ((1 << lg_half) - 1), ry2 = ry & ((1 << lg_half) - 1);
        rank = rank * 4 + (((ry2 >> 2) * 2) + (rx2 >> 2)); /* PU size is 4 */
    }
    return rank;
}

static int zorder_available(int nx, int ny, int width, int height, int is_luma, long long cur_rank) {
    if (nx < 0 || ny < 0 || nx >= width || ny >= height) return 0;
    return zorder_rank(nx, ny, width, is_luma) < cur_rank;
}

/* hevc_refs_t is declared in hevc_intra.h - the reference set is part of
 * the public interface now, so callers evaluating several modes for one
 * block can gather once instead of per mode.
 *
 * Gathers all 17 neighbour samples and applies Rec. ITU-T H.265
 * 8.4.4.2.2's substitution scan, which runs from p[-1][2*nTbS-1] up the
 * left edge, through the corner p[-1][-1], and along the top to
 * p[2*nTbS-1][-1], each unavailable entry taking the value of the
 * previous one in that order (and the whole set defaulting to 1<<(bd-1)
 * = 128 when nothing at all is available).
 *
 * Availability is evaluated by z-scan rank (see zorder_rank() above)
 * rather than by mere picture-boundary containment: a neighbour can sit
 * inside the picture and still not be decoded yet.
 *
 * The 17 samples fall into exactly 5 already-coded blocks - below-left
 * p[-1][4..7], left p[-1][0..3], the corner, above p[0..3][-1] and
 * above-right p[4..7][-1] - so 5 rank comparisons decide all 17. That is
 * exact rather than an approximation here, and only because of two
 * alignment facts this encoder guarantees: (x0,y0) is always 4-aligned in
 * its own plane, and the coded picture dimensions are always a multiple of
 * the CTU size, so each 4-sample run lies wholly inside one block AND is
 * wholly in or wholly out of the picture. A run that could straddle either
 * boundary would need the per-sample form. */
void hevc_gather_refs(const uint8_t *plane, int stride, int width, int height,
                       int x0, int y0, int is_luma, hevc_refs_t *r) {
    long long cur_rank = zorder_rank(x0, y0, width, is_luma);

    int av_bl = zorder_available(x0 - 1, y0 + 4, width, height, is_luma, cur_rank);
    int av_l  = zorder_available(x0 - 1, y0,     width, height, is_luma, cur_rank);
    int av_c  = zorder_available(x0 - 1, y0 - 1, width, height, is_luma, cur_rank);
    int av_a  = zorder_available(x0,     y0 - 1, width, height, is_luma, cur_rank);
    int av_ar = zorder_available(x0 + 4, y0 - 1, width, height, is_luma, cur_rank);

    /* Filled in the spec's own substitution order: p[-1][7] up the left
     * edge, the corner, then along the top to p[7][-1]. */
    uint8_t sv[17], sa[17];
    for (int i = 0; i < 8; i++) {
        int y = 7 - i;
        sa[i] = (uint8_t)(y >= 4 ? av_bl : av_l);
        sv[i] = sa[i] ? plane[(size_t)(y0 + y) * stride + (x0 - 1)] : 0;
    }
    sa[8] = (uint8_t)av_c;
    sv[8] = av_c ? plane[(size_t)(y0 - 1) * stride + (x0 - 1)] : 0;
    for (int x = 0; x < 8; x++) {
        sa[9 + x] = (uint8_t)(x >= 4 ? av_ar : av_a);
        sv[9 + x] = sa[9 + x] ? plane[(size_t)(y0 - 1) * stride + (x0 + x)] : 0;
    }

    int first = -1;
    for (int i = 0; i < 17; i++) { if (sa[i]) { first = i; break; } }

    if (first < 0) {
        for (int i = 0; i < 17; i++) sv[i] = 128;
    } else {
        for (int i = 0; i < first; i++) sv[i] = sv[first];
        for (int i = first + 1; i < 17; i++) if (!sa[i]) sv[i] = sv[i - 1];
    }

    for (int y = 0; y < 8; y++) r->left[y] = sv[7 - y];
    r->corner = sv[8];
    for (int x = 0; x < 8; x++) r->top[x] = sv[9 + x];
}

/* ===================== prediction (8.4.4.2.4-8.4.4.2.6) ===================== */

/* Rec. ITU-T H.265 Table 8-5: intraPredAngle by predModeIntra. Entries 0
 * and 1 (Planar/DC) are unused - those are not angular modes. */
static const int8_t intra_pred_angle[35] = {
      0,   0,  32,  26,  21,  17,  13,   9,   5,   2,   0,  -2,
     -5,  -9, -13, -17, -21, -26, -32, -26, -21, -17, -13,  -9,
     -5,  -2,   0,   2,   5,   9,  13,  17,  21,  26,  32
};

/* Rec. ITU-T H.265 Table 8-6: invAngle, defined only for predModeIntra
 * 11..25 (the modes with a negative angle, which are the only ones that
 * project reference samples from the opposite edge). Index is mode-11. */
static const int16_t inv_angle[15] = {
    -4096, -1638, -910, -630, -482, -390, -315, -256, -315, -390, -482,
     -630,  -910, -1638, -4096
};

/* Rec. ITU-T H.265 8.4.4.2.6, specialized to nTbS == 4.
 *
 * Note what is deliberately NOT here: 8.4.4.2.3's reference-sample
 * smoothing filter. That clause sets filterFlag = 0 unconditionally when
 * nTbS is equal to 4, and every transform block in this encoder is 4x4
 * (see encoder_h265.c's picture structure), so the filtered reference
 * array pF[][] never applies. Applying it anyway would put this encoder's
 * reconstruction out of step with every conforming decoder. */
static void predict_angular(const hevc_refs_t *r, int mode, int is_luma, uint8_t out[16]) {
    const int nTbS = 4;
    int angle = intra_pred_angle[mode];

    /* ref[] is indexed from -nTbS to 2*nTbS+1 in the spec's terms; store it
     * offset so index 0 of the array is spec index -nTbS. */
    int ref[4 + 8 + 2];
#define REF(i) ref[(i) + 4]

    /* For the near-vertical modes (>= 18) the reference array runs along
     * the top row and the projection walks it per output row; for the
     * near-horizontal modes the two axes swap roles wholesale. */
    const uint8_t *main_edge = (mode >= 18) ? r->top  : r->left;
    const uint8_t *side_edge = (mode >= 18) ? r->left : r->top;

    REF(0) = r->corner;
    for (int i = 1; i <= nTbS; i++) REF(i) = main_edge[i - 1];

    if (angle < 0) {
        int lim = (nTbS * angle) >> 5;
        if (lim < -1) {
            int inv = inv_angle[mode - 11];
            for (int i = -1; i >= lim; i--) {
                int k = -1 + ((i * inv + 128) >> 8);
                if (k < 0) REF(i) = r->corner;
                else       REF(i) = side_edge[k > 7 ? 7 : k];
            }
        }
    } else {
        for (int i = nTbS + 1; i <= 2 * nTbS; i++) REF(i) = main_edge[i - 1];
    }

    for (int j = 0; j < nTbS; j++) {
        int idx  = ((j + 1) * angle) >> 5;
        int fact = ((j + 1) * angle) & 31;
        for (int i = 0; i < nTbS; i++) {
            int v = fact ? (((32 - fact) * REF(i + idx + 1) + fact * REF(i + idx + 2) + 16) >> 5)
                         : REF(i + idx + 1);
            /* j is y (row) for the vertical family and x (column) for the
             * horizontal one - the whole prediction is transposed between
             * the two branches, which is exactly how the spec writes it. */
            if (mode >= 18) out[j * 4 + i] = (uint8_t)v;
            else            out[i * 4 + j] = (uint8_t)v;
        }
    }
#undef REF

    /* The exactly-vertical and exactly-horizontal modes get one edge
     * column/row gradient-filtered, luma only, for nTbS < 32 (always true
     * here). */
    if (is_luma && mode == 26)
        for (int y = 0; y < 4; y++)
            out[y * 4] = clip8(r->top[0] + ((r->left[y] - r->corner) >> 1));
    else if (is_luma && mode == 10)
        for (int x = 0; x < 4; x++)
            out[x] = clip8(r->left[0] + ((r->top[x] - r->corner) >> 1));
}

void hevc_predict_4x4_refs(const hevc_refs_t *r, int mode, int is_luma, uint8_t pred_out[16]) {
    if (mode == HEVC_MODE_PLANAR) {
        /* 8.4.4.2.5. p[nTbS][-1] is top[4] and p[-1][nTbS] is left[4]. */
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int v = (3 - x) * r->left[y] + (x + 1) * r->top[4] +
                        (3 - y) * r->top[x]  + (y + 1) * r->left[4] + 4;
                pred_out[y * 4 + x] = (uint8_t)(v >> 3);
            }
    } else if (mode == HEVC_MODE_DC) {
        /* 8.4.4.2.4. */
        int dc = (r->left[0] + r->left[1] + r->left[2] + r->left[3] +
                  r->top[0]  + r->top[1]  + r->top[2]  + r->top[3] + 4) >> 3;
        for (int i = 0; i < 16; i++) pred_out[i] = (uint8_t)dc;
        if (is_luma) {
            pred_out[0] = (uint8_t)((r->left[0] + 2 * dc + r->top[0] + 2) >> 2);
            for (int x = 1; x < 4; x++) pred_out[x] = (uint8_t)((r->top[x] + 3 * dc + 2) >> 2);
            for (int y = 1; y < 4; y++) pred_out[y * 4] = (uint8_t)((r->left[y] + 3 * dc + 2) >> 2);
        }
    } else if (mode >= 2 && mode <= 34) {
        predict_angular(r, mode, is_luma, pred_out);
    } else {
        for (int i = 0; i < 16; i++) pred_out[i] = 128;
    }
}

void hevc_predict_4x4(const uint8_t *recon_plane, int stride, int width, int height,
                      int x0, int y0, int mode, int is_luma, uint8_t pred_out[16]) {
    hevc_refs_t refs;
    hevc_gather_refs(recon_plane, stride, width, height, x0, y0, is_luma, &refs);
    hevc_predict_4x4_refs(&refs, mode, is_luma, pred_out);
}

/* ===================== mode decision ===================== */

/* lambda for a SAD-domain cost, in 1/256ths. The usual HEVC lambda is an
 * SSE-domain 0.57 * 2^((qp-12)/3); comparing SADs instead of SSEs means
 * taking its square root, which halves the exponent's slope - so the value
 * doubles every 6 QP rather than every 3, and a 6-entry base table plus a
 * shift covers the whole QP range exactly. base[q] = round(256 *
 * sqrt(0.57 * 2^((q-12)/3))) for q = 0..5. */
static const int lambda_sad_q8_base[6] = { 48, 54, 61, 68, 77, 86 };

int hevc_lambda_sad_q8(int qp) {
    if (qp < 0) qp = 0;
    if (qp > 51) qp = 51;
    return lambda_sad_q8_base[qp % 6] << (qp / 6);
}

/* Bits it costs to signal `mode` given this PU's MPM list:
 * prev_intra_luma_pred_flag plus either mpm_idx (1 or 2 bypass bins) or a
 * 5-bit rem_intra_luma_pred_mode. Matches what
 * hevc_cabac_code_intra_luma_flag()/_data() actually emit. */
int hevc_mode_signal_bits(int mode, const int mpm[3]) {
    if (!mpm) return 0;
    if (mode == mpm[0]) return 2;
    if (mode == mpm[1] || mode == mpm[2]) return 3;
    return 6;
}

static long block_sad(const uint8_t *src, int stride, int x0, int y0, const uint8_t pred[16]) {
    long sad = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int d = src[(size_t)(y0 + y) * stride + (x0 + x)] - pred[y * 4 + x];
            sad += d < 0 ? -d : d;
        }
    return sad;
}

/*
 * Searching all 35 modes exhaustively for every 4x4 block is ~9x the
 * prediction work of the old 4-mode search, on an encoder that is already
 * CPU-bound. This uses the standard coarse-then-refine shape instead: a
 * step-4 sweep across the angular range plus Planar/DC, then a +/-1 and
 * +/-2 refinement around whichever angular mode won, plus the three MPMs
 * (which are nearly free to signal and therefore worth always costing).
 * That is at most 18 candidates and typically fewer after de-duplication,
 * while still landing on or adjacent to the true best angle - the angular
 * cost surface is smooth enough at 4x4 that a step-4 probe does not skip
 * over the minimum.
 *
 * The references are gathered ONCE here and shared across every candidate,
 * rather than re-running the 17-position substitution scan per mode the
 * way the old code's per-candidate hevc_predict_4x4() call did. That alone
 * more than pays for the extra candidates.
 */
/* The parallel half of the split decision - see hevc_intra.h. Scores all
 * 35 modes for one block; no pruning, because the point of this form is
 * that every block is independent and an evaluator that can run them all
 * at once gains nothing by searching fewer modes.
 *
 * A 4x4 SAD maxes out at 16*255 = 4080, so uint16 is exact here, not a
 * saturating approximation. */
void hevc_block_mode_costs(const uint8_t *ref_plane, const uint8_t *src_y, int stride,
                            int width, int height, int x0, int y0,
                            uint16_t costs_out[HEVC_MODE_COUNT]) {
    hevc_refs_t refs;
    hevc_gather_refs(ref_plane, stride, width, height, x0, y0, 1, &refs);
    for (int m = 0; m < HEVC_MODE_COUNT; m++) {
        uint8_t pred[16];
        hevc_predict_4x4_refs(&refs, m, 1, pred);
        costs_out[m] = (uint16_t)block_sad(src_y, stride, x0, y0, pred);
    }
}

/* The serial half: apply the rate term, which needs this block's real MPM
 * list and therefore its z-scan predecessors' committed modes.
 *
 * This deliberately searches the SAME coarse-then-refine candidate subset
 * hevc_choose_luma_mode() does, even though every mode's distortion is
 * already sitting in costs[] and an exhaustive argmin would be free here.
 * Measured: exhaustive costs +49.4% BD-rate. Not because it picks worse
 * modes per block - it picks better ones - but because it picks more
 * VARIED ones, and two things in this encoder reward agreement between
 * neighbours much more than they reward a slightly lower SAD: a mode
 * matching the MPM list costs 2 bits instead of 6, and a CU whose four
 * blocks all want the same mode collapses from PART_NxN to PART_2Nx2N
 * and signals one mode instead of four. Pruning to the coarse grid keeps
 * neighbouring blocks landing on the same few modes. The effect is
 * strongly content-dependent - it cost +59% on a flat desktop frame and
 * only +4.6% on dense detail, exactly where agreement was plentiful
 * versus already rare. */
int hevc_pick_mode_from_costs(const uint16_t costs[HEVC_MODE_COUNT],
                               const int mpm[3], int qp) {
    long lambda = mpm ? hevc_lambda_sad_q8(qp) : 0;
    uint8_t tried[HEVC_MODE_COUNT];
    memset(tried, 0, sizeof(tried));

    int best_mode = HEVC_MODE_DC;
    long best_cost = -1;
    int best_angular = -1;
    long best_angular_cost = -1;

    int cands[18];
    int n = 0;
    cands[n++] = HEVC_MODE_PLANAR;
    cands[n++] = HEVC_MODE_DC;
    for (int m = 2; m <= 34; m += 4) cands[n++] = m;
    if (mpm) for (int i = 0; i < 3; i++) cands[n++] = mpm[i];

    for (int pass = 0; pass < 2; pass++) {
        for (int c = 0; c < n; c++) {
            int m = cands[c];
            if (m < 0 || m >= HEVC_MODE_COUNT || tried[m]) continue;
            tried[m] = 1;
            long cost = (long)costs[m] + ((lambda * hevc_mode_signal_bits(m, mpm)) >> 8);
            if (best_cost < 0 || cost < best_cost) { best_cost = cost; best_mode = m; }
            if (m >= 2 && (best_angular_cost < 0 || cost < best_angular_cost)) {
                best_angular_cost = cost; best_angular = m;
            }
        }
        if (pass == 1 || best_angular < 0) break;
        n = 0;
        for (int d = -2; d <= 2; d++) {
            if (!d) continue;
            int m = best_angular + d;
            if (m >= 2 && m <= 34) cands[n++] = m;
        }
    }
    return best_mode;
}

void hevc_rank_modes_by_cost(const uint16_t costs[HEVC_MODE_COUNT], int n, uint8_t *out) {
    if (n > HEVC_MODE_COUNT) n = HEVC_MODE_COUNT;
    if (n < 1) n = 1;
    uint8_t taken[HEVC_MODE_COUNT];
    memset(taken, 0, sizeof(taken));
    for (int k = 0; k < n; k++) {
        int best = -1;
        for (int m = 0; m < HEVC_MODE_COUNT; m++)
            if (!taken[m] && (best < 0 || costs[m] < costs[best])) best = m;
        taken[best] = 1;
        out[k] = (uint8_t)best;
    }
}

int hevc_choose_among(const hevc_refs_t *refs, const uint8_t *src_y, int stride,
                       int x0, int y0, const int mpm[3], int qp,
                       const int *cands, int ncands) {
    long lambda = mpm ? hevc_lambda_sad_q8(qp) : 0;
    int best_mode = HEVC_MODE_DC;
    long best_cost = -1;
    /* Callers concatenate a shortlist with the MPM list, which overlap
     * often; a duplicate here costs a whole redundant prediction. */
    uint64_t seen_lo = 0, seen_hi = 0;
    for (int c = 0; c < ncands; c++) {
        int m = cands[c];
        if (m < 0 || m >= HEVC_MODE_COUNT) continue;
        uint64_t bit = 1ull << (m & 63);
        uint64_t *seen = (m < 64) ? &seen_lo : &seen_hi;
        if (*seen & bit) continue;
        *seen |= bit;
        uint8_t pred[16];
        hevc_predict_4x4_refs(refs, m, 1, pred);
        long cost = block_sad(src_y, stride, x0, y0, pred) +
                    ((lambda * hevc_mode_signal_bits(m, mpm)) >> 8);
        if (best_cost < 0 || cost < best_cost) { best_cost = cost; best_mode = m; }
    }
    return best_mode;
}

int hevc_choose_luma_mode(const uint8_t *src_y, const uint8_t *recon_y, int stride,
                           int width, int height, int x0, int y0,
                           const int mpm[3], int qp) {
    hevc_refs_t refs;
    hevc_gather_refs(recon_y, stride, width, height, x0, y0, 1, &refs);
    return hevc_choose_luma_mode_refs(&refs, src_y, stride, x0, y0, mpm, qp);
}

int hevc_choose_luma_mode_refs(const hevc_refs_t *refs, const uint8_t *src_y, int stride,
                                int x0, int y0, const int mpm[3], int qp) {
    int lambda = mpm ? hevc_lambda_sad_q8(qp) : 0;
    uint8_t tried[HEVC_MODE_COUNT];
    memset(tried, 0, sizeof(tried));

    int best_mode = HEVC_MODE_DC;
    long best_cost = -1;
    int best_angular = -1;
    long best_angular_cost = -1;

    int cands[18];
    int n = 0;
    cands[n++] = HEVC_MODE_PLANAR;
    cands[n++] = HEVC_MODE_DC;
    for (int m = 2; m <= 34; m += 4) cands[n++] = m;
    if (mpm) for (int i = 0; i < 3; i++) cands[n++] = mpm[i];

    for (int pass = 0; pass < 2; pass++) {
        for (int c = 0; c < n; c++) {
            int m = cands[c];
            if (m < 0 || m >= HEVC_MODE_COUNT || tried[m]) continue;
            tried[m] = 1;

            uint8_t pred[16];
            hevc_predict_4x4_refs(refs, m, 1, pred);
            long cost = block_sad(src_y, stride, x0, y0, pred) +
                        (((long)lambda * hevc_mode_signal_bits(m, mpm)) >> 8);

            if (best_cost < 0 || cost < best_cost) { best_cost = cost; best_mode = m; }
            if (m >= 2 && (best_angular_cost < 0 || cost < best_angular_cost)) {
                best_angular_cost = cost; best_angular = m;
            }
        }
        if (pass == 1 || best_angular < 0) break;

        /* Refinement pass around the best coarse angle. */
        n = 0;
        for (int d = -2; d <= 2; d++) {
            if (!d) continue;
            int m = best_angular + d;
            if (m >= 2 && m <= 34) cands[n++] = m;
        }
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

static int32_t dequant_level(int32_t level, int qp) {
    int per = qp / 6, rem = qp % 6;
    int64_t val = (int64_t)level * HEVC_FLAT_M * levelScale[rem];
    val <<= per;
    val = (val + (1 << (HEVC_BDSHIFT - 1))) >> HEVC_BDSHIFT;
    return clip_coeff((int32_t)val);
}

static int32_t quantize_coeff(int32_t coeff_raw, int qp) {
    int per = qp / 6, rem = qp % 6;
    int64_t denom = (int64_t)HEVC_FLAT_M * levelScale[rem] << per;
    int sign = coeff_raw < 0 ? -1 : 1;
    int64_t mag = coeff_raw < 0 ? -(int64_t)coeff_raw : (int64_t)coeff_raw;
    int64_t num = mag << HEVC_BDSHIFT;
    int64_t level = (num + denom / 2) / denom;
    return (int32_t)(sign * level);
}

void hevc_transform_quant_4x4(const int16_t residual[16], int qp, int use_dst,
                               int16_t coeff_out[16]) {
    int32_t raw[16];
    forward_transform_4x4(residual, use_dst ? DST4 : DCT4, raw);
    for (int i = 0; i < 16; i++) {
        int32_t level = quantize_coeff(raw[i], qp);
        if (level > 32767) level = 32767;
        if (level < -32768) level = -32768;
        coeff_out[i] = (int16_t)level;
    }
}

void hevc_dequant_itransform_4x4(const int16_t coeff[16], int qp, int use_dst,
                                  int16_t residual_out[16]) {
    int16_t dq[16];
    for (int i = 0; i < 16; i++) dq[i] = (int16_t)dequant_level(coeff[i], qp);
    inverse_transform_4x4(dq, use_dst ? DST4 : DCT4, residual_out);
}
