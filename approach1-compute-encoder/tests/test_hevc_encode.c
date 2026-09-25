/* bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * test_hevc_encode.c - End-to-end H.265/HEVC bitstream syntax test.
 *
 * GPU-free (uses hevc_encoder_encode_raw(), the same "no Vulkan context
 * needed" testing pattern test_encode.c uses for H.264 via
 * h264_encoder_encode_raw()) so this runs in plain `ctest` without a real
 * GPU. It checks structural invariants only (NAL start codes/types present,
 * output non-empty, no crash) - real visual/PSNR correctness against a
 * decoder is validated separately on real hardware with real content (see
 * the project's final report / tools/quality_test.sh), the same division
 * of labor this project already uses for H.264.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "encoder_h265.h"
#include "hevc_intra.h"

/* Pure-math regression: forward transform + real HEVC dequant/inverse
 * transform should round-trip a DC-only (constant) residual block back to
 * (approximately) itself at a very low QP, for both DST (luma) and DCT
 * (chroma) - this is the "does the shift/scale accounting actually
 * cancel" check described in hevc_intra.c's forward_transform_4x4()
 * comment, run as an actual assertion rather than just hand-derivation. */
static void test_transform_round_trip(void) {
    printf("[test_hevc_encode] Transform round-trip (DST + DCT, DC-only, QP=4)...\n");
    for (int use_dst = 0; use_dst <= 1; use_dst++) {
        int16_t residual[16];
        for (int i = 0; i < 16; i++) residual[i] = 20; /* constant residual */

        int16_t coeff[16];
        hevc_transform_quant_4x4(residual, 4, use_dst, coeff);

        int16_t recon[16];
        hevc_dequant_itransform_4x4(coeff, 4, use_dst, recon);

        for (int i = 0; i < 16; i++) {
            int diff = recon[i] - 20;
            if (diff < 0) diff = -diff;
            /* At QP=4 (near-lossless) a constant block should round-trip
             * within a couple of levels - if the forward/inverse shift
             * accounting were wrong (e.g. off by a power-of-2), this would
             * be off by 10s-100s, not 1-2. */
            assert(diff <= 3 && "transform/quant round trip drifted too far - shift/scale bug");
        }
    }
    printf("[test_hevc_encode] Transform round-trip OK.\n");
}

/* The transform/quant path in src/hevc_intra.c is a partial-butterfly
 * factorisation plus a reciprocal-multiply quantizer, both of which
 * REPLACED literal transcriptions of ITU-T H.265 8.6.3/8.6.4 for speed.
 * Both claim to be bit-identical to what they replaced, not merely
 * equivalent in intent, and the literal versions are still compiled in
 * (hevc_*_ref) purely so that claim can be checked here rather than
 * asserted in a comment.
 *
 * This is the test that has to fail if someone "simplifies" a butterfly
 * or widens a bound - a wrong transform still produces a structurally
 * valid bitstream that decodes to a plausible picture, which is exactly
 * how this encoder's earlier bugs survived. */
static uint32_t trng_state = 0x9E3779B9u;
static uint32_t trng(void) {
    trng_state ^= trng_state << 13;
    trng_state ^= trng_state >> 17;
    trng_state ^= trng_state << 5;
    return trng_state;
}

static void test_transform_butterfly_equivalence(void) {
    printf("[test_hevc_encode] Butterfly transforms == literal 8.6.4 matrix product...\n");

    /* Basis vectors. The transform is an exact integer linear map (its
     * only nonlinearity, the rounding shift, is identical in both
     * versions), so agreeing on e_k at every amplitude proves agreement
     * on every input in range. */
    for (int use_dst = 0; use_dst <= 1; use_dst++) {
        for (int k = 0; k < 16; k++) {
            for (int amp = -32768; amp <= 32767; amp += 97) {
                int16_t in[16] = { 0 };
                in[k] = (int16_t)amp;

                int32_t fa[16], fb[16];
                hevc_forward_transform_4x4(in, use_dst, fa);
                hevc_forward_transform_4x4_ref(in, use_dst, fb);
                assert(memcmp(fa, fb, sizeof fa) == 0 &&
                       "forward butterfly diverged from the 8.6.4.1 matrix product");

                int16_t ia[16], ib[16];
                hevc_inverse_transform_4x4(in, use_dst, ia);
                hevc_inverse_transform_4x4_ref(in, use_dst, ib);
                assert(memcmp(ia, ib, sizeof ia) == 0 &&
                       "inverse butterfly diverged from the 8.6.4.2 matrix product");
            }
        }
    }

    /* Random blocks. `kind == 2` saturates every coefficient to +-32768,
     * which is the only way to drive the inverse stage-2 accumulator to
     * its maximum - that bound is what makes the spec's stage-2 clip dead
     * code, and the reference still performs that clip, so a wrong bound
     * shows up here as a mismatch. */
    for (int it = 0; it < 200000; it++) {
        int use_dst = (int)(trng() & 1);
        int kind = (int)(trng() % 3);
        int16_t res[16], coeff[16];
        for (int i = 0; i < 16; i++) {
            res[i] = (int16_t)((int)(trng() % 511) - 255);
            coeff[i] = kind == 0 ? (int16_t)((int)(trng() % 2001) - 1000)
                     : kind == 1 ? (int16_t)(trng() & 0xFFFF)
                                 : (int16_t)((trng() & 1) ? 32767 : -32768);
        }
        int32_t fa[16], fb[16];
        hevc_forward_transform_4x4(res, use_dst, fa);
        hevc_forward_transform_4x4_ref(res, use_dst, fb);
        assert(memcmp(fa, fb, sizeof fa) == 0 && "forward butterfly diverged (random block)");

        int16_t ia[16], ib[16];
        hevc_inverse_transform_4x4(coeff, use_dst, ia);
        hevc_inverse_transform_4x4_ref(coeff, use_dst, ib);
        assert(memcmp(ia, ib, sizeof ia) == 0 && "inverse butterfly diverged (random block)");
    }
    printf("[test_hevc_encode] Butterfly transforms OK.\n");
}

