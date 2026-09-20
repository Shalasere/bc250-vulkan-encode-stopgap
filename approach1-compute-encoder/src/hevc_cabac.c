/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_cabac.c - see hevc_cabac.h for the full provenance note (this file's
 * arithmetic-coder core and residual_coding() control flow is adapted from
 * x265's source/encoder/entropy.cpp + source/common/contexts.h +
 * source/common/constants.cpp, Copyright (C) 2013-2020 MulticoreWare, Inc,
 * licensed GPL-2.0-or-later - combination-compatible with this project's
 * GPL-3.0-only license via the "or any later version" grant).
 */
#include "hevc_cabac.h"
#include <string.h>

/* ===================== spec/x265 numeric tables ===================== */

/* CABAC state-transition table: g_hevc_next_state[state<<1|mps][bin].
 * Identical to x265's g_nextState[128][2] / the HEVC spec's Table 9-47. */
static const uint8_t g_hevc_next_state[128][2] = {
    { 2, 1 }, { 0, 3 }, { 4, 0 }, { 1, 5 }, { 6, 2 }, { 3, 7 }, { 8, 4 }, { 5, 9 },
    { 10, 4 }, { 5, 11 }, { 12, 8 }, { 9, 13 }, { 14, 8 }, { 9, 15 }, { 16, 10 }, { 11, 17 },
    { 18, 12 }, { 13, 19 }, { 20, 14 }, { 15, 21 }, { 22, 16 }, { 17, 23 }, { 24, 18 }, { 19, 25 },
    { 26, 18 }, { 19, 27 }, { 28, 22 }, { 23, 29 }, { 30, 22 }, { 23, 31 }, { 32, 24 }, { 25, 33 },
    { 34, 26 }, { 27, 35 }, { 36, 26 }, { 27, 37 }, { 38, 30 }, { 31, 39 }, { 40, 30 }, { 31, 41 },
    { 42, 32 }, { 33, 43 }, { 44, 32 }, { 33, 45 }, { 46, 36 }, { 37, 47 }, { 48, 36 }, { 37, 49 },
    { 50, 38 }, { 39, 51 }, { 52, 38 }, { 39, 53 }, { 54, 42 }, { 43, 55 }, { 56, 42 }, { 43, 57 },
    { 58, 44 }, { 45, 59 }, { 60, 44 }, { 45, 61 }, { 62, 46 }, { 47, 63 }, { 64, 48 }, { 49, 65 },
    { 66, 48 }, { 49, 67 }, { 68, 50 }, { 51, 69 }, { 70, 52 }, { 53, 71 }, { 72, 52 }, { 53, 73 },
    { 74, 54 }, { 55, 75 }, { 76, 54 }, { 55, 77 }, { 78, 56 }, { 57, 79 }, { 80, 58 }, { 59, 81 },
    { 82, 58 }, { 59, 83 }, { 84, 60 }, { 61, 85 }, { 86, 60 }, { 61, 87 }, { 88, 60 }, { 61, 89 },
    { 90, 62 }, { 63, 91 }, { 92, 64 }, { 65, 93 }, { 94, 64 }, { 65, 95 }, { 96, 66 }, { 67, 97 },
    { 98, 66 }, { 67, 99 }, { 100, 66 }, { 67, 101 }, { 102, 68 }, { 69, 103 }, { 104, 68 }, { 69, 105 },
    { 106, 70 }, { 71, 107 }, { 108, 70 }, { 71, 109 }, { 110, 70 }, { 71, 111 }, { 112, 72 }, { 73, 113 },
    { 114, 72 }, { 73, 115 }, { 116, 72 }, { 73, 117 }, { 118, 74 }, { 75, 119 }, { 120, 74 }, { 75, 121 },
    { 122, 74 }, { 75, 123 }, { 124, 76 }, { 77, 125 }, { 124, 76 }, { 77, 125 }, { 126, 126 }, { 127, 127 }
};

