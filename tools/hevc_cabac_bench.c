/* bc250-vulkan-encode-stopgap - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_cabac_bench.c - off-board harness for the CPU HEVC path, built to
 * answer one question first: what share of frame time does
 * hevc_cabac_code_residual_4x4() ACTUALLY have?
 *
 * WHY THIS EXISTS
 * ---------------
 * docs/backlog.md A4 sizes that function at "~5.9% of profile". 5.9% is
 * also the exact number gprof gave for bs_rbsp_to_ebsp (B2), which direct
 * clock_gettime instrumentation put at 0.93% - after 111 lines of
 * optimisation had already been written against it. gprof's sample bucket
 * on this codebase is 10 ms against a ~0.6 s run, so a single sample reads
 * as ~1.7% and the same bucket on a shorter run inflates to 5.9%.
 * docs/performance-measurement.md's first rule is therefore: instrument
 * the function directly before committing to the work.
 *
 * This tool does that, off-board, on the REAL frame path.
 * hevc_encoder_encode_raw() is GPU-free (see encoder_h265.h), so the whole
 * CPU HEVC encode - intra mode search, DST/DCT, quantise, dequantise,
 * reconstruct, CABAC - runs on a dev machine with no board and no GPU.
 * Nothing is synthesised: the coefficients reaching residual coding are
 * the ones the encoder actually produced for the picture.
 *
 * It is a CMake target, not a `gcc` line, for the reason
 * docs/performance-measurement.md gives: tools/hevc_host_drift.sh builds
 * its repro by hand at -O2, and an -O2 number on this codebase has been
 * wrong in BOTH directions (+15.4% -> +2.1% one way, +29.4% -> +44.5% the
 * other). Configured as part of the project this inherits the shipped
 * -O3 -DNDEBUG, and opts into -march=znver2 the way cavlc_bench does.
 * -falign-functions=64 -falign-loops=32 are pinned for the reason they
 * were pinned there: pure hot-loop alignment luck produced a 1.30x
 * phantom on this codebase once.
 *
 * TWO INDEPENDENT ESTIMATES OF THE SHARE, because one is not enough
 * -----------------------------------------------------------------
 *  `share`  - rdtsc bracket around each call (profile build). Measures
 *             the function and nothing else, but pays ~2 rdtsc per call
 *             and cannot see work that only exists because the function
 *             emitted bytes. The rdtsc pair cost is calibrated in-process
 *             and subtracted, and the raw (unsubtracted) figure is
 *             printed too so the correction is visible rather than
 *             hidden.
 *  `ablate` - the function is switched to an immediate return and the
 *             whole-frame time is remeasured, ABBAABBA-interleaved in one
 *             process against the un-ablated side. Includes the
 *             downstream cost of the bytes it writes (bs_rbsp_to_ebsp
 *             over a longer RBSP), so it should read slightly HIGHER
 *             than `share`. Ablating produces a garbage bitstream, which
 *             is fine and is why it is a separate mode from everything
 *             that checks output.
 *
 * If those two disagree by a lot, neither is a finding. The A/A row in
 * `ablate` is the rig's own resolution floor: a rig that cannot report
 * 1.00x with both sides running identical code cannot report anything.
 *
 * Rate control is pinned to RC_CQP. In any other mode rc_update_stats()
 * drains its leaky bucket by real elapsed CLOCK_MONOTONIC seconds, which
 * makes the bitstream a function of how fast the machine ran
 * (docs/performance-measurement.md) - fatal for a timing harness that
 * also wants byte-exactness.
 *
 * USAGE
 *   hevc_cabac_bench stats  [opts]           what reaches residual coding (prof)
 *   hevc_cabac_bench share  [opts]           rdtsc share of frame time (prof)
 *   hevc_cabac_bench ablate [opts]           ablation share + A/A floor (prof)
 *   hevc_cabac_bench bench  [opts]           frame time, best+median (both builds)
 *   hevc_cabac_bench emit   [opts] --out=F   write Annex-B .hevc for md5/ffmpeg
 *
 * OPTIONS
 *   --size=WxH     default 1280x720
 *   --qp=N         default 27
 *   --frames=N     frames per pass, default 4
 *   --gop=N        default 1 (all-intra). Must divide --frames.
 *   --pattern=N    0 flat, 1 bars, 2 diagonal ramp, 3 pseudo-random (default 3)
 *   --input=FILE   NV12 frames at --size instead of a synthetic pattern.
 *                  The synthetic patterns bracket the answer but none of
 *                  them IS representative content - pattern 3 is
 *                  near-incompressible noise and pattern 1 is almost
 *                  flat, and the residual-coding share differs by 16x
 *                  between them. Quote a figure from real frames.
 *   --samples=N    timing samples per side, default 9 (>=7 required)
 *   --iters=N      passes over the frame pool per timing sample (0 = auto)
 *   --out=FILE     output path for `emit`
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "encoder_h265.h"
#include "hevc_cabac.h"

/* ------------------------------------------------------------------ */

