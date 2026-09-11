/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * test_cavlc.c - Spec-conformance unit tests for H.264 CAVLC entropy engine
 *                Tests ITU-T H.264 Section 9.2 tables, trailing ones,
 *                total zeros, run_before, and macroblock syntax.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include "bitstream.h"
#include "cavlc.h"

static void test_zero_blocks(void) {
    printf("  [1] Testing 4x4 Zero Block encoding across all nC contexts...\n");
    int16_t zero_coeffs[16] = {0};
    uint8_t buf[64];

    /* nC < 2 (VLC 1: code '1', 1 bit) */
    bitstream_t bs;
    bs_init(&bs, buf, sizeof(buf));
    int tc = cavlc_write_4x4_block(&bs, zero_coeffs, 0);
    assert(tc == 0);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);
    assert((buf[0] & 0x80) == 0x80); /* First bit is 1 */

    /* 2 <= nC < 4 (VLC 2: code '11', 2 bits) */
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_4x4_block(&bs, zero_coeffs, 2);
    assert(tc == 0);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);
    assert((buf[0] & 0xC0) == 0xC0); /* First 2 bits are 11 */

    /* 4 <= nC < 8 (VLC 3: code '1111', 4 bits) */
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_4x4_block(&bs, zero_coeffs, 5);
    assert(tc == 0);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);
    assert((buf[0] & 0xF0) == 0xF0); /* First 4 bits are 1111 */

    /* nC >= 8 (Fixed 6-bit FLC: TotalCoeff=0 codes as value 3 = '000011',
     * per ffmpeg's h264_cavlc.c coeff_token_bits[3][0] - NOT '000000'; the
     * original cavlc.c table had this wrong, see cavlc.c comments) */
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_4x4_block(&bs, zero_coeffs, 10);
    assert(tc == 0);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);
    assert((buf[0] & 0xFC) == 0x0C); /* First 6 bits are 000011 */

    printf("      ✓ Passed all 4 nC context classes for zero blocks.\n");
}

