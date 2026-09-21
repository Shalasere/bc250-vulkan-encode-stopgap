/* Host-side reproduction of the CPU HEVC luma drift - no GPU required.
 *
 * hevc_encoder_encode_raw() is the GPU-free entry point, so the whole
 * root-cause loop (encode -> dump recon -> decode -> diff) runs on a dev
 * machine. Writes <out>.hevc and, via BC250_HEVC_DEBUG_RECON, the
 * encoder's own reconstruction as planar I420 at CODED dimensions.
 *
 * usage: hostrepro <w> <h> <qp> <frames> <out-prefix> [pattern] [gop] [kbps]
 *   pattern: 0 = flat grey (expected byte-exact), 1 = vertical bars,
 *            2 = diagonal ramp, 3 = pseudo-random blocks
 *   gop:     1 (default) = force an IDR on every frame, which is what the
 *            intra cases want and what this tool did unconditionally before.
 *            >1 = a real GOP: one IDR then gop-1 P-frames, so the drift
 *            oracle actually covers the inter path (cu_skip/merge, the
 *            reference-picture chain, the P-slice header). Without this the
 *            oracle could not see an inter bug at all - every frame was an
 *            IDR, so encoder_h265.c's entire !is_idr branch was dead code
 *            under the strongest check this project has.
 *
 * Patterns 2 and 3 are frame-dependent (they advance with `frame`), so a
 * multi-frame run is genuinely moving content, not a repeated still.
 *
 *   kbps:    0 (default) = constant QP, which is what every byte-exactness
 *            case wants. >0 switches to VBR rate control at that bitrate,
 *            so QP moves from frame to frame - which is how the driver
 *            actually runs (`lab qsweep` passes a bitrate, never a QP) and
 *            which exercises a non-zero slice_qp_delta on P-frames and a
 *            CABAC context re-init at a QP the parameter sets did not
 *            announce. Constant-QP cases cannot reach either.
 *
 * BC250_HOSTREPRO_INPUT=<file.nv12> reads real frames (NV12, w x h, no
 * padding, frames back to back) instead of a synthetic pattern, so the
 * quality figures `tools/lab qsweep` produces on the board can be
 * reproduced on a dev machine with no GPU. Short files loop. This is what
 * lets a PSNR question about the inter path be answered off-board; the
 * synthetic patterns are for byte-exactness, not for quality.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "encoder_h265.h"

/* Wall-clock spent inside hevc_encoder_encode_raw(), and nothing else -
 * not the synthetic pattern fill, not the bitstream fwrite, not process
 * startup. Printed as the last line so this harness can drive an A/B of a
 * CPU-path change when built through CMake at the shipped -O3/-march (see
 * the hevc_host_repro target in approach1-compute-encoder/CMakeLists.txt;
 * tools/hevc_host_drift.sh's hand-rolled -O2 build is for the correctness
 * oracle, never for a number). */
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void fill(uint8_t *y, uint8_t *uv, int w, int h, int pat, int frame) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int v;
            switch (pat) {
            case 0:  v = 128; break;
            case 1:  v = ((i / 8) & 1) ? 200 : 40; break;
            case 2:  v = (i + j + frame * 3) & 0xFF; break;
            default: v = (((i * 73 + j * 151 + frame * 37) >> 3) * 2654435761u) >> 24; break;
            }
            y[j * w + i] = (uint8_t)v;
        }
    /* i + 1 < w, not i < w: an odd width would otherwise write one byte past
     * the end of the row (and of the allocation on the last row). */
    for (int j = 0; j < h / 2; j++)
        for (int i = 0; i + 1 < w; i += 2) {
            int v = (pat == 0) ? 128 : (128 + ((i / 16 + j / 16) & 1) * 40);
            uv[j * w + i]     = (uint8_t)v;
            uv[j * w + i + 1] = (uint8_t)(255 - v);
        }
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s w h qp frames out [pattern] [gop] [kbps]\n", argv[0]); return 2; }
    int w = atoi(argv[1]), h = atoi(argv[2]), qp = atoi(argv[3]), nf = atoi(argv[4]);
    const char *out = argv[5];
    int pat = (argc > 6) ? atoi(argv[6]) : 1;
    int gop = (argc > 7) ? atoi(argv[7]) : 1;
    int kbps = (argc > 8) ? atoi(argv[8]) : 0;
    if (gop < 1) gop = 1;

    hevc_encoder_t *enc = hevc_encoder_create(NULL, w, h, 60,
                                              kbps > 0 ? (uint32_t)kbps * 1000u : 10000000u);
    if (!enc) { fprintf(stderr, "encoder_create failed\n"); return 1; }
    hevc_encoder_set_rc_mode(enc, kbps > 0 ? RC_VBR : RC_CQP);
    hevc_encoder_set_qp(enc, qp);
    hevc_encoder_set_gop_size(enc, (uint32_t)gop);

    uint8_t *y  = malloc((size_t)w * h);
    uint8_t *uv = malloc((size_t)w * (h / 2));
    /* Must clear the encoder's own worst case, not scale with the picture:
     * 2 bytes/luma-sample at the CODED (16-aligned) size, plus a fixed
     * allowance for parameter sets. The old `w * h * 3` was both too small
     * at low QP on busy content *and*, for a picture smaller than one CTU,
     * smaller than VPS+SPS+PPS alone - 4x4 gave 48 bytes of budget for a
     * ~140-byte access unit, and encode_raw() failed with -1. That looked
     * like an encoder limit at small sizes and was only ever this buffer. */
    size_t cw = (size_t)((w + 15) / 16 * 16), chh = (size_t)((h + 15) / 16 * 16);
    size_t cap = cw * chh * 3 + (1u << 20);
    uint8_t *bs = malloc(cap);
    if (!y || !uv || !bs) { fprintf(stderr, "oom\n"); return 1; }

    char path[512];
    snprintf(path, sizeof(path), "%s.hevc", out);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return 1; }

    const char *inpath = getenv("BC250_HOSTREPRO_INPUT");
    FILE *fin = NULL;
    if (inpath) {
        fin = fopen(inpath, "rb");
        if (!fin) { perror("BC250_HOSTREPRO_INPUT"); return 1; }
    }

    double encode_ms = 0.0;
    for (int k = 0; k < nf; k++) {
        if (fin) {
            size_t ysz = (size_t)w * h, uvsz = (size_t)w * (h / 2);
            if (fread(y, 1, ysz, fin) != ysz || fread(uv, 1, uvsz, fin) != uvsz) {
                rewind(fin);
                if (fread(y, 1, ysz, fin) != ysz || fread(uv, 1, uvsz, fin) != uvsz) {
                    fprintf(stderr, "input too short for one %dx%d NV12 frame\n", w, h);
                    return 1;
                }
            }
        } else {
            fill(y, uv, w, h, pat, k);
        }
        /* gop == 1 keeps the original unconditional-IDR behaviour exactly.
         * For gop > 1 the encoder's own frame_count % gop_size rule decides,
         * so frame 0 is the IDR and the rest are P. */
        if (gop == 1) hevc_encoder_set_force_idr(enc);
        double t0 = now_ms();
        int n = hevc_encoder_encode_raw(enc, y, w, uv, w, bs, cap);
        encode_ms += now_ms() - t0;
        if (n <= 0) { fprintf(stderr, "frame %d: encode returned %d\n", k, n); return 1; }
        fwrite(bs, 1, (size_t)n, f);
        printf("frame %d: %d bytes\n", k, n);
    }
    fclose(f);
    if (fin) fclose(fin);
    printf("wrote %s.hevc (%dx%d qp=%d pattern=%d gop=%d frames=%d kbps=%d)\n",
           out, w, h, qp, pat, gop, nf, kbps);
    printf("encode_ms_total %.3f\n", encode_ms);
    hevc_encoder_destroy(enc);
    return 0;
}
