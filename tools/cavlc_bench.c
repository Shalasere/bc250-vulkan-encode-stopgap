/* bc250-vulkan-encode-stopgap - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * cavlc_bench.c - off-board harness for the H.264 CAVLC entropy coder.
 *
 * WHY THIS EXISTS
 * ---------------
 * CAVLC is ~57% of the shipping H.264 path's frame time and was the last
 * untouched performance lever, but it could not be worked on off-board:
 * h264_encoder_encode_raw() is header-only by design (it codes no residual,
 * see commit 629fc1d), so tests/test_encode never reaches residual coding,
 * and everything that does reach it needs a GPU. docs/backlog.md A2.
 *
 * This tool drives src/cavlc.c's real cavlc_write_*() functions directly
 * with synthetic quantized coefficients, on a dev machine, with no Vulkan,
 * no board and no GPU. Same shape as tools/hevc_host_repro.c (a standalone
 * .c driving one GPU-free path), but built by CMake rather than by a shell
 * script, so it inherits the exact flags the driver ships with - see
 * docs/performance-measurement.md on why an -O2 number is not usable here.
 *
 * WHY THE INPUTS ARE REPRESENTATIVE
 * ---------------------------------
 * Three things about the input decide what CAVLC actually executes, and all
 * three are modelled rather than randomised:
 *
 *  1. *Which functions get called, and in what order.* The macroblock
 *     emission below is a deliberate mirror of encoder_h264.c's
 *     encode_mb_i16x16()/encode_mb_p16x16() - same syntax order, same cbp
 *     derivation, same "all 16 luma AC blocks are coded (including the
 *     all-zero ones) as soon as any one of them is nonzero" gating, same
 *     mb_skip_run discipline. This project only ever emits I_16x16 and
 *     P_L0_16x16, 4:2:0, no 8x8 transform (see cabac.h's SCOPE note), so
 *     the four block shapes exercised here - luma DC (16-coeff), luma AC
 *     (15-coeff), chroma DC (2x2) and chroma AC (15-coeff) - are exactly
 *     the four the encoder emits, and nothing else is modelled.
 *
 *  2. *Coefficient sparsity and clustering.* A real quantized 4x4 residual
 *     is mostly zeros with a few small values at low frequencies, so
 *     nonzeros are placed by ZIGZAG SCAN index with probability decaying
 *     geometrically in frequency, and magnitudes follow a heavy-1 profile
 *     (~64% are +-1). Uniform random coefficients would make almost every
 *     block dense, which is the single least representative thing a CAVLC
 *     benchmark can do: dense blocks take a completely different path
 *     through coeff_token, total_zeros and run_before than the real
 *     sparse ones. The `stats` command prints the resulting distribution
 *     so the claim is checkable rather than asserted - in particular the
 *     fraction of entirely-zero 4x4 blocks, which the shipping encoder's
 *     own nz_mask comment (encoder_h264.c, nc_ctx_t::nz_mask) puts at
 *     ~90-96%, and which the default profile is tuned to land inside.
 *
 *  3. *nC, i.e. which VLC table gets selected.* coeff_token is read from
 *     one of four tables chosen by the neighbouring blocks' nonzero counts.
 *     Per-block independent randomness would keep nC pinned in one class.
 *     So macroblocks are drawn from a small set of activity classes with
 *     spatial persistence (a busy MB is likely to have busy neighbours),
 *     and nC is derived with byte-for-byte copies of encoder_h264.c's
 *     luma_nc()/chroma_nc(). `stats` prints the four-way nC class
 *     occupancy; all four are exercised by the default profile.
 *
 * THE ORACLE
 * ----------
 * See the `verify` command. Two independent checks, deliberately covering
 * different failure classes - read verify_frame()'s comment for what each
 * one does and does not cover.
 *
 * USAGE
 *   cavlc_bench stats   [opts]            input distribution report
 *   cavlc_bench verify  [opts]            reference-decoder round trip
 *   cavlc_bench emit    [opts] --out=F    write an Annex-B .264 for ffmpeg
 *   cavlc_bench bench   [opts]            timing, best+median over samples
 *                                         (with no --ab this is the A/A self-test)
 *   cavlc_bench profile [opts]            where the time goes (profile build)
 *   cavlc_bench ab      [opts] --ab=A:B   one interleaved A/B (profile build)
 *   cavlc_bench count   [opts]            exact per-stage bits/calls (profile build)
 *   cavlc_bench scan    [opts]            gather+scan in isolation (profile build)
 *
 * OPTIONS
 *   --size=WxH      coded size, default 1280x720
 *   --frames=N      frames in the pool, default 8 (frame 0 IDR, rest P)
 *   --profile=quiet|typical|busy   coefficient density, default typical
 *   --seed=N        generator seed, default 1
 *   --iters=N       encode passes over the pool per timing sample
 *   --samples=N     timing samples per side, default 9 (>=7 required)
 *   --ab=A:B        ablation masks to compare (see cavlc.h CAVLC_S_*)
 *   --out=FILE      output path for `emit`
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>

#include "bitstream.h"
#include "cavlc.h"

/* ========================================================================
 * Generator
 * ======================================================================== */

/* splitmix64 - small, fast, reproducible; the harness must be able to
 * regenerate byte-identical input from a seed alone, since every A/B
 * comparison here depends on both sides seeing the same coefficients. */
typedef struct { uint64_t s; } rng_t;