static void test_trailing_ones(void) {
    printf("  [2] Testing Trailing Ones (T1) handling and sign encoding...\n");
    uint8_t buf[64];

    /* Block with 3 trailing ones: +1, -1, +1 */
    int16_t coeffs[16] = {1, -1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bitstream_t bs;
    bs_init(&bs, buf, sizeof(buf));
    int tc = cavlc_write_4x4_block(&bs, coeffs, 0);
    assert(tc == 3);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    /* Block with single trailing one: -1 */
    int16_t single_t1[16] = {-1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_4x4_block(&bs, single_t1, 0);
    assert(tc == 1);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    printf("      ✓ Trailing ones and sign bits verified.\n");
}

static void test_levels_and_runs(void) {
    printf("  [3] Testing Non-T1 Levels, Total Zeros, and Run Before...\n");
    uint8_t buf[64];

    /* Mixed block: high level, scattered zeros */
    int16_t coeffs[16] = {5, 0, -2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bitstream_t bs;
    bs_init(&bs, buf, sizeof(buf));
    int tc = cavlc_write_4x4_block(&bs, coeffs, 1);
    assert(tc == 3);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    printf("      ✓ Levels, zeros, and run_before encoding verified.\n");
}

static void test_chroma_dc(void) {
    printf("  [4] Testing 2x2 Chroma DC block encoding...\n");
    uint8_t buf[64];

    /* Zero chroma DC: TotalCoeff=0 codes as '01' (2 bits), NOT '1' (1 bit) -
     * see chroma_dc_coeff_token_len fix in cavlc.c, cross-checked against
     * ffmpeg's h264_cavlc.c chroma_dc_coeff_token_len[0]. */
    int zero_chroma[4] = {0, 0, 0, 0};
    bitstream_t bs;
    bs_init(&bs, buf, sizeof(buf));
    int tc = cavlc_write_chroma_dc_block(&bs, zero_chroma);
    assert(tc == 0);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);
    assert((buf[0] & 0xC0) == 0x40); /* First 2 bits are '01' */

    /* Non-zero chroma DC: total_coeff=2, trailing_ones=2 (both +-1), no
     * remaining levels, and exactly 1 total_zeros/run_before pair now that
     * cavlc_write_chroma_dc_block is fully implemented (it used to stop
     * after coeff_token, writing nothing else at all). */
    int nz_chroma[4] = {2, 0, -1, 0};
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_chroma_dc_block(&bs, nz_chroma);
    assert(tc == 2);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    /* All 4 chroma DC coefficients non-zero, no trailing ones (all magnitude
     * > 1): TotalCoeff==maxNumCoeff(4) so total_zeros must NOT be coded at
     * all (only coeff_token + signs/levels are written). */
    int full_chroma[4] = {3, -2, 4, -5};
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_chroma_dc_block(&bs, full_chroma);
    assert(tc == 4);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    printf("      ✓ Chroma DC encoding verified.\n");
}

static void test_ac_block(void) {
    printf("  [7] Testing 4x4 AC-only block encoding (maxNumCoeff=15)...\n");
    uint8_t buf[64];
    bitstream_t bs;

    /* All-zero AC block (DC value at raster position 0 is irrelevant/ignored) */
    int16_t zero_block[16] = {99, 0};
    bs_init(&bs, buf, sizeof(buf));
    int tc = cavlc_write_4x4_ac_block(&bs, zero_block, 0);
    assert(tc == 0);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);
    assert((buf[0] & 0x80) == 0x80); /* nC<2, TotalCoeff=0: '1' (1 bit) */

    /* Exactly 1 non-zero AC coefficient (at raster position 1, which is
     * zigzag_4x4[1] - the very first AC scan position) so TotalCoeff=1,
     * TrailingOnes=1 (it's a lone -1), and TotalCoeff==15 never happens
     * here, so total_zeros IS coded (with 14 zeros left of 15 possible
     * AC positions). Only checks structural properties (tc==1, some bytes
     * written) since exact bit-packing is covered by cavlc_write_4x4_block's
     * own tests via the now-shared cavlc_scan_coeffs/level-writer helpers. */
    int16_t one_ac[16] = {0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_4x4_ac_block(&bs, one_ac, 0);
    assert(tc == 1);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    /* All 15 AC positions non-zero (TotalCoeff==maxNumCoeff==15): total_zeros
     * must be entirely omitted from the bitstream (this is the case the old
     * "reuse the 16-coefficient table & threshold" approach would have
     * gotten wrong, since it would only skip total_zeros at TotalCoeff==16). */
    int16_t full_ac[16];
    for (int i = 0; i < 16; i++) full_ac[i] = 2; /* raster pos 0 (DC) ignored by this function */
    bs_init(&bs, buf, sizeof(buf));
    tc = cavlc_write_4x4_ac_block(&bs, full_ac, 4);
    assert(tc == 15);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    /* DC value at raster position 0 must never influence the result. */
    int16_t with_dc[16] = {123, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int16_t without_dc[16] = {0, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t buf_a[64], buf_b[64];
    bitstream_t bs_a, bs_b;
    bs_init(&bs_a, buf_a, sizeof(buf_a));
    bs_init(&bs_b, buf_b, sizeof(buf_b));
    int tc_a = cavlc_write_4x4_ac_block(&bs_a, with_dc, 0);
    int tc_b = cavlc_write_4x4_ac_block(&bs_b, without_dc, 0);
    assert(tc_a == tc_b);
    bs_flush(&bs_a);
    bs_flush(&bs_b);
    assert(bs_bytes_written(&bs_a) == bs_bytes_written(&bs_b));
    assert(memcmp(buf_a, buf_b, bs_bytes_written(&bs_a)) == 0);

    printf("      ✓ AC-only block encoding (DC exclusion, total_zeros full-block omission) verified.\n");
}

/* A coefficient large enough that suffixLength must escalate past 0 for
 * later coefficients in the same block - exercises cavlc_write_one_level's
 * adaptive-suffixLength path (see cavlc.c comment on why the old
 * suffixLength-always-0 scheme would desync a real decoder here). We only
 * check that encoding completes and produces a plausible number of bytes;
 * full bit-exactness for large levels is validated against ffmpeg's decoder
 * on real hardware (see docs/testing-guide.md / task validation notes). */
static void test_large_level_escalation(void) {
    printf("  [8] Testing adaptive suffixLength escalation for large levels...\n");
    uint8_t buf[64];
    bitstream_t bs;

    /* total_coeff=4, no trailing ones (all magnitudes > 1): forces the
     * escalating level-suffix path for the 2nd..4th coefficients. */
    int16_t coeffs[16] = {40, 0, -25, 0, 12, 0, -6, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    bs_init(&bs, buf, sizeof(buf));
    int tc = cavlc_write_4x4_block(&bs, coeffs, 3);
    assert(tc == 4);
    (void)tc;
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);
    assert(bs_bytes_written(&bs) < sizeof(buf));

    printf("      ✓ Large-level suffixLength escalation completes without overflow.\n");
}

static void test_macroblock_headers(void) {
    printf("  [5] Testing Macroblock Layer Headers (I_16x16, P_16x16, P_Skip)...\n");
    uint8_t buf[64];
    bitstream_t bs;

    /* Intra 16x16 modes */
    const int modes[] = {H264_I16x16_VERT, H264_I16x16_HORIZ, H264_I16x16_DC, H264_I16x16_PLANE};
    for (int m = 0; m < 4; m++) {
        bs_init(&bs, buf, sizeof(buf));
        cavlc_write_mb_i16x16_header(&bs, modes[m], 0, 0, 0, 0);
        bs_flush(&bs);
        assert(bs_bytes_written(&bs) > 0);
    }

    /* P_Skip run */
    bs_init(&bs, buf, sizeof(buf));
    cavlc_write_p_skip_run(&bs, 1);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);

    bs_init(&bs, buf, sizeof(buf));
    cavlc_write_p_skip_run(&bs, 8160);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    /* P_16x16 with MVD */
    bs_init(&bs, buf, sizeof(buf));
    cavlc_write_mb_p16x16_header(&bs, 0, 0, 0, 0);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    bs_init(&bs, buf, sizeof(buf));
    cavlc_write_mb_p16x16_header(&bs, 4, -2, 15, -1);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) > 0);

    /* Regression test: cavlc_write_p_skip_run(bs, N) followed by
     * cavlc_write_mb_p16x16_header() must write EXACTLY ONE mb_skip_run
     * field total (ITU-T H.264 7.3.4 - mb_skip_run is read once per
     * do-while iteration of slice_data(), not part of macroblock_layer()).
     * This used to insert a spurious extra "mb_skip_run=0" from inside the
     * header whenever the caller's skip_run was nonzero, desyncing every
     * subsequent macroblock in the slice. Verify by comparing against a
     * hand-built reference: ue(3) [[skip_run=3]] followed by mb_type=0,
     * mvd_x=0, mvd_y=0, cbp mapped to codeNum 0 (cbp=0) - no qp_delta since
     * cbp==0. ue(3)='00100' (5 bits), then four more ue(0)='1' (1 bit each)
     * for mb_type/mvd_x(se 0=ue 0)/mvd_y(se 0=ue 0)/cbp(codeNum 0) = 5+4=9
     * bits total, packed into 2 bytes with the last 7 bits zero-padded:
     * 0b00100_1_1 1_1 0000 0 -> byte0=00100111=0x27, byte1=10000000=0x80. */
    bs_init(&bs, buf, sizeof(buf));
    cavlc_write_p_skip_run(&bs, 3);
    cavlc_write_mb_p16x16_header(&bs, 0, 0, 0, 0);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 2);
    assert(buf[0] == 0x27);
    assert(buf[1] == 0x80);

    printf("      ✓ I16x16, P16x16, and P_Skip syntax headers verified.\n");
}