static void test_quantizer_reciprocal_equivalence(void) {
    printf("[test_hevc_encode] Reciprocal quantizer == literal 8.6.3 division...\n");
    /* Every QP the encoder can use, against every raw magnitude a real
     * 8-bit residual can produce (|raw| <= 32640, so 0..65535 covers it
     * with room), plus a sparse sweep out to the proved worst-case bound
     * of 2^22 for a synthetic full-range int16 residual. Both signs, via
     * raw[0] = +m and raw[1] = -m. */
    for (int qp = 0; qp <= 51; qp++) {
        for (int32_t m = 0; m <= 65535; m++) {
            int32_t raw[16] = { 0 };
            int16_t a[16], b[16];
            raw[0] = m; raw[1] = -m;
            hevc_quantize_4x4(raw, qp, a);
            hevc_quantize_4x4_ref(raw, qp, b);
            assert(a[0] == b[0] && a[1] == b[1] &&
                   "reciprocal quantizer diverged from the 8.6.3 division");
        }
        /* Straddle the quotient boundaries out at the top of the range,
         * where a magic-number scheme fails first if the shift is wrong. */
        for (int32_t m = 65536; m <= (1 << 22); m += 4093) {
            int32_t raw[16] = { 0 };
            int16_t a[16], b[16];
            for (int d = -2; d <= 2; d++) {
                int32_t v = m + d;
                raw[0] = v; raw[1] = -v;
                hevc_quantize_4x4(raw, qp, a);
                hevc_quantize_4x4_ref(raw, qp, b);
                assert(a[0] == b[0] && a[1] == b[1] &&
                       "reciprocal quantizer diverged at a high magnitude");
            }
        }
    }
    printf("[test_hevc_encode] Reciprocal quantizer OK.\n");
}

static void test_mpm_derivation(void) {
    printf("[test_hevc_encode] MPM derivation sanity...\n");
    int mpm[3];

    /* Both neighbors unavailable -> both treated as DC -> DC==DC branch,
     * DC < 2 -> {Planar, DC, Vertical}. */
    hevc_derive_mpm(0, 0, 0, 0, mpm);
    assert(mpm[0] == HEVC_MODE_PLANAR && mpm[1] == HEVC_MODE_DC && mpm[2] == HEVC_MODE_VERTICAL);

    /* Distinct available neighbors, neither Planar -> third candidate is
     * Planar. */
    hevc_derive_mpm(HEVC_MODE_DC, 1, HEVC_MODE_VERTICAL, 1, mpm);
    assert(mpm[0] == HEVC_MODE_DC && mpm[1] == HEVC_MODE_VERTICAL && mpm[2] == HEVC_MODE_PLANAR);

    printf("[test_hevc_encode] MPM derivation OK.\n");
}

/* Checks that `buf[0..len)` is a sequence of Annex-B NAL units (each
 * starting with 00 00 00 01) whose nal_unit_type (bits 1-6 of the byte
 * right after the start code, for an HEVC 2-byte NAL header) match
 * `expected_types[]` in order. Returns 1 on match, 0 otherwise (printing
 * why). */