static inline uint64_t rng_next(rng_t *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline uint32_t rng_u32(rng_t *r, uint32_t n) { return (uint32_t)(rng_next(r) % n); }
/* Uniform in [0,1) with 24 bits of resolution - enough for the probability
 * thresholds below and exactly reproducible across machines. */
static inline double rng_unit(rng_t *r) { return (double)(rng_next(r) & 0xFFFFFFu) / 16777216.0; }

/* H.264 zigzag scan for a 4x4 block: zigzag[i] is the RASTER position of
 * scan index i. Same table as cavlc.c's; duplicated because cavlc.c's copy
 * is static, and the generator needs it to place nonzeros by frequency. */
static const int zz[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };

/* Spec blkIdx <-> raster, copied from encoder_h264.c. */
static const int blk_x4[16] = {0,1,0,1, 2,3,2,3, 0,1,0,1, 2,3,2,3};
static const int blk_y4[16] = {0,0,1,1, 0,0,1,1, 2,2,3,3, 2,2,3,3};
static const int spec_blk_idx_from_xy[4][4] = {
    { 0, 1, 4, 5 },
    { 2, 3, 6, 7 },
    { 8, 9,12,13 },
    {10,11,14,15 }
};
static inline int raster_from_spec(int blk_idx) { return blk_y4[blk_idx] * 4 + blk_x4[blk_idx]; }

/* One macroblock activity class. p0/tau describe P(nonzero) at scan index
 * i as p0 * exp(-i/tau) for luma; chroma is scaled down (smoother content,
 * 4:2:0-subsampled - the reason chroma PSNR was far less damaged than luma
 * by the run_before ordering bug, see cavlc.c's comment on it). */
typedef struct {
    double weight;    /* share of macroblocks in this class */
    double p0, tau;   /* luma AC nonzero density */
    double dc_p;      /* P(nonzero) per luma-DC / chroma-DC coefficient */
    double skip_p;    /* P(this MB is coded as P_Skip) in a P slice */
} act_class_t;

typedef struct {
    const char *name;
    act_class_t c[4];
} density_profile_t;

/*
 * `typical` is the default and is tuned so that the fraction of 4x4 blocks
 * in the frame that quantize entirely to zero lands inside the ~90-96%
 * band encoder_h264.c's nz_mask comment reports for real content - the only
 * real-content anchor this project has written down. `active` and `busy`
 * bracket it upward: `busy` is the only profile that puts meaningful
 * traffic in the nC>=8 coeff_token table, which is why `verify` uses it.
 * Run `cavlc_bench stats` after touching any number here - the report
 * prints the fraction so the claim stays checkable.
 */
static const density_profile_t profiles[] = {
    { "quiet", {
        { 0.80, 0.00, 1.0, 0.08, 0.84 },
        { 0.15, 0.05, 2.0, 0.26, 0.40 },
        { 0.04, 0.18, 3.0, 0.50, 0.12 },
        { 0.01, 0.40, 5.0, 0.78, 0.02 } } },
    { "typical", {
        { 0.71, 0.00, 1.0, 0.15, 0.76 },
        { 0.20, 0.10, 2.4, 0.38, 0.32 },
        { 0.07, 0.32, 3.8, 0.62, 0.09 },
        { 0.02, 0.62, 6.5, 0.85, 0.02 } } },
    { "active", {
        { 0.52, 0.00, 1.0, 0.18, 0.72 },
        { 0.29, 0.10, 2.4, 0.38, 0.30 },
        { 0.14, 0.32, 3.8, 0.62, 0.08 },
        { 0.05, 0.62, 6.5, 0.85, 0.02 } } },
    { "busy", {
        { 0.18, 0.02, 1.5, 0.30, 0.30 },
        { 0.30, 0.22, 3.0, 0.55, 0.10 },
        { 0.32, 0.48, 5.0, 0.80, 0.03 },
        { 0.20, 0.78, 9.0, 0.92, 0.00 } } },
};
#define NUM_PROFILES ((int)(sizeof(profiles) / sizeof(profiles[0])))

/* Quantized magnitude: dominated by 1, with a geometric tail. The 1-in-5000
 * outlier exists so the level_prefix>=15 escape in cavlc_write_one_level()
 * is actually exercised by `verify` (it is reachable on real content only
 * at very low QP); at that rate it is far too rare to move a timing number,
 * and both sides of every A/B see the identical draw anyway. */
static int gen_magnitude(rng_t *r) {
    if (rng_u32(r, 5000) == 0) return 200 + (int)rng_u32(r, 2000);
    uint32_t v = rng_u32(r, 1000);
    if (v < 640) return 1;
    if (v < 820) return 2;
    if (v < 900) return 3;
    if (v < 950) return 4;
    int m = 5;
    while (m < 60 && rng_u32(r, 100) < 55) m++;
    return m;
}

static inline int16_t signed_mag(rng_t *r, int mag) {
    return (int16_t)((rng_next(r) & 1) ? -mag : mag);
}

/* One frame's worth of already-quantized coefficients, in exactly the
 * layout encoder_h264.c hands to CAVLC. */
typedef struct {
    bool     is_idr;
    uint32_t mbs;
    int16_t (*blk)[24][16];   /* [mb][block][raster coefficient position]
                               * blocks 0..15  = luma, RASTER block index
                               * blocks 16..19 = Cb AC, 20..23 = Cr AC */
    int16_t (*luma_dc)[16];   /* [mb][16] I16x16 luma DC, natural order */
    int     (*cdc)[8];        /* [mb][0..3] = Cb DC, [4..7] = Cr DC */
    uint8_t  *skip;           /* [mb] P_Skip flag */
    int8_t  (*mvd)[2];        /* [mb] quarter-pel mvd, small by construction */
    uint8_t (*modes)[2];      /* [mb] I16x16 pred mode, chroma pred mode */
    /* Per-block nonzero bitmask, bit p set iff raster coefficient p is
     * nonzero. The shipping encoder gets exactly this from the GPU
     * (gpu_compute.h's nz_staging_buffers, consumed via nc_ctx_t::nz_mask)
     * and derives every cbp from it instead of reading the 22MB coefficient
     * buffer. Modelling it matters for the profile, not just for tidiness:
     * without it the harness would spend real time on 24 linear 16-entry
     * scans per macroblock that the encoder does not do, and that cost
     * would land in the baseline and shrink every reported share. */
    uint32_t (*nzmask)[24];
    uint32_t *ldc_mask;       /* [mb] same, for the 16 luma DC values */
} frame_t;

static void frame_free(frame_t *f) {
    free(f->blk); free(f->luma_dc); free(f->cdc);
    free(f->skip); free(f->mvd); free(f->modes);
    free(f->nzmask); free(f->ldc_mask);
    memset(f, 0, sizeof(*f));
}

static bool frame_alloc(frame_t *f, uint32_t mbs) {
    memset(f, 0, sizeof(*f));
    f->mbs = mbs;
    f->blk     = calloc(mbs, sizeof(*f->blk));
    f->luma_dc = calloc(mbs, sizeof(*f->luma_dc));
    f->cdc     = calloc(mbs, sizeof(*f->cdc));
    f->skip    = calloc(mbs, 1);
    f->mvd     = calloc(mbs, sizeof(*f->mvd));
    f->modes   = calloc(mbs, sizeof(*f->modes));
    f->nzmask  = calloc(mbs, sizeof(*f->nzmask));
    f->ldc_mask = calloc(mbs, sizeof(*f->ldc_mask));
    if (!f->blk || !f->luma_dc || !f->cdc || !f->skip || !f->mvd || !f->modes
        || !f->nzmask || !f->ldc_mask) {
        frame_free(f);
        return false;
    }
    return true;
}

/* Fill one 4x4 block by scan index. `from_scan` is 1 for AC-only blocks
 * (the DC position belongs to the separate DC block) and 0 otherwise. */
static void gen_block(rng_t *r, int16_t *b, double p0, double tau, int from_scan) {
    if (p0 <= 0.0) return;
    for (int i = from_scan; i < 16; i++) {
        if (rng_unit(r) < p0 * exp(-(double)(i - from_scan) / tau))
            b[zz[i]] = signed_mag(r, gen_magnitude(r));
    }
}

/*
 * Activity class per macroblock, with spatial persistence: an MB keeps its
 * left neighbour's class with probability 0.6, otherwise redraws. Real
 * content is spatially correlated, and nC is derived from neighbours - a
 * per-MB independent draw would wash the neighbour signal out and leave the
 * four coeff_token tables selected almost at random rather than in the
 * clustered way they are selected on real frames.
 */
static int pick_class(rng_t *r, const density_profile_t *p, int prev) {
    if (prev >= 0 && rng_unit(r) < 0.6) return prev;
    double x = rng_unit(r), acc = 0.0;
    for (int i = 0; i < 4; i++) {
        acc += p->c[i].weight;
        if (x < acc) return i;
    }
    return 3;
}

static void gen_frame(frame_t *f, const density_profile_t *p, uint32_t width_in_mbs,
                      bool is_idr, uint64_t seed) {
    rng_t r = { seed * 0x2545F4914F6CDD1Dull + 0x9E3779B9u };
    f->is_idr = is_idr;
    int prev_class = -1;
    for (uint32_t mb = 0; mb < f->mbs; mb++) {
        if (mb % width_in_mbs == 0) prev_class = -1;
        int k = pick_class(&r, p, prev_class);
        prev_class = k;
        const act_class_t *c = &p->c[k];

        /* Intra prediction modes must be legal for this macroblock's
         * neighbour availability, or the stream is non-conformant and
         * ffmpeg rejects it at MB 0 ("left block unavailable for requested
         * intra mode") before any entropy coding is even examined - which
         * would silently destroy the ffmpeg half of the oracle. Luma:
         * VERT needs top, HORIZ needs left, PLANE needs both, DC always.
         * Chroma (different numbering, see cavlc.h): HORIZ needs left,
         * VERT needs top, PLANE needs both, DC always. */
        uint32_t mbx_ = mb % width_in_mbs, mby_ = mb / width_in_mbs;
        bool has_left = mbx_ > 0, has_top = mby_ > 0;
        int luma_ok[4], nluma = 0, chroma_ok[4], nchroma = 0;
        luma_ok[nluma++] = H264_I16x16_DC;
        if (has_top)  luma_ok[nluma++] = H264_I16x16_VERT;
        if (has_left) luma_ok[nluma++] = H264_I16x16_HORIZ;
        if (has_left && has_top) luma_ok[nluma++] = H264_I16x16_PLANE;
        chroma_ok[nchroma++] = H264_CHROMA_DC;
        if (has_left) chroma_ok[nchroma++] = H264_CHROMA_HORIZ;
        if (has_top)  chroma_ok[nchroma++] = H264_CHROMA_VERT;
        if (has_left && has_top) chroma_ok[nchroma++] = H264_CHROMA_PLANE;
        f->modes[mb][0] = (uint8_t)luma_ok[rng_u32(&r, (uint32_t)nluma)];
        f->modes[mb][1] = (uint8_t)chroma_ok[rng_u32(&r, (uint32_t)nchroma)];
        f->mvd[mb][0] = (int8_t)((int)rng_u32(&r, 17) - 8);
        f->mvd[mb][1] = (int8_t)((int)rng_u32(&r, 17) - 8);
        f->skip[mb] = (uint8_t)(!is_idr && rng_unit(&r) < c->skip_p);
        if (f->skip[mb]) continue;   /* a skipped MB codes no residual at all */

        /* Luma DC (I16x16 only, but generated unconditionally so the two
         * frame types draw from the same stream position). Hadamard output
         * is not frequency-ordered the way an AC block is, hence a flat
         * per-coefficient probability rather than a decay. */
        for (int i = 0; i < 16; i++)
            if (rng_unit(&r) < c->dc_p)
                f->luma_dc[mb][i] = signed_mag(&r, gen_magnitude(&r));

        /* Luma AC. For a P macroblock the DC position is coded in the same
         * block (no separate DC transform), so scan index 0 is filled too. */
        int from = is_idr ? 1 : 0;
        for (int b = 0; b < 16; b++)
            gen_block(&r, f->blk[mb][b], c->p0, c->tau, from);

        /* Chroma: same model, lower density. */
        for (int b = 16; b < 24; b++)
            gen_block(&r, f->blk[mb][b], c->p0 * 0.45, c->tau * 0.7, 1);
        for (int i = 0; i < 8; i++)
            if (rng_unit(&r) < c->dc_p * 0.6)
                f->cdc[mb][i] = signed_mag(&r, gen_magnitude(&r));
    }

    /* The GPU hands the shipping encoder this mask; build it here so the
     * harness's cbp derivation costs what the encoder's costs. */
    for (uint32_t mb = 0; mb < f->mbs; mb++) {
        for (int b = 0; b < 24; b++) {
            uint32_t m = 0;
            for (int i = 0; i < 16; i++) if (f->blk[mb][b][i]) m |= 1u << i;
            f->nzmask[mb][b] = m;
        }
        uint32_t m = 0;
        for (int i = 0; i < 16; i++) if (f->luma_dc[mb][i]) m |= 1u << i;
        f->ldc_mask[mb] = m;
    }
}

/* ========================================================================
 * nC neighbour context - byte-for-byte mirror of encoder_h264.c
 * ======================================================================== */

typedef struct {
    uint8_t (*nz_luma)[16];   /* spec blkIdx order */
    uint8_t (*nz_cb)[4];
    uint8_t (*nz_cr)[4];
    uint32_t width_in_mbs;
} ncctx_t;

static int luma_nc(const ncctx_t *nc, uint32_t mb, uint32_t mbx, uint32_t mby, int blk_idx) {
    int x4 = blk_x4[blk_idx], y4 = blk_y4[blk_idx];
    int nA = -1, nB = -1;
    if (x4 > 0)        nA = nc->nz_luma[mb][spec_blk_idx_from_xy[y4][x4 - 1]];
    else if (mbx > 0)  nA = nc->nz_luma[mb - 1][spec_blk_idx_from_xy[y4][3]];
    if (y4 > 0)        nB = nc->nz_luma[mb][spec_blk_idx_from_xy[y4 - 1][x4]];
    else if (mby > 0)  nB = nc->nz_luma[mb - nc->width_in_mbs][spec_blk_idx_from_xy[3][x4]];
    if (nA >= 0 && nB >= 0) return (nA + nB + 1) >> 1;
    if (nA >= 0) return nA;
    if (nB >= 0) return nB;
    return 0;
}

static int chroma_nc(uint8_t (*nz_c)[4], uint32_t mb, uint32_t mbx, uint32_t mby,
                     uint32_t width_in_mbs, int blk_idx) {
    int x2 = blk_idx % 2, y2 = blk_idx / 2;
    int nA = -1, nB = -1;
    if (x2 > 0)        nA = nz_c[mb][y2 * 2 + (x2 - 1)];
    else if (mbx > 0)  nA = nz_c[mb - 1][y2 * 2 + 1];
    if (y2 > 0)        nB = nz_c[mb][(y2 - 1) * 2 + x2];
    else if (mby > 0)  nB = nz_c[mb - width_in_mbs][1 * 2 + x2];
    if (nA >= 0 && nB >= 0) return (nA + nB + 1) >> 1;
    if (nA >= 0) return nA;
    if (nB >= 0) return nB;
    return 0;
}

/* ========================================================================
 * Slice emission - mirror of encode_mb_i16x16() / encode_mb_p16x16()
 * ======================================================================== */

static bool block_any_nonzero(const int16_t *b, int from, int to) {
    for (int i = from; i < to; i++) if (b[i] != 0) return true;
    return false;
}

static inline int popcnt16(uint32_t x) {
    int n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}

/*
 * glue_only - measure the macroblock-layer glue WITHOUT cavlc.c.
 *
 * The timed region below is deliberately the same span the shipping
 * encoder's own BC250_PERF_STATS timer covers (encoder_h264.c brackets the
 * whole per-slice loop, not just the entropy calls), so it includes the cbp
 * derivation, the nC derivation and the neighbour-array bookkeeping. Those
 * are real and they ship - but they live in encoder_h264.c, not cavlc.c,
 * and conflating them would make every share reported by `profile` a share
 * of the wrong denominator. With this flag set the three residual entry
 * points and the header writers are skipped and the total_coeff each one
 * would have returned is taken from the precomputed nonzero mask instead,
 * so the nC context still evolves EXACTLY as it does in a real run and the
 * glue does identical work. baseline - glue_only = time inside cavlc.c.
 */
static int glue_only;
/* Selects glue_only from an --ab mask; deliberately above the six
 * CAVLC_S_* ablation bits so the two cannot collide. */
#define MASK_GLUE 0x100u

static inline int w_full(bitstream_t *bs, const int16_t *c, int nC, uint32_t mask) {
    if (glue_only) return popcnt16(mask);
    return cavlc_write_4x4_block(bs, c, nC);
}
static inline int w_ac(bitstream_t *bs, const int16_t *c, int nC, uint32_t mask) {
    if (glue_only) return popcnt16(mask & ~1u);
    return cavlc_write_4x4_ac_block(bs, c, nC);
}
static inline int w_cdc(bitstream_t *bs, const int *c) {
    if (glue_only) {
        int n = 0;
        for (int i = 0; i < 4; i++) if (c[i]) n++;
        return n;
    }
    return cavlc_write_chroma_dc_block(bs, c);
}

/* Statistics collected while emitting, for the `stats` command. Kept out of
 * the timed path: gather_stats is false for every `bench`/`ab` run. */
typedef struct {
    uint64_t blocks[4];        /* luma DC, luma AC, chroma DC, chroma AC */
    uint64_t zero_blocks[4];
    uint64_t nc_class[4];      /* nC<2, <4, <8, >=8 (4x4/AC blocks only) */
    uint64_t total_coeff_hist[17];
    uint64_t mb_i16, mb_p, mb_skip;
    uint64_t coeffs_nonzero;
} genstats_t;

static bool gather_stats;
static genstats_t gstats;

static inline void tally(int kind, int nC, int tc, bool have_nc) {
    if (!gather_stats) return;
    gstats.blocks[kind]++;
    if (tc == 0) gstats.zero_blocks[kind]++;
    gstats.total_coeff_hist[tc]++;
    gstats.coeffs_nonzero += (uint64_t)tc;
    if (have_nc) {
        int cls = (nC < 2) ? 0 : (nC < 4) ? 1 : (nC < 8) ? 2 : 3;
        gstats.nc_class[cls]++;
    }
}

static void emit_mb_i16x16(bitstream_t *bs, const frame_t *f, uint32_t mb,
                           uint32_t mbx, uint32_t mby, ncctx_t *nc) {
    const int16_t (*blk)[16] = f->blk[mb];
    const uint32_t *nzm = f->nzmask[mb];

    int cbp_luma_flag = 0;
    for (int b = 0; b < 16 && !cbp_luma_flag; b++)
        if (nzm[b] & ~1u) cbp_luma_flag = 1;

    const int *cb_dc = &f->cdc[mb][0];
    const int *cr_dc = &f->cdc[mb][4];
    int chroma_dc_nonzero = 0;
    for (int i = 0; i < 4; i++) if (cb_dc[i] || cr_dc[i]) { chroma_dc_nonzero = 1; break; }
    int chroma_ac_nonzero = 0;
    for (int b = 16; b < 24 && !chroma_ac_nonzero; b++)
        if (nzm[b] & ~1u) chroma_ac_nonzero = 1;
    int cbp_chroma = chroma_ac_nonzero ? 2 : (chroma_dc_nonzero ? 1 : 0);

    if (!glue_only)
        cavlc_write_mb_i16x16_header(bs, f->modes[mb][0], f->modes[mb][1],
                                     cbp_chroma, cbp_luma_flag ? 15 : 0, 0);

    /* Luma DC first, with luma4x4BlkIdx 0's neighbour chain - and BEFORE the
     * AC loop overwrites nz_luma[mb][0]. See encoder_h264.c's long comment
     * on why the DC block has no neighbour chain of its own. */
    int nC_dc = luma_nc(nc, mb, mbx, mby, 0);
    int tc_dc = w_full(bs, f->luma_dc[mb], nC_dc, f->ldc_mask[mb]);
    tally(0, nC_dc, tc_dc, true);

    if (cbp_luma_flag) {
        for (int blk_idx = 0; blk_idx < 16; blk_idx++) {
            int raster = raster_from_spec(blk_idx);
            int nC = luma_nc(nc, mb, mbx, mby, blk_idx);
            int tc = w_ac(bs, blk[raster], nC, nzm[raster]);
            nc->nz_luma[mb][blk_idx] = (uint8_t)tc;
            tally(1, nC, tc, true);
        }
    } else {
        memset(nc->nz_luma[mb], 0, 16);
    }

    if (cbp_chroma >= 1) {
        int t0 = w_cdc(bs, cb_dc);
        int t1 = w_cdc(bs, cr_dc);
        tally(2, 0, t0, false);
        tally(2, 0, t1, false);
    }
    if (cbp_chroma == 2) {
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cb, mb, mbx, mby, nc->width_in_mbs, blk_idx);
            int tc = w_ac(bs, blk[16 + blk_idx], nC, nzm[16 + blk_idx]);
            nc->nz_cb[mb][blk_idx] = (uint8_t)tc;
            tally(3, nC, tc, true);
        }
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cr, mb, mbx, mby, nc->width_in_mbs, blk_idx);
            int tc = w_ac(bs, blk[20 + blk_idx], nC, nzm[20 + blk_idx]);
            nc->nz_cr[mb][blk_idx] = (uint8_t)tc;
            tally(3, nC, tc, true);
        }
    } else {
        memset(nc->nz_cb[mb], 0, 4);
        memset(nc->nz_cr[mb], 0, 4);
    }
    if (gather_stats) gstats.mb_i16++;
}