typedef struct {
    int w, h, qp, frames, gop, pattern, samples;
    long iters;
    const char *out;
    const char *input;
} opts_t;

static void opts_default(opts_t *o) {
    o->w = 1280; o->h = 720; o->qp = 27; o->frames = 4; o->gop = 1;
    o->pattern = 3; o->samples = 9; o->iters = 0; o->out = NULL; o->input = NULL;
}

static int parse_opts(opts_t *o, int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--size=", 7)) {
            if (sscanf(a + 7, "%dx%d", &o->w, &o->h) != 2) return 0;
        } else if (!strncmp(a, "--qp=", 5))      o->qp = atoi(a + 5);
        else if (!strncmp(a, "--frames=", 9))    o->frames = atoi(a + 9);
        else if (!strncmp(a, "--gop=", 6))       o->gop = atoi(a + 6);
        else if (!strncmp(a, "--pattern=", 10))  o->pattern = atoi(a + 10);
        else if (!strncmp(a, "--samples=", 10))  o->samples = atoi(a + 10);
        else if (!strncmp(a, "--iters=", 8))     o->iters = atol(a + 8);
        else if (!strncmp(a, "--out=", 6))       o->out = a + 6;
        else if (!strncmp(a, "--input=", 8))     o->input = a + 8;
        else { fprintf(stderr, "unknown option: %s\n", a); return 0; }
    }
    if (o->w < 4 || o->h < 4 || o->frames < 1 || o->gop < 1) return 0;
    if (o->frames % o->gop) {
        fprintf(stderr, "--frames must be a multiple of --gop so every pass "
                        "starts on an IDR (see pool_encode)\n");
        return 0;
    }
    if (o->samples < 7) { fprintf(stderr, "--samples must be >= 7\n"); return 0; }
    return 1;
}

/* Same synthetic content generator as tools/hevc_host_repro.c, so a case
 * measured here can be re-run under the drift oracle unchanged. */
static void fill(uint8_t *y, uint8_t *uv, int w, int h, int pat, int frame) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int v;
            switch (pat) {
            case 0:  v = 128; break;
            case 1:  v = ((i / 8) & 1) ? 200 : 40; break;
            case 2:  v = (i + j + frame * 3) & 0xFF; break;
            default: v = (int)((((unsigned)(i * 73 + j * 151 + frame * 37) >> 3)
                                * 2654435761u) >> 24); break;
            }
            y[j * w + i] = (uint8_t)v;
        }
    for (int j = 0; j < h / 2; j++)
        for (int i = 0; i + 1 < w; i += 2) {
            int v = (pat == 0) ? 128 : (128 + ((i / 16 + j / 16) & 1) * 40);
            uv[j * w + i]     = (uint8_t)v;
            uv[j * w + i + 1] = (uint8_t)(255 - v);
        }
}

typedef struct {
    uint8_t **y, **uv;
    uint8_t  *bs;
    size_t    cap;
    int       n;
} pool_t;

