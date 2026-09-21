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

/* ===================== off-board profiling hook ===================== */
/*
 * Compiled in ONLY for tools/hevc_bench.c's `hevc_bench_prof` target
 * (-DHEVC_INTRA_PROFILE). In the shipped build both macros below expand to
 * nothing at all, so src/hevc_intra.c compiles to byte-identical machine
 * code with this block present - the same property tools/cavlc_bench.c's
 * CAVLC_PROFILE hooks carry, and verified the same way (disassembly diff).
 *
 * The ablation is a RECURSIVE self-call with the flag cleared, rather than
 * a restructure into an inner function plus a wrapper, precisely so the
 * shipped code path is not reshaped for the benefit of the profiler. The
 * second call's answer is asserted equal and thrown away, so the bitstream
 * is bit-identical either way and the A/B difference is the marginal cost
 * of exactly one mode search. Single-threaded harness only.
 */
#ifdef HEVC_INTRA_PROFILE
#include <x86intrin.h>
int hevc_intra_prof_dup_mode_search = 0;
int hevc_intra_prof_timing = 0;
unsigned long long hevc_intra_prof_mode_cycles = 0;
unsigned long long hevc_intra_prof_mode_calls = 0;
unsigned long long hevc_intra_prof_rdtsc(void) { return __rdtsc(); }
#define HEVC_PROF_ENTER()                                                   \
    unsigned long long prof_t0_ = hevc_intra_prof_timing ? __rdtsc() : 0ull
#define HEVC_PROF_LEAVE(result, ...) do {                                   \
    if (hevc_intra_prof_timing)                                             \
        hevc_intra_prof_mode_cycles += __rdtsc() - prof_t0_;                \
    hevc_intra_prof_mode_calls++;                                           \
    if (hevc_intra_prof_dup_mode_search) {                                  \
        hevc_intra_prof_dup_mode_search = 0;                                \
        uint8_t prof_p2_[16];                                               \
        int prof_m2_ = hevc_choose_luma_mode(__VA_ARGS__, prof_p2_);        \
        hevc_intra_prof_dup_mode_search = 1;                                \
        if (prof_m2_ != (result)) abort();                                  \
    }                                                                       \
} while (0)
#else
#define HEVC_PROF_ENTER()          ((void)0)
#define HEVC_PROF_LEAVE(result, ...) ((void)0)
#endif

/* ===================== mode/scan helpers ===================== */

/* Rec. ITU-T H.265 Table 8-... scan derivation, general over the whole
 * 0-34 mode range. This predates backlog A6 (the port that added modes
 * 2-9/11-25/27-34 below) and needed NO change for it - it was already
 * written against the spec's real predModeIntra ranges, not against the
 * four modes this encoder happened to reach at the time. */
int hevc_scan_idx_for_mode(int mode) {
    if (mode >= 6 && mode <= 14) return 2;  /* SCAN_VER */
    if (mode >= 22 && mode <= 30) return 1; /* SCAN_HOR */
    return 0;                               /* SCAN_DIAG */
}

