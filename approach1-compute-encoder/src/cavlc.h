/* bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * cavlc.h - Spec-compliant H.264 CAVLC (Context-Adaptive Variable-Length Coding)
 *           per ITU-T H.264 / ISO/IEC 14496-10 Section 9.2.
 */
#ifndef CAVLC_H
#define CAVLC_H

#include <stdint.h>
#include <stdbool.h>
#include "bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ===========================================================================
 * Off-board profiling hooks (CAVLC_PROFILE) - NOT part of the shipped driver
 * ===========================================================================
 * `tools/cavlc_bench.c` drives this file directly with synthetic coefficient
 * data so CAVLC can be profiled and optimised on a dev machine (the shipping
 * H.264 entry point h264_encoder_encode_raw() is header-only by design and
 * never reaches residual coding - see docs/backlog.md A2).
 *
 * Everything below, and every CAVLC_STAGE() use in cavlc.c, is inside
 * `#ifdef CAVLC_PROFILE`. The driver build never defines it, so the
 * preprocessed driver source is unchanged - CAVLC_STAGE(s, ...) expands to
 * its body verbatim. Only the `cavlc_bench_prof` CMake target defines it.
 *
 * The profile build supports two things the normal build does not:
 *   - `cavlc_ablate_mask`: suppress one syntax element's bitstream writes
 *     while leaving every other instruction in the function untouched, so
 *     stage cost can be measured as a wall-clock delta between two runs of
 *     the SAME binary over the SAME data (no sampling profiler, no probe
 *     inserted into the timed path - see docs/performance-measurement.md on
 *     why gprof is not trusted here).
 *   - `cavlc_count_enable`: exact per-stage call and bit counts. Only ever
 *     enabled in an untimed run.
 */
#ifdef CAVLC_PROFILE
enum {
    CAVLC_S_TOKEN = 0,   /* coeff_token */
    CAVLC_S_SIGNS = 1,   /* trailing_ones_sign_flag */
    CAVLC_S_LEVELS = 2,  /* level_prefix/level_suffix */
    CAVLC_S_TZEROS = 3,  /* total_zeros */
    CAVLC_S_RUNS = 4,    /* run_before */
    CAVLC_S_HEADER = 5,  /* macroblock_layer() headers + mb_skip_run */
    CAVLC_S_COUNT = 6
};
/* Bit i set => stage i writes nothing. 0 = normal operation. */
extern unsigned cavlc_ablate_mask;
extern int cavlc_count_enable;
extern unsigned long long cavlc_stage_bits[CAVLC_S_COUNT];
extern unsigned long long cavlc_stage_calls[CAVLC_S_COUNT];
/* Number of times each of the three residual entry points was called, and
 * how many of those calls were on an entirely-zero block. */
extern unsigned long long cavlc_blocks_total[3];
extern unsigned long long cavlc_blocks_zero[3];
/* Gather+scan only, no bitstream writes: lets the harness price
 * cavlc_scan_coeffs()+the zigzag gather in isolation. Returns the discovered
 * total_coeff so the call cannot be optimised away. */
int cavlc_prof_scan_only(const int16_t *coeffs, int max_coeff);
void cavlc_prof_reset(void);
#endif /* CAVLC_PROFILE */

/* Intra 16x16 Prediction Modes */
#define H264_I16x16_VERT  0
#define H264_I16x16_HORIZ 1
#define H264_I16x16_DC    2
#define H264_I16x16_PLANE 3

/* Intra Chroma Prediction Modes */
#define H264_CHROMA_DC    0
#define H264_CHROMA_HORIZ 1
#define H264_CHROMA_VERT  2
#define H264_CHROMA_PLANE 3

