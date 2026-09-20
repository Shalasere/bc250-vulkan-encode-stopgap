/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
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

/*
 * Angular intra prediction (ITU-T H.265 8.4.4.2.6) against hand-derived
 * spec values.
 *
 * Block (8,16) of a 32x32 luma plane is used because it is the rare
 * position where ALL FIVE neighbour groups - left, above, above-right,
 * below-left and the corner - are genuinely z-scan available (ranks 33,
 * 14, 15, 35 and 11 against the block's own 36, see hevc_intra.c's
 * zorder_rank()). That matters: anywhere else the 8.4.4.2.2 substitution
 * scan would rewrite some references, and the expected values below would
 * be testing the substitution rather than the angular derivation. The
 * whole reference set is therefore just direct plane reads.
 */
static void test_angular_prediction(void) {
    printf("[test_hevc_encode] Angular intra prediction vs. spec-derived values...\n");
    enum { S = 32, X0 = 8, Y0 = 16 };
    static uint8_t plane[S * S];
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++)
            plane[y * S + x] = (uint8_t)((x * 7 + y * 13 + ((x * y) & 31)) & 0xff);

    uint8_t left[8], top[8], corner = plane[(Y0 - 1) * S + (X0 - 1)];
    for (int i = 0; i < 8; i++) {
        left[i] = plane[(Y0 + i) * S + (X0 - 1)];
        top[i]  = plane[(Y0 - 1) * S + (X0 + i)];
    }

    uint8_t pred[16];

    /* Mode 34: intraPredAngle = +32, vertical family. iIdx = y+1 and
     * iFact = 0 for every row, so predSamples[x][y] = ref[x+y+2] and
     * ref[k>=1] is p[k-1][-1]. */
    hevc_predict_4x4(plane, S, S, S, X0, Y0, 34, 1, pred);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            assert(pred[y * 4 + x] == top[x + y + 1] && "mode 34 must read straight down the top row");

    /* Mode 2: same angle, horizontal family - the exact transpose, reading
     * the left column instead. */
    hevc_predict_4x4(plane, S, S, S, X0, Y0, 2, 1, pred);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            assert(pred[y * 4 + x] == left[x + y + 1] && "mode 2 must read straight down the left column");

    /* Mode 18: intraPredAngle = -32, the one diagonal that needs the
     * negative half of ref[] projected from the opposite edge via
     * invAngle (Table 8-6 entry -256). iFact = 0, so predSamples[x][y] =
     * ref[x-y] exactly: the corner on the main diagonal, the top row above
     * it, the left column below it. */
    hevc_predict_4x4(plane, S, S, S, X0, Y0, 18, 1, pred);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            uint8_t want = (x == y) ? corner : (x > y ? top[x - y - 1] : left[y - x - 1]);
            assert(pred[y * 4 + x] == want && "mode 18 diagonal / invAngle projection wrong");
        }

    /* Modes 26 and 10 are angle 0 - pure copy - plus the luma-only edge
     * gradient filter on the first column / first row (8.4.4.2.6's
     * cIdx == 0 && nTbS < 32 clauses). */
    hevc_predict_4x4(plane, S, S, S, X0, Y0, 26, 1, pred);
    for (int y = 0; y < 4; y++) {
        int want0 = top[0] + ((left[y] - corner) >> 1);
        want0 = want0 < 0 ? 0 : (want0 > 255 ? 255 : want0);
        assert(pred[y * 4] == want0 && "mode 26 luma edge filter wrong");
        for (int x = 1; x < 4; x++) assert(pred[y * 4 + x] == top[x]);
    }
    hevc_predict_4x4(plane, S, S, S, X0, Y0, 10, 1, pred);
    for (int x = 0; x < 4; x++) {
        int want0 = left[0] + ((top[x] - corner) >> 1);
        want0 = want0 < 0 ? 0 : (want0 > 255 ? 255 : want0);
        assert(pred[x] == want0 && "mode 10 luma edge filter wrong");
    }
    for (int y = 1; y < 4; y++)
        for (int x = 0; x < 4; x++) assert(pred[y * 4 + x] == left[y]);

    /* Chroma takes no edge filter at all, so 26 and 10 are pure copies. */
    hevc_predict_4x4(plane, S, S, S, X0, Y0, 26, 0, pred);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) assert(pred[y * 4 + x] == pred[x] && "chroma mode 26 must not be edge-filtered");

    /* Every angular mode interpolates between two reference samples, so no
     * output can escape the reference set's own range. This is the check
     * that catches an off-by-one or sign error in ref[] indexing even for
     * the modes with no hand-derived expectation above - including the
     * negative-angle ones whose projected entries are easiest to get
     * wrong. */
    int lo = corner, hi = corner;
    for (int i = 0; i < 8; i++) {
        if (left[i] < lo) lo = left[i];
        if (left[i] > hi) hi = left[i];
        if (top[i] < lo) lo = top[i];
        if (top[i] > hi) hi = top[i];
    }
    for (int mode = 2; mode <= 34; mode++) {
        if (mode == 10 || mode == 26) continue; /* edge filter can leave the range by design */
        hevc_predict_4x4(plane, S, S, S, X0, Y0, mode, 0, pred);
        for (int i = 0; i < 16; i++)
            assert(pred[i] >= lo && pred[i] <= hi && "angular prediction escaped its reference range");
    }

    printf("[test_hevc_encode] Angular intra prediction OK (all 33 angular modes).\n");
}