/* Rec. ITU-T H.265 8.4.2: build the 3-entry candidate mode list from the
 * left/above neighbor PUs' real intra modes. Unavailable neighbors
 * (off-picture, or - not applicable here, one slice per picture - a
 * different slice) are treated as INTRA_DC per the spec's substitution.
 *
 * Also already general over the full mode range - see the comment on
 * hevc_scan_idx_for_mode() above, same reasoning. */
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
 * (p[-1][-1]), applying the spec's neighbor-substitution scan. This is
 * kept ONLY as the historical record of the below-left fix referenced
 * elsewhere in this file (docs/hevc_scope_note.md); backlog A6 (below)
 * replaced every live caller with gather_neighbors_wide(), which needs
 * the full 2*nTbS extent for angular modes - see its comment for why this
 * narrow gather could not simply be extended in place. This function is
 * DEAD CODE, compiled out by -Wunused-function's absence of a call site;
 * left in place for the history rather than deleted so the below-left
 * bug's fix is still visible next to the comment that explains it.
 *
 * NOTE: an earlier version of this comment claimed "below and below-left
 * are always z-scan-unavailable in this encoder's coding order" and the
 * code hardcoded left[4] = left[3] on the strength of it. That was wrong
 * and it was a real decoder-visible bug; p[-1][4] gets a genuine
 * availability test below, and the reasoning is in the comment on it.
 * Every one of left/top/corner/top-right/below-left needs a rank check,
 * because each can be positionally plausible while genuinely undecoded
 * (see zorder_rank()'s comment above). */
__attribute__((unused))
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

/* ============ A6: full-extent neighbour gather for angular prediction ====
 *
 * Backlog A6 ports the full 33 angular intra modes (2-34) into this
 * file's 4x4-TU path. Planar/DC/Horizontal/Vertical only ever read as far
 * as p[-1][nTbS] (5 samples each side - what gather_neighbors() above
 * computed). Rec. ITU-T H.265 8.4.4.2.2's angular branch can reach
 * p[-1][2*nTbS-1] and p[2*nTbS-1][-1], almost twice as far, so every mode
 * needs the wider set below regardless of orientation (a "horizontal"
 * angular mode can still project into the TOP reference through the
 * negative-angle/invAngle path, and vice versa - see
 * predict_angular_sample()).
 *
 * This is the SAME formula hevc_intra_wavefront.comp's gather()/predict()
 * run for a 16x16 undivided CU - board-verified byte-exact against
 * ffmpeg's decode via `lab drift` (docs/hevc-gpu-intra.md). The angular
 * arithmetic itself is nTbS-agnostic (only the reference extent, 2*nTbS,
 * changes), so this is a direct parameterization to nTbS=4, not a new
 * derivation - the shader's ANGLE[]/INVANG[] tables are reproduced
 * verbatim below.
 *
 * This is NOT built by extending gather_neighbors() above: that
 * function's substitution scan starts partway through the real 4*nTbS+1
 * scan (at p[-1][nTbS] rather than p[-1][2*nTbS-1]), on the assumption -
 * true for Planar/DC/H/V, not provably true in general - that nothing
 * beyond nTbS is ever read. Layering the extra samples on top of that
 * truncated scan would substitute unavailable deep samples from the
 * wrong "first available" point; below is a straight, independent
 * application of 8.4.4.2.2's substitution process over the real extent. */
static void gather_neighbors_wide(const uint8_t *plane, int stride, int width, int height,
                                   int x0, int y0, int is_luma,
                                   uint8_t left[8], uint8_t top[8], uint8_t *corner) {
    int cur_rank = zorder_rank(x0, y0, width, is_luma);
    enum { HEVC_WIDE_SCAN_TOTAL = 17 };   /* 4*nTbS+1, nTbS=4 */
    uint8_t sv[HEVC_WIDE_SCAN_TOTAL];
    uint8_t sa[HEVC_WIDE_SCAN_TOTAL];

    /* Scan order per spec: p[-1][2N-1] first (deepest below-left) up
     * through p[-1][0], then the corner, then p[0][-1] across to
     * p[2N-1][-1]. sv[i], i=0..7 -> y=7-i; sv[8] -> corner; sv[9..16] ->
     * x=i-9. (N = nTbS = 4, so 2N = 8, matching left[]/top[]'s width.) */
    for (int i = 0; i < 8; i++) {
        int y = 7 - i;
        int avail = zorder_available(x0 - 1, y0 + y, width, height, is_luma, cur_rank);
        sa[i] = (uint8_t)avail;
        if (avail) sv[i] = plane[(y0 + y) * stride + (x0 - 1)];
    }
    sa[8] = (uint8_t)zorder_available(x0 - 1, y0 - 1, width, height, is_luma, cur_rank);
    if (sa[8]) sv[8] = plane[(y0 - 1) * stride + (x0 - 1)];
    for (int i = 9; i < HEVC_WIDE_SCAN_TOTAL; i++) {
        int x = i - 9;
        int avail = zorder_available(x0 + x, y0 - 1, width, height, is_luma, cur_rank);
        sa[i] = (uint8_t)avail;
        if (avail) sv[i] = plane[(y0 - 1) * stride + (x0 + x)];
    }

    int first = -1;
    for (int i = 0; i < HEVC_WIDE_SCAN_TOTAL; i++) { if (sa[i]) { first = i; break; } }
    if (first < 0) {
        for (int i = 0; i < HEVC_WIDE_SCAN_TOTAL; i++) sv[i] = 128;
    } else {
        for (int i = 0; i < first; i++) sv[i] = sv[first];
        for (int i = first + 1; i < HEVC_WIDE_SCAN_TOTAL; i++) if (!sa[i]) sv[i] = sv[i - 1];
    }

    for (int y = 0; y < 8; y++) left[y] = sv[7 - y];   /* left[y] = p[-1][y] */
    *corner = sv[8];
    for (int x = 0; x < 8; x++) top[x] = sv[9 + x];    /* top[x]  = p[x][-1] */
}

/* ===================== prediction (8.4.4.2.4-8.4.4.2.7) ===================== */

/* Angle table (8.4.4.2.6, predModeIntra 2..34) and Table 8-6's inverse,
 * lifted verbatim from hevc_intra_wavefront.comp's ANGLE[]/INVANG[] - the
 * same public spec constants, addressed here by an 8-entry (2*nTbS,
 * nTbS=4) reference set instead of the shader's 32-entry (2*nTbS, nTbS=16)
 * one. Both are exactly Table 8-5/8-6; nothing is nTbS-specific in the
 * tables themselves. */
static const int8_t  HEVC_ANGLE_TABLE[35] = {
      0,   0,  32,  26,  21,  17,  13,   9,   5,   2,   0,  -2,
     -5,  -9, -13, -17, -21, -26, -32, -26, -21, -17, -13,  -9,
     -5,  -2,   0,   2,   5,   9,  13,  17,  21,  26,  32
};
static const int16_t HEVC_INVANGLE_TABLE[15] = {
    -4096, -1638, -910, -630, -482, -390, -315, -256, -315, -390,
     -482,  -630, -910, -1638, -4096
};

/* One sample of the angular branch (8.4.4.2.6), mode 2..34, at (x,y) in a
 * 4x4 TU. No reference-sample smoothing (8.4.4.2.3): filterFlag is
 * unconditionally 0 whenever nTbS==4 (the minDistVerHor threshold table,
 * 8.4.4.2.3/Table 8-3, has rows only for nTbS in {8,16,32} - nTbS==4 skips
 * the whole filtering decision), so unlike the GPU shader's 16x16 CU this
 * needs only ONE reference set, not a raw/smoothed pair - one fewer thing
 * to get wrong porting this down from a 16x16 CU to a 4x4 TU. */
static int predict_angular_sample(const uint8_t left[8], const uint8_t top[8], uint8_t corner,
                                   int mode, int x, int y) {
    int ang = HEVC_ANGLE_TABLE[mode];
    int vert = (mode >= 18);
    int i = vert ? x : y;
    int j = vert ? y : x;
    int idx = ((j + 1) * ang) >> 5;
    int fact = ((j + 1) * ang) & 31;

    int r[2];
    for (int pass = 0; pass < 2; pass++) {
        int k = i + idx + 1 + pass;
        int v;
        if (k == 0) {
            v = corner;
        } else if (k > 0) {
            int kk = k - 1; if (kk > 7) kk = 7;
            v = vert ? top[kk] : left[kk];
        } else {
            /* Table 8-6: project through the OTHER reference array via
             * the inverse angle. Only reachable for the modes with a
             * negative HEVC_ANGLE_TABLE entry (predModeIntra 11..25),
             * which is exactly INVANGLE_TABLE's domain. */
            int m = -1 + ((k * HEVC_INVANGLE_TABLE[mode - 11] + 128) >> 8);
            if (m < 0) v = corner;
            else { if (m > 7) m = 7; v = vert ? left[m] : top[m]; }
        }
        r[pass] = v;
    }
    return (fact != 0) ? (((32 - fact) * r[0] + fact * r[1] + 16) >> 5) : r[0];
}

/* Predict one 4x4 block from an already-gathered wide reference set, for
 * ANY of the 35 modes (0=Planar, 1=DC, 2-34=angular - Rec. ITU-T H.265
 * 8.4.4.2.4/.5/.6). This is backlog A6's replacement for the old
 * Planar/DC/Horizontal/Vertical-only switch: modes 10 (Horizontal) and 26
 * (Vertical) are not special-cased for the BASE prediction, because
 * HEVC_ANGLE_TABLE[10] == HEVC_ANGLE_TABLE[26] == 0 collapses the general
 * angular formula to exactly the same straight copy the old dedicated
 * cases computed (checked by inspection: ang=0 => fact=0 => idx=0 => r[0]
 * is simply the reference sample one step along the corresponding line,
 * i.e. left[y] for mode 10 / top[x] for mode 26). They ARE still
 * special-cased for 8.4.4.2.6's exactly-horizontal/exactly-vertical edge
 * filter, which genuinely is mode-specific and luma-only. */
static inline void predict_block4(const uint8_t left[8], const uint8_t top[8], uint8_t corner,
                                   int mode, int is_luma, uint8_t pred_out[16]) {
    if (mode == HEVC_MODE_PLANAR) {
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int v = (3 - x) * left[y] + (x + 1) * top[4] +
                        (3 - y) * top[x] + (y + 1) * left[4] + 4;
                pred_out[y * 4 + x] = (uint8_t)(v >> 3);
            }
        return;
    }
    if (mode == HEVC_MODE_DC) {
        int dc = (left[0] + left[1] + left[2] + left[3] +
                  top[0]  + top[1]  + top[2]  + top[3]  + 4) >> 3;
        for (int i = 0; i < 16; i++) pred_out[i] = (uint8_t)dc;
        if (is_luma) {
            pred_out[0] = (uint8_t)((left[0] + 2 * dc + top[0] + 2) >> 2);
            for (int x = 1; x < 4; x++) pred_out[x] = (uint8_t)((top[x] + 3 * dc + 2) >> 2);
            for (int y = 1; y < 4; y++) pred_out[y * 4] = (uint8_t)((left[y] + 3 * dc + 2) >> 2);
        }
        return;
    }

    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            pred_out[y * 4 + x] = (uint8_t)predict_angular_sample(left, top, corner, mode, x, y);

    if (is_luma) {
        if (mode == HEVC_MODE_VERTICAL)          /* 26 */
            for (int y = 0; y < 4; y++)
                pred_out[y * 4] = clip8(top[0] + ((left[y] - corner) >> 1));
        else if (mode == HEVC_MODE_HORIZONTAL)   /* 10 */
            for (int x = 0; x < 4; x++)
                pred_out[x] = clip8(left[0] + ((top[x] - corner) >> 1));
    }
}