static int pool_init(pool_t *p, const opts_t *o) {
    p->n = o->frames;
    p->y  = calloc((size_t)p->n, sizeof(uint8_t *));
    p->uv = calloc((size_t)p->n, sizeof(uint8_t *));
    if (!p->y || !p->uv) return 0;
    FILE *fin = NULL;
    if (o->input) {
        fin = fopen(o->input, "rb");
        if (!fin) { perror(o->input); return 0; }
    }
    for (int k = 0; k < p->n; k++) {
        size_t ysz = (size_t)o->w * o->h, uvsz = (size_t)o->w * (o->h / 2);
        p->y[k]  = malloc(ysz);
        p->uv[k] = malloc(uvsz);
        if (!p->y[k] || !p->uv[k]) return 0;
        if (fin) {
            /* Short files loop, like tools/hevc_host_repro.c. A truncated
             * read that silently left a zeroed plane would look like
             * cheap, flat content and quietly deflate every number here. */
            if (fread(p->y[k], 1, ysz, fin) != ysz || fread(p->uv[k], 1, uvsz, fin) != uvsz) {
                rewind(fin);
                if (fread(p->y[k], 1, ysz, fin) != ysz ||
                    fread(p->uv[k], 1, uvsz, fin) != uvsz) {
                    fprintf(stderr, "%s: too short for one %dx%d NV12 frame\n",
                            o->input, o->w, o->h);
                    return 0;
                }
            }
        } else {
            fill(p->y[k], p->uv[k], o->w, o->h, o->pattern, k);
        }
    }
    if (fin) fclose(fin);
    size_t cw = (size_t)((o->w + 15) / 16 * 16), ch = (size_t)((o->h + 15) / 16 * 16);
    p->cap = cw * ch * 3 + (1u << 20);
    p->bs = malloc(p->cap);
    return p->bs != NULL;
}

static hevc_encoder_t *make_encoder(const opts_t *o) {
    hevc_encoder_t *e = hevc_encoder_create(NULL, (uint32_t)o->w, (uint32_t)o->h,
                                            60, 10000000u);
    if (!e) return NULL;
    /* RC_CQP: anything else makes the bitstream a function of wall time. */
    hevc_encoder_set_rc_mode(e, RC_CQP);
    hevc_encoder_set_qp(e, o->qp);
    hevc_encoder_set_gop_size(e, (uint32_t)o->gop);
    return e;
}

/* One pass over the pool. frames % gop == 0 is enforced in parse_opts, so
 * encoder->frame_count % gop_size is 0 again at the start of every pass and
 * every pass is the identical sequence of IDR + P frames. */
