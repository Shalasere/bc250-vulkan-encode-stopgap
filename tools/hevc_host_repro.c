/* Host-side reproduction of the CPU HEVC luma drift - no GPU required.
 *
 * hevc_encoder_encode_raw() is the GPU-free entry point, so the whole
 * root-cause loop (encode -> dump recon -> decode -> diff) runs on a dev
 * machine. Writes <out>.hevc and, via BC250_HEVC_DEBUG_RECON, the
 * encoder's own reconstruction as planar I420 at CODED dimensions.
 *
 * usage: hostrepro <w> <h> <qp> <frames> <out-prefix> [pattern]
 *   pattern: 0 = flat grey (expected byte-exact), 1 = vertical bars,
 *            2 = diagonal ramp, 3 = pseudo-random blocks
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "encoder_h265.h"

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
    for (int j = 0; j < h / 2; j++)
        for (int i = 0; i < w; i += 2) {
            int v = (pat == 0) ? 128 : (128 + ((i / 16 + j / 16) & 1) * 40);
            uv[j * w + i]     = (uint8_t)v;
            uv[j * w + i + 1] = (uint8_t)(255 - v);
        }
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s w h qp frames out [pattern]\n", argv[0]); return 2; }
    int w = atoi(argv[1]), h = atoi(argv[2]), qp = atoi(argv[3]), nf = atoi(argv[4]);
    const char *out = argv[5];
    int pat = (argc > 6) ? atoi(argv[6]) : 1;

    hevc_encoder_t *enc = hevc_encoder_create(NULL, w, h, 60, 10000000);
    if (!enc) { fprintf(stderr, "encoder_create failed\n"); return 1; }
    hevc_encoder_set_rc_mode(enc, RC_CQP);
    hevc_encoder_set_qp(enc, qp);
    hevc_encoder_set_gop_size(enc, 1);

    uint8_t *y  = malloc((size_t)w * h);
    uint8_t *uv = malloc((size_t)w * (h / 2));
    size_t cap = (size_t)w * h * 3;
    uint8_t *bs = malloc(cap);

    char path[512];
    snprintf(path, sizeof(path), "%s.hevc", out);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen"); return 1; }

    for (int k = 0; k < nf; k++) {
        fill(y, uv, w, h, pat, k);
        hevc_encoder_set_force_idr(enc);
        int n = hevc_encoder_encode_raw(enc, y, w, uv, w, bs, cap);
        if (n <= 0) { fprintf(stderr, "frame %d: encode returned %d\n", k, n); return 1; }
        fwrite(bs, 1, (size_t)n, f);
        printf("frame %d: %d bytes\n", k, n);
    }
    fclose(f);
    printf("wrote %s.hevc (%dx%d qp=%d pattern=%d)\n", out, w, h, qp, pat);
    hevc_encoder_destroy(enc);
    return 0;
}