/* LPS range table: g_hevc_lps_range[state][(range>>6)&3]. */
static const uint8_t g_hevc_lps_range[64][4] = {
    { 128, 176, 208, 240 }, { 128, 167, 197, 227 }, { 128, 158, 187, 216 }, { 123, 150, 178, 205 },
    { 116, 142, 169, 195 }, { 111, 135, 160, 185 }, { 105, 128, 152, 175 }, { 100, 122, 144, 166 },
    {  95, 116, 137, 158 }, {  90, 110, 130, 150 }, {  85, 104, 123, 142 }, {  81,  99, 117, 135 },
    {  77,  94, 111, 128 }, {  73,  89, 105, 122 }, {  69,  85, 100, 116 }, {  66,  80,  95, 110 },
    {  62,  76,  90, 104 }, {  59,  72,  86,  99 }, {  56,  69,  81,  94 }, {  53,  65,  77,  89 },
    {  51,  62,  73,  85 }, {  48,  59,  69,  80 }, {  46,  56,  66,  76 }, {  43,  53,  63,  72 },
    {  41,  50,  59,  69 }, {  39,  48,  56,  65 }, {  37,  45,  54,  62 }, {  35,  43,  51,  59 },
    {  33,  41,  48,  56 }, {  32,  39,  46,  53 }, {  30,  37,  43,  50 }, {  29,  35,  41,  48 },
    {  27,  33,  39,  45 }, {  26,  31,  37,  43 }, {  24,  30,  35,  41 }, {  23,  28,  33,  39 },
    {  22,  27,  32,  37 }, {  21,  26,  30,  35 }, {  20,  24,  29,  33 }, {  19,  23,  27,  31 },
    {  18,  22,  26,  30 }, {  17,  21,  25,  28 }, {  16,  20,  23,  27 }, {  15,  19,  22,  25 },
    {  14,  18,  21,  24 }, {  14,  17,  20,  23 }, {  13,  16,  19,  22 }, {  12,  15,  18,  21 },
    {  12,  14,  17,  20 }, {  11,  14,  16,  19 }, {  11,  13,  15,  18 }, {  10,  12,  15,  17 },
    {  10,  12,  14,  16 }, {   9,  11,  13,  15 }, {   9,  11,  12,  14 }, {   8,  10,  12,  14 },
    {   8,   9,  11,  13 }, {   7,   9,  11,  12 }, {   7,   9,  10,  12 }, {   7,   8,  10,  11 },
    {   6,   8,   9,  11 }, {   6,   7,   9,  10 }, {   6,   7,   8,   9 }, {   2,   2,   2,   2 }
};

/* 4x4 scan tables: scan position -> raster index (row*4+col). Index 0 =
 * diagonal (up-right), 1 = horizontal, 2 = vertical - matches x265's
 * SCAN_DIAG/SCAN_HOR/SCAN_VER numbering, which hevc_intra.c's
 * hevc_scan_idx_for_mode() also follows. */