void hevc_predict_4x4(const uint8_t *recon_plane, int stride, int width, int height,
                      int x0, int y0, int mode, int is_luma, uint8_t pred_out[16]) {
    uint8_t left[8], top[8], corner;
    gather_neighbors_wide(recon_plane, stride, width, height, x0, y0, is_luma, left, top, &corner);
    predict_block4(left, top, corner, mode, is_luma, pred_out);
}

/* Sum of absolute differences over two contiguous 16-byte blocks. Written
 * as a plain loop on purpose rather than as an _mm_sad_epu8() intrinsic:
 * at -O3 the vectoriser already emits one packed SAD for this shape, and a
 * hand-written intrinsic would have to carry its own runtime dispatch for
 * the 32-bit build (docs: BUILD_32BIT, Steam Link) while producing the same
 * instruction. Exact integer arithmetic, so summation order is irrelevant
 * and the result is bit-identical to the scalar form it replaced. */
static inline int sad_4x4(const uint8_t a[16], const uint8_t b[16]) {
    int sad = 0;
    for (int i = 0; i < 16; i++) {
        int d = (int)a[i] - (int)b[i];
        sad += d < 0 ? -d : d;
    }
    return sad;
}

/* PERF: the mode search and the winning mode's prediction are ONE function,
 * because the caller needs both and the second was being recomputed from
 * scratch.
 *
 * encoder_h265.c's per-PU sequence was hevc_choose_luma_mode() followed
 * immediately by hevc_predict_4x4() with the mode it returned - and nothing
 * writes recon_y in between, so that second call re-ran the gather and
 * re-ran the prediction for a result the search had already built and
 * discarded. Keeping the winner costs one 16-byte copy per improvement and
 * removes a whole gather plus a whole prediction; that part of the shape
 * is unchanged by A6.
 *
 * A6 (2026-09-21): backlog.md's A6 - port the 33 angular modes,
 * all-TU-size transforms and undivided 16x16 CUs from cavlc-residual-coding
 * - widened this from a 4-candidate unrolled search to a 35-candidate loop.
 * Only piece (1), the angular modes, is done here; see
 * docs/notes/a6-cavlc-residual-port.md for why (2) and (3) were not
 * attempted and what would be needed. The old A5 comment about this being
 * "four modes... unrolled... a compile-time constant in each" no longer
 * applies - a runtime loop is the only shape that makes sense over 35
 * candidates, and this is now an ordinary SAD-only, no-RDO exhaustive
 * search (still no rate term, matching this encoder's existing "SAD only"
 * convention, just over more candidates). Ties go to the lowest-numbered
 * mode (strict `<`), which is this encoder's own convention, not a spec
 * requirement - HEVC does not mandate a tie-break.
 *
 * This is exhaustive, NOT coarse-to-fine, unlike
 * hevc_intra_wavefront.comp's 11-of-35 coarse grid search (that shader's
 * own SCOPE comment: "mode refinement... not implemented here yet"). A
 * cheaper search here is legitimate future perf work in the spirit of A5,
 * now against a 35-entry candidate set, and is explicitly NOT attempted -
 * see docs/notes/a6-cavlc-residual-port.md. */