static void emit_mb_p16x16(bitstream_t *bs, const frame_t *f, uint32_t mb,
                           uint32_t mbx, uint32_t mby, ncctx_t *nc) {
    const int16_t (*blk)[16] = f->blk[mb];
    const uint32_t *nzm = f->nzmask[mb];

    int luma_cbp = 0;
    for (int q = 0; q < 4; q++) {
        int any = 0;
        for (int sub = 0; sub < 4 && !any; sub++)
            if (nzm[raster_from_spec(q * 4 + sub)]) any = 1;
        if (any) luma_cbp |= (1 << q);
    }

    const int *cb_dc = &f->cdc[mb][0];
    const int *cr_dc = &f->cdc[mb][4];
    int chroma_dc_nonzero = 0;
    for (int i = 0; i < 4; i++) if (cb_dc[i] || cr_dc[i]) { chroma_dc_nonzero = 1; break; }
    int chroma_ac_nonzero = 0;
    for (int b = 16; b < 24 && !chroma_ac_nonzero; b++)
        if (nzm[b] & ~1u) chroma_ac_nonzero = 1;
    int cbp_chroma = chroma_ac_nonzero ? 2 : (chroma_dc_nonzero ? 1 : 0);

    int cbp = (luma_cbp & 0xF) | (cbp_chroma << 4);
    if (!glue_only)
        cavlc_write_mb_p16x16_header(bs, f->mvd[mb][0], f->mvd[mb][1], cbp, 0);

    for (int blk_idx = 0; blk_idx < 16; blk_idx++) {
        if (luma_cbp & (1 << (blk_idx / 4))) {
            int raster = raster_from_spec(blk_idx);
            int nC = luma_nc(nc, mb, mbx, mby, blk_idx);
            int tc = w_full(bs, blk[raster], nC, nzm[raster]);
            nc->nz_luma[mb][blk_idx] = (uint8_t)tc;
            tally(1, nC, tc, true);
        } else {
            nc->nz_luma[mb][blk_idx] = 0;
        }
    }

    if (cbp_chroma >= 1) {
        int t0 = w_cdc(bs, cb_dc);
        int t1 = w_cdc(bs, cr_dc);
        tally(2, 0, t0, false);
        tally(2, 0, t1, false);
    }
    if (cbp_chroma == 2) {
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cb, mb, mbx, mby, nc->width_in_mbs, blk_idx);
            int tc = w_ac(bs, blk[16 + blk_idx], nC, nzm[16 + blk_idx]);
            nc->nz_cb[mb][blk_idx] = (uint8_t)tc;
            tally(3, nC, tc, true);
        }
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cr, mb, mbx, mby, nc->width_in_mbs, blk_idx);
            int tc = w_ac(bs, blk[20 + blk_idx], nC, nzm[20 + blk_idx]);
            nc->nz_cr[mb][blk_idx] = (uint8_t)tc;
            tally(3, nC, tc, true);
        }
    } else {
        memset(nc->nz_cb[mb], 0, 4);
        memset(nc->nz_cr[mb], 0, 4);
    }
    if (gather_stats) gstats.mb_p++;
}

/* Slice header, mirroring encoder_h264.c's. Written here rather than reused
 * because the encoder's copy is inline in a function that needs a GPU
 * context; its correctness is checked by ffmpeg accepting the stream. */
static void emit_slice_header(bitstream_t *bs, bool is_idr, uint32_t frame_num,
                              uint32_t idr_pic_id, int poc, int qp,
                              const h264_sps_t *sps, const h264_pps_t *pps) {
    bs_write_ue(bs, 0);                                      /* first_mb_in_slice */
    bs_write_ue(bs, (uint32_t)(is_idr ? SLICE_TYPE_I : SLICE_TYPE_P));
    bs_write_ue(bs, pps->pps_id);
    bs_write_u(bs, sps->log2_max_frame_num + 4, frame_num);
    if (is_idr) bs_write_ue(bs, idr_pic_id);
    int poc_bits = sps->log2_max_poc_lsb + 4;
    bs_write_u(bs, poc_bits, (uint32_t)poc & ((1u << poc_bits) - 1u));
    if (!is_idr) {
        bs_write1(bs, 0);   /* num_ref_idx_active_override_flag */
        bs_write1(bs, 0);   /* ref_pic_list_modification_flag_l0 */
        bs_write1(bs, 0);   /* adaptive_ref_pic_marking_mode_flag */
    } else {
        bs_write1(bs, 0);   /* no_output_of_prior_pics_flag */
        bs_write1(bs, 0);   /* long_term_reference_flag */
    }
    bs_write_se(bs, qp - 26 - pps->pic_init_qp);             /* slice_qp_delta */
    bs_write_ue(bs, 0);                                      /* disable_deblocking_filter_idc */
    bs_write_se(bs, 0);
    bs_write_se(bs, 0);
}

/* Encode one frame's slice data (no slice header) into `bs`. This is the
 * function every timing measurement brackets. */
static void emit_slice_data(bitstream_t *bs, const frame_t *f, ncctx_t *nc) {
    uint32_t w = nc->width_in_mbs;
    if (f->is_idr) {
        for (uint32_t mb = 0; mb < f->mbs; mb++)
            emit_mb_i16x16(bs, f, mb, mb % w, mb / w, nc);
    } else {
        uint32_t skip_run = 0;
        for (uint32_t mb = 0; mb < f->mbs; mb++) {
            if (f->skip[mb]) {
                skip_run++;
                memset(nc->nz_luma[mb], 0, 16);
                memset(nc->nz_cb[mb], 0, 4);
                memset(nc->nz_cr[mb], 0, 4);
                if (gather_stats) gstats.mb_skip++;
                continue;
            }
            if (!glue_only) cavlc_write_p_skip_run(bs, skip_run);
            skip_run = 0;
            emit_mb_p16x16(bs, f, mb, mb % w, mb / w, nc);
        }
        if (skip_run > 0 && !glue_only) cavlc_write_p_skip_run(bs, skip_run);
    }
    if (!glue_only) cavlc_write_slice_trailing_bits(bs);
    bs_flush(bs);
}

