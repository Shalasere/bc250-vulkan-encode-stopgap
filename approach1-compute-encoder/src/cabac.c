/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * cabac.c - H.264 CABAC entropy encoder implementation.
 *
 * See cabac.h's top-of-file comment for the full provenance disclosure
 * (adapted from x264, https://github.com/mirror/x264, GPL-2.0-or-later +
 * commercial dual license, copyright (C) 2003-2024 x264 project, authors
 * Laurent Aimar / Loren Merritt / Fiona Glaser - verified directly against
 * a fresh clone's COPYING file and per-file license headers before any of
 * this file was written).
 *
 * ============================================================================
 * CONTEXT-INDEX MAP (ctxIdx 0-275 - see CABAC_NUM_CTX in cabac.h)
 * ============================================================================
 * This project only ever emits I_16x16 intra and P_L0_16x16 inter
 * macroblocks (4:2:0, frame-only, single reference, no 8x8 transform, no
 * B-slices) - see cabac.h's SCOPE note. That restricts the real H.264 CABAC
 * context space (ctxIdx 0-1023) down to the ranges actually reachable here,
 * which is why this file's tables stop at 275 rather than covering the full
 * spec range the way x264 itself does (x264 supports every H.264 feature,
 * so its own tables go to 1023). Every index below was cross-checked against
 * x264's common/tables.c (x264_cabac_context_init_I / _PB[0]) entry-by-entry,
 * not reconstructed from the spec text or memory:
 *
 *   3-10   : mb_type (I slice: 3+ctx0..2 prefix, 6/7/8=embedded cbp, 9/10=
 *            Intra16x16PredMode bins)                     [I-slice only]
 *   11-13  : mb_skip_flag (P slice, ctx 0..2)              [P-slice only]
 *   14-16  : mb_type (P slice: 14=prefix vs intra, 15/16=partition bins;
 *            this project only ever codes the D_16x16 path, i.e. 14=0,15=0,
 *            16=0 - all three still need real contexts since they're real
 *            coded bins, just always with a fixed bit value here) [P-slice]
 *   40-46  : mvd_l0 horizontal (ctx 0..2 presence, 3..6 unary continuation)
 *   47-53  : mvd_l0 vertical (same structure, offset +7)
 *   60-63  : mb_qp_delta (ctx 0..3)
 *   64-67  : intra_chroma_pred_mode (ctx 0..2 presence, 3 continuation)
 *            [I-slice only - this project's P slices have no intra MBs]
 *   73-76  : coded_block_pattern, luma nibble (ctx 0..3)   [P-slice only -
 *            I_16x16's cbp is embedded in mb_type at 6/7/8 instead]
 *   77-83  : coded_block_pattern, chroma (ctx 0..6)        [P-slice only]
 *   85-88  : coded_block_flag, ctxBlockCat=LUMA_DC   (ctx 0..3)
 *   89-92  : coded_block_flag, ctxBlockCat=LUMA_AC   (ctx 0..3)
 *   93-96  : coded_block_flag, ctxBlockCat=LUMA_4x4  (ctx 0..3)
 *   97-100 : coded_block_flag, ctxBlockCat=CHROMA_DC (ctx 0..3)
 *  101-104 : coded_block_flag, ctxBlockCat=CHROMA_AC (ctx 0..3)
 *  105-165 : significant_coeff_flag, per-category bases {105,120,134,149,152}
 *  166-226 : last_significant_coeff_flag, per-category bases
 *            {166,181,195,210,213}
 *  227-275 : coeff_abs_level_minus1, per-category bases
 *            {227,237,247,257,266}
 *
 * ctxIdx 0-2 and 17-39/54-59/68-72/84 are allocated (present in the tables
 * below, copied verbatim from x264 for exact positional fidelity even where
 * this project never reads them) but never actually indexed by any function
 * in this file - they correspond to I_NxN/I_PCM mb_type suffixes, ref_idx,
 * sub_mb_type, transform_size_8x8_flag, and B-slice syntax this project's
 * encoder never emits.
 */
#include "cabac.h"
#include <string.h>

/* ===========================================================================
 * Arithmetic coding engine - adapted from x264's common/cabac.c (see this
 * file's top-of-file provenance comment). Renamed x264_cabac_t ->
 * cabac_engine_t and functions to this project's cabac_* naming convention;
 * the algorithm and constants are numerically identical to x264's.
 * ===========================================================================
 */

/* ITU-T Table 9-46 (rangeTabLPS) == x264's x264_cabac_range_lps[64][4],
 * copied verbatim (spec-mandated constant table). */
static const uint8_t cabac_range_lps[64][4] = {
    {  2,   2,   2,   2}, {  6,   7,   8,   9}, {  6,   7,   9,  10}, {  6,   8,   9,  11},
    {  7,   8,  10,  11}, {  7,   9,  10,  12}, {  7,   9,  11,  12}, {  8,   9,  11,  13},
    {  8,  10,  12,  14}, {  9,  11,  12,  14}, {  9,  11,  13,  15}, { 10,  12,  14,  16},
    { 10,  12,  15,  17}, { 11,  13,  15,  18}, { 11,  14,  16,  19}, { 12,  14,  17,  20},
    { 12,  15,  18,  21}, { 13,  16,  19,  22}, { 14,  17,  20,  23}, { 14,  18,  21,  24},
    { 15,  19,  22,  25}, { 16,  20,  23,  27}, { 17,  21,  25,  28}, { 18,  22,  26,  30},
    { 19,  23,  27,  31}, { 20,  24,  29,  33}, { 21,  26,  30,  35}, { 22,  27,  32,  37},
    { 23,  28,  33,  39}, { 24,  30,  35,  41}, { 26,  31,  37,  43}, { 27,  33,  39,  45},
    { 29,  35,  41,  48}, { 30,  37,  43,  50}, { 32,  39,  46,  53}, { 33,  41,  48,  56},
    { 35,  43,  51,  59}, { 37,  45,  54,  62}, { 39,  48,  56,  65}, { 41,  50,  59,  69},
    { 43,  53,  63,  72}, { 46,  56,  66,  76}, { 48,  59,  69,  80}, { 51,  62,  73,  85},
    { 53,  65,  77,  89}, { 56,  69,  81,  94}, { 59,  72,  86,  99}, { 62,  76,  90, 104},
    { 66,  80,  95, 110}, { 69,  85, 100, 116}, { 73,  89, 105, 122}, { 77,  94, 111, 128},
    { 81,  99, 117, 135}, { 85, 104, 123, 142}, { 90, 110, 130, 150}, { 95, 116, 137, 158},
    {100, 122, 144, 166}, {105, 128, 152, 175}, {111, 135, 160, 185}, {116, 142, 169, 195},
    {123, 150, 178, 205}, {128, 158, 187, 216}, {128, 167, 197, 227}, {128, 176, 208, 240}
};

/* ITU-T Table 9-45 (transIdxLPS / transIdxMPS) == x264's
 * x264_cabac_transition[128][2], copied verbatim. Row i (packed pStateIdx<<1
 * | valMPS) gives {new state if bin!=valMPS, new state if bin==valMPS}. */
static const uint8_t cabac_transition[128][2] = {
    {  0,   0}, {  1,   1}, {  2,  50}, { 51,   3}, {  2,  50}, { 51,   3}, {  4,  52}, { 53,   5},
    {  6,  52}, { 53,   7}, {  8,  52}, { 53,   9}, { 10,  54}, { 55,  11}, { 12,  54}, { 55,  13},
    { 14,  54}, { 55,  15}, { 16,  56}, { 57,  17}, { 18,  56}, { 57,  19}, { 20,  56}, { 57,  21},
    { 22,  58}, { 59,  23}, { 24,  58}, { 59,  25}, { 26,  60}, { 61,  27}, { 28,  60}, { 61,  29},
    { 30,  60}, { 61,  31}, { 32,  62}, { 63,  33}, { 34,  62}, { 63,  35}, { 36,  64}, { 65,  37},
    { 38,  66}, { 67,  39}, { 40,  66}, { 67,  41}, { 42,  66}, { 67,  43}, { 44,  68}, { 69,  45},
    { 46,  68}, { 69,  47}, { 48,  70}, { 71,  49}, { 50,  72}, { 73,  51}, { 52,  72}, { 73,  53},
    { 54,  74}, { 75,  55}, { 56,  74}, { 75,  57}, { 58,  76}, { 77,  59}, { 60,  78}, { 79,  61},
    { 62,  78}, { 79,  63}, { 64,  80}, { 81,  65}, { 66,  82}, { 83,  67}, { 68,  82}, { 83,  69},
    { 70,  84}, { 85,  71}, { 72,  84}, { 85,  73}, { 74,  88}, { 89,  75}, { 76,  88}, { 89,  77},
    { 78,  90}, { 91,  79}, { 80,  90}, { 91,  81}, { 82,  94}, { 95,  83}, { 84,  94}, { 95,  85},
    { 86,  96}, { 97,  87}, { 88,  96}, { 97,  89}, { 90, 100}, {101,  91}, { 92, 100}, {101,  93},
    { 94, 102}, {103,  95}, { 96, 104}, {105,  97}, { 98, 104}, {105,  99}, {100, 108}, {109, 101},
    {102, 108}, {109, 103}, {104, 110}, {111, 105}, {106, 112}, {113, 107}, {108, 114}, {115, 109},
    {110, 116}, {117, 111}, {112, 118}, {119, 113}, {114, 118}, {119, 115}, {116, 122}, {123, 117},
    {118, 122}, {123, 119}, {120, 124}, {125, 121}, {122, 126}, {127, 123}, {124, 127}, {126, 125}
};

/* ITU-T Table 9-47 (renormalization shift, precomputed by codIRange>>3) ==
 * x264's x264_cabac_renorm_shift[64], copied verbatim. */
static const uint8_t cabac_renorm_shift[64] = {
    6,5,4,4,3,3,3,3,2,2,2,2,2,2,2,2,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* Context initialization (m,n) pairs, ctxIdx 0..275, for the I-slice table
 * and the P/B-slice table with cabac_init_idc==0 - copied verbatim (entry by
 * entry, cross-checked against a fresh x264 clone) from
 * x264_cabac_context_init_I[0..275] and x264_cabac_context_init_PB[0][0..275]
 * (common/tables.c). Real H.264 CABAC also defines cabac_init_idc 1 and 2
 * for P/B slices (different probability tuning for different content); this
 * project always signals cabac_init_idc=0 (see encoder_h264.c), so only
 * the idc==0 table is needed. Entries this project never reads (see the
 * context-index map above) are still copied verbatim for positional
 * fidelity with x264's real table, rather than zeroed out. */
static const int8_t cabac_init_I[CABAC_NUM_CTX][2] = {
    /* 0-10 */
    { 20, -15 }, {  2,  54 }, {  3,  74 }, { 20, -15 }, {  2,  54 }, {  3,  74 },
    { -28, 127 }, { -23, 104 }, { -6, 53 }, { -1, 54 }, { 7, 51 },
    /* 11-23 (unused for I) */
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /* 24-39 (unused for I) */
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /* 40-53 (unused for I) */
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /* 54-59 (unused for I) */
    {0,0},{0,0},{0,0},{0,0},{0,0},{0,0},
    /* 60-69 */
    { 0, 41 }, { 0, 63 }, { 0, 63 }, { 0, 63 }, { -9, 83 }, { 4, 86 }, { 0, 97 }, { -7, 72 }, { 13, 41 }, { 3, 62 },
    /* 70-87 */
    { 0, 11 }, { 1, 55 }, { 0, 69 }, { -17, 127 }, { -13, 102 }, { 0, 82 }, { -7, 74 }, { -21, 107 },
    { -27, 127 }, { -31, 127 }, { -24, 127 }, { -18, 95 }, { -27, 127 }, { -21, 114 }, { -30, 127 },
    { -17, 123 }, { -12, 115 }, { -16, 122 },
    /* 88-104 */
    { -11, 115 }, { -12, 63 }, { -2, 68 }, { -15, 84 }, { -13, 104 }, { -3, 70 }, { -8, 93 }, { -10, 90 },
    { -30, 127 }, { -1, 74 }, { -6, 97 }, { -7, 91 }, { -20, 127 }, { -4, 56 }, { -5, 82 }, { -7, 76 }, { -22, 125 },
    /* 105-135 */
    { -7, 93 }, { -11, 87 }, { -3, 77 }, { -5, 71 }, { -4, 63 }, { -4, 68 }, { -12, 84 }, { -7, 62 },
    { -7, 65 }, { 8, 61 }, { 5, 56 }, { -2, 66 }, { 1, 64 }, { 0, 61 }, { -2, 78 }, { 1, 50 },
    { 7, 52 }, { 10, 35 }, { 0, 44 }, { 11, 38 }, { 1, 45 }, { 0, 46 }, { 5, 44 }, { 31, 17 },
    { 1, 51 }, { 7, 50 }, { 28, 19 }, { 16, 33 }, { 14, 62 }, { -13, 108 }, { -15, 100 },
    /* 136-165 */
    { -13, 101 }, { -13, 91 }, { -12, 94 }, { -10, 88 }, { -16, 84 }, { -10, 86 }, { -7, 83 }, { -13, 87 },
    { -19, 94 }, { 1, 70 }, { 0, 72 }, { -5, 74 }, { 18, 59 }, { -8, 102 }, { -15, 100 }, { 0, 95 },
    { -4, 75 }, { 2, 72 }, { -11, 75 }, { -3, 71 }, { 15, 46 }, { -13, 69 }, { 0, 62 }, { 0, 65 },
    { 21, 37 }, { -15, 72 }, { 9, 57 }, { 16, 54 }, { 0, 62 }, { 12, 72 },
    /* 166-196 */
    { 24, 0 }, { 15, 9 }, { 8, 25 }, { 13, 18 }, { 15, 9 }, { 13, 19 }, { 10, 37 }, { 12, 18 },
    { 6, 29 }, { 20, 33 }, { 15, 30 }, { 4, 45 }, { 1, 58 }, { 0, 62 }, { 7, 61 }, { 12, 38 },
    { 11, 45 }, { 15, 39 }, { 11, 42 }, { 13, 44 }, { 16, 45 }, { 12, 41 }, { 10, 49 }, { 30, 34 },
    { 18, 42 }, { 10, 55 }, { 17, 51 }, { 17, 46 }, { 0, 89 }, { 26, -19 }, { 22, -17 },
    /* 197-226 */
    { 26, -17 }, { 30, -25 }, { 28, -20 }, { 33, -23 }, { 37, -27 }, { 33, -23 }, { 40, -28 }, { 38, -17 },
    { 33, -11 }, { 40, -15 }, { 41, -6 }, { 38, 1 }, { 41, 17 }, { 30, -6 }, { 27, 3 }, { 26, 22 },
    { 37, -16 }, { 35, -4 }, { 38, -8 }, { 38, -3 }, { 37, 3 }, { 38, 5 }, { 42, 0 }, { 35, 16 },
    { 39, 22 }, { 14, 48 }, { 27, 37 }, { 21, 60 }, { 12, 68 }, { 2, 97 },
    /* 227-251 */
    { -3, 71 }, { -6, 42 }, { -5, 50 }, { -3, 54 }, { -2, 62 }, { 0, 58 }, { 1, 63 }, { -2, 72 },
    { -1, 74 }, { -9, 91 }, { -5, 67 }, { -5, 27 }, { -3, 39 }, { -2, 44 }, { 0, 46 }, { -16, 64 },
    { -8, 68 }, { -10, 78 }, { -6, 77 }, { -10, 86 }, { -12, 92 }, { -15, 55 }, { -10, 60 }, { -6, 62 }, { -4, 65 },
    /* 252-275 */
    { -12, 73 }, { -8, 76 }, { -7, 80 }, { -9, 88 }, { -17, 110 }, { -11, 97 }, { -20, 84 }, { -11, 79 },
    { -6, 73 }, { -4, 74 }, { -13, 86 }, { -13, 96 }, { -11, 97 }, { -19, 117 }, { -8, 78 }, { -5, 33 },
    { -4, 48 }, { -2, 53 }, { -3, 62 }, { -13, 71 }, { -10, 79 }, { -12, 86 }, { -13, 90 }, { -14, 97 },
};

static const int8_t cabac_init_PB0[CABAC_NUM_CTX][2] = {
    /* 0-10 */
    { 20, -15 }, {  2,  54 }, {  3,  74 }, { 20, -15 }, {  2,  54 }, {  3,  74 },
    { -28, 127 }, { -23, 104 }, { -6, 53 }, { -1, 54 }, { 7, 51 },
    /* 11-23 */
    { 23, 33 }, { 23, 2 }, { 21, 0 }, { 1, 9 }, { 0, 49 }, { -37, 118 }, { 5, 57 }, { -13, 78 },
    { -11, 65 }, { 1, 62 }, { 12, 49 }, { -4, 73 }, { 17, 50 },
    /* 24-39 */
    { 18, 64 }, { 9, 43 }, { 29, 0 }, { 26, 67 }, { 16, 90 }, { 9, 104 }, { -46, 127 }, { -20, 104 },
    { 1, 67 }, { -13, 78 }, { -11, 65 }, { 1, 62 }, { -6, 86 }, { -17, 95 }, { -6, 61 }, { 9, 45 },
    /* 40-53 */
    { -3, 69 }, { -6, 81 }, { -11, 96 }, { 6, 55 }, { 7, 67 }, { -5, 86 }, { 2, 88 }, { 0, 58 },
    { -3, 76 }, { -10, 94 }, { 5, 54 }, { 4, 69 }, { -3, 81 }, { 0, 88 },
    /* 54-59 */
    { -7, 67 }, { -5, 74 }, { -4, 74 }, { -5, 80 }, { -7, 72 }, { 1, 58 },
    /* 60-69 */
    { 0, 41 }, { 0, 63 }, { 0, 63 }, { 0, 63 }, { -9, 83 }, { 4, 86 }, { 0, 97 }, { -7, 72 }, { 13, 41 }, { 3, 62 },
    /* 70-104 */
    { 0, 45 }, { -4, 78 }, { -3, 96 }, { -27, 126 }, { -28, 98 }, { -25, 101 }, { -23, 67 }, { -28, 82 },
    { -20, 94 }, { -16, 83 }, { -22, 110 }, { -21, 91 }, { -18, 102 }, { -13, 93 }, { -29, 127 }, { -7, 92 },
    { -5, 89 }, { -7, 96 }, { -13, 108 }, { -3, 46 }, { -1, 65 }, { -1, 57 }, { -9, 93 }, { -3, 74 },
    { -9, 92 }, { -8, 87 }, { -23, 126 }, { 5, 54 }, { 6, 60 }, { 6, 59 }, { 6, 69 }, { -1, 48 },
    { 0, 68 }, { -4, 69 }, { -8, 88 },
    /* 105-165 */
    { -2, 85 }, { -6, 78 }, { -1, 75 }, { -7, 77 }, { 2, 54 }, { 5, 50 }, { -3, 68 }, { 1, 50 },
    { 6, 42 }, { -4, 81 }, { 1, 63 }, { -4, 70 }, { 0, 67 }, { 2, 57 }, { -2, 76 }, { 11, 35 },
    { 4, 64 }, { 1, 61 }, { 11, 35 }, { 18, 25 }, { 12, 24 }, { 13, 29 }, { 13, 36 }, { -10, 93 },
    { -7, 73 }, { -2, 73 }, { 13, 46 }, { 9, 49 }, { -7, 100 }, { 9, 53 }, { 2, 53 }, { 5, 53 },
    { -2, 61 }, { 0, 56 }, { 0, 56 }, { -13, 63 }, { -5, 60 }, { -1, 62 }, { 4, 57 }, { -6, 69 },
    { 4, 57 }, { 14, 39 }, { 4, 51 }, { 13, 68 }, { 3, 64 }, { 1, 61 }, { 9, 63 }, { 7, 50 },
    { 16, 39 }, { 5, 44 }, { 4, 52 }, { 11, 48 }, { -5, 60 }, { -1, 59 }, { 0, 59 }, { 22, 33 },
    { 5, 44 }, { 14, 43 }, { -1, 78 }, { 0, 60 }, { 9, 69 },
    /* 166-226 */
    { 11, 28 }, { 2, 40 }, { 3, 44 }, { 0, 49 }, { 0, 46 }, { 2, 44 }, { 2, 51 }, { 0, 47 },
    { 4, 39 }, { 2, 62 }, { 6, 46 }, { 0, 54 }, { 3, 54 }, { 2, 58 }, { 4, 63 }, { 6, 51 },
    { 6, 57 }, { 7, 53 }, { 6, 52 }, { 6, 55 }, { 11, 45 }, { 14, 36 }, { 8, 53 }, { -1, 82 },
    { 7, 55 }, { -3, 78 }, { 15, 46 }, { 22, 31 }, { -1, 84 }, { 25, 7 }, { 30, -7 }, { 28, 3 },
    { 28, 4 }, { 32, 0 }, { 34, -1 }, { 30, 6 }, { 30, 6 }, { 32, 9 }, { 31, 19 }, { 26, 27 },
    { 26, 30 }, { 37, 20 }, { 28, 34 }, { 17, 70 }, { 1, 67 }, { 5, 59 }, { 9, 67 }, { 16, 30 },
    { 18, 32 }, { 18, 35 }, { 22, 29 }, { 24, 31 }, { 23, 38 }, { 18, 43 }, { 20, 41 }, { 11, 63 },
    { 9, 59 }, { 9, 64 }, { -1, 94 }, { -2, 89 }, { -9, 108 },
    /* 227-275 */
    { -6, 76 }, { -2, 44 }, { 0, 45 }, { 0, 52 }, { -3, 64 }, { -2, 59 }, { -4, 70 }, { -4, 75 },
    { -8, 82 }, { -17, 102 }, { -9, 77 }, { 3, 24 }, { 0, 42 }, { 0, 48 }, { 0, 55 }, { -6, 59 },
    { -7, 71 }, { -12, 83 }, { -11, 87 }, { -30, 119 }, { 1, 58 }, { -3, 29 }, { -1, 36 }, { 1, 38 },
    { 2, 43 }, { -6, 55 }, { 0, 58 }, { 0, 64 }, { -3, 74 }, { -10, 90 }, { 0, 70 }, { -4, 29 },
    { 5, 31 }, { 7, 42 }, { 1, 59 }, { -2, 58 }, { -3, 72 }, { -3, 81 }, { -11, 97 }, { 0, 58 },
    { 8, 5 }, { 10, 14 }, { 14, 18 }, { 13, 27 }, { 2, 40 }, { 0, 58 }, { -3, 70 }, { -6, 79 },
    { -8, 85 },
};

static inline int cabac_clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }

void cabac_context_init(cabac_engine_t *cb, bool is_intra_slice, int cabac_init_idc, int qp) {
    (void)cabac_init_idc; /* always 0 in this project - see this file's top comment */
    const int8_t (*table)[2] = is_intra_slice ? cabac_init_I : cabac_init_PB0;
    if (qp < 0) qp = 0;
    if (qp > 51) qp = 51;
    for (int j = 0; j < CABAC_NUM_CTX; j++) {
        int state = cabac_clip3(1, 126, ((table[j][0] * qp) >> 4) + table[j][1]);
        int pstate = state < 127 - state ? state : 127 - state;
        int val_mps = state >= 64;
        cb->state[j] = (uint8_t)((pstate << 1) | val_mps);
    }
}

void cabac_engine_init(cabac_engine_t *cb, uint8_t *p_data, uint8_t *p_end) {
    cb->i_low = 0;
    cb->i_range = 0x01FE;
    cb->i_queue = -9; /* first bit shifted away, not written - matches x264 */
    cb->i_bytes_outstanding = 0;
    cb->p_start = p_data;
    cb->p = p_data;
    cb->p_end = p_end;
    cb->overflow = false;
}

static inline void cabac_putbyte(cabac_engine_t *cb) {
    if (cb->i_queue < 0) return;
    int out = cb->i_low >> (cb->i_queue + 10);
    cb->i_low &= (0x400 << cb->i_queue) - 1;
    cb->i_queue -= 8;

    if ((out & 0xff) == 0xff) {
        cb->i_bytes_outstanding++;
    } else {
        int carry = out >> 8;
        int bytes_outstanding = cb->i_bytes_outstanding;
        /* This can legitimately write to cb->p[-1] even on the very FIRST
         * cabac_putbyte() call (cb->p == cb->p_start): p_start is set by the
         * caller to point just past the slice header's already-written,
         * byte-aligned bytes in the SAME underlying buffer (see
         * encoder_h264.c), so p[-1] here is the slice header's last byte,
         * not out-of-bounds - this is the same invariant x264 relies on
         * (see its own comment: "this can't modify before the beginning of
         * the stream ... a slice header always comes before cabac data").
         * It can never walk further back than one byte (a carry only ever
         * ripples into the single immediately-preceding byte, never
         * beyond), so no bounds check is needed here as long as the caller
         * upholds that contract. */
        cb->p[-1] = (uint8_t)(cb->p[-1] + carry);
        while (bytes_outstanding > 0) {
            if (cb->p >= cb->p_end) { cb->overflow = true; break; }
            *(cb->p++) = (uint8_t)(carry - 1);
            bytes_outstanding--;
        }
        if (cb->p >= cb->p_end) { cb->overflow = true; }
        else *(cb->p++) = (uint8_t)out;
        cb->i_bytes_outstanding = 0;
    }
}

static inline void cabac_encode_renorm(cabac_engine_t *cb) {
    int shift = cabac_renorm_shift[cb->i_range >> 3];
    cb->i_range <<= shift;
    cb->i_low <<= shift;
    cb->i_queue += shift;
    cabac_putbyte(cb);
}

void cabac_encode_decision(cabac_engine_t *cb, int ctx_idx, int bit) {
    int i_state = cb->state[ctx_idx];
    int i_range_lps = cabac_range_lps[i_state >> 1][(cb->i_range >> 6) - 4];
    cb->i_range -= i_range_lps;
    if (bit != (i_state & 1)) {
        cb->i_low += cb->i_range;
        cb->i_range = i_range_lps;
    }
    cb->state[ctx_idx] = cabac_transition[i_state][bit];
    cabac_encode_renorm(cb);
}

void cabac_encode_bypass(cabac_engine_t *cb, int bit) {
    cb->i_low <<= 1;
    cb->i_low += (-bit) & cb->i_range; /* bit is 0/1 here (unlike x264's
                                          negated-b convention) - (-bit) is
                                          all-ones when bit=1, 0 when bit=0,
                                          reproducing x264's `b & i_range`
                                          with its caller-negated `b`. */
    cb->i_queue += 1;
    cabac_putbyte(cb);
}

void cabac_encode_terminal(cabac_engine_t *cb) {
    /* Value-0 ("continue") case only - see cabac.h's doc comment on this
     * function for why the value-1 case is handled entirely inside
     * cabac_encode_flush() instead, matching x264's
     * x264_cabac_encode_terminal_c() exactly (that function never takes a
     * bit parameter either, for the same reason). */
    cb->i_range -= 2;
    cabac_encode_renorm(cb);
}

static const int32_t cabac_bypass_lut[16] = {
    -1,      0x2,     0x14,     0x68,     0x1d0,     0x7a0,     0x1f40,     0x7e80,
    0x1fd00, 0x7fa00, 0x1ff400, 0x7fe800, 0x1ffd000, 0x7ffa000, 0x1fff4000, 0x7ffe8000
};

static inline int cabac_clz32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(x);
#else
    int n = 0;
    while (!(x & 0x80000000u)) { x <<= 1; n++; }
    return n;
#endif
}

void cabac_encode_ue_bypass(cabac_engine_t *cb, int exp_bits, int val) {
    uint32_t v = (uint32_t)val + (1u << exp_bits);
    int k = 31 - cabac_clz32(v);
    uint32_t x = ((uint32_t)cabac_bypass_lut[k - exp_bits] << exp_bits) + v;
    k = 2 * k + 1 - exp_bits;
    int i = ((k - 1) & 7) + 1;
    do {
        k -= i;
        cb->i_low <<= i;
        cb->i_low += (int32_t)(((x >> k) & 0xff) * (uint32_t)cb->i_range);
        cb->i_queue += i;
        cabac_putbyte(cb);
        i = 8;
    } while (k > 0);
}

size_t cabac_encode_flush(cabac_engine_t *cb, uint32_t frame_count) {
    cb->i_low += cb->i_range - 2;
    cb->i_low |= 1;
    cb->i_low <<= 9;
    cb->i_queue += 9;
    cabac_putbyte(cb);
    cabac_putbyte(cb);
    cb->i_low <<= -cb->i_queue;
    /* Single don't-care bit, seeded from frame_count the same cosmetic way
     * x264 seeds it from h->i_frame - see cabac.h's doc comment on this
     * function: any value here is spec-legal. */
    cb->i_low |= (int32_t)((0x35a4e4f5u >> (frame_count & 31)) & 1) << 10;
    cb->i_queue = 0;
    cabac_putbyte(cb);

    while (cb->i_bytes_outstanding > 0) {
        if (cb->p >= cb->p_end) { cb->overflow = true; break; }
        *(cb->p++) = 0xff;
        cb->i_bytes_outstanding--;
    }
    return (size_t)(cb->p - cb->p_start);
}

/* ===========================================================================
 * Syntax element encoders - context INDEX assignments and ctxIdxInc formulas
 * adapted from x264's encoder/cabac.c (see cabac.h's provenance comment for
 * the exact function-by-function mapping). Control flow independently
 * written to this project's own call shape (plain parameters, no x264
 * macroblock-cache object).
 * ===========================================================================
 */

void cabac_write_mb_type_i16x16(cabac_engine_t *cb, int ctx_neighbor_intra_count,
                                 bool cbp_luma_nonzero, int cbp_chroma,
                                 int i16x16_pred_mode) {
    int ctx0 = 3 + ctx_neighbor_intra_count; /* 3, 4, or 5 */
    cabac_encode_decision(cb, ctx0, 1); /* not I_NxN/I_PCM */
    cabac_encode_terminal(cb);          /* not I_PCM (terminal-0 bin) */

    cabac_encode_decision(cb, 6, cbp_luma_nonzero ? 1 : 0);
    if (cbp_chroma == 0) {
        cabac_encode_decision(cb, 7, 0);
    } else {
        cabac_encode_decision(cb, 7, 1);
        cabac_encode_decision(cb, 8, cbp_chroma >> 1);
    }
    cabac_encode_decision(cb, 9, i16x16_pred_mode >> 1);
    cabac_encode_decision(cb, 10, i16x16_pred_mode & 1);
}

void cabac_write_intra_chroma_pred_mode(cabac_engine_t *cb, int ctx_neighbor, int mode) {
    cabac_encode_decision(cb, 64 + ctx_neighbor, mode > 0);
    if (mode > 0) {
        cabac_encode_decision(cb, 67, mode > 1);
        if (mode > 1) cabac_encode_decision(cb, 67, mode > 2);
    }
}

void cabac_write_mb_skip_p(cabac_engine_t *cb, int ctx_neighbor_not_skipped, bool skip) {
    cabac_encode_decision(cb, 11 + ctx_neighbor_not_skipped, skip ? 1 : 0);
}

void cabac_write_mb_type_p_l0_16x16(cabac_engine_t *cb) {
    cabac_encode_decision(cb, 14, 0); /* not intra */
    cabac_encode_decision(cb, 15, 0); /* D_16x16 (not 16x8/8x16) */
    cabac_encode_decision(cb, 16, 0); /* (unused bin value for D_16x16 -
                                          matches x264's cabac_mb_header_p:
                                          the D_16x16 path writes ctx15=0
                                          then ctx16=0 unconditionally) */
}

static const uint8_t cabac_mvd_unary_ctx[8] = { 3, 4, 5, 6, 6, 6, 6, 6 };

int cabac_write_mvd_component(cabac_engine_t *cb, int is_vertical, int ctx_amvd, int mvd) {
    int ctxbase = is_vertical ? 47 : 40;
    if (mvd == 0) {
        cabac_encode_decision(cb, ctxbase + ctx_amvd, 0);
        return 0;
    }
    int i_abs = mvd < 0 ? -mvd : mvd;
    cabac_encode_decision(cb, ctxbase + ctx_amvd, 1);
    if (i_abs < 9) {
        for (int i = 1; i < i_abs; i++)
            cabac_encode_decision(cb, ctxbase + cabac_mvd_unary_ctx[i - 1], 1);
        cabac_encode_decision(cb, ctxbase + cabac_mvd_unary_ctx[i_abs - 1], 0);
    } else {
        for (int i = 1; i < 9; i++)
            cabac_encode_decision(cb, ctxbase + cabac_mvd_unary_ctx[i - 1], 1);
        cabac_encode_ue_bypass(cb, 3, i_abs - 9);
    }
    cabac_encode_bypass(cb, mvd < 0 ? 1 : 0);
    return i_abs < 66 ? i_abs : 66;
}

void cabac_write_cbp_luma(cabac_engine_t *cb, int cbp_luma, int cbp_l, int cbp_t) {
    /* Signed ints: cbp_l/cbp_t == -1 (unavailable) arithmetic-shifts to
     * all-ones, so every masked bit below reads as 1 for an unavailable
     * neighbor - this is x264's exact (and spec-correct, ITU-T 9.3.3.1.1.4)
     * "unavailable defaults to condTermFlag=1" behavior for luma cbp,
     * reproduced here the same way (relying on plain twos-complement
     * arithmetic right shift, which is what every real-world C compiler
     * does for a negative signed int - not undefined per ISO C, just
     * implementation-defined, and universal in practice). */
    cabac_encode_decision(cb, 76 - ((cbp_l >> 1) & 1) - ((cbp_t >> 1) & 2), (cbp_luma >> 0) & 1);
    cabac_encode_decision(cb, 76 - ((cbp_luma >> 0) & 1) - ((cbp_t >> 2) & 2), (cbp_luma >> 1) & 1);
    cabac_encode_decision(cb, 76 - ((cbp_l >> 3) & 1) - ((cbp_luma << 1) & 2), (cbp_luma >> 2) & 1);
    cabac_encode_decision(cb, 76 - ((cbp_luma >> 2) & 1) - ((cbp_luma >> 0) & 2), (cbp_luma >> 3) & 1);
}

void cabac_write_cbp_chroma(cabac_engine_t *cb, int cbp_chroma, int cbp_l, int cbp_t) {
    int cbp_a = cbp_l & 0x30;
    int cbp_b = cbp_t & 0x30;
    int ctx = 0;
    if (cbp_a && cbp_l != -1) ctx++;
    if (cbp_b && cbp_t != -1) ctx += 2;
    if (cbp_chroma == 0) {
        cabac_encode_decision(cb, 77 + ctx, 0);
    } else {
        cabac_encode_decision(cb, 77 + ctx, 1);
        ctx = 4;
        if (cbp_a == 0x20) ctx++;
        if (cbp_b == 0x20) ctx += 2;
        cabac_encode_decision(cb, 77 + ctx, cbp_chroma >> 1);
    }
}

bool cabac_write_qp_delta(cabac_engine_t *cb, int dqp, bool last_dqp_nonzero) {
    int ctx = last_dqp_nonzero ? 1 : 0;
    if (dqp != 0) {
        int val = dqp > 0 ? (2 * dqp - 1) : (-2 * dqp);
        do {
            cabac_encode_decision(cb, 60 + ctx, 1);
            ctx = 2 + (ctx >> 1);
        } while (--val);
    }
    cabac_encode_decision(cb, 60 + ctx, 0);
    return dqp != 0;
}

/* Per-category tables (ITU-T Table 9-42 restricted to this project's 5
 * categories) - see this file's top-of-file context-index map. */
static const int cabac_count_m1[5]  = { 15, 14, 15, 3, 14 };
static const int cabac_sig_base[5]  = { 105, 120, 134, 149, 152 };
static const int cabac_last_base[5] = { 166, 181, 195, 210, 213 };
static const int cabac_level_base[5]= { 227, 237, 247, 257, 266 };
static const int cabac_cbf_base[5]  = { 85, 89, 93, 97, 101 };

int cabac_count_coeffs(cabac_ctx_block_cat_t cat) {
    return cabac_count_m1[cat] + 1;
}

void cabac_write_coded_block_flag(cabac_engine_t *cb, cabac_ctx_block_cat_t cat,
                                   int nA, int nB, bool cbf) {
    int ctx = cabac_cbf_base[cat] + 2 * (nB ? 1 : 0) + (nA ? 1 : 0);
    cabac_encode_decision(cb, ctx, cbf ? 1 : 0);
}

/* node ctx: 0..3 = abs-level-1 run (with abs-level>1 count==0);
 *           4..7 = abs-level>1 seen (+3) - see x264's encoder/cabac.c
 *           coeff_abs_level1_ctx/coeff_abs_levelgt1_ctx/
 *           coeff_abs_level_transition, copied verbatim. */
static const uint8_t cabac_level1_ctx[8]    = { 1, 2, 3, 4, 0, 0, 0, 0 };
static const uint8_t cabac_levelgt1_ctx[8]  = { 5, 5, 5, 5, 6, 7, 8, 9 };
static const uint8_t cabac_level_transition[2][8] = {
    { 1, 2, 3, 3, 4, 5, 6, 7 },
    { 4, 4, 4, 4, 5, 6, 7, 7 },
};

void cabac_write_residual_block(cabac_engine_t *cb, cabac_ctx_block_cat_t cat, const int *scanned) {
    int count_m1 = cabac_count_m1[cat];
    int ctx_sig = cabac_sig_base[cat];
    int ctx_last = cabac_last_base[cat];
    int ctx_level = cabac_level_base[cat];

    int last = -1;
    for (int i = count_m1; i >= 0; i--) {
        if (scanned[i] != 0) { last = i; break; }
    }
    if (last < 0) return; /* caller must gate this call on cbf==1 */

    int coeffs[16];
    int coeff_idx = -1;
    int i = 0;
    for (;;) {
        if (scanned[i] != 0) {
            coeffs[++coeff_idx] = scanned[i];
            cabac_encode_decision(cb, ctx_sig + i, 1);
            if (i == last) {
                cabac_encode_decision(cb, ctx_last + i, 1);
                break;
            } else {
                cabac_encode_decision(cb, ctx_last + i, 0);
            }
        } else {
            cabac_encode_decision(cb, ctx_sig + i, 0);
        }
        if (++i == count_m1) {
            coeffs[++coeff_idx] = scanned[i];
            break;
        }
    }

    int node_ctx = 0;
    do {
        int coeff = coeffs[coeff_idx];
        int abs_coeff = coeff < 0 ? -coeff : coeff;
        int sign = coeff < 0 ? 1 : 0;
        int ctx = cabac_level1_ctx[node_ctx] + ctx_level;

        if (abs_coeff > 1) {
            cabac_encode_decision(cb, ctx, 1);
            ctx = cabac_levelgt1_ctx[node_ctx] + ctx_level;
            int capped = abs_coeff < 15 ? abs_coeff : 15;
            for (int k = capped - 2; k > 0; k--)
                cabac_encode_decision(cb, ctx, 1);
            if (abs_coeff < 15)
                cabac_encode_decision(cb, ctx, 0);
            else
                cabac_encode_ue_bypass(cb, 0, abs_coeff - 15);
            node_ctx = cabac_level_transition[1][node_ctx];
        } else {
            cabac_encode_decision(cb, ctx, 0);
            node_ctx = cabac_level_transition[0][node_ctx];
        }
        cabac_encode_bypass(cb, sign);
    } while (--coeff_idx >= 0);
}
