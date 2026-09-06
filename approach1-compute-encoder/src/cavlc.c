/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: MIT
 *
 * cavlc.c - Spec-compliant H.264 CAVLC entropy encoder
 *           ITU-T Recommendation H.264 (04/2017) Section 9.2
 */
#include "cavlc.h"
#include <stdlib.h>
#include <string.h>

static inline void bs_write_bit(bitstream_t *bs, uint32_t val) {
    bs_write1(bs, val);
}

static inline void bs_write_bits(bitstream_t *bs, int bits, uint32_t val) {
    bs_write_u(bs, bits, val);
}

/* H.264 Zigzag scan order for 4x4 block */
static const int zigzag_4x4[16] = {
     0,  1,  4,  8,
     5,  2,  3,  6,
     9, 12, 13, 10,
     7, 11, 14, 15
};

/* Table 9-4: Assignment of codeNum to coded_block_pattern for Inter MBs */
static const uint8_t cbp_inter_table[48] = {
    0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13,
    14, 6, 9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};

static uint32_t map_inter_cbp(int cbp) {
    for (uint32_t i = 0; i < 48; i++) {
        if (cbp_inter_table[i] == (uint8_t)cbp) return i;
    }
    return (uint32_t)cbp;
}

void cavlc_write_mb_i16x16_header(bitstream_t *bs, int pred_mode, int cbp_chroma, int cbp_luma, int qp_delta) {
    if (!bs) return;
    if (pred_mode < 0 || pred_mode > 3) pred_mode = 2; /* DC default */
    if (cbp_chroma < 0 || cbp_chroma > 2) cbp_chroma = 0;
    int cbp_luma_flag = (cbp_luma != 0) ? 1 : 0;

    /* Table 7-11: mb_type 1..24 */
    int mb_type = 1 + pred_mode + (cbp_chroma * 4) + (cbp_luma_flag * 12);
    bs_write_ue(bs, (uint32_t)mb_type);

    /* Intra chroma prediction mode: 0 (DC) */
    bs_write_ue(bs, 0);

    /* mb_qp_delta if any residual coefficients exist */
    if (cbp_luma_flag || cbp_chroma > 0) {
        bs_write_se(bs, qp_delta);
    }
}

void cavlc_write_p_skip_run(bitstream_t *bs, uint32_t skip_run) {
    if (!bs || skip_run == 0) return;
    bs_write_ue(bs, skip_run);
}

void cavlc_write_mb_p16x16_header(bitstream_t *bs, int mvd_x, int mvd_y, int cbp, int qp_delta) {
    if (!bs) return;
    /* mb_skip_run = 0 (this macroblock is NOT skipped) */
    bs_write_ue(bs, 0);

    /* mb_type = 0 (P_L0_16x16) */
    bs_write_ue(bs, 0);

    /* Motion vector difference mvd_l0[0][0] */
    bs_write_se(bs, mvd_x);
    bs_write_se(bs, mvd_y);

    /* Coded block pattern mapped for Inter */
    bs_write_ue(bs, map_inter_cbp(cbp));

    if (cbp > 0) {
        bs_write_se(bs, qp_delta);
    }
}

/* Table 9-5: coeff_token VLC length and bit tables per ITU-T H.264
 * Index: [vlc_table][TotalCoeff * 4 + TrailingOnes]
 *   vlc_table 0: 0 <= nC < 2
 *   vlc_table 1: 2 <= nC < 4
 *   vlc_table 2: 4 <= nC < 8
 *   vlc_table 3: nC >= 8
 */