/**
 * Write an Intra 16x16 macroblock header and parameters to the bitstream.
 *
 * @param bs               Bitstream context
 * @param pred_mode        Intra 16x16 prediction mode (0=VERT, 1=HORIZ, 2=DC, 3=PLANE)
 * @param chroma_pred_mode Intra chroma prediction mode (0=DC, 1=HORIZ, 2=VERT,
 *                         3=PLANE - see H264_CHROMA_* above; ITU-T 8.3.4 - a
 *                         real per-MB mode decision, not hardcoded DC, since a
 *                         macroblock whose top spatial neighbor is very
 *                         different content from its own (e.g. straddling a
 *                         hard color-region boundary) gets a badly wrong
 *                         chroma DC blend otherwise - see
 *                         residual_predict.comp's chroma mode-decision comment)
 * @param cbp_chroma       Coded block pattern for chroma: 0 (none), 1 (DC only), 2 (DC+AC)
 * @param cbp_luma         0 if no AC coefficients, 15 if AC coefficients present
 * @param qp_delta         Quantizer delta from previous macroblock (or slice QP)
 */
void cavlc_write_mb_i16x16_header(bitstream_t *bs, int pred_mode, int chroma_pred_mode, int cbp_chroma, int cbp_luma, int qp_delta);

/**
 * Write mb_skip_run (ITU-T H.264 7.3.4 slice_data()). MUST be called exactly
 * once, unconditionally (skip_run may be 0), immediately before every
 * cavlc_write_mb_p16x16_header() call in a P/SP slice - mb_skip_run is read
 * once per do-while iteration of slice_data(), not once per actual skip, and
 * is NOT part of macroblock_layer(); cavlc_write_mb_p16x16_header() does not
 * write it itself.
 *
 * @param bs        Bitstream context
 * @param skip_run  Number of consecutive skipped macroblocks immediately
 *                  preceding the next coded macroblock (0 if none)
 */
void cavlc_write_p_skip_run(bitstream_t *bs, uint32_t skip_run);

/**
 * Write a P_L0_16x16 macroblock header (macroblock_layer(), NOT including
 * mb_skip_run - see cavlc_write_p_skip_run()) with motion vector difference.
 *
 * @param bs          Bitstream context
 * @param mvd_x       Motion vector difference horizontal component (half/quarter-pel units)
 * @param mvd_y       Motion vector difference vertical component
 * @param cbp         Coded block pattern (0 = no residual)
 * @param qp_delta    Quantizer delta
 */
void cavlc_write_mb_p16x16_header(bitstream_t *bs, int mvd_x, int mvd_y, int cbp, int qp_delta);

/**
 * Encode a 4x4 block of quantized transform coefficients using CAVLC.
 *
 * @param bs          Bitstream context
 * @param coeffs      Array of 16 quantized coefficients in raster scan order
 * @param nC          Context prediction value (average of left and top block non-zero counts)
 * @return            Total number of non-zero coefficients (for updating neighbor context)
 */
int cavlc_write_4x4_block(bitstream_t *bs, const int16_t *coeffs, int nC);

/**
 * Encode the 15 AC coefficients of a 4x4 block (maxNumCoeff=15) using CAVLC.
 * Used for I16x16 luma AC blocks and chroma AC blocks, both of which code
 * their own DC coefficient separately (luma via the Hadamard DC block,
 * chroma via cavlc_write_chroma_dc_block()).
 *
 * @param bs          Bitstream context
 * @param coeffs      Array of 16 quantized coefficients in raster scan order
 *                     (same convention as cavlc_write_4x4_block) - position 0
 *                     (the DC coefficient) is read but never written; only
 *                     zigzag_4x4[1..15] (the 15 AC positions) are encoded.
 * @param nC          Context prediction value (average of left and top block non-zero counts)
 * @return            Total number of non-zero AC coefficients (for updating neighbor context)
 */
int cavlc_write_4x4_ac_block(bitstream_t *bs, const int16_t *coeffs, int nC);

/**
 * Encode a 2x2 chroma DC block using CAVLC.
 *
 * @param bs          Bitstream context
 * @param coeffs      Array of 4 chroma DC coefficients in row-major order
 *                     (no zigzag permutation - see ITU-T 8.5.11)
 * @return            Total number of non-zero coefficients
 */
int cavlc_write_chroma_dc_block(bitstream_t *bs, const int *coeffs);

/**
 * Write rbsp_slice_trailing_bits (1-bit followed by zero bits to byte boundary).
 *
 * @param bs          Bitstream context
 */
void cavlc_write_slice_trailing_bits(bitstream_t *bs);

#ifdef __cplusplus
}
#endif

#endif /* CAVLC_H */