int hevc_choose_luma_mode(const uint8_t *src_y, const uint8_t *recon_y, int stride,
                           int width, int height, int x0, int y0,
                           uint8_t pred_out[16]) {
    HEVC_PROF_ENTER();

    /* Gather ONCE for all 35 candidates - the reference set does not
     * depend on the mode. See gather_neighbors_wide()'s comment. */
    uint8_t left[8], top[8], corner;
    gather_neighbors_wide(recon_y, stride, width, height, x0, y0, 1, left, top, &corner);

    /* PERF: hoist the source block - unchanged from A5, still valid with
     * any candidate count since it has nothing to do with how many modes
     * are tried. */
    uint8_t src16[16];
    const uint8_t *srow = src_y + (size_t)y0 * (size_t)stride + (size_t)x0;
    memcpy(src16 + 0,  srow,                      4);
    memcpy(src16 + 4,  srow + stride,             4);
    memcpy(src16 + 8,  srow + 2 * (size_t)stride, 4);
    memcpy(src16 + 12, srow + 3 * (size_t)stride, 4);

    /* Mode 0 (Planar) evaluated straight into pred_out, same as before -
     * it is always tried first, so this is the same "first candidate wins
     * ties by default" initialisation as the old code, one 16-byte copy
     * cheaper than a sentinel `best_sad < 0` scheme. */
    predict_block4(left, top, corner, HEVC_MODE_PLANAR, 1 /* luma */, pred_out);
    int best_mode = HEVC_MODE_PLANAR;
    int best_sad = sad_4x4(src16, pred_out);

    for (int m = 1; m < HEVC_MODE_COUNT; m++) {
        uint8_t pred_[16];
        predict_block4(left, top, corner, m, 1 /* luma */, pred_);
        int sad_ = sad_4x4(src16, pred_);
        if (sad_ < best_sad) {
            best_sad = sad_;
            best_mode = m;
            memcpy(pred_out, pred_, 16);
        }
    }

    HEVC_PROF_LEAVE(best_mode, src_y, recon_y, stride, width, height, x0, y0);
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
/* The four kernels below are partial-butterfly factorisations of the two
 * matrices above - the same ones HM/x265 use (HM's partialButterfly4 /
 * fastForwardDst / fastInverseDst). They are not an approximation and not
 * a reordering of a floating-point sum: each output is the *same integer*
 * the 4-term dot product produces, reassociated using exact integer
 * add/sub, so the value handed to the (unchanged) rounding shift is
 * bit-identical by construction. The saving is arithmetic count - 16
 * multiplies and 12 adds per 4-point transform becomes 4-6 multiplies and
 * 8-10 add/subs.
 *
 * DCT-II exploits the matrix's even/odd symmetry: rows 0 and 2 depend only
 * on the sums x0+x3, x1+x2, rows 1 and 3 only on the differences. The
 * DST-VII matrix has no such symmetry, but its four distinct magnitudes
 * (29, 55, 74, 84 = 29+55) let three of the four outputs share the
 * sub-expressions below anyway.
 *
 * Equivalence is checked, not asserted: because the transform is an exact
 * integer linear map, agreeing with the naive matrix product on the four
 * basis vectors proves agreement on every input. tests/test_hevc_encode.c
 * (test_transform_butterfly_equivalence) does exactly that, over the full
 * 2D transform and both matrices, plus random blocks.
 *
 * Every kernel snapshots its four inputs into locals before writing any
 * output, so an in-place call - KERNEL(a, b, c, d, a, b, c, d) - is safe;
 * the stage-2 invocations below rely on that. */
#define FWD_DCT4(x0, x1, x2, x3, y0, y1, y2, y3) do {              \
    const int32_t i0_ = (x0), i1_ = (x1), i2_ = (x2), i3_ = (x3);  \
    const int32_t s03_ = i0_ + i3_, s12_ = i1_ + i2_;              \
    const int32_t d03_ = i0_ - i3_, d12_ = i1_ - i2_;              \
    (y0) = 64 * (s03_ + s12_);                                     \
    (y1) = 83 * d03_ + 36 * d12_;                                  \
    (y2) = 64 * (s03_ - s12_);                                     \
    (y3) = 36 * d03_ - 83 * d12_;                                  \
} while (0)

