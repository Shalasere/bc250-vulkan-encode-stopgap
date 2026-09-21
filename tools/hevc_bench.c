/* bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * hevc_bench.c - off-board timing harness for the CPU HEVC encode path.
 *
 * WHY THIS EXISTS. `tools/lab` is the canonical performance tool and needs
 * the board. `tools/hevc_host_drift.sh` reaches the same code off-board but
 * builds it by hand at `-O2`, which docs/performance-measurement.md says
 * produces numbers that are wrong in either direction (and has been, twice,
 * in both directions). This is the same sanctioned exception
 * `tools/cavlc_bench.c` already occupies for H.264 CAVLC, applied to HEVC:
 * a real CMake target, so it inherits the shipped `-O3 -DNDEBUG` plus the
 * `-march=znver2` the driver is built with on real hardware, with
 * `-falign-functions=64 -falign-loops=32` pinned for the same reason
 * cavlc_bench pins them (a 1.30x alignment phantom on identical code).
 *
 * `hevc_encoder_encode_raw()` is GPU-free, so the whole shipping CPU HEVC
 * path - intra mode decision, prediction, transform, quantisation, CABAC -
 * runs here with no board and no GPU.
 *
 * WHAT IT DOES NOT DO. It has no pixels and no decoder, so it says nothing
 * about quality or conformance. `tools/hevc_host_drift.sh` remains the
 * correctness oracle; this only answers "how long did it take" and "did the
 * bytes change". Every run prints a checksum of the whole bitstream for
 * exactly that second question - a timing claim on this path is only a
 * result if the checksum is unchanged.
 *
 * usage:
 *   hevc_bench bench   [opts]   timing; prints per-sample ms + median
 *   hevc_bench emit    [opts]   write <prefix>.hevc (for ffmpeg/md5)
 *   hevc_bench_prof profile [opts]   ablation: share of one subsystem
 *
 * opts: --size=WxH --qp=N --frames=N --pattern=N --gop=N --samples=N
 *       --input=<file.nv12> --out=<prefix> --quiet
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "encoder_h265.h"

#ifdef HEVC_INTRA_PROFILE
/* Defined in src/hevc_intra.c behind the same #ifdef. See its comment. */
extern int hevc_intra_prof_dup_mode_search;
extern int hevc_intra_prof_timing;
extern unsigned long long hevc_intra_prof_mode_cycles;
extern unsigned long long hevc_intra_prof_mode_calls;
/* A6 mode-search-perf: per-candidate breakdown within the mode search. */
extern unsigned long long hevc_intra_prof_cyc_predict_planardc;
extern unsigned long long hevc_intra_prof_cyc_predict_angular;
extern unsigned long long hevc_intra_prof_cyc_sad;
extern unsigned long long hevc_intra_prof_calls_predict_planardc;
extern unsigned long long hevc_intra_prof_calls_predict_angular;
extern unsigned long long hevc_intra_prof_calls_sad;
#endif

/* Same synthetic patterns as tools/hevc_host_repro.c, so a timing case and
 * a drift case can name the same content. */
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
        for (int i = 0; i + 1 < w; i += 2) {
            int v = (pat == 0) ? 128 : (128 + ((i / 16 + j / 16) & 1) * 40);
            uv[j * w + i]     = (uint8_t)v;
            uv[j * w + i + 1] = (uint8_t)(255 - v);
        }
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

typedef struct {
    int w, h, qp, frames, pattern, gop, samples, quiet;
    const char *input, *out;
} opts_t;

/* FNV-1a 64 over every byte of every frame. The point is byte-identity
 * across two builds, not cryptographic strength. */