static const uint8_t g_hevc_scan4x4[3][16] = {
    { 0,  4,  1,  8,  5,  2, 12,  9,  6,  3, 13, 10,  7, 14, 11, 15 },
    { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
    { 0,  4,  8, 12,  1,  5,  9, 13,  2,  6, 10, 14,  3,  7, 11, 15 }
};

/* (The 4-entry last-position table this used to carry is superseded by
 * the full g_group_idx/g_min_in_group pair further down, which covers
 * every position up to 31 and the bypass suffix that positions above 3
 * require.) */

/* sig_coeff_flag context increment for a lone 4x4 coefficient group,
 * indexed directly by raster block position (0-15) - x265's
 * table_cnt[4][...] (the "4x4" special case of its 5-entry pattern table). */
static const uint8_t g_hevc_sig_ctx4[16] = {
    0, 1, 4, 5,
    2, 3, 4, 5,
    6, 6, 8, 8,
    7, 7, 8, 8
};

#define COEF_REMAIN_BIN_REDUCTION 3
#define C1FLAG_NUMBER             8

/* ===================== I-slice context init values ===================== */
/* These are row index 2 ("I_SLICE") of x265's INIT_* tables in entropy.cpp -
 * this encoder only ever codes I-slices, so the other two rows (B/P) are
 * never needed and are not reproduced here. */

static const uint8_t INIT_SPLIT_FLAG[3]      = { 139, 141, 157 };
static const uint8_t INIT_PART_SIZE          = 184; /* only ctx index 0 used */
static const uint8_t INIT_INTRA_PRED_MODE    = 184;
static const uint8_t INIT_CHROMA_PRED_MODE[2] = { 63, 139 };
static const uint8_t INIT_QT_CBF[7]          = { 111, 141, 94, 138, 182, 154, 154 };
static const uint8_t INIT_SIG_FLAG[42] = {
    111, 111, 125, 110, 110,  94, 124, 108, 124, 107, 125, 141, 179, 153, 125,
    107, 125, 141, 179, 153, 125, 107, 125, 141, 179, 153, 125, 140, 139, 182,
    182, 152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111
};
static const uint8_t INIT_LAST[18] = {
    110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111, 79, 108, 123, 63
};
static const uint8_t INIT_ONE_FLAG[24] = {
    140,  92, 137, 138, 140, 152, 138, 139, 153,  74, 149,  92,
    139, 107, 122, 152, 140, 179, 166, 182, 140, 227, 122, 197
};
static const uint8_t INIT_ABS_FLAG[6] = { 138, 153, 136, 167, 152, 152 };
/* Both banks are new with transform sizes above 4x4: a lone 4x4 TU is
 * exactly one coefficient group (so coded_sub_block_flag is inferred)
 * inside a transform tree whose only split is inferred too. I-slice
 * (initType 0) rows of Rec. ITU-T H.265 Tables 9-25 and 9-30. */
static const uint8_t INIT_SIG_CG_FLAG[4]     = { 91, 171, 134, 141 };
static const uint8_t INIT_TRANS_SUBDIV[3]    = { 153, 138, 138 };

/* ===================== context init formula (Rec. ITU-T H.265 9.3.2.2) === */

static uint8_t hevc_sbac_init_state(int qp, int init_value) {
    if (qp < 0) qp = 0;
    if (qp > 51) qp = 51;
    int slope = (init_value >> 4) * 5 - 45;
    int offset = ((init_value & 15) << 3) - 16;
    int init_state = ((slope * qp) >> 4) + offset;
    if (init_state < 1) init_state = 1;
    if (init_state > 126) init_state = 126;
    uint32_t mps = (uint32_t)(init_state >= 64);
    uint32_t state = ((mps ? (uint32_t)(init_state - 64) : (uint32_t)(63 - init_state)) << 1) + mps;
    return (uint8_t)state;
}

static void init_bank(uint8_t *ctx, const uint8_t *init_values, int count, int qp) {
    for (int i = 0; i < count; i++) ctx[i] = hevc_sbac_init_state(qp, init_values[i]);
}

void hevc_cabac_reset_contexts(hevc_cabac_t *cb, int slice_qp) {
    init_bank(&cb->ctx[HEVC_CTX_SPLIT_FLAG], INIT_SPLIT_FLAG, 3, slice_qp);
    cb->ctx[HEVC_CTX_PART_SIZE] = hevc_sbac_init_state(slice_qp, INIT_PART_SIZE);
    cb->ctx[HEVC_CTX_INTRA_PRED] = hevc_sbac_init_state(slice_qp, INIT_INTRA_PRED_MODE);
    init_bank(&cb->ctx[HEVC_CTX_CHROMA_PRED], INIT_CHROMA_PRED_MODE, 2, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_QT_CBF], INIT_QT_CBF, 7, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_SIG_FLAG], INIT_SIG_FLAG, 42, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_LAST_X], INIT_LAST, 18, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_LAST_Y], INIT_LAST, 18, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_ONE_FLAG], INIT_ONE_FLAG, 24, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_ABS_FLAG], INIT_ABS_FLAG, 6, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_SIG_CG], INIT_SIG_CG_FLAG, 4, slice_qp);
    init_bank(&cb->ctx[HEVC_CTX_TRANS_SUBDIV], INIT_TRANS_SUBDIV, 3, slice_qp);
}

/* ===================== arithmetic coder core ===================== */

void hevc_cabac_init(hevc_cabac_t *cb, bitstream_t *bs) {
    memset(cb, 0, sizeof(*cb));
    cb->bs = bs;
}

void hevc_cabac_start(hevc_cabac_t *cb) {
    cb->low = 0;
    cb->range = 510;
    cb->bits_left = -12;
    cb->num_buffered_bytes = 0;
    cb->buffered_byte = 0xff;
}

static void cabac_put_byte(hevc_cabac_t *cb, uint8_t b) {
    bs_write_u(cb->bs, 8, b);
}

static void cabac_write_out(hevc_cabac_t *cb) {
    uint32_t lead_byte = cb->low >> (13 + cb->bits_left);
    uint32_t low_mask = (uint32_t)(~0u) >> (11 + 8 - cb->bits_left);

    cb->bits_left -= 8;
    cb->low &= low_mask;

    if (lead_byte == 0xff) {
        cb->num_buffered_bytes++;
    } else {
        if (cb->num_buffered_bytes > 0) {
            uint32_t carry = lead_byte >> 8;
            uint32_t byte_to_write = cb->buffered_byte + carry;
            cabac_put_byte(cb, (uint8_t)byte_to_write);

            byte_to_write = (0xff + carry) & 0xff;
            while (cb->num_buffered_bytes > 1) {
                cabac_put_byte(cb, (uint8_t)byte_to_write);
                cb->num_buffered_bytes--;
            }
        }
        cb->num_buffered_bytes = 1;
        cb->buffered_byte = lead_byte;
    }
}