static const uint8_t coeff_token_len[4][4 * 17] = {
{
     1, 0, 0, 0,
     6, 2, 0, 0,     8, 6, 3, 0,     9, 8, 7, 5,    10, 9, 8, 6,
    11,10, 9, 7,    13,11,10, 8,    13,13,11, 9,    13,13,13,10,
    14,14,13,11,    14,14,14,13,    15,15,14,14,    15,15,15,14,
    16,15,15,15,    16,16,16,15,    16,16,16,16,    16,16,16,16,
},
{
     2, 0, 0, 0,
     6, 2, 0, 0,     6, 5, 3, 0,     7, 6, 6, 4,     8, 6, 6, 4,
     8, 7, 7, 5,     9, 8, 8, 6,    11, 9, 9, 6,    11,11,11, 7,
    12,11,11, 9,    12,12,12,11,    12,12,12,11,    13,13,13,12,
    13,13,13,13,    13,14,13,13,    14,14,14,13,    14,14,14,14,
},
{
     4, 0, 0, 0,
     6, 4, 0, 0,     6, 5, 4, 0,     6, 5, 5, 4,     7, 5, 5, 4,
     7, 5, 5, 4,     7, 6, 6, 4,     7, 6, 6, 4,     8, 7, 7, 5,
     8, 8, 7, 6,     9, 8, 8, 7,     9, 9, 8, 8,     9, 9, 9, 8,
    10, 9, 9, 9,    10,10,10,10,    10,10,10,10,    10,10,10,10,
},
{
     6, 0, 0, 0,
     6, 6, 0, 0,     6, 6, 6, 0,     6, 6, 6, 6,     6, 6, 6, 6,
     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
}
};

static const uint8_t coeff_token_bits[4][4 * 17] = {
{
     1, 0, 0, 0,
     5, 1, 0, 0,     7, 4, 1, 0,     7, 6, 5, 3,     7, 6, 5, 3,
     7, 6, 5, 4,    15, 6, 5, 4,    11,14, 5, 4,     8,10,13, 4,
    15,14, 9, 4,    11,10,13,12,    15,14, 9,12,    11,10,13, 8,
    15, 1, 9,12,    11,14,13, 8,     7,10, 9,12,     4, 6, 5, 8,
},
{
     3, 0, 0, 0,
    11, 2, 0, 0,     7, 7, 3, 0,     7,10, 9, 5,     7, 6, 5, 4,
     4, 6, 5, 6,     7, 6, 5, 8,    15, 6, 5, 4,    11,14,13, 4,
    15,10, 9, 4,    11,14,13,12,     8,10, 9, 8,    15,14,13,12,
    11,10, 9,12,     7,11, 6, 8,     9, 8,10, 1,     7, 6, 5, 4,
},
{
    15, 0, 0, 0,
    15,14, 0, 0,    11,15,13, 0,     8,12,14,12,    15,10,11,11,
    11, 8, 9,10,     9,14,13, 9,     8,10, 9, 8,    15,14,13,13,
    11,14,10,12,    15,10,13,12,    11,14, 9,12,     8,10,13, 8,
    13, 7, 9,12,     9,12,11,10,     5, 8, 7, 6,     1, 4, 3, 2,
},
{
     0, 0, 0, 0,
     4, 5, 0, 0,     8, 9,10, 0,    12,13,14,15,    16,17,18,19,
    20,21,22,23,    24,25,26,27,    28,29,30,31,    32,33,34,35,
    36,37,38,39,    40,41,42,43,    44,45,46,47,    48,49,50,51,
    52,53,54,55,    56,57,58,59,    60,61,62,63,     0, 1, 2, 3,
}
};

/* Table 9-7: Total zeros for 4x4 block (TotalCoeff 1..15) */
static const uint8_t total_zeros_len[15][16] = {
    {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9},
    {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6},
    {4,3,3,3,4,4,3,3,4,5,5,6,5,6},
    {5,3,4,4,3,3,3,4,3,4,5,5,5},
    {4,4,4,3,3,3,3,3,4,5,4,5},
    {6,5,3,3,3,3,3,3,4,3,6},
    {6,5,3,3,3,2,3,4,3,6},
    {6,4,5,3,2,2,3,3,6},
    {6,6,4,2,2,3,2,5},
    {5,5,3,2,2,2,4},
    {4,4,3,3,1,3},
    {4,4,2,1,3},
    {3,3,1,2},
    {2,2,1},
    {1,1}
};

