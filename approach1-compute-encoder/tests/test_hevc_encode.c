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
            int expected_types[] = { 32 /* VPS */, 33 /* SPS */, 34 /* PPS */, 19 /* IDR_W_RADL */ };
            int ok = check_nal_sequence(out_buf, (size_t)written, expected_types, 4);
            assert(ok && "expected VPS,SPS,PPS,IDR sequence on keyframe");
            if (frame == 0) idr_bytes = written;
        } else {
            int expected_types[] = { 1 /* TRAIL_R */ };
            int ok = check_nal_sequence(out_buf, (size_t)written, expected_types, 1);
            assert(ok && "expected TRAIL_R P-slice NAL");
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
    test_mpm_derivation();
    test_multi_frame_gop();
    test_dynamic_qp_and_rate_control();

    printf("[test_hevc_encode] ALL HEVC BITSTREAM STRUCTURE TESTS PASSED!\n");
    return 0;
}