static uint64_t pool_encode(hevc_encoder_t *e, pool_t *p, const opts_t *o, long iters) {
    uint64_t bytes = 0;
    for (long it = 0; it < iters; it++)
        for (int k = 0; k < p->n; k++) {
            int n = hevc_encoder_encode_raw(e, p->y[k], o->w, p->uv[k], o->w,
                                            p->bs, p->cap);
            if (n <= 0) { fprintf(stderr, "encode_raw returned %d\n", n); exit(1); }
            bytes += (uint64_t)n;
        }
    return bytes;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t timed_pass(hevc_encoder_t *e, pool_t *p, const opts_t *o,
                           long iters, uint64_t *sink) {
    uint64_t t0 = now_ns();
    uint64_t b  = pool_encode(e, p, o, iters);
    uint64_t t1 = now_ns();
    *sink += b;
    return t1 - t0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static long calibrate_iters(hevc_encoder_t *e, pool_t *p, const opts_t *o) {
    if (o->iters > 0) return o->iters;
    uint64_t sink = 0;
    long it = 1;
    for (;;) {
        uint64_t ns = timed_pass(e, p, o, it, &sink);
        if (ns > 80000000ull || it > 4096) {
            long want = (long)((double)it * 300e6 / (double)(ns ? ns : 1));
            return want < 1 ? 1 : want;
        }
        it *= 4;
    }
}

/* ------------------------------------------------------------------ */

#ifdef HEVC_CABAC_PROFILE
/* Cost of the rdtsc pair itself, measured with the same instruction
 * sequence the wrapper uses, so the correction applied to `share` is a
 * measurement and not an assumption. Median of many batches. */
static double calibrate_rdtsc_pair(void) {
    enum { BATCH = 1024, ROUNDS = 201 };
    uint64_t v[ROUNDS];
    volatile uint64_t acc = 0;
    for (int r = 0; r < ROUNDS; r++) {
        uint64_t t = 0;
        for (int i = 0; i < BATCH; i++) {
            uint64_t a = __builtin_ia32_rdtsc();
            uint64_t b = __builtin_ia32_rdtsc();
            t += b - a;
        }
        acc += t;
        v[r] = t;
    }
    (void)acc;
    qsort(v, ROUNDS, sizeof(uint64_t), cmp_u64);
    return (double)v[ROUNDS / 2] / (double)BATCH;
}

typedef struct { double best_a, med_a, best_b, med_b; } ab_result_t;

/* One interleaved A/B over the real frame path. mode_a == mode_b makes it
 * the rig self-test. ABBAABBA, not ABABAB: plain alternation left whichever
 * side ran first in each pair carrying a reproducible ~2% penalty with both
 * sides running identical code, on cavlc_bench's version of this loop. */
static ab_result_t run_ab(hevc_encoder_t *e, pool_t *p, const opts_t *o,
                          int mode_a, int mode_b, long iters, uint64_t *sink) {
    int samples = o->samples;
    uint64_t *va = malloc(sizeof(uint64_t) * (size_t)samples);
    uint64_t *vb = malloc(sizeof(uint64_t) * (size_t)samples);
    ab_result_t r = {0, 0, 0, 0};
    if (!va || !vb) { free(va); free(vb); return r; }

    for (int s = 0; s < samples; s++) {
        int a_first = ((s & 1) == 0);
        for (int half = 0; half < 2; half++) {
            int doing_a = ((half == 0) == a_first);
            g_hevc_res4_mode = doing_a ? mode_a : mode_b;
            uint64_t ns = timed_pass(e, p, o, iters, sink);
            if (doing_a) va[s] = ns; else vb[s] = ns;
        }
    }
    g_hevc_res4_mode = HEVC_RES4_MODE_OFF;
    qsort(va, (size_t)samples, sizeof(uint64_t), cmp_u64);
    qsort(vb, (size_t)samples, sizeof(uint64_t), cmp_u64);
    r.best_a = (double)va[0];
    r.best_b = (double)vb[0];
    r.med_a  = (samples & 1) ? (double)va[samples / 2]
                             : ((double)va[samples / 2 - 1] + (double)va[samples / 2]) / 2.0;
    r.med_b  = (samples & 1) ? (double)vb[samples / 2]
                             : ((double)vb[samples / 2 - 1] + (double)vb[samples / 2]) / 2.0;
    free(va); free(vb);
    return r;
}
#endif /* HEVC_CABAC_PROFILE */

static void banner(const opts_t *o) {
    printf("  %dx%d qp=%d frames=%d gop=%d ", o->w, o->h, o->qp, o->frames, o->gop);
    if (o->input) printf("input=%s", o->input); else printf("pattern=%d", o->pattern);
    printf("  (CPU HEVC path, hevc_encoder_encode_raw, RC_CQP)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s stats|share|ablate|bench|emit [opts]\n"
                        "see the comment at the top of tools/hevc_cabac_bench.c\n",
                argv[0]);
        return 2;
    }
    const char *cmd = argv[1];
    opts_t o; opts_default(&o);
    if (!parse_opts(&o, argc, argv)) return 2;

    pool_t pool;
    memset(&pool, 0, sizeof(pool));
    if (!pool_init(&pool, &o)) { fprintf(stderr, "oom\n"); return 1; }
    hevc_encoder_t *enc = make_encoder(&o);
    if (!enc) { fprintf(stderr, "hevc_encoder_create failed\n"); return 1; }

    uint64_t sink = 0;

    if (!strcmp(cmd, "emit")) {
        if (!o.out) { fprintf(stderr, "emit needs --out=FILE\n"); return 2; }
        FILE *f = fopen(o.out, "wb");
        if (!f) { perror("fopen"); return 1; }
        for (int k = 0; k < pool.n; k++) {
            int n = hevc_encoder_encode_raw(enc, pool.y[k], o.w, pool.uv[k], o.w,
                                            pool.bs, pool.cap);
            if (n <= 0) { fprintf(stderr, "frame %d: %d\n", k, n); return 1; }
            fwrite(pool.bs, 1, (size_t)n, f);
            printf("frame %d: %d bytes\n", k, n);
        }
        fclose(f);
        printf("wrote %s\n", o.out);
        hevc_encoder_destroy(enc);
        return 0;
    }

    if (!strcmp(cmd, "bench")) {
        printf("bench - whole-frame encode time\n");
        banner(&o);
        long iters = calibrate_iters(enc, &pool, &o);
        (void)timed_pass(enc, &pool, &o, 1, &sink);      /* warm */
        uint64_t *v = malloc(sizeof(uint64_t) * (size_t)o.samples);
        for (int s = 0; s < o.samples; s++)
            v[s] = timed_pass(enc, &pool, &o, iters, &sink);
        qsort(v, (size_t)o.samples, sizeof(uint64_t), cmp_u64);
        double per_frame_best = (double)v[0] / (double)(iters * pool.n) / 1e6;
        double per_frame_med  = (double)v[o.samples / 2] / (double)(iters * pool.n) / 1e6;
        printf("  iters/sample %ld  samples %d\n", iters, o.samples);
        printf("  per frame: best %.3f ms  median %.3f ms  worst %.3f ms\n",
               per_frame_best, per_frame_med,
               (double)v[o.samples - 1] / (double)(iters * pool.n) / 1e6);
        printf("  spread (worst-best)/best = %.2f%%\n",
               100.0 * ((double)v[o.samples - 1] - (double)v[0]) / (double)v[0]);
        free(v);
        printf("  [sink %llu]\n", (unsigned long long)sink);
        hevc_encoder_destroy(enc);
        return 0;
    }

#ifndef HEVC_CABAC_PROFILE
    fprintf(stderr, "'%s' needs the profile build (hevc_cabac_bench_prof)\n", cmd);
    return 2;
#else
    if (!strcmp(cmd, "stats")) {
        printf("stats - what reaches hevc_cabac_code_residual_4x4()\n");
        banner(&o);
        g_hevc_res4_calls = g_hevc_res4_nonzero = g_hevc_res4_luma = 0;
        g_hevc_res4_mode = HEVC_RES4_MODE_STATS;
        uint64_t bytes = pool_encode(enc, &pool, &o, 1);
        g_hevc_res4_mode = HEVC_RES4_MODE_OFF;

        /* Every 8x8 CU offers 4 luma + 2 chroma 4x4 TUs; a call happens only
         * when that TU's cbf is set, so calls/possible is the cbf hit rate. */
        long cus = (long)(((o.w + 15) / 16) * ((o.h + 15) / 16)) * 4;
        long possible = cus * 6 * o.frames;
        printf("  bytes/frame            %.0f\n", (double)bytes / o.frames);
        printf("  residual_4x4 calls     %llu  (%.1f per frame)\n",
               (unsigned long long)g_hevc_res4_calls,
               (double)g_hevc_res4_calls / o.frames);
        printf("  coded TUs / possible   %llu / %ld = %.1f%%\n",
               (unsigned long long)g_hevc_res4_calls, possible,
               100.0 * (double)g_hevc_res4_calls / (double)(possible ? possible : 1));
        printf("  luma / chroma calls    %llu / %llu\n",
               (unsigned long long)g_hevc_res4_luma,
               (unsigned long long)(g_hevc_res4_calls - g_hevc_res4_luma));
        printf("  nonzero coeffs / call  %.2f  (of 16)\n",
               (double)g_hevc_res4_nonzero / (double)(g_hevc_res4_calls ? g_hevc_res4_calls : 1));
        hevc_encoder_destroy(enc);
        return 0;
    }

    if (!strcmp(cmd, "share")) {
        printf("share - rdtsc bracket on hevc_cabac_code_residual_4x4()\n");
        banner(&o);
        double pair = calibrate_rdtsc_pair();
        printf("  rdtsc-pair cost        %.2f cycles (median of 201 batches of 1024)\n", pair);

        (void)pool_encode(enc, &pool, &o, 1);     /* warm */
        g_hevc_res4_cycles = g_hevc_res4_calls = 0;
        g_hevc_res4_mode = HEVC_RES4_MODE_CYCLES;
        uint64_t c0 = __builtin_ia32_rdtsc();
        uint64_t w0 = now_ns();
        (void)pool_encode(enc, &pool, &o, 1);
        uint64_t w1 = now_ns();
        uint64_t c1 = __builtin_ia32_rdtsc();
        g_hevc_res4_mode = HEVC_RES4_MODE_OFF;

        double total = (double)(c1 - c0);
        double raw   = (double)g_hevc_res4_cycles;
        double corr  = raw - pair * (double)g_hevc_res4_calls;
        printf("  frames                 %d   (%.3f ms/frame wall)\n",
               o.frames, (double)(w1 - w0) / 1e6 / o.frames);
        printf("  calls                  %llu\n", (unsigned long long)g_hevc_res4_calls);
        printf("  cycles in function     %.0f raw, %.0f after removing the rdtsc pairs\n",
               raw, corr);
        printf("  cycles per call        %.1f raw, %.1f corrected\n",
               raw / (double)(g_hevc_res4_calls ? g_hevc_res4_calls : 1),
               corr / (double)(g_hevc_res4_calls ? g_hevc_res4_calls : 1));
        printf("  total encode cycles    %.0f\n", total);
        printf("  SHARE OF FRAME TIME    %.2f%% raw, %.2f%% corrected\n",
               100.0 * raw / total, 100.0 * corr / total);
        printf("  (the corrected figure is the one to quote; it still counts\n"
               "   the noinline call boundary the wrapper forces)\n");
        hevc_encoder_destroy(enc);
        return 0;
    }

    if (!strcmp(cmd, "ablate")) {
        printf("ablate - frame time with residual_4x4 switched to an immediate return\n");
        banner(&o);
        long iters = calibrate_iters(enc, &pool, &o);
        (void)timed_pass(enc, &pool, &o, 1, &sink);   /* warm */

        ab_result_t aa = run_ab(enc, &pool, &o, HEVC_RES4_MODE_OFF,
                                HEVC_RES4_MODE_OFF, iters, &sink);
        ab_result_t ab = run_ab(enc, &pool, &o, HEVC_RES4_MODE_OFF,
                                HEVC_RES4_MODE_ABLATE, iters, &sink);
        double nf = (double)(iters * pool.n);
        printf("  iters/sample %ld  samples %d\n", iters, o.samples);
        printf("  A/A  floor   B/A best %.4fx  median %.4fx   <- the rig's resolution\n",
               aa.best_b / aa.best_a, aa.med_b / aa.med_a);
        printf("  A    normal  best %.3f ms/frame  median %.3f ms/frame\n",
               aa.best_a / nf / 1e6, aa.med_a / nf / 1e6);
        printf("  B    ablated best %.3f ms/frame  median %.3f ms/frame\n",
               ab.best_b / nf / 1e6, ab.med_b / nf / 1e6);
        printf("  SHARE OF FRAME TIME  %.2f%% (best)  %.2f%% (median)\n",
               100.0 * (ab.best_a - ab.best_b) / ab.best_a,
               100.0 * (ab.med_a - ab.med_b) / ab.med_a);
        printf("  (includes the downstream cost of the bytes it writes, so it\n"
               "   should read a little HIGHER than `share`)\n");
        printf("  [sink %llu]\n", (unsigned long long)sink);
        hevc_encoder_destroy(enc);
        return 0;
    }

    fprintf(stderr, "unknown command: %s\n", cmd);
    return 2;
#endif
}