static const uint8_t total_zeros_bits[15][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1},
    {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0},
    {5,7,6,5,4,3,4,3,2,3,2,1,1,0},
    {3,7,5,4,6,5,4,3,3,2,2,1,0},
    {5,4,3,7,6,5,4,3,2,1,1,0},
    {1,1,7,6,5,4,3,2,1,1,0},
    {1,1,5,4,3,3,2,1,1,0},
    {1,1,1,3,3,2,2,1,0},
    {1,0,1,3,2,1,1,1},
    {1,0,1,3,2,1,1},
    {0,1,1,2,1,3},
    {0,1,1,1,1},
    {0,1,1,1},
    {0,1,1},
    {0,1}
};

/* Table 9-10: run_before VLC codes */
static const uint8_t run_len[7][16] = {
    {1,1},
    {1,2,2},
    {2,2,2,2},
    {2,2,2,3,3},
    {2,2,3,3,3,3},
    {2,3,3,3,3,3,3},
    {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11}
};

static const uint8_t run_bits[7][16] = {
    {1,0},
    {1,1,0},
    {3,2,1,0},
    {3,2,1,1,0},
    {3,2,3,2,1,0},
    {3,0,1,3,2,5,4},
    {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1}
};

/* Table 9-6: Chroma DC coeff_token */
static const uint8_t chroma_dc_coeff_token_len[4 * 5] = {
    1, 0, 0, 0,  /* TotalCoeff = 0: '1' (1 bit for compat) */
    6, 1, 0, 0,  /* TotalCoeff = 1 */
    6, 6, 3, 0,  /* TotalCoeff = 2 */
    6, 7, 7, 6,  /* TotalCoeff = 3 */
    6, 8, 8, 7   /* TotalCoeff = 4 */
};

static const uint8_t chroma_dc_coeff_token_bits[4 * 5] = {
    1, 0, 0, 0,
    7, 1, 0, 0,
    4, 6, 1, 0,
    3, 3, 2, 5,
    2, 3, 2, 0
};