void hevc_cabac_encode_bin(hevc_cabac_t *cb, int ctx_idx, uint32_t bin) {
    uint32_t mstate = cb->ctx[ctx_idx];
    cb->ctx[ctx_idx] = g_hevc_next_state[mstate][bin & 1];

    uint32_t range = cb->range;
    uint32_t state = mstate >> 1;
    uint32_t lps = g_hevc_lps_range[state][(range >> 6) & 3];
    range -= lps;

    int num_bits = (int)(((uint32_t)(range - 256)) >> 31);
    uint32_t low = cb->low;

    if ((bin ^ mstate) & 1u) {
        unsigned idx = 31u - (unsigned)__builtin_clz(lps);
        num_bits = (int)(8 - idx);
        if (state >= 63) num_bits = 6;
        low += range;
        range = lps;
    }
    cb->low = low << num_bits;
    cb->range = range << num_bits;
    cb->bits_left += num_bits;
    if (cb->bits_left >= 0) cabac_write_out(cb);
}

void hevc_cabac_encode_bypass(hevc_cabac_t *cb, uint32_t bin) {
    cb->low <<= 1;
    if (bin) cb->low += cb->range;
    cb->bits_left++;
    if (cb->bits_left >= 0) cabac_write_out(cb);
}

void hevc_cabac_encode_bypass_bins(hevc_cabac_t *cb, uint32_t value, int num_bins) {
    while (num_bins > 8) {
        num_bins -= 8;
        uint32_t pattern = value >> num_bins;
        cb->low <<= 8;
        cb->low += cb->range * pattern;
        value -= pattern << num_bins;
        cb->bits_left += 8;
        if (cb->bits_left >= 0) cabac_write_out(cb);
    }
    cb->low <<= num_bins;
    cb->low += cb->range * value;
    cb->bits_left += num_bins;
    if (cb->bits_left >= 0) cabac_write_out(cb);
}

void hevc_cabac_encode_terminate(hevc_cabac_t *cb, uint32_t bin) {
    cb->range -= 2;
    if (bin) {
        cb->low += cb->range;
        cb->low <<= 7;
        cb->range = 2 << 7;
        cb->bits_left += 7;
    } else if (cb->range >= 256) {
        return;
    } else {
        cb->low <<= 1;
        cb->range <<= 1;
        cb->bits_left++;
    }
    if (cb->bits_left >= 0) cabac_write_out(cb);
}

void hevc_cabac_finish(hevc_cabac_t *cb) {
    if (cb->low >> (21 + cb->bits_left)) {
        cabac_put_byte(cb, (uint8_t)(cb->buffered_byte + 1));
        while (cb->num_buffered_bytes > 1) {
            cabac_put_byte(cb, 0x00);
            cb->num_buffered_bytes--;
        }
        cb->low -= 1u << (21 + cb->bits_left);
    } else {
        if (cb->num_buffered_bytes > 0)
            cabac_put_byte(cb, (uint8_t)cb->buffered_byte);
        while (cb->num_buffered_bytes > 1) {
            cabac_put_byte(cb, 0xff);
            cb->num_buffered_bytes--;
        }
    }
    /* Emit the remaining (13 + bits_left) bits of low, MSB-first - same as
     * x265's m_bitIf->write(m_low>>8, 13+m_bitsLeft). This is generally
     * NOT a whole number of bytes, so it goes through bs_write_u() (the
     * same bit-level accumulator CAVLC uses) rather than cabac_put_byte();
     * the caller finishes byte-alignment with bs_rbsp_trailing_bits() on
     * the same bitstream_t right after this call (see encoder_h265.c). */
    int nbits = 13 + cb->bits_left;
    if (nbits > 0)
        bs_write_u(cb->bs, nbits, cb->low >> 8);
}

/* ===================== syntax element wrappers ===================== */

void hevc_cabac_code_split_cu_flag(hevc_cabac_t *cb, int bin, int ctx_inc) {
    hevc_cabac_encode_bin(cb, HEVC_CTX_SPLIT_FLAG + ctx_inc, (uint32_t)(bin ? 1 : 0));
}

void hevc_cabac_code_part_mode_intra(hevc_cabac_t *cb, int is_2nx2n) {
    hevc_cabac_encode_bin(cb, HEVC_CTX_PART_SIZE, (uint32_t)(is_2nx2n ? 1 : 0));
}