#define FWD_DST4(x0, x1, x2, x3, y0, y1, y2, y3) do {              \
    const int32_t i0_ = (x0), i1_ = (x1), i2_ = (x2), i3_ = (x3);  \
    const int32_t c0_ = i0_ + i3_, c1_ = i1_ + i3_;                \
    const int32_t c2_ = i0_ - i1_, c3_ = 74 * i2_;                 \
    (y0) = 29 * c0_ + 55 * c1_ + c3_;                              \
    (y1) = 74 * (i0_ + i1_ - i3_);                                 \
    (y2) = 29 * c2_ + 55 * c0_ - c3_;                              \
    (y3) = 55 * c2_ - 29 * c1_ + c3_;                              \
} while (0)

/* Transposed (inverse-direction) forms: y[r] = sum_k M[k][r] * x[k]. */
#define INV_DCT4(x0, x1, x2, x3, y0, y1, y2, y3) do {              \
    const int32_t i0_ = (x0), i1_ = (x1), i2_ = (x2), i3_ = (x3);  \
    const int32_t e0_ = 64 * (i0_ + i2_), e1_ = 64 * (i0_ - i2_);  \
    const int32_t o0_ = 83 * i1_ + 36 * i3_;                       \
    const int32_t o1_ = 36 * i1_ - 83 * i3_;                       \
    (y0) = e0_ + o0_;                                              \
    (y1) = e1_ + o1_;                                              \
    (y2) = e1_ - o1_;                                              \
    (y3) = e0_ - o0_;                                              \
} while (0)

#define INV_DST4(x0, x1, x2, x3, y0, y1, y2, y3) do {              \
    const int32_t i0_ = (x0), i1_ = (x1), i2_ = (x2), i3_ = (x3);  \
    const int32_t c0_ = i0_ + i2_, c1_ = i2_ + i3_;                \
    const int32_t c2_ = i0_ - i3_, c3_ = 74 * i1_;                 \
    (y0) = 29 * c0_ + 55 * c1_ + c3_;                              \
    (y1) = 55 * c2_ - 29 * c1_ + c3_;                              \
    (y2) = 74 * (i0_ - i2_ + i3_);                                 \
    (y3) = 55 * c0_ + 29 * c2_ - c3_;                              \
} while (0)