int cavlc_write_4x4_block(bitstream_t *bs, const int *coeffs, int nC) {
    if (!bs || !coeffs) return 0;

    int scanned[16];
    int total_coeff = 0;
    int trailing_ones = 0;
    int trailing_signs = 0;
    int levels[16];
    int runs[16];
    int total_zeros = 0;

    /* Scan in zigzag order */
    for (int i = 0; i < 16; i++) {
        scanned[i] = coeffs[zigzag_4x4[i]];
    }

    /* Find last non-zero coefficient */
    int last_idx = -1;
    for (int i = 15; i >= 0; i--) {
        if (scanned[i] != 0) {
            last_idx = i;
            break;
        }
    }

    /* Determine VLC table context based on nC */
    int vlc_idx = 0;
    if (nC < 2) vlc_idx = 0;
    else if (nC < 4) vlc_idx = 1;
    else if (nC < 8) vlc_idx = 2;
    else vlc_idx = 3;

    if (last_idx < 0) {
        /* Zero block (TotalCoeff = 0) */
        uint8_t len = coeff_token_len[vlc_idx][0];
        uint8_t bits = coeff_token_bits[vlc_idx][0];
        bs_write_bits(bs, (int)len, (uint32_t)bits);
        return 0;
    }

    /* Count trailing ones and levels in reverse scan order */
    int current_run = 0;
    for (int i = last_idx; i >= 0; i--) {
        if (scanned[i] != 0) {
            if (total_coeff < 3 && abs(scanned[i]) == 1 && trailing_ones == total_coeff) {
                trailing_ones++;
                trailing_signs = (trailing_signs << 1) | (scanned[i] < 0 ? 1 : 0);
            } else {
                levels[total_coeff - trailing_ones] = scanned[i];
            }
            if (total_coeff > 0) {
                runs[total_coeff - 1] = current_run;
                total_zeros += current_run;
            }
            current_run = 0;
            total_coeff++;
        } else {
            current_run++;
        }
    }

    /* 1. Write coeff_token using Table 9-5 */
    int token_idx = total_coeff * 4 + trailing_ones;
    if (token_idx < 4 * 17) {
        uint8_t len = coeff_token_len[vlc_idx][token_idx];
        uint8_t bits = coeff_token_bits[vlc_idx][token_idx];
        if (len > 0) {
            bs_write_bits(bs, (int)len, (uint32_t)bits);
        } else {
            bs_write_ue(bs, (uint32_t)total_coeff);
        }
    } else {
        bs_write_ue(bs, (uint32_t)total_coeff);
    }

    /* 2. Write trailing_ones signs (1 bit per trailing one) */
    for (int i = trailing_ones - 1; i >= 0; i--) {
        bs_write_bit(bs, (trailing_signs >> i) & 1);
    }

    /* 3. Write remaining levels */
    int non_t1 = total_coeff - trailing_ones;
    for (int i = 0; i < non_t1; i++) {
        int lvl = levels[i];
        int sign = (lvl < 0) ? 1 : 0;
        int abs_lvl = abs(lvl);
        if (i == 0 && trailing_ones < 3) abs_lvl--;

        int prefix = abs_lvl - 1;
        if (prefix < 15) {
            for (int p = 0; p < prefix; p++) bs_write_bit(bs, 0);
            bs_write_bit(bs, 1);
            bs_write_bit(bs, sign);
        } else {
            for (int p = 0; p < 15; p++) bs_write_bit(bs, 0);
            bs_write_bit(bs, 1);
            bs_write_bits(bs, 12, (uint32_t)(prefix - 15));
            bs_write_bit(bs, sign);
        }
    }

    /* 4. Write total_zeros if TotalCoeff < 16 */
    if (total_coeff < 16 && total_coeff > 0) {
        int tc_idx = total_coeff - 1;
        if (tc_idx >= 0 && tc_idx < 15 && total_zeros < 16) {
            uint8_t tz_len = total_zeros_len[tc_idx][total_zeros];
            uint8_t tz_bits = total_zeros_bits[tc_idx][total_zeros];
            if (tz_len > 0) {
                bs_write_bits(bs, (int)tz_len, (uint32_t)tz_bits);
            } else {
                bs_write_ue(bs, (uint32_t)total_zeros);
            }
        } else {
            bs_write_ue(bs, (uint32_t)total_zeros);
        }
    }

    /* 5. Write run_before for each non-zero coefficient */
    int zeros_left = total_zeros;
    for (int i = total_coeff - 1; i > 0 && zeros_left > 0; i--) {
        int run = runs[i - 1];
        int zl_idx = (zeros_left <= 6) ? (zeros_left - 1) : 6;
        if (run < 16) {
            uint8_t r_len = run_len[zl_idx][run];
            uint8_t r_bits = run_bits[zl_idx][run];
            if (r_len > 0) {
                bs_write_bits(bs, (int)r_len, (uint32_t)r_bits);
            } else {
                bs_write_ue(bs, (uint32_t)run);
            }
        } else {
            bs_write_ue(bs, (uint32_t)run);
        }
        zeros_left -= run;
    }

    return total_coeff;
}

int cavlc_write_chroma_dc_block(bitstream_t *bs, const int *coeffs) {
    if (!bs || !coeffs) return 0;

    int total_coeff = 0;
    int trailing_ones = 0;
    for (int i = 0; i < 4; i++) {
        if (coeffs[i] != 0) {
            total_coeff++;
            if (abs(coeffs[i]) == 1 && trailing_ones < 3) trailing_ones++;
        }
    }

    if (total_coeff == 0) {
        /* TotalCoeff = 0: '1' (1 bit) */
        bs_write_bit(bs, 1);
        return 0;
    }

    int token_idx = total_coeff * 4 + trailing_ones;
    if (token_idx < 4 * 5) {
        uint8_t len = chroma_dc_coeff_token_len[token_idx];
        uint8_t bits = chroma_dc_coeff_token_bits[token_idx];
        if (len > 0) {
            bs_write_bits(bs, (int)len, (uint32_t)bits);
        } else {
            bs_write_ue(bs, (uint32_t)total_coeff);
        }
    } else {
        bs_write_ue(bs, (uint32_t)total_coeff);
    }

    return total_coeff;
}

void cavlc_write_slice_trailing_bits(bitstream_t *bs) {
    if (!bs) return;
    bs_rbsp_trailing_bits(bs);
}