int hevc_cabac_code_intra_luma_flag(hevc_cabac_t *cb, int mode, const int mpm[3]) {
    int pred_idx = -1;
    for (int i = 0; i < 3; i++) {
        if (mode == mpm[i]) { pred_idx = i; break; }
    }
    hevc_cabac_encode_bin(cb, HEVC_CTX_INTRA_PRED, (uint32_t)(pred_idx != -1 ? 1 : 0));
    return pred_idx;
}

void hevc_cabac_code_intra_luma_data(hevc_cabac_t *cb, int mode, int pred_idx, const int mpm_in[3]) {
    if (pred_idx != -1) {
        int nonzero = (pred_idx != 0);
        hevc_cabac_encode_bypass_bins(cb, (uint32_t)(pred_idx + nonzero), 1 + nonzero);
    } else {
        int mpm[3] = { mpm_in[0], mpm_in[1], mpm_in[2] };
        if (mpm[0] > mpm[1]) { int t = mpm[0]; mpm[0] = mpm[1]; mpm[1] = t; }
        if (mpm[0] > mpm[2]) { int t = mpm[0]; mpm[0] = mpm[2]; mpm[2] = t; }
        if (mpm[1] > mpm[2]) { int t = mpm[1]; mpm[1] = mpm[2]; mpm[2] = t; }
        int dir = mode;
        dir += (dir > mpm[2]) ? -1 : 0;
        dir += (dir > mpm[1]) ? -1 : 0;
        dir += (dir > mpm[0]) ? -1 : 0;
        hevc_cabac_encode_bypass_bins(cb, (uint32_t)dir, 5);
    }
}

int hevc_chroma_mode_from_idx(int idx, int luma_mode_pu0) {
    /* Table 8-2's modeIdx -> IntraPredModeC candidate list. */
    static const int cand[4] = { 0 /*Planar*/, 26 /*Vertical*/, 10 /*Horizontal*/, 1 /*DC*/ };
    if (idx == 4) return luma_mode_pu0;          /* DM_CHROMA */
    /* 8.4.3: a candidate that collides with the luma mode would be
     * redundant (DM_CHROMA already reaches it), so the spec substitutes
     * mode 34 in that slot. */
    return (cand[idx] == luma_mode_pu0) ? 34 : cand[idx];
}

void hevc_cabac_code_intra_chroma_pred_mode(hevc_cabac_t *cb, int idx) {
    if (idx == 4) {
        hevc_cabac_encode_bin(cb, HEVC_CTX_CHROMA_PRED, 0);   /* DM_CHROMA */
    } else {
        hevc_cabac_encode_bin(cb, HEVC_CTX_CHROMA_PRED, 1);
        hevc_cabac_encode_bypass_bins(cb, (uint32_t)idx, 2);
    }
}

void hevc_cabac_code_cbf_luma(hevc_cabac_t *cb, int cbf, int trafo_depth) {
    int ctx = (trafo_depth == 0) ? 1 : 0;
    hevc_cabac_encode_bin(cb, HEVC_CTX_QT_CBF + ctx, (uint32_t)(cbf ? 1 : 0));
}

void hevc_cabac_code_cbf_chroma(hevc_cabac_t *cb, int cbf, int trafo_depth) {
    int ctx = 2 + trafo_depth;
    hevc_cabac_encode_bin(cb, HEVC_CTX_QT_CBF + ctx, (uint32_t)(cbf ? 1 : 0));
}

/* ===================== residual_coding() for one 4x4 TU ===================== */

static void write_coef_remain_exp_golomb(hevc_cabac_t *cb, uint32_t code_number, uint32_t rice) {
    uint32_t code_remain = code_number & ((1u << rice) - 1);
    if ((code_number >> rice) < COEF_REMAIN_BIN_REDUCTION) {
        uint32_t length = code_number >> rice;
        hevc_cabac_encode_bypass_bins(cb, (((1u << (length + 1)) - 2) << rice) + code_remain,
                                       (int)(length + 1 + rice));
    } else {
        uint32_t cn = (code_number >> rice) - COEF_REMAIN_BIN_REDUCTION;
        unsigned idx = 31u - (unsigned)__builtin_clz(cn + 1);
        uint32_t length = idx;
        cn -= (1u << idx) - 1;
        cn = (cn << rice) + code_remain;
        hevc_cabac_encode_bypass_bins(cb, (1u << (COEF_REMAIN_BIN_REDUCTION + length + 1)) - 2,
                                       (int)(COEF_REMAIN_BIN_REDUCTION + length + 1));
        hevc_cabac_encode_bypass_bins(cb, cn, (int)(length + rice));
    }
}

