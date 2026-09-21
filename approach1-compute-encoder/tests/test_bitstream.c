/* bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * test_bitstream.c - Unit tests for H.264 Bitstream & Exp-Golomb Writer
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "bitstream.h"

static void test_exp_golomb_unsigned(void) {
    uint8_t buf[16];
    bitstream_t bs;

    /* ue(0) -> '1' (1 bit) */
    bs_init(&bs, buf, sizeof(buf));
    bs_write_ue(&bs, 0);
    bs_rbsp_trailing_bits(&bs);
    assert(buf[0] == 0xC0);

    /* ue(1) -> '010' */
    bs_init(&bs, buf, sizeof(buf));
    bs_write_ue(&bs, 1);
    bs_rbsp_trailing_bits(&bs);
    assert((buf[0] & 0xe0) == 0x40);

    /* ue(2) -> '011' */
    bs_init(&bs, buf, sizeof(buf));
    bs_write_ue(&bs, 2);
    bs_rbsp_trailing_bits(&bs);
    assert((buf[0] & 0xe0) == 0x60);

    /* Extreme unsigned edge case: UINT32_MAX must not cause unsigned 32-bit overflow */
    uint8_t large_buf[32];
    bs_init(&bs, large_buf, sizeof(large_buf));
    bs_write_ue(&bs, 0xFFFFFFFFU);
    bs_rbsp_trailing_bits(&bs);
    assert(bs_bytes_written(&bs) > 0);

    printf("[PASS] Exp-Golomb Unsigned (ue) tests\n");
}

static void test_exp_golomb_signed(void) {
    uint8_t buf[16];
    bitstream_t bs;

    /* se(0) -> ue(0) -> '1' */
    bs_init(&bs, buf, sizeof(buf));
    bs_write_se(&bs, 0);
    bs_rbsp_trailing_bits(&bs);
    assert(buf[0] == 0xC0);

    /* se(1) -> ue(1) -> '010' */
    bs_init(&bs, buf, sizeof(buf));
    bs_write_se(&bs, 1);
    bs_rbsp_trailing_bits(&bs);
    assert((buf[0] & 0xe0) == 0x40);

    /* se(-1) -> ue(2) -> '011' */
    bs_init(&bs, buf, sizeof(buf));
    bs_write_se(&bs, -1);
    bs_rbsp_trailing_bits(&bs);
    assert((buf[0] & 0xe0) == 0x60);

    /* Extreme signed edge case: INT32_MIN must not cause signed integer overflow or crash */
    uint8_t s_large_buf[32];
    bs_init(&bs, s_large_buf, sizeof(s_large_buf));
    bs_write_se(&bs, -2147483647 - 1);
    bs_rbsp_trailing_bits(&bs);
    assert(bs_bytes_written(&bs) > 0);

    printf("[PASS] Exp-Golomb Signed (se) tests\n");
}

static void test_emulation_prevention(void) {
    uint8_t raw[4] = {0x00, 0x00, 0x01, 0xAA};
    uint8_t escaped[8];
    size_t out_len = bs_rbsp_to_ebsp(escaped, sizeof(escaped), raw, sizeof(raw));

    /* Expected: 00 00 03 01 AA (5 bytes) */
    assert(out_len == 5);
    (void)out_len;
    assert(escaped[0] == 0x00);
    assert(escaped[1] == 0x00);
    assert(escaped[2] == 0x03);
    assert(escaped[3] == 0x01);
    assert(escaped[4] == 0xAA);

    printf("[PASS] Emulation prevention (EBSP) test\n");
}

/* The spec definition of emulation prevention, written the slow obvious
 * way: one byte at a time, no run detection, no bulk copy. This is
 * literally the implementation bs_rbsp_to_ebsp() had before it was
 * rewritten to scan for zero pairs with memchr()/memcpy(), kept here as
 * the oracle that rewrite is checked against - including its exact
 * truncation behaviour when the destination fills up mid-stream. */
static size_t ebsp_reference(uint8_t *dst, size_t dst_size,
                             const uint8_t *src, size_t src_size) {
    size_t i, j = 0;
    int zero_count = 0;
    for (i = 0; i < src_size; i++) {
        if (j >= dst_size) break;
        if (zero_count == 2 && src[i] <= 0x03) {
            dst[j++] = 0x03;
            zero_count = 0;
            if (j >= dst_size) break;
        }
        dst[j++] = src[i];
        if (src[i] == 0x00) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }
    return j;
}