/* The mode decision must only ever return a mode this encoder can actually
 * signal and a decoder can actually reproduce (0..34). */
static void test_mode_decision_range(void) {
    printf("[test_hevc_encode] Mode decision range...\n");
    enum { S = 64 };
    static uint8_t src[S * S], recon[S * S];
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            src[y * S + x] = (uint8_t)((x * 3) ^ (y * 5));
            recon[y * S + x] = (uint8_t)((x + y) * 2);
        }
    int mpm[3];
    hevc_derive_mpm(HEVC_MODE_DC, 1, HEVC_MODE_VERTICAL, 1, mpm);
    for (int y = 0; y < S; y += 4)
        for (int x = 0; x < S; x += 4) {
            int m = hevc_choose_luma_mode(src, recon, S, S, S, x, y, mpm, 27);
            assert(m >= 0 && m < HEVC_MODE_COUNT && "mode decision returned an unsignalable mode");
            int m2 = hevc_choose_luma_mode(src, recon, S, S, S, x, y, NULL, 27);
            assert(m2 >= 0 && m2 < HEVC_MODE_COUNT);
        }
    printf("[test_hevc_encode] Mode decision range OK.\n");
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

int main(void) {
    test_transform_round_trip();
    test_mpm_derivation();
    test_angular_prediction();
    test_mode_decision_range();

    printf("[test_hevc_encode] Starting H.265 end-to-end bitstream encoding test (GPU-free)...\n");

    const uint32_t width = 128, height = 96, fps = 30, bitrate = 2000000;
    hevc_encoder_t *enc = hevc_encoder_create(NULL, width, height, fps, bitrate);
    assert(enc != NULL);

    uint8_t *y_plane = malloc((size_t)width * height);
    uint8_t *uv_plane = malloc((size_t)(width / 2) * (height / 2) * 2);
    assert(y_plane && uv_plane);
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

    int written = hevc_encoder_encode_raw(enc, y_plane, (int)width, uv_plane, (int)width, out_buf, out_cap);
    printf("[test_hevc_encode] Encoded frame: %d bytes\n", written);
    assert(written > 0);

    int expected_types[] = { 32 /* VPS */, 33 /* SPS */, 34 /* PPS */, 19 /* IDR_W_RADL */ };
    int ok = check_nal_sequence(out_buf, (size_t)written, expected_types, 4);
    assert(ok && "expected VPS,SPS,PPS,IDR NAL sequence not found");
    printf("[test_hevc_encode] NAL sequence (VPS,SPS,PPS,IDR slice) verified.\n");

    FILE *f = fopen("bc250_test_stream.hevc", "wb");
    assert(f != NULL);
    fwrite(out_buf, 1, (size_t)written, f);
    fclose(f);
    printf("[test_hevc_encode] Wrote bc250_test_stream.hevc (%d bytes) for external ffmpeg-decode validation.\n", written);

    free(y_plane); free(uv_plane); free(out_buf);
    hevc_encoder_destroy(enc);

    printf("[test_hevc_encode] ALL HEVC BITSTREAM STRUCTURE TESTS PASSED!\n");
    return 0;
}