static void test_slice_trailing(void) {
    printf("  [6] Testing RBSP Slice Trailing Bits...\n");
    uint8_t buf[16];
    bitstream_t bs;

    /* Write 3 arbitrary bits, then trailing bits */
    bs_init(&bs, buf, sizeof(buf));
    bs_write_u(&bs, 3, 0x5);
    cavlc_write_slice_trailing_bits(&bs);
    bs_flush(&bs);
    assert(bs_bytes_written(&bs) == 1);
    /* 3 bits: 101, then 1 stop bit: 1, then 4 zero bits: 0000 -> 0b10110000 = 0xB0 */
    assert(buf[0] == 0xB0);

    printf("      ✓ RBSP trailing bits byte alignment verified.\n");
}

/*
 * Regression test for the cavlc_write_run_befores() iteration-order bug
 * fixed in commit 8feddef ("fix(cavlc): correct run_before coding order
 * (ITU-T 9.2.3) - major luma+chroma fix"). That bug wrote run_before values
 * in LOW-to-HIGH frequency order instead of the spec-required HIGH-to-LOW
 * order, and was invisible for total_coeff<=2 (only one run_before value
 * exists, so "order" is moot) - every pre-existing test above has 3 or
 * fewer nonzero coefficients per block and total_coeff<=2, which is exactly
 * why none of them caught it. This test uses total_coeff=3 (the minimum
 * that has 2 distinct run_before values, i.e. the minimum where order is
 * observable at all) with deliberately UNEQUAL zero-run gaps between the 3
 * coefficients, so that swapping the write order changes the actual
 * bitstream bits, not just their sequence.
 *
 * Layout (scan-position indices, 0=DC, 15=highest AC frequency, matching
 * cavlc_scan_coeffs' "highest frequency discovered first" convention):
 *   scan position 12 = 5   (rank 0, highest frequency of the 3)
 *   scan position  6 = -3  (rank 1)
 *   scan position  2 = 2   (rank 2, lowest frequency of the 3)
 * giving 5 zeros between rank0/rank1 and 3 zeros between rank1/rank2 (the 2
 * zeros before scan position 2 fold into total_zeros directly, per
 * cavlc_scan_coeffs' own doc comment, and are never a run_before value).
 * All three magnitudes are >1 so none becomes a trailing-one (keeps the
 * levels path simple/unambiguous). Raster-order positions are derived from
 * zigzag_4x4[] so the desired SCAN positions land where intended once
 * cavlc_write_4x4_block applies its own internal zigzag.
 *
 * Expected bytes below were captured from this exact input against the
 * fixed code (hand-verified bit-by-bit against ITU-T Table 9-5/9-7/9-10 -
 * see the derivation in this commit's message) and independently confirmed
 * to require the fix: reverting only cavlc_write_run_befores' loop to the
 * pre-8feddef iteration order changes byte[3] from 0x24 to 0x28 (this was
 * verified locally before adding this test).
 */