/* Both forward stages apply M in the same (non-transposed) orientation, so
 * one kernel serves both: stage 1 walks the four columns of the residual
 * (stride 4) producing tmp[i][c] = tNN with NN = i*4+c, stage 2 walks the
 * four rows of tmp in place.
 *
 * The two 4-iteration loops are left as loops on purpose. Spelling all 16
 * tmp values out as named scalars (so they could live in registers and the
 * function would own no array, dropping Ubuntu's default
 * -fstack-protector-strong canary) was tried and is WORSE: 16 live int32s
 * plus the kernel's own temporaries exceed the 15 allocatable GPRs, and
 * GCC 13 -O2 spilled the difference - inverse_transform_4x4_dct went from
 * 139 instructions / 31 memory operands to 370 / 127. The array form's
 * stack slots are cheaper than the spill code that replaces them. */
#define FWD_BODY(KERNEL)                                                    \
    int32_t t[16];                                                          \
    for (int c = 0; c < 4; c++) {                                           \
        int32_t y0, y1, y2, y3;                                             \
        KERNEL(residual[c], residual[4 + c], residual[8 + c],               \
               residual[12 + c], y0, y1, y2, y3);                           \
        t[c] = (y0 + 1) >> 1;      t[4 + c] = (y1 + 1) >> 1;                \
        t[8 + c] = (y2 + 1) >> 1;  t[12 + c] = (y3 + 1) >> 1;               \
    }                                                                       \
    for (int i = 0; i < 4; i++) {                                           \
        int32_t y0, y1, y2, y3;                                             \
        KERNEL(t[i * 4], t[i * 4 + 1], t[i * 4 + 2], t[i * 4 + 3],          \
               y0, y1, y2, y3);                                             \
        out[i * 4]     = (y0 + 128) >> 8;                                   \
        out[i * 4 + 1] = (y1 + 128) >> 8;                                   \
        out[i * 4 + 2] = (y2 + 128) >> 8;                                   \
        out[i * 4 + 3] = (y3 + 128) >> 8;                                   \
    }

static void forward_transform_4x4_dct(const int16_t residual[16], int32_t out[16]) {
    FWD_BODY(FWD_DCT4)
}

static void forward_transform_4x4_dst(const int16_t residual[16], int32_t out[16]) {
    FWD_BODY(FWD_DST4)
}

/* Reference implementation kept for the equivalence test only - the
 * literal transcription of 8.6.4.1's matrix product that the butterflies
 * above replaced. Nothing on the encode path calls it. */