static void test_emulation_prevention_vs_reference(void) {
    /* Deterministic xorshift, so a failure is reproducible. */
    uint64_t rs = 0x9e3779b97f4a7c15ULL;
    #define NEXT_RND() (rs ^= rs << 13, rs ^= rs >> 7, rs ^= rs << 17, (uint32_t)(rs >> 32))

    static uint8_t src[300];
    static uint8_t got[640], want[640];

    /* Hand-picked shapes first: the boundary cases the bulk path has to
     * get right - a pair at the very start, at the very end, a long zero
     * run (escapes every third byte), 00 00 03 (the escape byte itself
     * must be escaped), and 00 00 04 (must NOT be). */
    static const uint8_t fixed[][10] = {
        { 0x00, 0x00, 0x00 },
        { 0x00, 0x00, 0x03 },
        { 0x00, 0x00, 0x04 },
        { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0xAA, 0x00, 0x00, 0x01, 0xBB },
        { 0x00, 0x00 },
        { 0x00 },
        { 0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0xFF },
    };
    static const size_t fixed_len[] = { 3, 3, 3, 8, 5, 2, 1, 10 };

    for (size_t t = 0; t < sizeof(fixed_len) / sizeof(fixed_len[0]); t++) {
        for (size_t cap = 0; cap <= fixed_len[t] * 2 + 2; cap++) {
            memset(got, 0xA5, sizeof(got));
            memset(want, 0xA5, sizeof(want));
            size_t a = bs_rbsp_to_ebsp(got, cap, fixed[t], fixed_len[t]);
            size_t b = ebsp_reference(want, cap, fixed[t], fixed_len[t]);
            assert(a == b);
            assert(memcmp(got, want, sizeof(got)) == 0);
        }
    }

    /* Then fuzz, at several zero densities, against every destination
     * capacity from 0 to just past the worst-case expansion - so the
     * bulk path's memcpy clamp is exercised at every possible cut. */
    for (int trial = 0; trial < 400; trial++) {
        size_t n = NEXT_RND() % (sizeof(src) + 1);
        unsigned density = 1u + (NEXT_RND() % 60u);   /* % of bytes forced to 0 */
        for (size_t k = 0; k < n; k++) {
            src[k] = (NEXT_RND() % 100u < density) ? 0x00
                                                   : (uint8_t)(NEXT_RND() | 1u);
        }
        for (size_t cap = 0; cap <= n + n / 2 + 2; cap++) {
            memset(got, 0xA5, sizeof(got));
            memset(want, 0xA5, sizeof(want));
            size_t a = bs_rbsp_to_ebsp(got, cap, src, n);
            size_t b = ebsp_reference(want, cap, src, n);
            assert(a == b);
            assert(memcmp(got, want, sizeof(got)) == 0);
        }
    }
    #undef NEXT_RND

    printf("[PASS] EBSP matches the byte-at-a-time reference (fixed + fuzz,\n"
           "       every destination capacity, including truncation)\n");
}

static void test_sps_pps_generation(void) {
    uint8_t sps_buf[512];
    uint8_t pps_buf[512];
    h264_sps_t sps;
    h264_pps_t pps;

    h264_sps_default(&sps, 1920, 1080, 60, PROFILE_HIGH);
    size_t sps_bytes = bs_write_sps(sps_buf, sizeof(sps_buf), &sps);

    /* Verify NAL start code (0x00000001) and SPS NAL type (0x67 for High profile) */
    assert(sps_bytes > 10);
    assert(sps_buf[0] == 0x00 && sps_buf[1] == 0x00 && sps_buf[2] == 0x00 && sps_buf[3] == 0x01);
    assert((sps_buf[4] & 0x1F) == NAL_TYPE_SPS);

    h264_pps_default(&pps, sps.sps_id, false, 26);
    size_t pps_bytes = bs_write_pps(pps_buf, sizeof(pps_buf), &pps);

    assert(pps_bytes > 5);
    assert(pps_buf[0] == 0x00 && pps_buf[1] == 0x00 && pps_buf[2] == 0x00 && pps_buf[3] == 0x01);
    assert((pps_buf[4] & 0x1F) == NAL_TYPE_PPS);

    printf("[PASS] SPS/PPS serialization test: SPS %zu bytes, PPS %zu bytes\n", sps_bytes, pps_bytes);
}

int main(void) {
    printf("=== Running BC-250 Bitstream Unit Tests ===\n");
    test_exp_golomb_unsigned();
    test_exp_golomb_signed();
    test_emulation_prevention();
    test_emulation_prevention_vs_reference();
    test_sps_pps_generation();
    printf("=== All Bitstream Tests Passed Successfully! ===\n");
    return 0;
}