static int check_nal_sequence(const uint8_t *buf, size_t len, const int *expected_types, int n) {
    size_t pos = 0;
    for (int i = 0; i < n; i++) {
        if (pos + 6 > len) { printf("  short: ran out of bytes before NAL %d\n", i); return 0; }
        if (!(buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 0 && buf[pos + 3] == 1)) {
            printf("  NAL %d: missing start code at offset %zu\n", i, pos);
            return 0;
        }
        int nal_type = (buf[pos + 4] >> 1) & 0x3f;
        if (nal_type != expected_types[i]) {
            printf("  NAL %d: expected type %d, got %d\n", i, expected_types[i], nal_type);
            return 0;
        }
        /* Advance to the next start code (or end of buffer). */
        size_t next = pos + 4;
        while (next + 3 < len && !(buf[next] == 0 && buf[next + 1] == 0 && buf[next + 2] == 0 && buf[next + 3] == 1))
            next++;
        pos = (next + 3 < len) ? next : len;
    }
    return 1;
}

static void test_multi_frame_gop(void) {
    printf("[test_hevc_encode] Testing multi-frame GOP (IDR + P-frames)...\n");

    const uint32_t width = 128, height = 96, fps = 30, bitrate = 2000000;
    hevc_encoder_t *enc = hevc_encoder_create(NULL, width, height, fps, bitrate);
    assert(enc != NULL);
    hevc_encoder_set_gop_size(enc, 30);

    uint8_t *y_plane = malloc((size_t)width * height);
    uint8_t *uv_plane = malloc((size_t)(width / 2) * (height / 2) * 2);
    assert(y_plane && uv_plane);

    /* Frame 0 pattern */
    for (uint32_t r = 0; r < height; r++)
        for (uint32_t c = 0; c < width; c++)
            y_plane[r * width + c] = (uint8_t)(((c * 255) / width) ^ ((r * 128) / height));
    for (uint32_t r = 0; r < height / 2; r++)
        for (uint32_t c = 0; c < width / 2; c++) {
            uv_plane[r * width + c * 2 + 0] = (uint8_t)(100 + (c % 32));
            uv_plane[r * width + c * 2 + 1] = (uint8_t)(150 + (r % 32));
        }

    const size_t out_cap = (size_t)width * height * 2 + 65536;
    uint8_t *out_buf = malloc(out_cap);
    assert(out_buf != NULL);

    FILE *f = fopen("bc250_test_stream.hevc", "wb");
    if (!f) {
        f = fopen("/tmp/bc250_test_stream.hevc", "wb");
    }
    if (!f) {
        fprintf(stderr, "[test_hevc_encode] Warning: could not open output stream file for writing, proceeding in memory\n");
    }

    int idr_bytes = 0;
    int static_p_bytes = 0;
    uint32_t static_sad = 0;

    for (int frame = 0; frame < 30; frame++) {
        if (frame >= 15 && frame < 29) {
            /* Introduce moving box pattern */
            for (uint32_t r = 32; r < 64; r++) {
                for (uint32_t c = (uint32_t)(frame * 2); c < (uint32_t)(frame * 2 + 32) && c < width; c++) {
                    y_plane[r * width + c] = (uint8_t)(200 + ((frame * 5) % 55));
                }
            }
        }
        if (frame == 29) {
            /* Test explicit force-IDR request */
            hevc_encoder_set_force_idr(enc);
        }

        int written = hevc_encoder_encode_raw(enc, y_plane, (int)width, uv_plane, (int)width, out_buf, out_cap);
        assert(written > 0);
        if (f) fwrite(out_buf, 1, (size_t)written, f);

        if (frame == 0 || frame == 29) {
            int expected_types[] = { 35 /* AUD */, 32 /* VPS */, 33 /* SPS */, 34 /* PPS */, 19 /* IDR_W_RADL */ };
            int ok = check_nal_sequence(out_buf, (size_t)written, expected_types, 5);
            assert(ok && "expected AUD,VPS,SPS,PPS,IDR sequence on keyframe");
            if (frame == 0) idr_bytes = written;
        } else {
            int expected_types[] = { 35 /* AUD */, 1 /* TRAIL_R */ };
            int ok = check_nal_sequence(out_buf, (size_t)written, expected_types, 2);
            assert(ok && "expected AUD,TRAIL_R P-slice NAL");
            if (frame == 1) {
                static_p_bytes = written;
                static_sad = hevc_encoder_get_last_frame_sad(enc);
                assert(static_sad > 0 && "Static P-frame should measure baseline temporal quantization SAD");
            } else if (frame == 16) {
                uint32_t moving_sad = hevc_encoder_get_last_frame_sad(enc);
                assert(moving_sad > static_sad && "Moving frame should have higher motion SAD than static baseline");
            }
        }
    }

    if (f) fclose(f);
    printf("[test_hevc_encode] Multi-frame GOP complete: Frame 0 (IDR) = %d bytes, Frame 1 (Static P) = %d bytes\n",
           idr_bytes, static_p_bytes);
    assert(static_p_bytes < idr_bytes && "P-frame with static content should be smaller than IDR");

    free(y_plane); free(uv_plane); free(out_buf);
    hevc_encoder_destroy(enc);
    printf("[test_hevc_encode] Multi-frame GOP OK.\n");
}