void hevc_forward_transform_4x4_ref(const int16_t residual[16], int use_dst, int32_t out[16]) {
    const int16_t (*M)[4] = use_dst ? DST4 : DCT4;
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
/* Same shape as FWD_BODY, plus 8.6.4.2's stage-1 clip.
 *
 * The stage-2 clip the spec's text also carries is NOT dropped for speed
 * on a hunch - it is unreachable arithmetic. Stage 1 has just clipped
 * every tmp value into [-32768, 32767], and the largest column sum of
 * |M| is 247 for DCT-II (64+83+64+36) and 242 for DST-VII, so the stage-2
 * accumulator is bounded by 247 * 32768 = 8093696; (8093696 + 2048) >> 12
 * = 1976, and the negative side floors at -1976. A stage-2 result can
 * therefore never leave int16 range for ANY 16 input coefficients, and
 * the clip is dead code on every path. tests/test_hevc_encode.c still
 * compares this against the clip-carrying reference, including blocks of
 * saturated +-32768 coefficients, so the bound is checked and not just
 * argued. */
/* LOAD(i) supplies input coefficient i. Stage 1 reads each of the 16
 * exactly once, which is what lets the dequantization step (8.6.3) be
 * folded straight into it: dequant_itransform_4x4_dct()/_dst() below pass
 * a LOAD that dequantizes on the fly, so the encode path no longer writes
 * a 16-entry dq[] buffer and reads it straight back. */
#define INV_BODY(KERNEL, LOAD)                                              \
    int32_t t[16];                                                          \
    for (int c = 0; c < 4; c++) {                                           \
        int32_t y0, y1, y2, y3;                                             \
        KERNEL(LOAD(c), LOAD(4 + c), LOAD(8 + c), LOAD(12 + c),             \
               y0, y1, y2, y3);                                             \
        t[c] = clip_coeff((y0 + 64) >> 7);                                  \
        t[4 + c] = clip_coeff((y1 + 64) >> 7);                              \
        t[8 + c] = clip_coeff((y2 + 64) >> 7);                              \
        t[12 + c] = clip_coeff((y3 + 64) >> 7);                             \
    }                                                                       \
    for (int r = 0; r < 4; r++) {                                           \
        int32_t y0, y1, y2, y3;                                             \
        KERNEL(t[r * 4], t[r * 4 + 1], t[r * 4 + 2], t[r * 4 + 3],          \
               y0, y1, y2, y3);                                             \
        out[r * 4]     = (int16_t)((y0 + 2048) >> 12);                      \
        out[r * 4 + 1] = (int16_t)((y1 + 2048) >> 12);                      \
        out[r * 4 + 2] = (int16_t)((y2 + 2048) >> 12);                      \
        out[r * 4 + 3] = (int16_t)((y3 + 2048) >> 12);                      \
    }

#define INV_LOAD_PLAIN(i) ((int32_t)coeff[i])

static void inverse_transform_4x4_dct(const int16_t coeff[16], int16_t out[16]) {
    INV_BODY(INV_DCT4, INV_LOAD_PLAIN)
}

static void inverse_transform_4x4_dst(const int16_t coeff[16], int16_t out[16]) {
    INV_BODY(INV_DST4, INV_LOAD_PLAIN)
}

/* The dequantizing variants live further down, past HEVC_BDSHIFT's
 * definition in the quantization section. */

/* Thin wrappers over the butterfly kernels, so the equivalence test can
 * reach the transform stage on its own (the encode path calls the
 * specialized functions directly and never pays this dispatch). */
void hevc_forward_transform_4x4(const int16_t residual[16], int use_dst, int32_t out[16]) {
    if (use_dst) forward_transform_4x4_dst(residual, out);
    else         forward_transform_4x4_dct(residual, out);
}

void hevc_inverse_transform_4x4(const int16_t coeff[16], int use_dst, int16_t out[16]) {
    if (use_dst) inverse_transform_4x4_dst(coeff, out);
    else         inverse_transform_4x4_dct(coeff, out);
}

/* Reference implementation, test-only - see hevc_forward_transform_4x4_ref. */
void hevc_inverse_transform_4x4_ref(const int16_t coeff[16], int use_dst, int16_t out[16]) {
    const int16_t (*M)[4] = use_dst ? DST4 : DCT4;
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

/* Division-free forward quantization.
 *
 * The normative quotient is level = (|raw|<<5 + denom/2) / denom with
 * denom = (16 * levelScale[rem]) << per. Because floor(n / (a*b)) ==
 * floor(floor(n/a) / b) for positive integers, the variable `per` factors
 * out of the division entirely: the only divisor is d0 = 16 *
 * levelScale[rem], which has exactly SIX possible values. So there is no
 * runtime divisor at all - a table of six compile-time reciprocals does
 * the whole job, and even the one-per-block division that a classic
 * magic-number scheme would still need disappears.
 *
 * QUANT_MAGIC[rem] = ceil(2^42 / d0). For n >= 0 this gives exactly
 * floor(n / d0) whenever n * e < 2^42, where e = magic*d0 - 2^42 < d0.
 * Both factors are bounded here and the bound is not close:
 *
 *   - |raw| <= 2^22. residual is int16 so |residual| <= 32768; the largest
 *     row sum of |M| is 256 (DCT-II row 0), so forward stage 1 is bounded
 *     by (256*32768 + 1) >> 1 = 2^22 and stage 2 by (256*2^22 + 128) >> 8
 *     = 2^22. Hence n = (|raw| << 5) + denom/2 <= 2^27 + 147456 < 2^28.
 *   - e <= 848 < 2^10 across the six entries.
 *
 * so n*e < 2^38, versus the 2^42 required - four bits of margin. The
 * widest product n*magic is 2^60, comfortably inside uint64.
 *
 * This is verified by exhaustion, not argued. Offline, the reciprocal was
 * checked against the literal division for all 2^22+1 magnitudes at each
 * of the 52 QPs - 218 million pairs, zero mismatches.
 * tests/test_hevc_encode.c keeps the part of that which is cheap enough
 * to run every build: every magnitude 0..65535 (a real 8-bit residual
 * cannot exceed |raw| = 32640, so that range is already exhaustive for
 * the encoder) at all 52 QPs, plus a boundary-straddling sweep out to
 * 2^22.
 *
 * NOT to be confused with the change this replaced a measurement of: doing
 * the same division in 32 bits instead of 64 was tried and measured
 * SLIGHTLY SLOWER (median 1019ms vs 1011ms over 12 interleaved samples),
 * because Zen 4's divider costs about the same either width. Removing the
 * division is a different thing from narrowing it. */
#define QUANT_RECIP_SHIFT 42
static const uint64_t QUANT_MAGIC[6] = {
    6871947674ull,  /* d0 =  640 */
    6108397933ull,  /* d0 =  720 */
    5389762882ull,  /* d0 =  816 */
    4822419421ull,  /* d0 =  912 */
    4294967296ull,  /* d0 = 1024 */
    3817748708ull   /* d0 = 1152 */
};

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

static inline void quantize_levels(const int32_t raw[16], int qp, int16_t coeff_out[16]) {
    int per = qp / 6, rem = qp % 6;
    uint64_t half_denom = ((uint64_t)HEVC_FLAT_M * (uint64_t)levelScale[rem] << per) >> 1;
    uint64_t magic = QUANT_MAGIC[rem];
    int shift = QUANT_RECIP_SHIFT + per;
    for (int i = 0; i < 16; i++) {
        int32_t coeff_raw = raw[i];
        int neg = coeff_raw < 0;
        uint64_t mag = neg ? -(uint64_t)(int64_t)coeff_raw : (uint64_t)(int64_t)coeff_raw;
        uint64_t num = mag << HEVC_BDSHIFT;
        int64_t level = (int64_t)(((num + half_denom) * magic) >> shift);
        int32_t res = (int32_t)(neg ? -level : level);
        if (res > 32767) res = 32767;
        if (res < -32768) res = -32768;
        coeff_out[i] = (int16_t)res;
    }
}

/* Test hooks: the shipping quantizer and the literal-division original it
 * replaced, so tests/test_hevc_encode.c can feed them raw coefficients
 * directly instead of trying to steer a residual to a particular value. */
void hevc_quantize_4x4(const int32_t raw[16], int qp, int16_t coeff_out[16]) {
    quantize_levels(raw, qp, coeff_out);
}

void hevc_quantize_4x4_ref(const int32_t raw[16], int qp, int16_t coeff_out[16]) {
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

void hevc_transform_quant_4x4(const int16_t residual[16], int qp, int use_dst,
                               int16_t coeff_out[16]) {
    int32_t raw[16];
    if (use_dst) forward_transform_4x4_dst(residual, raw);
    else         forward_transform_4x4_dct(residual, raw);
    quantize_levels(raw, qp, coeff_out);
}

/* One coefficient's worth of 8.6.3 dequantization, expression for
 * expression what the standalone dq[] loop used to do: the int64 product,
 * the +16 rounding shift, the narrowing to int32 and then the clip. The
 * (int32_t) cast before clip_coeff() cannot actually truncate (the shifted
 * value is at most 32768*233472/32 = 2^27.8), but the order is preserved
 * anyway so the two versions are the same expression and not merely the
 * same intent. */
static inline int32_t dequant_one(int16_t level, int64_t scale, int64_t half_scale) {
    int64_t val = (int64_t)level * scale;
    val = (val + half_scale) >> HEVC_BDSHIFT;
    return clip_coeff((int32_t)val);
}

#define INV_LOAD_DQ(i) dequant_one(coeff[i], scale, half_scale)

static void dequant_itransform_4x4_dct(const int16_t coeff[16], int64_t scale,
                                       int64_t half_scale, int16_t out[16]) {
    INV_BODY(INV_DCT4, INV_LOAD_DQ)
}

static void dequant_itransform_4x4_dst(const int16_t coeff[16], int64_t scale,
                                       int64_t half_scale, int16_t out[16]) {
    INV_BODY(INV_DST4, INV_LOAD_DQ)
}

void hevc_dequant_itransform_4x4(const int16_t coeff[16], int qp, int use_dst,
                                  int16_t residual_out[16]) {
    /* MEASURED AND REJECTED: an all-zero-coefficient early-out here (four
     * 64-bit ORs over coeff[], then memset the output) is a valid
     * identity - zeros dequantize to zeros and both stages then produce
     * (0+64)>>7 == 0 and (0+2048)>>12 == 0 - and it fires often: 12.8% of
     * the 195840 blocks in a 1080p frame have cbf == 0 at QP 27, 8.7% at
     * QP 20, 13.9% at QP 51. It is still not worth it. Interleaved A/B,
     * byte-identical output both ways: +1.53% (9 samples, -O2), +1.39%
     * (9 samples, -O3 -march=znver2), and +0.71% on the largest and so
     * most trustworthy run (15 samples, -O2: best 812.6 -> 805.6 ms,
     * median 819.6 -> 813.7 ms). The effect shrinks as the sample grows,
     * which is what a noise artifact does. That is inside this project's
     * stated ~2.5% significance floor, and it is
     * the only candidate in this area that ADDS a branch rather than
     * removing work, so it was reverted. Don't re-add it without a
     * significance-tested number above the floor. */
    int per = qp / 6, rem = qp % 6;
    int64_t scale = ((int64_t)HEVC_FLAT_M * levelScale[rem]) << per;
    int64_t half_scale = 1 << (HEVC_BDSHIFT - 1);
    if (use_dst) dequant_itransform_4x4_dst(coeff, scale, half_scale, residual_out);
    else         dequant_itransform_4x4_dct(coeff, scale, half_scale, residual_out);
}