static uint64_t fnv1a(uint64_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

typedef struct {
    uint8_t **y, **uv;
    int n, w, h;
} clip_t;

static int clip_load(clip_t *c, const opts_t *o) {
    c->n = o->frames; c->w = o->w; c->h = o->h;
    c->y  = calloc((size_t)c->n, sizeof(uint8_t *));
    c->uv = calloc((size_t)c->n, sizeof(uint8_t *));
    if (!c->y || !c->uv) return 0;
    FILE *fin = NULL;
    if (o->input) {
        fin = fopen(o->input, "rb");
        if (!fin) { perror(o->input); return 0; }
    }
    size_t ysz = (size_t)o->w * o->h, uvsz = (size_t)o->w * (o->h / 2);
    for (int k = 0; k < c->n; k++) {
        c->y[k] = malloc(ysz);
        c->uv[k] = malloc(uvsz);
        if (!c->y[k] || !c->uv[k]) return 0;
        if (fin) {
            if (fread(c->y[k], 1, ysz, fin) != ysz || fread(c->uv[k], 1, uvsz, fin) != uvsz) {
                rewind(fin);
                if (fread(c->y[k], 1, ysz, fin) != ysz || fread(c->uv[k], 1, uvsz, fin) != uvsz) {
                    fprintf(stderr, "input too short for one %dx%d NV12 frame\n", o->w, o->h);
                    return 0;
                }
            }
        } else {
            fill(c->y[k], c->uv[k], o->w, o->h, o->pattern, k);
        }
    }
    if (fin) fclose(fin);
    return 1;
}

/* One timed pass over the whole clip. Source generation and allocation are
 * outside the timer; only hevc_encoder_encode_raw() is inside it. */
static double run_once(const clip_t *c, const opts_t *o, uint8_t *bs, size_t cap,
                       uint64_t *sum_out, long *bytes_out, FILE *emit) {
    hevc_encoder_t *enc = hevc_encoder_create(NULL, o->w, o->h, 60, 10000000u);
    if (!enc) { fprintf(stderr, "encoder_create failed\n"); exit(1); }
    hevc_encoder_set_rc_mode(enc, RC_CQP);
    hevc_encoder_set_qp(enc, o->qp);
    hevc_encoder_set_gop_size(enc, (uint32_t)o->gop);

    uint64_t sum = 1469598103934665603ull;
    long bytes = 0;
    double t0 = now_ms();
    for (int k = 0; k < c->n; k++) {
        if (o->gop == 1) hevc_encoder_set_force_idr(enc);
        int n = hevc_encoder_encode_raw(enc, c->y[k], o->w, c->uv[k], o->w, bs, cap);
        if (n <= 0) { fprintf(stderr, "frame %d: encode returned %d\n", k, n); exit(1); }
        bytes += n;
        sum = fnv1a(sum, bs, (size_t)n);
        if (emit) fwrite(bs, 1, (size_t)n, emit);
    }
    double dt = now_ms() - t0;
    hevc_encoder_destroy(enc);
    *sum_out = sum;
    *bytes_out = bytes;
    return dt;
}

static void usage(void) {
    fprintf(stderr,
        "usage: hevc_bench <bench|emit|profile> [--size=WxH] [--qp=N] [--frames=N]\n"
        "                  [--pattern=0..3] [--gop=N] [--samples=N] [--input=f.nv12]\n"
        "                  [--out=prefix] [--quiet]\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *mode = argv[1];
    opts_t o = { 640, 480, 27, 12, 3, 1, 9, 0, NULL, "hevc_bench", };

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if      (!strncmp(a, "--size=", 7))    sscanf(a + 7, "%dx%d", &o.w, &o.h);
        else if (!strncmp(a, "--qp=", 5))      o.qp = atoi(a + 5);
        else if (!strncmp(a, "--frames=", 9))  o.frames = atoi(a + 9);
        else if (!strncmp(a, "--pattern=", 10))o.pattern = atoi(a + 10);
        else if (!strncmp(a, "--gop=", 6))     o.gop = atoi(a + 6);
        else if (!strncmp(a, "--samples=", 10))o.samples = atoi(a + 10);
        else if (!strncmp(a, "--input=", 8))   o.input = a + 8;
        else if (!strncmp(a, "--out=", 6))     o.out = a + 6;
        else if (!strcmp(a, "--quiet"))        o.quiet = 1;
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }
    if (o.gop < 1) o.gop = 1;
    if (o.samples < 1) o.samples = 1;

    clip_t c;
    if (!clip_load(&c, &o)) return 1;

    size_t cw = (size_t)((o.w + 15) / 16 * 16), chh = (size_t)((o.h + 15) / 16 * 16);
    size_t cap = cw * chh * 3 + (1u << 20);
    uint8_t *bs = malloc(cap);
    if (!bs) { fprintf(stderr, "oom\n"); return 1; }

    uint64_t sum = 0; long bytes = 0;

    if (!strcmp(mode, "emit")) {
        char path[512];
        snprintf(path, sizeof(path), "%s.hevc", o.out);
        FILE *f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        run_once(&c, &o, bs, cap, &sum, &bytes, f);
        fclose(f);
        printf("wrote %s (%ld bytes) checksum=%016llx\n", path, bytes,
               (unsigned long long)sum);
        return 0;
    }

    if (!strcmp(mode, "bench")) {
        double *t = malloc(sizeof(double) * (size_t)o.samples);
        uint64_t first = 0;
        /* One untimed warm-up: first-touch page faults on the encoder's own
         * buffers otherwise land entirely in sample 0. */
        run_once(&c, &o, bs, cap, &first, &bytes, NULL);
        for (int s = 0; s < o.samples; s++) {
            t[s] = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
            if (sum != first) { fprintf(stderr, "NONDETERMINISTIC: sample %d checksum differs\n", s); return 1; }
            if (!o.quiet) printf("sample %d: %.3f ms\n", s, t[s]);
        }
        qsort(t, (size_t)o.samples, sizeof(double), cmp_d);
        printf("RESULT %dx%d qp=%d pat=%d gop=%d frames=%d samples=%d "
               "min=%.3f median=%.3f max=%.3f bytes=%ld checksum=%016llx\n",
               o.w, o.h, o.qp, o.pattern, o.gop, o.frames, o.samples,
               t[0], t[o.samples / 2], t[o.samples - 1], bytes,
               (unsigned long long)sum);
        return 0;
    }

#ifdef HEVC_INTRA_PROFILE
    if (!strcmp(mode, "profile")) {
        /* Ablation, not sampling, and output-preserving: side B runs the
         * mode search TWICE per 4x4 PU and throws the second answer away
         * (asserting it matches), so the bitstream is bit-identical and the
         * A/B difference is the marginal cost of exactly one mode search.
         *
         * This measures a MARGINAL call with the block's data already hot,
         * so it is a lower bound on the real first call, not an upper one.
         * The rdtsc figure below brackets it from the other side: its probe
         * overhead inflates rather than deflates.
         *
         * A/A first. A rig that cannot report ~0% on identical work cannot
         * report anything else - docs/performance-measurement.md. */
        int S = o.samples;
        double *a = malloc(sizeof(double) * (size_t)S);
        double *b = malloc(sizeof(double) * (size_t)S);
        double *aa = malloc(sizeof(double) * (size_t)S);
        uint64_t first = 0;
        hevc_intra_prof_timing = 0;
        hevc_intra_prof_dup_mode_search = 0;
        run_once(&c, &o, bs, cap, &first, &bytes, NULL);

        /* ABBA ordering: plain alternation left whichever side ran first in
         * each pair carrying a reproducible penalty (cavlc_bench's note). */
        for (int s = 0; s < S; s++) {
            hevc_intra_prof_dup_mode_search = 0;
            a[s]  = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
            if (sum != first) { fprintf(stderr, "checksum drift (A)\n"); return 1; }
            hevc_intra_prof_dup_mode_search = 1;
            b[s]  = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
            if (sum != first) { fprintf(stderr, "ABLATION CHANGED OUTPUT\n"); return 1; }
            hevc_intra_prof_dup_mode_search = 1;
            double b2 = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
            hevc_intra_prof_dup_mode_search = 0;
            double a2 = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
            if (b2 < b[s]) b[s] = b2;
            if (a2 < a[s]) a[s] = a2;
            /* A/A control: two more A runs, same ordering shape. */
            hevc_intra_prof_dup_mode_search = 0;
            double x = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
            double y = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
            aa[s] = (y - x);
        }
        qsort(a, (size_t)S, sizeof(double), cmp_d);
        qsort(b, (size_t)S, sizeof(double), cmp_d);
        qsort(aa, (size_t)S, sizeof(double), cmp_d);
        double ma = a[S / 2], mb = b[S / 2], maa = aa[S / 2];
        printf("ablation: base median %.3f ms, +1 mode search %.3f ms\n", ma, mb);
        printf("  one hevc_choose_luma_mode pass = %.3f ms = %.2f%% of encode\n",
               mb - ma, 100.0 * (mb - ma) / ma);
        printf("  A/A control (same code, same shape) = %.3f ms = %.2f%%\n",
               maa, 100.0 * maa / ma);

        /* Second, independent estimate: rdtsc around the real first call. */
        hevc_intra_prof_timing = 1;
        hevc_intra_prof_mode_cycles = 0;
        hevc_intra_prof_mode_calls = 0;
        hevc_intra_prof_cyc_predict_planardc = 0;
        hevc_intra_prof_cyc_predict_angular = 0;
        hevc_intra_prof_cyc_sad = 0;
        hevc_intra_prof_calls_predict_planardc = 0;
        hevc_intra_prof_calls_predict_angular = 0;
        hevc_intra_prof_calls_sad = 0;
        double tt = run_once(&c, &o, bs, cap, &sum, &bytes, NULL);
        unsigned long long cyc = hevc_intra_prof_mode_cycles;
        unsigned long long calls = hevc_intra_prof_mode_calls;
        unsigned long long cyc_pdc = hevc_intra_prof_cyc_predict_planardc;
        unsigned long long cyc_ang = hevc_intra_prof_cyc_predict_angular;
        unsigned long long cyc_sad = hevc_intra_prof_cyc_sad;
        unsigned long long calls_pdc = hevc_intra_prof_calls_predict_planardc;
        unsigned long long calls_ang = hevc_intra_prof_calls_predict_angular;
        unsigned long long calls_sad = hevc_intra_prof_calls_sad;
        hevc_intra_prof_timing = 0;
        double tsc_ghz = 0.0;
        {   /* calibrate rdtsc against CLOCK_MONOTONIC */
            extern unsigned long long hevc_intra_prof_rdtsc(void);
            double t0 = now_ms();
            unsigned long long c0 = hevc_intra_prof_rdtsc();
            struct timespec req = { 0, 200000000L };
            nanosleep(&req, NULL);
            unsigned long long c1 = hevc_intra_prof_rdtsc();
            double t1 = now_ms();
            tsc_ghz = (double)(c1 - c0) / ((t1 - t0) * 1e6);
        }
        printf("rdtsc: %llu calls, %llu cycles, %.1f cyc/call, tsc %.3f GHz\n",
               calls, cyc, calls ? (double)cyc / (double)calls : 0.0, tsc_ghz);
        if (tsc_ghz > 0.0) {
            double ms = (double)cyc / (tsc_ghz * 1e6);
            printf("  instrumented total %.3f ms of a %.3f ms run = %.2f%% "
                   "(INCLUDES probe overhead, so an upper bound)\n",
                   ms, tt, 100.0 * ms / tt);

            /* A6 mode-search-perf: per-candidate breakdown within the loop.
             * Each row is separately rdtsc-bracketed (own probe overhead,
             * so these three rows do not have to sum to the row above), and
             * is comparable ACROSS builds because it brackets the identical
             * call site in both the exhaustive and coarse-then-refine
             * shapes - see hevc_intra.c's HEVC_PROF_BRACKET comment. */
            double ms_pdc = (double)cyc_pdc / (tsc_ghz * 1e6);
            double ms_ang = (double)cyc_ang / (tsc_ghz * 1e6);
            double ms_sad = (double)cyc_sad / (tsc_ghz * 1e6);
            printf("  within the loop (each row independently bracketed, "
                   "own probe overhead, rows need not sum to the total):\n");
            printf("    predict_block4 planar/dc: %llu calls, %.3f ms (%.2f%% of run)\n",
                   calls_pdc, ms_pdc, 100.0 * ms_pdc / tt);
            printf("    predict_block4 angular:   %llu calls, %.3f ms (%.2f%% of run)\n",
                   calls_ang, ms_ang, 100.0 * ms_ang / tt);
            printf("    sad_4x4:                  %llu calls, %.3f ms (%.2f%% of run)\n",
                   calls_sad, ms_sad, 100.0 * ms_sad / tt);
            printf("    remainder (RD cost arith, loop overhead, one-time "
                   "gather+hoist): %.3f ms (%.2f%% of run)\n",
                   ms - ms_pdc - ms_ang - ms_sad,
                   100.0 * (ms - ms_pdc - ms_ang - ms_sad) / tt);
        }
        return 0;
    }
#endif

    usage();
    return 2;
}