static void test_dynamic_qp_and_rate_control(void) {
    printf("[test_hevc_encode] Testing dynamic QP and rate control...\n");

    const uint32_t width = 128, height = 96, fps = 30, bitrate = 2000000;
    hevc_encoder_t *enc = hevc_encoder_create(NULL, width, height, fps, bitrate);
    assert(enc != NULL);

    /* Verify default properties */
    assert(hevc_encoder_get_gop_size(enc) == 30);
    assert(hevc_encoder_get_qp(enc) == 27);
    assert(hevc_encoder_get_bitrate(enc) == 2000000);
    assert(hevc_encoder_get_fps(enc) == 30);
    assert(hevc_encoder_get_rc_mode(enc) == RC_CQP);

    uint8_t *y_plane = malloc((size_t)width * height);
    uint8_t *uv_plane = malloc((size_t)(width / 2) * (height / 2) * 2);
    const size_t out_cap = (size_t)width * height * 2 + 65536;
    uint8_t *out_buf = malloc(out_cap);
    assert(y_plane && uv_plane && out_buf);

    /* Generate rich pattern with high frequency content */
    for (uint32_t r = 0; r < height; r++)
        for (uint32_t c = 0; c < width; c++)
            y_plane[r * width + c] = (uint8_t)(((r * 17) ^ (c * 31)) & 0xFF);
    for (uint32_t r = 0; r < height / 2; r++)
        for (uint32_t c = 0; c < width / 2; c++) {
            uv_plane[r * width + c * 2 + 0] = (uint8_t)((r * 13 + c * 7) & 0xFF);
            uv_plane[r * width + c * 2 + 1] = (uint8_t)((r * 23 + c * 11) & 0xFF);
        }

    /* Encode at low QP (18) */
    hevc_encoder_set_qp(enc, 18);
    assert(hevc_encoder_get_qp(enc) == 18);
    hevc_encoder_set_force_idr(enc);
    int size_qp18 = hevc_encoder_encode_raw(enc, y_plane, (int)width, uv_plane, (int)width, out_buf, out_cap);
    assert(size_qp18 > 0);

    /* Encode at high QP (40) */
    hevc_encoder_set_qp(enc, 40);
    assert(hevc_encoder_get_qp(enc) == 40);
    hevc_encoder_set_force_idr(enc);
    int size_qp40 = hevc_encoder_encode_raw(enc, y_plane, (int)width, uv_plane, (int)width, out_buf, out_cap);
    assert(size_qp40 > 0);

    printf("  QP 18 IDR size: %d bytes | QP 40 IDR size: %d bytes\n", size_qp18, size_qp40);
    assert(size_qp40 < size_qp18 && "QP 40 output must be more compressed than QP 18");

    /* Test Rate Control Mode and Parameter Setters */
    hevc_encoder_set_rc_mode(enc, RC_VBR);
    assert(hevc_encoder_get_rc_mode(enc) == RC_VBR);

    hevc_encoder_set_bitrate(enc, 6000000);
    assert(hevc_encoder_get_bitrate(enc) == 6000000);

    hevc_encoder_set_fps(enc, 60);
    assert(hevc_encoder_get_fps(enc) == 60);

    hevc_encoder_set_rc_mode(enc, RC_LOW_LATENCY);
    assert(hevc_encoder_get_rc_mode(enc) == RC_LOW_LATENCY);

    /* Encode frames under rate control */
    for (int f = 0; f < 5; f++) {
        int w = hevc_encoder_encode_raw(enc, y_plane, (int)width, uv_plane, (int)width, out_buf, out_cap);
        assert(w > 0);
    }

    free(y_plane); free(uv_plane); free(out_buf);
    hevc_encoder_destroy(enc);
    printf("[test_hevc_encode] Dynamic QP and rate control OK.\n");
}

int main(void) {
    test_transform_round_trip();
    test_transform_butterfly_equivalence();
    test_quantizer_reciprocal_equivalence();
    test_mpm_derivation();
    test_multi_frame_gop();
    test_dynamic_qp_and_rate_control();

    printf("[test_hevc_encode] ALL HEVC BITSTREAM STRUCTURE TESTS PASSED!\n");
    return 0;
}