/* ========================================================================
 * Reference CAVLC decoder - oracle #1
 * ======================================================================== */

typedef struct {
    const uint8_t *buf;
    size_t bits;      /* total readable bits */
    size_t pos;
    int    err;
} br_t;

static uint32_t br_u(br_t *r, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (r->pos >= r->bits) { r->err = 1; return v; }
        v = (v << 1) | ((r->buf[r->pos >> 3] >> (7 - (r->pos & 7))) & 1u);
        r->pos++;
    }
    return v;
}
static uint32_t br_peek(const br_t *r, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t p = r->pos + (size_t)i;
        uint32_t b = (p < r->bits) ? ((r->buf[p >> 3] >> (7 - (p & 7))) & 1u) : 0u;
        v = (v << 1) | b;
    }
    return v;
}
static uint32_t br_ue(br_t *r) {
    int zeros = 0;
    while (r->pos < r->bits && br_u(r, 1) == 0) {
        if (++zeros > 32) { r->err = 1; return 0; }
    }
    if (zeros == 0) return 0;
    return (1u << zeros) - 1u + br_u(r, zeros);
}
static int32_t br_se(br_t *r) {
    uint32_t k = br_ue(r);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

/*
 * VLC tables. These are the SAME NUMBERS as cavlc.c's - they are copied,
 * not independently re-derived, and that is a deliberate, stated limit of
 * this oracle (see verify_frame()'s comment). cavlc.c's copies are static,
 * and the point of this decoder is to check the encoder's *structure*
 * (scan mapping, field order, suffixLength state, run_before direction),
 * which is where every bug found in this file so far actually lived. Table
 * CONTENT is checked by the ffmpeg round trip and by tests/test_cavlc.c.
 */
static const uint8_t ref_ct_len[4][68] = {
{  1,0,0,0,  6,2,0,0,  8,6,3,0,  9,8,7,5, 10,9,8,6,
  11,10,9,7, 13,11,10,8, 13,13,11,9, 13,13,13,10,
  14,14,13,11, 14,14,14,13, 15,15,14,14, 15,15,15,14,
  16,15,15,15, 16,16,16,15, 16,16,16,16, 16,16,16,16 },
{  2,0,0,0,  6,2,0,0,  6,5,3,0,  7,6,6,4,  8,6,6,4,
   8,7,7,5,  9,8,8,6, 11,9,9,6, 11,11,11,7,
  12,11,11,9, 12,12,12,11, 12,12,12,11, 13,13,13,12,
  13,13,13,13, 13,14,13,13, 14,14,14,13, 14,14,14,14 },
{  4,0,0,0,  6,4,0,0,  6,5,4,0,  6,5,5,4,  7,5,5,4,
   7,5,5,4,  7,6,6,4,  7,6,6,4,  8,7,7,5,
   8,8,7,6,  9,8,8,7,  9,9,8,8,  9,9,9,8,
  10,9,9,9, 10,10,10,10, 10,10,10,10, 10,10,10,10 },
{  6,0,0,0,  6,6,0,0,  6,6,6,0,  6,6,6,6,  6,6,6,6,
   6,6,6,6,  6,6,6,6,  6,6,6,6,  6,6,6,6,
   6,6,6,6,  6,6,6,6,  6,6,6,6,  6,6,6,6,
   6,6,6,6,  6,6,6,6,  6,6,6,6,  6,6,6,6 }
};
static const uint8_t ref_ct_bits[4][68] = {
{  1,0,0,0,  5,1,0,0,  7,4,1,0,  7,6,5,3,  7,6,5,3,
   7,6,5,4, 15,6,5,4, 11,14,5,4,  8,10,13,4,
  15,14,9,4, 11,10,13,12, 15,14,9,12, 11,10,13,8,
  15,1,9,12, 11,14,13,8,  7,10,9,12,  4,6,5,8 },
{  3,0,0,0, 11,2,0,0,  7,7,3,0,  7,10,9,5,  7,6,5,4,
   4,6,5,6,  7,6,5,8, 15,6,5,4, 11,14,13,4,
  15,10,9,4, 11,14,13,12,  8,10,9,8, 15,14,13,12,
  11,10,9,12,  7,11,6,8,  9,8,10,1,  7,6,5,4 },
{ 15,0,0,0, 15,14,0,0, 11,15,13,0,  8,12,14,12, 15,10,11,11,
  11,8,9,10,  9,14,13,9,  8,10,9,8, 15,14,13,13,
  11,14,10,12, 15,10,13,12, 11,14,9,12,  8,10,13,8,
  13,7,9,12,  9,12,11,10,  5,8,7,6,  1,4,3,2 },
{  3,0,0,0,  0,1,0,0,  4,5,6,0,  8,9,10,11, 12,13,14,15,
  16,17,18,19, 20,21,22,23, 24,25,26,27, 28,29,30,31,
  32,33,34,35, 36,37,38,39, 40,41,42,43, 44,45,46,47,
  48,49,50,51, 52,53,54,55, 56,57,58,59, 60,61,62,63 }
};
static const uint8_t ref_cdc_len[20] = {
    2,0,0,0,  6,1,0,0,  6,6,3,0,  6,7,7,6,  6,8,8,7
};
static const uint8_t ref_cdc_bits[20] = {
    1,0,0,0,  7,1,0,0,  4,6,1,0,  3,3,2,5,  2,3,2,0
};
static const uint8_t ref_tz_len[15][16] = {
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
static const uint8_t ref_tz_bits[15][16] = {
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
static const uint8_t ref_cdc_tz_len[3][4]  = { {1,2,3,3}, {1,2,2,0}, {1,1,0,0} };
static const uint8_t ref_cdc_tz_bits[3][4] = { {1,1,1,0}, {1,1,0,0}, {1,0,0,0} };
static const uint8_t ref_run_len[7][16] = {
    {1,1},{1,2,2},{2,2,2,2},{2,2,2,3,3},{2,2,3,3,3,3},{2,3,3,3,3,3,3},
    {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11}
};
static const uint8_t ref_run_bits[7][16] = {
    {1,0},{1,1,0},{3,2,1,0},{3,2,1,1,0},{3,2,3,2,1,0},{3,0,1,3,2,5,4},
    {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1}
};
static const uint8_t ref_cbp_inter[48] = {
    0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13,
    14, 6, 9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};

/* Generic longest-match-free prefix decode: find the unique entry whose
 * `len` leading bits equal the next `len` bits of the stream. Uniqueness is
 * guaranteed by check_prefix_codes() below, which is run before any decode. */
static int vlc_decode(br_t *r, const uint8_t *len, const uint8_t *bits, int n) {
    for (int i = 0; i < n; i++) {
        if (len[i] == 0) continue;
        if (br_peek(r, len[i]) == bits[i]) { br_u(r, len[i]); return i; }
    }
    r->err = 1;
    return -1;
}

/* Every VLC table used here must be a prefix code, or the decoder above is
 * not well defined - and a table that is NOT a prefix code cannot be
 * decoded by any decoder, which makes this a genuine (if partial) check on
 * table content that does not depend on ffmpeg. */
static bool check_one_prefix_code(const char *name, const uint8_t *len, const uint8_t *bits, int n) {
    bool ok = true;
    for (int i = 0; i < n; i++) {
        if (!len[i]) continue;
        for (int j = i + 1; j < n; j++) {
            if (!len[j]) continue;
            int shorter = (len[i] <= len[j]) ? i : j;
            int longer  = (shorter == i) ? j : i;
            uint32_t hi = (uint32_t)bits[longer] >> (len[longer] - len[shorter]);
            if (hi == bits[shorter]) {
                fprintf(stderr, "  table %s: entry %d is a prefix of entry %d\n", name, shorter, longer);
                ok = false;
            }
        }
    }
    return ok;
}

static bool check_prefix_codes(void) {
    bool ok = true;
    char nm[64];
    for (int t = 0; t < 4; t++) {
        snprintf(nm, sizeof(nm), "coeff_token[%d]", t);
        ok &= check_one_prefix_code(nm, ref_ct_len[t], ref_ct_bits[t], 68);
    }
    ok &= check_one_prefix_code("chroma_dc_coeff_token", ref_cdc_len, ref_cdc_bits, 20);
    for (int t = 0; t < 15; t++) {
        snprintf(nm, sizeof(nm), "total_zeros[%d]", t);
        ok &= check_one_prefix_code(nm, ref_tz_len[t], ref_tz_bits[t], 16);
    }
    for (int t = 0; t < 3; t++) {
        snprintf(nm, sizeof(nm), "chroma_dc_total_zeros[%d]", t);
        ok &= check_one_prefix_code(nm, ref_cdc_tz_len[t], ref_cdc_tz_bits[t], 4);
    }
    for (int t = 0; t < 7; t++) {
        snprintf(nm, sizeof(nm), "run_before[%d]", t);
        ok &= check_one_prefix_code(nm, ref_run_len[t], ref_run_bits[t], 16);
    }
    return ok;
}

/*
 * Decode one residual block into `out` (scan order, `max_coeff` entries).
 * Written from the ITU-T H.264 9.2 decoding process, NOT by inverting
 * cavlc.c's control flow: the level state machine, the coefficient
 * placement from total_zeros/run_before and the high-to-low frequency
 * direction are all re-derived here. Returns TotalCoeff, or -1 on error.
 */
static int ref_decode_block(br_t *r, int *out, int max_coeff, int nC, bool chroma_dc) {
    memset(out, 0, sizeof(int) * (size_t)max_coeff);

    int total_coeff, trailing_ones;
    if (chroma_dc) {
        int idx = vlc_decode(r, ref_cdc_len, ref_cdc_bits, 20);
        if (idx < 0) return -1;
        total_coeff = idx / 4;
        trailing_ones = idx % 4;
    } else {
        int t = (nC < 2) ? 0 : (nC < 4) ? 1 : (nC < 8) ? 2 : 3;
        int idx = vlc_decode(r, ref_ct_len[t], ref_ct_bits[t], 68);
        if (idx < 0) return -1;
        total_coeff = idx / 4;
        trailing_ones = idx % 4;
    }
    if (total_coeff == 0) return 0;
    if (total_coeff > max_coeff || trailing_ones > total_coeff) { r->err = 1; return -1; }

    /* levels[] is in high-to-low frequency order, the order they are coded. */
    int levels[16];
    for (int i = 0; i < trailing_ones; i++)
        levels[i] = br_u(r, 1) ? -1 : 1;

    int suffix_length = (total_coeff > 10 && trailing_ones < 3) ? 1 : 0;
    for (int i = trailing_ones; i < total_coeff; i++) {
        int level_prefix = 0;
        while (r->pos < r->bits && br_u(r, 1) == 0) {
            if (++level_prefix > 32) { r->err = 1; return -1; }
        }
        int suffix_size = suffix_length;
        if (level_prefix == 14 && suffix_length == 0) suffix_size = 4;
        else if (level_prefix >= 15) suffix_size = level_prefix - 3;
        int level_suffix = suffix_size ? (int)br_u(r, suffix_size) : 0;

        int level_code = ((level_prefix < 15 ? level_prefix : 15) << suffix_length) + level_suffix;
        if (level_prefix >= 15 && suffix_length == 0) level_code += 15;
        if (level_prefix >= 16) level_code += (1 << (level_prefix - 3)) - 4096;
        if (i == trailing_ones && trailing_ones < 3) level_code += 2;

        levels[i] = (level_code % 2 == 0) ? ((level_code + 2) >> 1) : ((-level_code - 1) >> 1);
        if (suffix_length == 0) suffix_length = 1;
        int a = levels[i] < 0 ? -levels[i] : levels[i];
        if (a > (3 << (suffix_length - 1)) && suffix_length < 6) suffix_length++;
    }

    int total_zeros = 0;
    if (total_coeff < max_coeff) {
        if (chroma_dc) {
            int idx = vlc_decode(r, ref_cdc_tz_len[total_coeff - 1], ref_cdc_tz_bits[total_coeff - 1], 4);
            if (idx < 0) return -1;
            total_zeros = idx;
        } else {
            int idx = vlc_decode(r, ref_tz_len[total_coeff - 1], ref_tz_bits[total_coeff - 1], 16);
            if (idx < 0) return -1;
            total_zeros = idx;
        }
    }

    /* run[j] = zeros between coefficient rank j and rank j+1, coded
     * high-to-low frequency (ITU-T 9.2.3). Whatever is left over after the
     * loop sits below the lowest-frequency coefficient. */
    int run[16] = {0};
    int zeros_left = total_zeros;
    for (int i = 1; i < total_coeff && zeros_left > 0; i++) {
        int zl = (zeros_left <= 6) ? (zeros_left - 1) : 6;
        int idx = vlc_decode(r, ref_run_len[zl], ref_run_bits[zl], 16);
        if (idx < 0) return -1;
        run[i - 1] = idx;
        zeros_left -= idx;
    }
    if (zeros_left < 0) { r->err = 1; return -1; }

    int pos = zeros_left;                       /* lowest-frequency coefficient */
    for (int j = total_coeff - 1; j >= 0; j--) {
        if (pos >= max_coeff) { r->err = 1; return -1; }
        out[pos] = levels[j];
        if (j > 0) pos += run[j - 1] + 1;
    }
    return total_coeff;
}

/* ========================================================================
 * verify - the oracle
 * ======================================================================== */

typedef struct { uint64_t blocks, coeffs, mismatches; } verify_result_t;

static bool cmp_block(const char *what, uint32_t mb, int blk, const int *dec,
                      const int16_t *ref_raster, int max_coeff, int from_scan,
                      verify_result_t *vr) {
    for (int i = 0; i < max_coeff; i++) {
        int want = ref_raster[zz[i + from_scan]];
        if (dec[i] != want) {
            if (vr->mismatches < 8)
                fprintf(stderr, "  MISMATCH %s mb=%u blk=%d scan=%d: decoded %d, encoded %d\n",
                        what, mb, blk, i, dec[i], want);
            vr->mismatches++;
            return false;
        }
        vr->coeffs++;
    }
    vr->blocks++;
    return true;
}

static bool cmp_chroma_dc(uint32_t mb, const int *dec, const int *ref, verify_result_t *vr) {
    for (int i = 0; i < 4; i++) {
        if (dec[i] != ref[i]) {
            if (vr->mismatches < 8)
                fprintf(stderr, "  MISMATCH chromaDC mb=%u pos=%d: decoded %d, encoded %d\n",
                        mb, i, dec[i], ref[i]);
            vr->mismatches++;
            return false;
        }
        vr->coeffs++;
    }
    vr->blocks++;
    return true;
}

static bool all_zero(const int16_t *b, int from, int to) {
    for (int i = from; i < to; i++) if (b[i]) return false;
    return true;
}

/*
 * verify_frame - reference-decode one slice's RBSP and compare every
 * coefficient against what the generator put in.
 *
 * WHAT THIS ORACLE COVERS
 *   - Coefficient PLACEMENT. The decoder reconstructs scan positions from
 *     total_zeros + run_before independently, so a wrong run_before
 *     direction puts coefficients in the wrong frequency band and this
 *     fails. That matters more than it sounds: the run_before ordering bug
 *     this file actually shipped produced perfectly valid CAVLC syntax -
 *     ffmpeg reported zero decode errors while luma PSNR sat at ~15.5dB.
 *     A decoder-error-count oracle cannot see that class of bug at all.
 *   - The suffixLength state machine across a whole block, the
 *     level_prefix>=15 escape, trailing-one sign packing, the AC/DC and
 *     2x2-chroma-DC variants, and the nC-driven table selection (the
 *     decoder re-derives nC from its own decoded TotalCoeff values, so a
 *     disagreement about nC desyncs immediately).
 *   - Macroblock-layer framing: mb_type, cbp, mb_skip_run placement, and
 *     that the slice ends exactly on its rbsp_stop_one_bit.
 *
 * WHAT IT DOES NOT COVER
 *   - VLC table CONTENT. The ref_ct_, ref_tz_ and ref_run_ tables above are
 *     copies of cavlc.c's numbers, so a wrong-but-self-consistent codeword decodes
 *     back cleanly here. This is exactly how the nC>=8 coeff_token table
 *     shipped with a uniform +4 offset for its whole life. Two other
 *     checks cover that gap and neither shares this code:
 *       * `cavlc_bench emit` writes a complete Annex-B stream; decoding it
 *         with ffmpeg (an independent implementation with its own tables)
 *         must report zero errors. That is what caught the +4 offset, the
 *         chroma-DC zero token, the missing mb_qp_delta and the mb_skip_run
 *         duplication, per cavlc.c's own commit comments.
 *       * tests/test_cavlc.c pins exact bytes for hand-derived cases.
 *     check_prefix_codes() additionally proves each table is a decodable
 *     prefix code, which no amount of self-consistency can fake.
 *   - Rate/quality. Nothing here says the bitstream is a GOOD encoding of
 *     anything - only that it is the encoding of the coefficients it was
 *     given. This harness has no pixels.
 */
static bool verify_frame(const frame_t *f, const uint8_t *rbsp, size_t rbsp_len,
                         size_t header_bits, uint32_t width_in_mbs, verify_result_t *vr) {
    br_t r = { rbsp, rbsp_len * 8, header_bits, 0 };

    uint8_t (*nz_luma)[16] = calloc(f->mbs, sizeof(*nz_luma));
    uint8_t (*nz_cb)[4] = calloc(f->mbs, sizeof(*nz_cb));
    uint8_t (*nz_cr)[4] = calloc(f->mbs, sizeof(*nz_cr));
    if (!nz_luma || !nz_cb || !nz_cr) { free(nz_luma); free(nz_cb); free(nz_cr); return false; }
    ncctx_t nc = { nz_luma, nz_cb, nz_cr, width_in_mbs };

    bool ok = true;
    int dec[16];
    uint32_t pending_skip = 0;

    for (uint32_t mb = 0; mb < f->mbs && ok && !r.err; mb++) {
        uint32_t mbx = mb % width_in_mbs, mby = mb / width_in_mbs;
        const int16_t (*blk)[16] = f->blk[mb];
        int cbp_luma_flag = 0, cbp_chroma = 0, luma_cbp = 0;
        bool intra = f->is_idr;

        if (!intra) {
            if (pending_skip == 0) pending_skip = br_ue(&r) + 1;  /* +1 marks "read" */
            if (pending_skip > 1) {           /* this MB is skipped */
                pending_skip--;
                memset(nz_luma[mb], 0, 16);
                memset(nz_cb[mb], 0, 4);
                memset(nz_cr[mb], 0, 4);
                if (!f->skip[mb]) {
                    fprintf(stderr, "  MISMATCH mb=%u decoded as skipped, encoder coded it\n", mb);
                    ok = false;
                }
                continue;
            }
            pending_skip = 0;
            if (f->skip[mb]) {
                fprintf(stderr, "  MISMATCH mb=%u encoder skipped it, decoder sees a coded MB\n", mb);
                ok = false;
                break;
            }
            uint32_t mb_type = br_ue(&r);
            if (mb_type != 0) {
                fprintf(stderr, "  mb=%u: expected P_L0_16x16 (mb_type 0), got %u\n", mb, mb_type);
                ok = false;
                break;
            }
            int32_t mvd_x = br_se(&r), mvd_y = br_se(&r);
            if (mvd_x != f->mvd[mb][0] || mvd_y != f->mvd[mb][1]) {
                fprintf(stderr, "  MISMATCH mb=%u mvd (%d,%d) != (%d,%d)\n",
                        mb, mvd_x, mvd_y, f->mvd[mb][0], f->mvd[mb][1]);
                ok = false;
                break;
            }
            uint32_t code_num = br_ue(&r);
            if (code_num >= 48) { ok = false; break; }
            int cbp = ref_cbp_inter[code_num];
            luma_cbp = cbp & 0xF;
            cbp_chroma = cbp >> 4;
            if (cbp > 0) (void)br_se(&r);   /* mb_qp_delta */
        } else {
            uint32_t mb_type = br_ue(&r);
            if (mb_type < 1 || mb_type > 24) {
                fprintf(stderr, "  mb=%u: I16x16 mb_type out of range: %u\n", mb, mb_type);
                ok = false;
                break;
            }
            uint32_t t = mb_type - 1;
            uint32_t pred_mode = t % 4;
            cbp_chroma = (t / 4) % 3;
            cbp_luma_flag = (t >= 12);
            uint32_t chroma_pred = br_ue(&r);
            (void)br_se(&r);                /* mb_qp_delta, unconditional for I16x16 */
            if (pred_mode != f->modes[mb][0] || chroma_pred != f->modes[mb][1]) {
                fprintf(stderr, "  MISMATCH mb=%u pred modes (%u,%u) != (%u,%u)\n",
                        mb, pred_mode, chroma_pred, f->modes[mb][0], f->modes[mb][1]);
                ok = false;
                break;
            }
        }

        if (intra) {
            int nC_dc = luma_nc(&nc, mb, mbx, mby, 0);
            if (ref_decode_block(&r, dec, 16, nC_dc, false) < 0) { ok = false; break; }
            if (!cmp_block("lumaDC", mb, -1, dec, f->luma_dc[mb], 16, 0, vr)) { ok = false; break; }

            if (cbp_luma_flag) {
                for (int bi = 0; bi < 16 && ok; bi++) {
                    int nCv = luma_nc(&nc, mb, mbx, mby, bi);
                    int tc = ref_decode_block(&r, dec, 15, nCv, false);
                    if (tc < 0) { ok = false; break; }
                    nz_luma[mb][bi] = (uint8_t)tc;
                    if (!cmp_block("lumaAC", mb, bi, dec, blk[raster_from_spec(bi)], 15, 1, vr)) ok = false;
                }
                if (!ok) break;
            } else {
                memset(nz_luma[mb], 0, 16);
                for (int b = 0; b < 16; b++)
                    if (!all_zero(blk[b], 1, 16)) {
                        fprintf(stderr, "  MISMATCH mb=%u cbp_luma=0 but block %d has AC\n", mb, b);
                        ok = false;
                    }
                if (!ok) break;
            }
        } else {
            for (int bi = 0; bi < 16 && ok; bi++) {
                if (luma_cbp & (1 << (bi / 4))) {
                    int nCv = luma_nc(&nc, mb, mbx, mby, bi);
                    int tc = ref_decode_block(&r, dec, 16, nCv, false);
                    if (tc < 0) { ok = false; break; }
                    nz_luma[mb][bi] = (uint8_t)tc;
                    if (!cmp_block("lumaP", mb, bi, dec, blk[raster_from_spec(bi)], 16, 0, vr)) ok = false;
                } else {
                    nz_luma[mb][bi] = 0;
                    if (!all_zero(blk[raster_from_spec(bi)], 0, 16)) {
                        fprintf(stderr, "  MISMATCH mb=%u quadrant cbp=0 but block %d nonzero\n", mb, bi);
                        ok = false;
                    }
                }
            }
            if (!ok) break;
        }

        if (cbp_chroma >= 1) {
            if (ref_decode_block(&r, dec, 4, 0, true) < 0) { ok = false; break; }
            if (!cmp_chroma_dc(mb, dec, &f->cdc[mb][0], vr)) { ok = false; break; }
            if (ref_decode_block(&r, dec, 4, 0, true) < 0) { ok = false; break; }
            if (!cmp_chroma_dc(mb, dec, &f->cdc[mb][4], vr)) { ok = false; break; }
        } else {
            for (int i = 0; i < 8; i++)
                if (f->cdc[mb][i]) {
                    fprintf(stderr, "  MISMATCH mb=%u cbp_chroma=0 but chroma DC nonzero\n", mb);
                    ok = false;
                }
            if (!ok) break;
        }

        if (cbp_chroma == 2) {
            for (int c = 0; c < 2 && ok; c++) {
                uint8_t (*nzc)[4] = c ? nz_cr : nz_cb;
                for (int bi = 0; bi < 4 && ok; bi++) {
                    int nCv = chroma_nc(nzc, mb, mbx, mby, width_in_mbs, bi);
                    int tc = ref_decode_block(&r, dec, 15, nCv, false);
                    if (tc < 0) { ok = false; break; }
                    nzc[mb][bi] = (uint8_t)tc;
                    if (!cmp_block(c ? "crAC" : "cbAC", mb, bi, dec, blk[(c ? 20 : 16) + bi], 15, 1, vr))
                        ok = false;
                }
            }
            if (!ok) break;
        } else {
            memset(nz_cb[mb], 0, 4);
            memset(nz_cr[mb], 0, 4);
            for (int b = 16; b < 24; b++)
                if (!all_zero(blk[b], 1, 16)) {
                    fprintf(stderr, "  MISMATCH mb=%u cbp_chroma<2 but chroma block %d has AC\n", mb, b);
                    ok = false;
                }
            if (!ok) break;
        }
    }

    if (ok && !r.err && !f->is_idr && pending_skip == 0) {
        /* A trailing skip run is written after the last coded MB. Nothing to
         * consume here - the position check below covers it. */
    }

    if (r.err) {
        fprintf(stderr, "  bit reader ran off the end of the slice\n");
        ok = false;
    }

    /* The slice must end exactly on rbsp_stop_one_bit + zero padding: any
     * leftover or missing bit means an element was mis-sized somewhere. */
    if (ok) {
        if (br_u(&r, 1) != 1) {
            fprintf(stderr, "  no rbsp_stop_one_bit where the slice should end (bit %zu of %zu)\n",
                    r.pos - 1, rbsp_len * 8);
            ok = false;
        } else {
            while (r.pos < rbsp_len * 8) {
                if (br_u(&r, 1) != 0) {
                    fprintf(stderr, "  nonzero rbsp trailing padding at bit %zu\n", r.pos - 1);
                    ok = false;
                    break;
                }
            }
        }
    }

    free(nz_luma); free(nz_cb); free(nz_cr);
    return ok;
}

/* ========================================================================
 * Driver
 * ======================================================================== */

typedef struct {
    uint32_t width, height, width_in_mbs, height_in_mbs, mbs;
    uint32_t frames;
    int qp;
    uint64_t seed;
    const density_profile_t *prof;
    long iters, samples;
    unsigned mask_a, mask_b;
    const char *out;
} opts_t;

typedef struct {
    frame_t *f;
    uint32_t n;
    uint8_t *rbsp;
    size_t rbsp_cap;
    uint8_t (*nz_luma)[16];
    uint8_t (*nz_cb)[4];
    uint8_t (*nz_cr)[4];
} pool_t;

static void pool_free(pool_t *p) {
    for (uint32_t i = 0; i < p->n; i++) frame_free(&p->f[i]);
    free(p->f); free(p->rbsp); free(p->nz_luma); free(p->nz_cb); free(p->nz_cr);
    memset(p, 0, sizeof(*p));
}

static bool pool_build(pool_t *p, const opts_t *o) {
    memset(p, 0, sizeof(*p));
    p->n = o->frames;
    p->f = calloc(p->n, sizeof(frame_t));
    if (!p->f) return false;
    for (uint32_t i = 0; i < p->n; i++) {
        if (!frame_alloc(&p->f[i], o->mbs)) return false;
        gen_frame(&p->f[i], o->prof, o->width_in_mbs, i == 0, o->seed + i);
    }
    /* Same allowance the shipping encoder uses for a CAVLC slice. */
    p->rbsp_cap = (size_t)o->mbs * 2560 + 4096;
    p->rbsp = malloc(p->rbsp_cap);
    p->nz_luma = calloc(o->mbs, sizeof(*p->nz_luma));
    p->nz_cb = calloc(o->mbs, sizeof(*p->nz_cb));
    p->nz_cr = calloc(o->mbs, sizeof(*p->nz_cr));
    return p->rbsp && p->nz_luma && p->nz_cb && p->nz_cr;
}

/* Encode one frame's slice data into p->rbsp. Returns bytes written. */
static size_t encode_one(pool_t *p, const opts_t *o, uint32_t idx, size_t header_bits_out[1]) {
    bitstream_t bs;
    bs_init(&bs, p->rbsp, p->rbsp_cap);
    memset(p->nz_luma, 0, (size_t)o->mbs * sizeof(*p->nz_luma));
    memset(p->nz_cb, 0, (size_t)o->mbs * sizeof(*p->nz_cb));
    memset(p->nz_cr, 0, (size_t)o->mbs * sizeof(*p->nz_cr));
    ncctx_t nc = { p->nz_luma, p->nz_cb, p->nz_cr, o->width_in_mbs };
    if (header_bits_out) *header_bits_out = 0;
    emit_slice_data(&bs, &p->f[idx], &nc);
    if (bs.overflow) { fprintf(stderr, "slice buffer overflow\n"); exit(1); }
    return bs_bytes_written(&bs);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One timing sample: `iters` passes over the whole frame pool. Returns ns.
 * The byte total is folded into `sink` so nothing can be elided. */
static uint64_t timed_pass(pool_t *p, const opts_t *o, long iters, uint64_t *sink) {
    uint64_t acc = 0;
    uint64_t t0 = now_ns();
    for (long it = 0; it < iters; it++)
        for (uint32_t i = 0; i < p->n; i++)
            acc += encode_one(p, o, i, NULL);
    uint64_t t1 = now_ns();
    *sink += acc;
    return t1 - t0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

#ifdef CAVLC_PROFILE
/* Only the `scan` command reports a single one-sided series; everything
 * else goes through run_ab() below. */
static void summarize(const char *label, uint64_t *v, long n, double mb_per_pass, long iters) {
    qsort(v, (size_t)n, sizeof(uint64_t), cmp_u64);
    double best = (double)v[0];
    double med = (n & 1) ? (double)v[n / 2] : ((double)v[n / 2 - 1] + (double)v[n / 2]) / 2.0;
    double mbs = mb_per_pass * (double)iters;
    printf("  %-10s best %9.3f ms  median %9.3f ms  worst %9.3f ms  (%.1f / %.1f ns per MB)\n",
           label, best / 1e6, med / 1e6, (double)v[n - 1] / 1e6, best / mbs, med / mbs);
}
#endif

/* ========================================================================
 * Annex-B stream writer (for the ffmpeg round trip)
 * ======================================================================== */

static bool emit_stream(pool_t *p, const opts_t *o, const char *path, bool do_verify) {
    FILE *fp = NULL;
    if (path) {
        fp = fopen(path, "wb");
        if (!fp) { perror("fopen"); return false; }
    }

    h264_sps_t sps;
    h264_pps_t pps;
    h264_sps_default(&sps, o->width, o->height, 60, PROFILE_BASELINE);
    h264_pps_default(&pps, 0, false /* CAVLC */, o->qp);

    uint8_t ps[512];
    if (fp) {
        size_t n = bs_write_sps(ps, sizeof(ps), &sps);
        fwrite(ps, 1, n, fp);
        n = bs_write_pps(ps, sizeof(ps), &pps);
        fwrite(ps, 1, n, fp);
    }

    uint8_t *ebsp = malloc(p->rbsp_cap + p->rbsp_cap / 2 + 64);
    if (!ebsp) { if (fp) fclose(fp); return false; }

    bool ok = true;
    verify_result_t vr = {0, 0, 0};
    uint32_t frame_num = 0;

    for (uint32_t i = 0; i < p->n && ok; i++) {
        bool is_idr = p->f[i].is_idr;

        bitstream_t bs;
        bs_init(&bs, p->rbsp, p->rbsp_cap);
        emit_slice_header(&bs, is_idr, frame_num, 0, (int)(2 * i), o->qp, &sps, &pps);
        size_t header_bits = (size_t)bs.byte_offset * 8 + (size_t)bs.bit_offset;

        memset(p->nz_luma, 0, (size_t)o->mbs * sizeof(*p->nz_luma));
        memset(p->nz_cb, 0, (size_t)o->mbs * sizeof(*p->nz_cb));
        memset(p->nz_cr, 0, (size_t)o->mbs * sizeof(*p->nz_cr));
        ncctx_t nc = { p->nz_luma, p->nz_cb, p->nz_cr, o->width_in_mbs };
        emit_slice_data(&bs, &p->f[i], &nc);
        if (bs.overflow) { fprintf(stderr, "slice buffer overflow\n"); ok = false; break; }
        size_t rbsp_len = bs_bytes_written(&bs);

        if (do_verify && !verify_frame(&p->f[i], p->rbsp, rbsp_len, header_bits, o->width_in_mbs, &vr)) {
            fprintf(stderr, "  frame %u (%s) FAILED reference-decode round trip\n",
                    i, is_idr ? "IDR" : "P");
            ok = false;
        }

        if (fp) {
            bitstream_t hb;
            uint8_t nal[8];
            bs_init(&hb, nal, sizeof(nal));
            bs_write_nal_header(&hb, is_idr ? NAL_REF_IDC_HIGH : NAL_REF_IDC_LOW,
                                is_idr ? NAL_TYPE_IDR_SLICE : NAL_TYPE_SLICE);
            size_t hn = bs_bytes_written(&hb);
            fwrite(nal, 1, hn, fp);
            size_t en = bs_rbsp_to_ebsp(ebsp, p->rbsp_cap + p->rbsp_cap / 2 + 64, p->rbsp, rbsp_len);
            fwrite(ebsp, 1, en, fp);
        }
        if (!is_idr) frame_num++;
        else frame_num = 1;
    }

    if (do_verify && ok)
        printf("  reference decoder: %llu blocks / %llu coefficients round-tripped exactly\n",
               (unsigned long long)vr.blocks, (unsigned long long)vr.coeffs);

    free(ebsp);
    if (fp) fclose(fp);
    return ok;
}

/* ========================================================================
 * Commands
 * ======================================================================== */

static void print_stats(pool_t *p, const opts_t *o) {
    gather_stats = true;
    memset(&gstats, 0, sizeof(gstats));
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < p->n; i++) bytes += encode_one(p, o, i, NULL);
    gather_stats = false;

    uint64_t blocks = 0, zero = 0;
    for (int i = 0; i < 4; i++) { blocks += gstats.blocks[i]; zero += gstats.zero_blocks[i]; }
    static const char *kind[4] = { "luma DC", "luma AC", "chroma DC", "chroma AC" };

    printf("input distribution: %ux%u (%u MB) x %u frames, profile '%s', seed %llu\n",
           o->width, o->height, o->mbs, p->n, o->prof->name, (unsigned long long)o->seed);
    printf("  macroblocks      I_16x16 %llu   P_L0_16x16 %llu   P_Skip %llu\n",
           (unsigned long long)gstats.mb_i16, (unsigned long long)gstats.mb_p,
           (unsigned long long)gstats.mb_skip);
    printf("  coded bytes      %llu (%.1f bytes/MB)\n", (unsigned long long)bytes,
           (double)bytes / (double)(gstats.mb_i16 + gstats.mb_p + gstats.mb_skip));
    /* Two different denominators, both worth knowing and easy to confuse.
     * "present" counts every 4x4 block in every macroblock of the frame,
     * which is what encoder_h264.c's nz_mask comment (~90-96% entirely
     * zero on real content) is measured over - most of those blocks never
     * reach CAVLC at all because cbp gates them out. "reaching CAVLC" is
     * the subset the entropy coder actually has to process, and is the
     * denominator that matters for a CAVLC profile. */
    uint64_t present = (uint64_t)o->mbs * 24ull * (uint64_t)p->n;
    uint64_t present_nonzero = 0;
    for (uint32_t i = 0; i < p->n; i++)
        for (uint32_t mb = 0; mb < o->mbs; mb++)
            for (int b = 0; b < 24; b++)
                if (block_any_nonzero(p->f[i].blk[mb][b], 0, 16)) present_nonzero++;
    printf("  4x4 blocks present  %llu, entirely zero %.1f%%"
           "   [encoder_h264.c nz_mask comment: ~90-96%% on real content]\n",
           (unsigned long long)present,
           100.0 * (double)(present - present_nonzero) / (double)present);
    printf("  4x4 blocks reaching CAVLC %llu, entirely zero %llu (%.1f%%)\n",
           (unsigned long long)blocks, (unsigned long long)zero,
           100.0 * (double)zero / (double)(blocks ? blocks : 1));
    for (int i = 0; i < 4; i++)
        printf("    %-10s  %10llu blocks, %5.1f%% zero\n", kind[i],
               (unsigned long long)gstats.blocks[i],
               100.0 * (double)gstats.zero_blocks[i] / (double)(gstats.blocks[i] ? gstats.blocks[i] : 1));
    printf("  coeff_token table selection (nC class):\n");
    static const char *ncn[4] = { "0 (nC<2)", "1 (2<=nC<4)", "2 (4<=nC<8)", "3 (nC>=8)" };
    uint64_t nctot = 0;
    for (int i = 0; i < 4; i++) nctot += gstats.nc_class[i];
    for (int i = 0; i < 4; i++)
        printf("    table %-12s %10llu  %5.1f%%\n", ncn[i],
               (unsigned long long)gstats.nc_class[i],
               100.0 * (double)gstats.nc_class[i] / (double)(nctot ? nctot : 1));
    printf("  TotalCoeff histogram (all coded blocks):\n    ");
    for (int i = 0; i <= 16; i++) {
        if (!gstats.total_coeff_hist[i] && i > 8) continue;
        printf("%d:%.2f%% ", i, 100.0 * (double)gstats.total_coeff_hist[i] / (double)(blocks ? blocks : 1));
    }
    printf("\n  nonzero coefficients per coded block: %.2f\n",
           (double)gstats.coeffs_nonzero / (double)(blocks ? blocks : 1));
}

typedef struct { double best_a, med_a, best_b, med_b; } ab_result_t;

/*
 * One interleaved A/B measurement. Both sides run in this process, over the
 * same pool, alternating order every sample (see the comment inside), and
 * the caller gets best and median for each side. With mask_a == mask_b this
 * is the rig self-test: it must come back at 1.00x, because a harness that
 * cannot report 1.00x on identical code cannot report anything else either.
 */
static ab_result_t run_ab(pool_t *p, const opts_t *o, unsigned mask_a, unsigned mask_b,
                          long iters, long samples, uint64_t *sink) {
    uint64_t *va = malloc(sizeof(uint64_t) * (size_t)samples);
    uint64_t *vb = malloc(sizeof(uint64_t) * (size_t)samples);
    ab_result_t r = {0, 0, 0, 0};
    if (!va || !vb) { free(va); free(vb); return r; }
    (void)mask_a; (void)mask_b;

    for (long s = 0; s < samples; s++) {
        /* ABBAABBA..., not ABABAB. Plain alternation leaves whichever side
         * runs first in each pair carrying a systematic penalty - measured
         * here at a reproducible ~2% with both sides running IDENTICAL
         * code, which is the same size as a win worth chasing. */
        bool a_first = ((s & 1) == 0);
        for (int half = 0; half < 2; half++) {
            bool doing_a = ((half == 0) == a_first);
            unsigned m = doing_a ? mask_a : mask_b;
#ifdef CAVLC_PROFILE
            cavlc_ablate_mask = m & 0x7Fu;   /* 0x3F ablations + 0x40 fast-path switch */
#endif
            /* MASK_GLUE (bit 8) is a harness-side switch, not a cavlc.c
             * ablation, so it works in both builds - see glue_only. */
            glue_only = (m & MASK_GLUE) ? 1 : 0;
            uint64_t ns = timed_pass(p, o, iters, sink);
            if (doing_a) va[s] = ns; else vb[s] = ns;
        }
    }
    glue_only = 0;
#ifdef CAVLC_PROFILE
    cavlc_ablate_mask = 0;
#endif
    qsort(va, (size_t)samples, sizeof(uint64_t), cmp_u64);
    qsort(vb, (size_t)samples, sizeof(uint64_t), cmp_u64);
    r.best_a = (double)va[0];
    r.best_b = (double)vb[0];
    r.med_a = (samples & 1) ? (double)va[samples / 2]
                            : ((double)va[samples / 2 - 1] + (double)va[samples / 2]) / 2.0;
    r.med_b = (samples & 1) ? (double)vb[samples / 2]
                            : ((double)vb[samples / 2 - 1] + (double)vb[samples / 2]) / 2.0;
    free(va); free(vb);
    return r;
}

static long calibrate_iters(pool_t *p, const opts_t *o) {
    uint64_t sink = 0;
    long it = 1;
    for (;;) {
        uint64_t ns = timed_pass(p, o, it, &sink);
        if (ns > 60000000ull || it > 100000) {
            long want = (long)((double)it * 300e6 / (double)(ns ? ns : 1));
            if (want < 1) want = 1;
            return want;
        }
        it *= 4;
    }
}

int main(int argc, char **argv) {
    opts_t o;
    memset(&o, 0, sizeof(o));
    o.width = 1280; o.height = 720; o.frames = 8; o.qp = 26;
    o.seed = 1; o.prof = &profiles[1]; o.iters = 0; o.samples = 9;
    o.mask_a = 0; o.mask_b = 0; o.out = NULL;

    const char *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--size=", 7)) {
            if (sscanf(a + 7, "%ux%u", &o.width, &o.height) != 2) { fprintf(stderr, "bad --size\n"); return 2; }
        } else if (!strncmp(a, "--frames=", 9)) { o.frames = (uint32_t)strtoul(a + 9, NULL, 10);
        } else if (!strncmp(a, "--qp=", 5))     { o.qp = atoi(a + 5);
        } else if (!strncmp(a, "--seed=", 7))   { o.seed = strtoull(a + 7, NULL, 10);
        } else if (!strncmp(a, "--iters=", 8))  { o.iters = strtol(a + 8, NULL, 10);
        } else if (!strncmp(a, "--samples=", 10)) { o.samples = strtol(a + 10, NULL, 10);
        } else if (!strncmp(a, "--out=", 6))    { o.out = a + 6;
        } else if (!strncmp(a, "--ab=", 5))     {
            if (sscanf(a + 5, "%x:%x", &o.mask_a, &o.mask_b) != 2) { fprintf(stderr, "bad --ab (want hex:hex)\n"); return 2; }
        } else if (!strncmp(a, "--profile=", 10)) {
            const char *n = a + 10;
            const density_profile_t *found = NULL;
            for (int k = 0; k < NUM_PROFILES; k++) if (!strcmp(profiles[k].name, n)) found = &profiles[k];
            if (!found) { fprintf(stderr, "unknown profile '%s'\n", n); return 2; }
            o.prof = found;
        } else if (a[0] == '-') {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        } else if (!cmd) {
            cmd = a;
        } else {
            fprintf(stderr, "unexpected argument %s\n", a);
            return 2;
        }
    }
    if (!cmd) {
        fprintf(stderr, "usage: %s {stats|verify|emit|bench|ab|profile|count|scan} [options]\n"
                        "see the comment at the top of tools/cavlc_bench.c\n", argv[0]);
        return 2;
    }
    if (o.frames < 1) o.frames = 1;
    if (o.samples < 7) {
        fprintf(stderr, "--samples must be >= 7 (docs/performance-measurement.md)\n");
        return 2;
    }

    o.width_in_mbs = (o.width + 15) / 16;
    o.height_in_mbs = (o.height + 15) / 16;
    o.mbs = o.width_in_mbs * o.height_in_mbs;

    pool_t pool;
    if (!pool_build(&pool, &o)) { fprintf(stderr, "out of memory\n"); return 1; }

    int rc = 0;
    if (!strcmp(cmd, "stats")) {
        print_stats(&pool, &o);
    } else if (!strcmp(cmd, "verify")) {
        printf("verify: %ux%u, %u frames, profile '%s', seed %llu\n",
               o.width, o.height, o.frames, o.prof->name, (unsigned long long)o.seed);
        if (!check_prefix_codes()) {
            fprintf(stderr, "  VLC table prefix-code check FAILED\n");
            rc = 1;
        } else {
            printf("  every VLC table is a valid prefix code\n");
            if (!emit_stream(&pool, &o, o.out, true)) rc = 1;
        }
        printf(rc ? "VERIFY FAILED\n" : "VERIFY OK\n");
    } else if (!strcmp(cmd, "emit")) {
        if (!o.out) { fprintf(stderr, "emit needs --out=FILE\n"); rc = 2; }
        else if (!emit_stream(&pool, &o, o.out, false)) rc = 1;
        else printf("wrote %s (%ux%u, %u frames)\n", o.out, o.width, o.height, o.frames);
    } else if (!strcmp(cmd, "bench") || !strcmp(cmd, "ab")) {
        long iters = o.iters ? o.iters : calibrate_iters(&pool, &o);
        double mbs = (double)o.mbs * (double)pool.n * (double)iters;
        uint64_t sink = 0;
        printf("%s: %ux%u x%u frames, profile '%s', %ld iters/sample, %ld samples\n",
               cmd, o.width, o.height, pool.n, o.prof->name, iters, o.samples);
#ifdef CAVLC_PROFILE
        printf("  ablation masks: A=0x%02x B=0x%02x\n", o.mask_a, o.mask_b);
#else
        if ((o.mask_a | o.mask_b) & 0x7Fu) {
            fprintf(stderr, "  ablation bits 0x01-0x40 need the CAVLC_PROFILE build "
                            "(target cavlc_bench_prof); 0x100 (MB-layer glue) works here\n");
            rc = 2;
        }
#endif
        if (rc == 0) {
            (void)timed_pass(&pool, &o, 1, &sink);   /* warm up page faults / caches */
            ab_result_t r = run_ab(&pool, &o, o.mask_a, o.mask_b, iters, o.samples, &sink);
            printf("  A          best %9.3f ms  median %9.3f ms   (%.1f / %.1f ns per MB)\n",
                   r.best_a / 1e6, r.med_a / 1e6, r.best_a / mbs, r.med_a / mbs);
            printf("  B          best %9.3f ms  median %9.3f ms   (%.1f / %.1f ns per MB)\n",
                   r.best_b / 1e6, r.med_b / 1e6, r.best_b / mbs, r.med_b / mbs);
            printf("  B/A  best %.4fx  median %.4fx    (B faster than A when < 1)\n",
                   r.best_b / r.best_a, r.med_b / r.med_a);
            printf("  A-B  best %.3f ms median %.3f ms  (cost of what B removes)\n",
                   (r.best_a - r.best_b) / 1e6, (r.med_a - r.med_b) / 1e6);
            printf("  checksum %llu\n", (unsigned long long)sink);
        }
    } else if (!strcmp(cmd, "profile")) {
#ifdef CAVLC_PROFILE
        /*
         * Where CAVLC time goes, by direct measurement rather than
         * sampling. For each syntax element in turn, the SAME binary runs
         * over the SAME data twice - once normally, once with only that
         * element's bitstream writes suppressed - interleaved, order
         * alternating, best and median over `samples`. Everything else in
         * the function (the scan, the table lookups, the control flow, the
         * counting branch) is bit-for-bit identical between the two sides,
         * so the wall-clock difference is that element's marginal cost.
         *
         * Two honest caveats, both printed with the table:
         *   - Marginal, not additive. Out-of-order execution overlaps these
         *     stages, so the parts do not have to sum to the whole; the
         *     "all writes" row is measured separately as a cross-check and
         *     the sum-vs-total gap is reported rather than hidden.
         *   - The self-test row (baseline vs baseline) is the resolution
         *     floor of this rig. Anything smaller than it is not a result.
         */
        long iters = o.iters ? o.iters : calibrate_iters(&pool, &o);
        uint64_t sink = 0;
        printf("CAVLC stage profile: %ux%u x%u frames, profile '%s', %ld iters/sample, %ld samples\n",
               o.width, o.height, pool.n, o.prof->name, iters, o.samples);
        printf("  (ablation deltas from the CAVLC_PROFILE build; its instrumentation\n"
               "   branches cost ~5%% against cavlc_bench - run `bench` on both binaries\n"
               "   to re-check. Shares below are of THIS build's own baseline.)\n");
        (void)timed_pass(&pool, &o, 1, &sink);

        struct { unsigned mask; const char *name; } rows[] = {
            { 0u,                    "SELF-TEST (A/A)" },
            { 1u << CAVLC_S_TOKEN,   "coeff_token" },
            { 1u << CAVLC_S_SIGNS,   "t1 sign bits" },
            { 1u << CAVLC_S_LEVELS,  "levels" },
            { 1u << CAVLC_S_TZEROS,  "total_zeros" },
            { 1u << CAVLC_S_RUNS,    "run_before" },
            { 1u << CAVLC_S_HEADER,  "mb headers" },
            { 0x3Fu,                 "ALL WRITES" },
            { MASK_GLUE,             "ALL of cavlc.c" },
        };
        int nrows = (int)(sizeof(rows) / sizeof(rows[0]));
        double baseline_best = 0, baseline_med = 0, sum_best = 0, floor_best = 0;
        printf("  %-16s %11s %11s %9s %9s\n", "stage", "best dt ms", "med dt ms", "%best", "%med");
        for (int i = 0; i < nrows; i++) {
            ab_result_t r = run_ab(&pool, &o, 0u, rows[i].mask, iters, o.samples, &sink);
            if (i == 0) { baseline_best = r.best_a; baseline_med = r.med_a; }
            double db = r.best_a - r.best_b, dm = r.med_a - r.med_b;
            if (i == 0) floor_best = (db < 0 ? -db : db);
            else if (rows[i].mask != 0x3Fu && rows[i].mask != MASK_GLUE) sum_best += db;
            printf("  %-16s %11.3f %11.3f %8.1f%% %8.1f%%\n", rows[i].name,
                   db / 1e6, dm / 1e6,
                   100.0 * db / baseline_best, 100.0 * dm / baseline_med);
        }
        printf("  baseline    best %.3f ms  median %.3f ms\n", baseline_best / 1e6, baseline_med / 1e6);
        printf("  sum of the six per-stage best deltas: %.1f%% of baseline\n",
               100.0 * sum_best / baseline_best);
        printf("  rig resolution floor (|A/A delta|): %.2f%% of baseline - "
               "anything smaller is noise\n", 100.0 * floor_best / baseline_best);
        printf("  checksum %llu\n", (unsigned long long)sink);
#else
        fprintf(stderr, "profile needs the CAVLC_PROFILE build (target cavlc_bench_prof)\n");
        rc = 2;
#endif
    } else if (!strcmp(cmd, "count")) {
#ifdef CAVLC_PROFILE
        cavlc_prof_reset();
        cavlc_count_enable = 1;
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < pool.n; i++) bytes += encode_one(&pool, &o, i, NULL);
        cavlc_count_enable = 0;
        static const char *sn[CAVLC_S_COUNT] = {
            "coeff_token", "t1 signs", "levels", "total_zeros", "run_before", "mb header"
        };
        unsigned long long tb = 0, tc = 0;
        for (int i = 0; i < CAVLC_S_COUNT; i++) { tb += cavlc_stage_bits[i]; tc += cavlc_stage_calls[i]; }
        printf("exact syntax-element accounting over %u frames (%llu coded bytes)\n",
               pool.n, (unsigned long long)bytes);
        printf("  %-12s %14s %8s %14s %8s %8s\n", "stage", "calls", "%calls", "bits", "%bits", "bits/call");
        for (int i = 0; i < CAVLC_S_COUNT; i++)
            printf("  %-12s %14llu %7.2f%% %14llu %7.2f%% %8.2f\n", sn[i],
                   cavlc_stage_calls[i], 100.0 * (double)cavlc_stage_calls[i] / (double)(tc ? tc : 1),
                   cavlc_stage_bits[i], 100.0 * (double)cavlc_stage_bits[i] / (double)(tb ? tb : 1),
                   (double)cavlc_stage_bits[i] / (double)(cavlc_stage_calls[i] ? cavlc_stage_calls[i] : 1));
        static const char *bn[3] = { "4x4 (full)", "4x4 AC", "chroma DC" };
        printf("  entry point calls:\n");
        for (int i = 0; i < 3; i++)
            printf("    %-12s %14llu  of which all-zero %llu (%.1f%%)\n", bn[i],
                   cavlc_blocks_total[i], cavlc_blocks_zero[i],
                   100.0 * (double)cavlc_blocks_zero[i] / (double)(cavlc_blocks_total[i] ? cavlc_blocks_total[i] : 1));
#else
        fprintf(stderr, "count needs the CAVLC_PROFILE build (target cavlc_bench_prof)\n");
        rc = 2;
#endif
    } else if (!strcmp(cmd, "scan")) {
#ifdef CAVLC_PROFILE
        /* Price the zigzag gather + cavlc_scan_coeffs() in isolation: the one
         * stage that cannot be ablated, because without it there is no
         * total_coeff to write anything about. */
        long iters = o.iters ? o.iters : 200;
        uint64_t *v = malloc(sizeof(uint64_t) * (size_t)o.samples);
        if (!v) { pool_free(&pool); return 1; }
        volatile int sink = 0;
        for (long s = 0; s < o.samples; s++) {
            uint64_t t0 = now_ns();
            for (long it = 0; it < iters; it++)
                for (uint32_t i = 0; i < pool.n; i++)
                    for (uint32_t mb = 0; mb < pool.f[i].mbs; mb++)
                        for (int b = 0; b < 24; b++)
                            sink += cavlc_prof_scan_only(pool.f[i].blk[mb][b], b < 16 ? 16 : 15);
            v[s] = now_ns() - t0;
        }
        summarize("scan", v, o.samples, (double)o.mbs * pool.n, iters);
        printf("  (24 blocks per MB, gather+scan only, no bitstream writes; sink %d)\n", sink);
        free(v);
#else
        fprintf(stderr, "scan needs the CAVLC_PROFILE build\n");
        rc = 2;
#endif
    } else {
        fprintf(stderr, "unknown command '%s'\n", cmd);
        rc = 2;
    }

    pool_free(&pool);
    return rc;
}