/* ===================== residual_coding() for any TU size ===================== */

/* Rec. ITU-T H.265 Table 9-40's group index / minimum-in-group tables for
 * last_sig_coeff_{x,y}. A position's prefix is its group; groups above 3
 * carry a fixed-length bypass suffix selecting within the group. */
static const uint8_t g_group_idx[32] = {
    0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9
};
static const uint8_t g_min_in_group[10] = { 0, 1, 2, 3, 4, 6, 8, 12, 16, 24 };

/* Rec. ITU-T H.265 6.5.3 up-right diagonal scan, plus the horizontal and
 * vertical scans, generated for a blk x blk grid into raster indices.
 * Used for the coefficient-group grid (up to 8x8 for a 32x32 TU); the
 * within-group 4x4 scans stay the static g_hevc_scan4x4 tables. */
static void build_scan(int blk, int scan_idx, uint8_t *out) {
    int i = 0;
    if (scan_idx == 1) {                       /* horizontal */
        for (int y = 0; y < blk; y++)
            for (int x = 0; x < blk; x++) out[i++] = (uint8_t)(y * blk + x);
        return;
    }
    if (scan_idx == 2) {                       /* vertical */
        for (int x = 0; x < blk; x++)
            for (int y = 0; y < blk; y++) out[i++] = (uint8_t)(y * blk + x);
        return;
    }
    int x = 0, y = 0;
    while (i < blk * blk) {
        while (y >= 0) {
            if (x < blk && y < blk) out[i++] = (uint8_t)(y * blk + x);
            y--; x++;
        }
        y = x; x = 0;
    }
}

/* 9.3.4.2.5 sig_coeff_flag context, for transform sizes above 4x4. */
static int sig_ctx_large(int xc, int yc, int log2_size, int is_luma, int scan_idx,
                          const uint8_t *csbf, int cg_blk) {
    if (xc + yc == 0) return is_luma ? 0 : 27;   /* DC always its own context */

    int xs = xc >> 2, ys = yc >> 2;
    int prev = 0;
    if (xs < cg_blk - 1) prev += csbf[ys * cg_blk + xs + 1];
    if (ys < cg_blk - 1) prev += csbf[(ys + 1) * cg_blk + xs] << 1;

    int xp = xc & 3, yp = yc & 3;
    int sig;
    if (prev == 0)      sig = (xp + yp == 0) ? 2 : (xp + yp < 3) ? 1 : 0;
    else if (prev == 1) sig = (yp == 0) ? 2 : (yp == 1) ? 1 : 0;
    else if (prev == 2) sig = (xp == 0) ? 2 : (xp == 1) ? 1 : 0;
    else                sig = 2;

    if (is_luma) {
        if (xs + ys > 0) sig += 3;
        sig += (log2_size == 3) ? ((scan_idx == 0) ? 9 : 15) : 21;
        return sig;
    }
    sig += (log2_size == 3) ? 9 : 12;
    return 27 + sig;
}