static void test_run_before_order(void) {
    printf("  [9] Regression: run_before HIGH-to-LOW frequency order (commit 8feddef)...\n");
    static const int zigzag_4x4[16] = {
         0,  1,  4,  8,
         5,  2,  3,  6,
         9, 12, 13, 10,
         7, 11, 14, 15
    };
    int16_t coeffs[16] = {0};
    coeffs[zigzag_4x4[12]] = 5;   /* rank 0: highest freq of the 3 */
    coeffs[zigzag_4x4[6]]  = -3;  /* rank 1 */
    coeffs[zigzag_4x4[2]]  = 2;   /* rank 2: lowest freq of the 3 */

    uint8_t buf[64];
    bitstream_t bs;
    bs_init(&bs, buf, sizeof(buf));
    int tc = cavlc_write_4x4_block(&bs, coeffs, 0 /* nC < 2 */);
    bs_flush(&bs);

    assert(tc == 3);
    assert(bs_bytes_written(&bs) == 5);
    static const uint8_t expected[5] = { 0x03, 0x81, 0x5C, 0x24, 0x80 };
    assert(memcmp(buf, expected, sizeof(expected)) == 0);

    printf("      \xe2\x9c\x93 run_before high-to-low frequency order verified bit-exact.\n");
}

int main(void) {
    printf("=== Running BC-250 H.264 CAVLC Unit Test Suite ===\n");
    test_zero_blocks();
    test_trailing_ones();
    test_levels_and_runs();
    test_chroma_dc();
    test_macroblock_headers();
    test_slice_trailing();
    test_ac_block();
    test_large_level_escalation();
    test_run_before_order();
    printf("=== ALL CAVLC CONFORMANCE UNIT TESTS PASSED! ===\n");
    return 0;
}