void hevc_cabac_code_residual(hevc_cabac_t *cb, const int16_t *coeff, int log2_size,
                               int is_luma, int scan_idx) {
    int n = 1 << log2_size;
    int log2_cg = log2_size - 2;
    int cg_blk = 1 << log2_cg;              /* coefficient groups per side */
    int num_cg = cg_blk * cg_blk;
    const uint8_t *sub_scan = g_hevc_scan4x4[scan_idx];   /* within a 4x4 group */
    uint8_t cg_scan[64];
    /* A 4x4 TU is a single group; skipping the scan build and touching
     * only the one csbf entry matters because that is by far the most
     * common size - paying a 64-byte clear and a scan construction per
     * transform block cost ~30% of the frame when this replaced the
     * 4x4-only coder. */
    if (num_cg == 1) cg_scan[0] = 0; else build_scan(cg_blk, scan_idx, cg_scan);

    /* ---- locate the last significant coefficient in scan order ---- */
    int last_cg = -1, last_sp = -1;
    for (int ci = num_cg - 1; ci >= 0 && last_cg < 0; ci--) {
        int cgr = cg_scan[ci];
        int cx = (cgr & (cg_blk - 1)) * 4, cy = (cgr >> log2_cg) * 4;
        for (int sp = 15; sp >= 0; sp--) {
            int r = sub_scan[sp];
            int xc = cx + (r & 3), yc = cy + (r >> 2);
            if (coeff[yc * n + xc]) { last_cg = ci; last_sp = sp; break; }
        }
    }
    if (last_cg < 0) return;   /* caller must not call this when cbf == 0 */

    int last_cgr = cg_scan[last_cg];
    int last_r = sub_scan[last_sp];
    int last_x = (last_cgr & (cg_blk - 1)) * 4 + (last_r & 3);
    int last_y = (last_cgr >> log2_cg) * 4 + (last_r >> 2);

    /* ---- last_sig_coeff_{x,y}_prefix / _suffix (9.3.4.2.3) ---- */
    {
        int px = last_x, py = last_y;
        if (scan_idx == 2) { int t = px; px = py; py = t; }  /* vertical swaps axes */

        int offset, shift;
        if (is_luma) {
            offset = 3 * (log2_size - 2) + ((log2_size - 1) >> 2);
            shift  = (log2_size + 1) >> 2;
        } else {
            offset = 15;
            shift  = log2_size - 2;
        }
        int max_group = (log2_size << 1) - 1;
        int pos[2] = { px, py };
        for (int i = 0; i < 2; i++) {
            int bank = (i == 0) ? HEVC_CTX_LAST_X : HEVC_CTX_LAST_Y;
            int group = g_group_idx[pos[i]];
            for (int b = 0; b < group; b++)
                hevc_cabac_encode_bin(cb, bank + offset + (b >> shift), 1);
            if (group < max_group)
                hevc_cabac_encode_bin(cb, bank + offset + (group >> shift), 0);
        }
        /* Suffixes are bypass and both follow both prefixes. */
        for (int i = 0; i < 2; i++) {
            int group = g_group_idx[pos[i]];
            if (group > 3) {
                int len = (group >> 1) - 1;
                hevc_cabac_encode_bypass_bins(cb, (uint32_t)(pos[i] - g_min_in_group[group]), len);
            }
        }
    }

    /* ---- coefficient groups, from the last one down to DC ---- */
    uint8_t csbf[64];
    memset(csbf, 0, (size_t)num_cg);
    csbf[last_cgr] = 1;

    int sig_base = is_luma ? 0 : 27;
    int one_bank = HEVC_CTX_ONE_FLAG + (is_luma ? 0 : 16);
    int abs_bank = HEVC_CTX_ABS_FLAG + (is_luma ? 0 : 4);
    uint32_t c1 = 1;   /* carries ACROSS groups - this is what ctxSet reads */

    for (int ci = last_cg; ci >= 0; ci--) {
        int cgr = cg_scan[ci];
        int xs = cgr & (cg_blk - 1), ys = cgr >> log2_cg;
        int cx = xs * 4, cy = ys * 4;

        /* coded_sub_block_flag (9.3.4.2.4). The group holding the last
         * significant coefficient is inferred 1, and so is the DC group. */
        /* 7.3.8.11 codes the flag only for groups that are neither the
         * last nor the DC group; both of those are inferred to 1 by
         * 7.4.9.11. Whether it was CODED (not merely 1) is what arms the
         * DC-coefficient inference below, so track it explicitly. */
        int explicit_csbf = (ci != last_cg && ci != 0);
        int coded;
        if (!explicit_csbf) {
            coded = 1;
            csbf[cgr] = 1;
        } else {
            coded = 0;
            for (int sp = 0; sp < 16 && !coded; sp++) {
                int r = sub_scan[sp];
                if (coeff[(cy + (r >> 2)) * n + cx + (r & 3)]) coded = 1;
            }
            int ctx = 0;
            if (xs < cg_blk - 1) ctx += csbf[ys * cg_blk + xs + 1];
            if (ys < cg_blk - 1) ctx += csbf[(ys + 1) * cg_blk + xs];
            if (ctx > 1) ctx = 1;
            hevc_cabac_encode_bin(cb, HEVC_CTX_SIG_CG + ctx + (is_luma ? 0 : 2), (uint32_t)coded);
            csbf[cgr] = (uint8_t)coded;
            if (!coded) continue;
        }

        /* ---- sig_coeff_flag over this group ---- */
        int16_t abs_coeff[16], sign[16];
        int num_nonzero = 0;
        int start = (ci == last_cg) ? last_sp - 1 : 15;
        /* The last-position coefficient is inferred significant. */
        if (ci == last_cg) {
            int v = coeff[last_y * n + last_x];
            abs_coeff[0] = (int16_t)(v < 0 ? -v : v);
            sign[0] = (int16_t)(v < 0);
            num_nonzero = 1;
        }
        for (int sp = start; sp >= 0; sp--) {
            int r = sub_scan[sp];
            int xc = cx + (r & 3), yc = cy + (r >> 2);
            int v = coeff[yc * n + xc];
            int sig = (v != 0);
            /* inferSbDcSigCoeffFlag (7.3.8.11): a group whose
             * coded_sub_block_flag was explicitly coded as 1 must contain
             * something, so if every other coefficient signalled zero the
             * DC one is inferred significant and not coded. This applies
             * ONLY to explicitly-coded groups - the DC group's flag is
             * inferred rather than coded, so its own DC coefficient is
             * always signalled and may legitimately be zero. */
            int infer = (sp == 0 && num_nonzero == 0 && explicit_csbf);
            if (!infer) {
                int ctx = (log2_size == 2)
                        ? (sig_base + g_hevc_sig_ctx4[r])
                        : sig_ctx_large(xc, yc, log2_size, is_luma, scan_idx, csbf, cg_blk);
                hevc_cabac_encode_bin(cb, HEVC_CTX_SIG_FLAG + ctx, (uint32_t)sig);
            }
            if (sig || infer) {
                abs_coeff[num_nonzero] = (int16_t)(v < 0 ? -v : v);
                sign[num_nonzero] = (int16_t)(v < 0);
                num_nonzero++;
            }
        }
        if (!num_nonzero) continue;

        /* ---- ctxSet (9.3.4.2.6): depends on whether this is the DC
         * group and on whether the PREVIOUS group had a level above 1,
         * which is what makes c1 carry across groups. ---- */
        int ctx_set = (ci > 0 && is_luma) ? 2 : 0;
        if (c1 == 0) ctx_set++;
        c1 = 1;

        int one_base = one_bank + ctx_set * 4;
        int abs_base = abs_bank + ctx_set;

        uint32_t c1_next = 0xFFFFFFFEu;
        int first_c2_idx = 8, first_c2_flag = 2;
        int num_c1_flag = num_nonzero < C1FLAG_NUMBER ? num_nonzero : C1FLAG_NUMBER;
        for (int idx = 0; idx < num_c1_flag; idx++) {
            int symbol1 = abs_coeff[idx] > 1;
            int symbol2 = abs_coeff[idx] > 2;
            hevc_cabac_encode_bin(cb, one_base + (int)c1, (uint32_t)symbol1);
            if (symbol1) c1_next = 0;
            if (symbol1 + first_c2_flag == 3) first_c2_flag = symbol2;
            if (symbol1 + first_c2_idx == 9) first_c2_idx = idx;
            c1 = c1_next & 3;
            c1_next >>= 2;
        }
        if (!c1)
            hevc_cabac_encode_bin(cb, abs_base, (uint32_t)first_c2_flag);

        for (int idx = 0; idx < num_nonzero; idx++)
            hevc_cabac_encode_bypass(cb, (uint32_t)sign[idx]);

        if (!c1 || num_nonzero > C1FLAG_NUMBER) {
            uint32_t go_rice = 0;          /* reset per group, per spec */
            int base_level = 3;
            uint32_t threshold = COEF_REMAIN_BIN_REDUCTION;
            int idx = first_c2_idx;
            do {
                if (idx >= C1FLAG_NUMBER) base_level = 1;
                if ((uint32_t)abs_coeff[idx] >= (uint32_t)base_level) {
                    write_coef_remain_exp_golomb(cb, (uint32_t)(abs_coeff[idx] - base_level), go_rice);
                    int adjust = (abs_coeff[idx] > (int)threshold) && (go_rice <= 3);
                    if (adjust) { go_rice++; threshold += threshold; }
                }
                base_level = 2;
                idx++;
            } while (idx < num_nonzero);
        }
    }
}

void hevc_cabac_code_split_transform_flag(hevc_cabac_t *cb, int split, int log2_size) {
    hevc_cabac_encode_bin(cb, HEVC_CTX_TRANS_SUBDIV + (5 - log2_size), (uint32_t)(split ? 1 : 0));
}

void hevc_cabac_code_residual_4x4(hevc_cabac_t *cb, const int16_t coeff[16],
                                   int is_luma, int scan_idx) {
    /* Now just the nTbS == 4 case of the general coder. Keeping a second
     * hand-written 4x4 implementation would mean the size this project
     * has actually validated against a real decoder was NOT the code
     * path the other sizes exercise - so the general coder is used at
     * every size, and the existing end-to-end test becomes a regression
     * test for it. */
    hevc_cabac_code_residual(cb, coeff, 2, is_luma, scan_idx);
}
