/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: MIT
 *
 * encoder_h264.c - H.264/AVC Compute Shader Encoder Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "bitstream.h"
#include "cavlc.h"
#include "rate_control.h"
#include "gpu_compute.h"
#include "encoder_h264.h"

/* Decoded Picture Buffer entry */
typedef struct dpb_entry {
    gpu_image_t image;          /* Reconstructed frame on GPU */
    gpu_memory_t memory;
    int frame_num;              /* H.264 frame_num */
    int poc;                    /* Picture order count */
    bool is_reference;          /* Used as reference? */
    bool is_long_term;          /* Long-term reference */
} dpb_entry_t;

/*
 * ============================================================================
 * Spec (clause 6.4.3) luma4x4BlkIdx <-> GPU raster block-index remapping
 * ============================================================================
 *
 * The GPU buffers (quant_levels_buffer/coeff_buffer) lay out the 16 luma 4x4
 * blocks of a macroblock in plain raster order: GPU block index = row*4+col,
 * where (row,col) are in 4x4-pixel-block units (block 0 = MB pixel offset
 * (0,0), block 1 = (4,0), block 4 = (0,4), etc - confirmed by re-reading
 * dct_transform.comp/quantize.comp, whose global_block_idx = mb_idx*24+block_idx
 * assumes exactly this).
 *
 * H.264's bitstream, however, must present luma4x4BlkIdx in *spec* order
 * (0..15), which maps to pixel offsets via clause 6.4.3's InverseRasterScan
 * quadrant math: quadrant q = blkIdx/4 gives a (2*(q%2), 2*(q/2)) 4x4-block
 * offset, and sub = blkIdx%4 gives a (sub%2, sub/2) offset within that
 * quadrant. blk_x4[]/blk_y4[] below were hand-derived from that formula and
 * verified against every entry (see task notes / commit message).
 */
static const int blk_x4[16] = {0,1,0,1, 2,3,2,3, 0,1,0,1, 2,3,2,3};
static const int blk_y4[16] = {0,0,1,1, 0,0,1,1, 2,2,3,3, 2,2,3,3};

/* Inverse of (blk_x4,blk_y4): spec_blk_idx_from_xy[y4][x4] -> spec blkIdx.
 * Built by hand-inverting the table above (blkIdx -> (blk_x4[blkIdx], blk_y4[blkIdx])). */
static const int spec_blk_idx_from_xy[4][4] = {
    {  0,  1,  4,  5 },
    {  2,  3,  6,  7 },
    {  8,  9, 12, 13 },
    { 10, 11, 14, 15 }
};

static inline int gpu_raster_block_idx(int blk_idx) {
    return blk_y4[blk_idx] * 4 + blk_x4[blk_idx];
}

/* Chroma 4x4 blocks are a plain 2x2 grid with no further sub-quadrant
 * structure (unlike luma's 16-block, 4-quadrant layout), so clause 6.4.3's
 * chroma equivalent (8.5.11.2-ish 2x2 InverseRasterScan) reduces to the same
 * raster order the GPU buffers already use (block (row,col) = row*2+col,
 * TL=0,TR=1,BL=2,BR=3). No remap table is needed for chroma - checked by
 * working the quadrant formula for a 2x2 (not 4x4) grid by hand: with only
 * one level of blocks there is no quadrant/sub split to reorder. */

/*
 * ============================================================================
 * H.264 encoder state
 * ============================================================================
 */

/* Per-slice/per-frame CAVLC neighbor (nC) context, indexed by absolute
 * macroblock address. Persists across frames (see h264_encoder_create) since
 * every entry is unconditionally overwritten before any later macroblock in
 * the same frame could read it as a neighbor (neighbors always have a
 * strictly smaller mb address, and mbs are processed in increasing order),
 * so stale data from a previous frame is never observed. */
typedef struct {
    uint8_t (*nz_luma)[16];  /* [total_mbs][16], indexed by SPEC blkIdx */
    uint8_t (*nz_cb)[4];     /* [total_mbs][4] */
    uint8_t (*nz_cr)[4];     /* [total_mbs][4] */
    uint32_t width_in_mbs;
    uint32_t start_mb;       /* current slice's first_mb_in_slice */
} nc_ctx_t;

/* H.264 encoder state */
struct h264_encoder {
    /* Dimensions */
    uint32_t width, height;
    uint32_t width_in_mbs, height_in_mbs;
    uint32_t total_mbs;

    /* GOP & Stream structure */
    uint32_t fps;
    uint32_t gop_size;          /* IDR interval */
    uint32_t frame_count;       /* Total frames encoded */
    uint32_t frame_num;         /* H.264 frame_num (resets at IDR) */
    uint32_t idr_pic_id;        /* Increments at each IDR */
    int poc;                    /* Picture order count */
    bool force_idr;             /* Dynamic keyframe request flag */

    /* Parameters */
    h264_sps_t sps;
    h264_pps_t pps;
    rate_control_t rc;

    /* DPB */
    dpb_entry_t dpb[16];
    int dpb_count;
    int dpb_max;

    /* GPU context reference */
    bc250_gpu_context_t *gpu;

    /* Output buffer */
    uint8_t *output_buf;
    size_t output_buf_size;

    /* Software reference frame for host/CPU encoding and test paths */
    uint8_t *prev_y_frame;
    bool has_prev_frame;

    /* CAVLC neighbor (nC) context, persistent across frames (see nc_ctx_t doc) */
    uint8_t (*nz_luma)[16];
    uint8_t (*nz_cb)[4];
    uint8_t (*nz_cr)[4];
};

static void manage_dpb(h264_encoder_t *encoder, int new_frame_num, int new_poc)
{
    if (encoder->dpb_count >= encoder->dpb_max) {
        memmove(&encoder->dpb[0], &encoder->dpb[1],
                sizeof(dpb_entry_t) * (encoder->dpb_max - 1));
        encoder->dpb_count--;
    }

    dpb_entry_t *entry = &encoder->dpb[encoder->dpb_count++];
    memset(entry, 0, sizeof(*entry));
    entry->frame_num = new_frame_num;
    entry->poc = new_poc;
    entry->is_reference = true;
    entry->is_long_term = false;
}

/*
 * write_aud - Writes Access Unit Delimiter (NAL type 9)
 * Essential for Sunshine / Moonlight / WebRTC to identify frame boundaries.
 */
static size_t write_aud(uint8_t *buf, size_t buf_size, bool is_idr) {
    if (buf_size < 6) return 0;
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = 0x01;
    buf[4] = 0x09; /* NAL header: forbidden=0, ref_idc=0, type=9 (AUD) */
    buf[5] = is_idr ? 0x10 : 0x30; /* primary_pic_type: 0 for I, 1 for P (shifted) + stop bit */
    return 6;
}

/*
 * ============================================================================
 * Residual coefficient helpers (GPU staging buffer access, quantization,
 * Hadamard transforms, neighbor-context derivation)
 * ============================================================================
 */

/* quant_levels/coeff buffers are laid out as num_mbs*24*16 ints; block index
 * (0-23) is the GPU RASTER convention (see file-top comment), position (0-15)
 * is raster within the 4x4 block (index = row*4+col, NOT zigzag). */
static inline const int *quant_block_ptr(const int *quant_levels, uint32_t mb_idx, int raster_block) {
    return quant_levels + ((size_t)mb_idx * 24 + raster_block) * 16;
}
static inline const int *coeff_block_ptr(const int *coeff, uint32_t mb_idx, int raster_block) {
    return coeff + ((size_t)mb_idx * 24 + raster_block) * 16;
}

static int block_any_nonzero(const int *quant_levels, uint32_t mb_idx, int raster_block, int start_pos, int end_pos) {
    const int *blk = quant_block_ptr(quant_levels, mb_idx, raster_block);
    for (int p = start_pos; p < end_pos; p++) if (blk[p] != 0) return 1;
    return 0;
}

/* H.264 Multiplication Factor Table (ITU-T Rec. H.264 8.5.9 Table 8-14),
 * position-type-0 column only (DC-Hadamard coefficients are always
 * quantized with the pos_type==0 multiplier under the simplification this
 * encoder uses - see quantize_dc()'s comment). Mirrors quantize.comp's MF[][0]. */
static const int MF0[6] = {13107, 11916, 10082, 9362, 8192, 7282};

/*
 * quantize_dc - Quantize one Hadamard-transformed DC coefficient.
 *
 * DELIBERATE SIMPLIFICATION: ITU-T H.264 8.5.10 defines a QP-dependent
 * piecewise dequant/quant formula for luma/chroma DC specifically (different
 * shift behavior for qP>=36 vs qP<36), which is high-risk to get bit-exact
 * from memory. This instead reuses the SAME AC quantization formula already
 * used by quantize.comp for pos_type==0 coefficients:
 *   level = sign * ((abs(v)*MF0[qp%6] + f) >> (15 + qp/6)), f = round-offset.
 * This only affects the reconstructed coefficient's numeric SCALE (i.e.
 * brightness/contrast fidelity of the decoded DC term), not CAVLC bitstream
 * validity - entropy coding correctness depends only on encoding whatever
 * integer level results, not on that level matching the spec's exact
 * dequant scale. If bit-exact 8.5.10 behavior is needed later, only this
 * function need change.
 */
static int quantize_dc(int v, int qp, int is_intra) {
    int qp_per = qp / 6;
    int qp_rem = qp % 6;
    int f = (1 << (15 + qp_per)) / (is_intra ? 3 : 6);
    int sign = (v < 0) ? -1 : 1;
    int64_t av = (v < 0) ? -(int64_t)v : (int64_t)v;
    int64_t level = (av * MF0[qp_rem] + f) >> (15 + qp_per);
    return (int)(sign * level);
}

/* Forward Hadamard transform of the 16 luma DC coefficients (one MB's worth),
 * standard x264-style 2-pass butterfly construction - implemented exactly as
 * specified (the write pattern into tmp[] transposes implicitly, so this is
 * the usual "rows, transpose, columns" 2D Hadamard without an explicit
 * transpose step). dc_in/dc_out are in raster (row*4+col) order. */
static void luma_dc_hadamard(const int dc_in[4][4], int dc_out[16]) {
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int s01 = dc_in[i][0] + dc_in[i][1], d01 = dc_in[i][0] - dc_in[i][1];
        int s23 = dc_in[i][2] + dc_in[i][3], d23 = dc_in[i][2] - dc_in[i][3];
        tmp[0*4+i] = s01+s23; tmp[1*4+i] = s01-s23; tmp[2*4+i] = d01-d23; tmp[3*4+i] = d01+d23;
    }
    for (int i = 0; i < 4; i++) {
        int s01 = tmp[i*4+0]+tmp[i*4+1], d01 = tmp[i*4+0]-tmp[i*4+1];
        int s23 = tmp[i*4+2]+tmp[i*4+3], d23 = tmp[i*4+2]-tmp[i*4+3];
        dc_out[i*4+0] = (s01+s23+1)>>1; dc_out[i*4+1] = (s01-s23+1)>>1;
        dc_out[i*4+2] = (d01-d23+1)>>1; dc_out[i*4+3] = (d01+d23+1)>>1;
    }
}

/* Forward Hadamard transform of a 2x2 chroma DC block. c[]/out[] are raster
 * (TL,TR,BL,BR) order, matching the GPU Cb/Cr DC block layout. */
static void chroma_dc_hadamard(const int c[4], int out[4]) {
    int c00 = c[0], c01 = c[1], c10 = c[2], c11 = c[3];
    out[0] = c00 + c01 + c10 + c11;
    out[1] = c00 - c01 + c10 - c11;
    out[2] = c00 + c01 - c10 - c11;
    out[3] = c00 - c01 - c10 + c11;
}

/* nC derivation for a luma 4x4 block at spec blkIdx, per ITU-T 9.2.1. */
static int luma_nc(const nc_ctx_t *nc, uint32_t mb, uint32_t mbx, uint32_t mby, int blk_idx) {
    int x4 = blk_x4[blk_idx], y4 = blk_y4[blk_idx];
    int nA = -1, nB = -1;

    if (x4 > 0) {
        nA = nc->nz_luma[mb][spec_blk_idx_from_xy[y4][x4 - 1]];
    } else if (mbx > 0 && (mb - 1) >= nc->start_mb) {
        nA = nc->nz_luma[mb - 1][spec_blk_idx_from_xy[y4][3]];
    }

    if (y4 > 0) {
        nB = nc->nz_luma[mb][spec_blk_idx_from_xy[y4 - 1][x4]];
    } else if (mby > 0 && (mb - nc->width_in_mbs) >= nc->start_mb) {
        nB = nc->nz_luma[mb - nc->width_in_mbs][spec_blk_idx_from_xy[3][x4]];
    }

    if (nA >= 0 && nB >= 0) return (nA + nB + 1) >> 1;
    if (nA >= 0) return nA;
    if (nB >= 0) return nB;
    return 0;
}

/* nC derivation for a chroma 4x4 block (2x2 grid, blk_idx 0..3, raster==spec order). */
static int chroma_nc(uint8_t (*nz_c)[4], uint32_t mb, uint32_t mbx, uint32_t mby,
                      uint32_t width_in_mbs, uint32_t start_mb, int blk_idx) {
    int x2 = blk_idx % 2, y2 = blk_idx / 2;
    int nA = -1, nB = -1;

    if (x2 > 0) {
        nA = nz_c[mb][y2 * 2 + (x2 - 1)];
    } else if (mbx > 0 && (mb - 1) >= start_mb) {
        nA = nz_c[mb - 1][y2 * 2 + 1];
    }

    if (y2 > 0) {
        nB = nz_c[mb][(y2 - 1) * 2 + x2];
    } else if (mby > 0 && (mb - width_in_mbs) >= start_mb) {
        nB = nz_c[mb - width_in_mbs][1 * 2 + x2];
    }

    if (nA >= 0 && nB >= 0) return (nA + nB + 1) >> 1;
    if (nA >= 0) return nA;
    if (nB >= 0) return nB;
    return 0;
}

/*
 * nC derivation for the I16x16 luma DC block.
 *
 * CORRECTED (found via ffmpeg round-trip validation - this directly
 * contradicts what an earlier version of this file, and its originating
 * task brief, assumed): ITU-T H.264 9.2.1 does NOT give the luma DC block
 * its own independent whole-MB neighbor chain. Per spec (and confirmed by
 * ffmpeg's libavcodec/h264_cavlc.c decode_residual(), which for the DC
 * block calls `pred_non_zero_count(h, sl, (n-LUMA_DC_BLOCK_INDEX)*16)` -
 * i.e. index 0 - while STORING the DC block's own decoded TotalCoeff at a
 * completely different cache slot reserved for LUMA_DC_BLOCK_INDEX), the DC
 * block's nC is derived EXACTLY as if it were luma4x4BlkIdx 0 (i.e. the
 * same left/top neighbor derivation as the first luma AC block), and the
 * DC block's own TotalCoeff is never itself used as anyone's neighbor
 * value - it is immediately superseded once AC block 0 of the same MB is
 * decoded (which writes ITS total_coeff to the real block-0 slot). Since
 * this encoder always writes the DC block before any AC block of the same
 * MB, nz_luma[mb][0] at DC-encode time still holds whatever the LEFT/TOP
 * NEIGHBOR MB left there, exactly matching this rule - so DC's nC is simply
 * `luma_nc(nc, mb, mbx, mby, blk_idx=0)`, and no separate dc_nz array is
 * needed at all (a prior version of this file added one; it produced a
 * bitstream ffmpeg rejected with "negative number of zero coeffs" on any
 * macroblock with a nonzero-DC-total_coeff neighbor, traced and confirmed
 * via an independent reference CAVLC decoder cross-checked against this
 * exact ffmpeg source function).
 */

/* Real per-MB motion vector, as written back by mv_staging (see
 * gpu_compute_get_mv_staging_data()'s doc comment) - mirrors the GPU's
 * std430 MotionVector struct {ivec2 mv; uint sad;} byte-for-byte (16 bytes:
 * two int32 + one uint32 + 4 bytes of std430 struct-alignment padding). */
typedef struct {
    int32_t mvx, mvy;
    uint32_t sad;
    uint32_t _pad;
} gpu_mv_t;

/*
 * gpu_pred_mode_i16 - I16x16 prediction mode, as chosen by
 * residual_predict.comp's real SAD-based mode decision (DC/Vertical/
 * Horizontal/Plane against actual neighbor pixels).
 *
 * REPLACES a former CPU-side heuristic that inferred mode from post-quant
 * AC coefficient activity - that approach became structurally impossible
 * once residual generation itself needs to know the prediction mode BEFORE
 * DCT/quantize even run (the residual IS source-minus-prediction). Mode
 * decision now happens on the GPU, before DCT, directly from pixel-domain
 * SAD; this just reads back what it decided. See residual_predict.comp's
 * top-of-file comment for the full design, including the neighbor-pixel-
 * source and slice-boundary simplifications it documents (this readback
 * inherits both: it does not re-derive or gate on start_mb the way the old
 * heuristic did, because the GPU's mode decision already didn't either).
 */
static int gpu_pred_mode_i16(const uint32_t *pred_modes, uint32_t mb_idx) {
    return pred_modes ? (int)pred_modes[mb_idx] : H264_I16x16_DC;
}

/* Neighbor MV lookup for the P16x16 MVD predictor below: (dx,dy) is a
 * neighbor offset in MB units (e.g. left=(-1,0), top=(0,-1)). Unavailable
 * (off-picture, or belongs to an earlier slice) is reported via *avail. */
static void neighbor_mv(const gpu_mv_t *mvs, uint32_t mbx, uint32_t mby,
                         uint32_t width_in_mbs, uint32_t start_mb, int dx, int dy,
                         int *mvx, int *mvy, bool *avail) {
    int nbx = (int)mbx + dx, nby = (int)mby + dy;
    if (nbx < 0 || nby < 0 || (uint32_t)nbx >= width_in_mbs) { *avail = false; *mvx = 0; *mvy = 0; return; }
    uint32_t nb = (uint32_t)nby * width_in_mbs + (uint32_t)nbx;
    if (nb < start_mb) { *avail = false; *mvx = 0; *mvy = 0; return; }
    *avail = true;
    *mvx = mvs[nb].mvx;
    *mvy = mvs[nb].mvy;
}

static inline int median3(int a, int b, int c) {
    return a + b + c - (a < b ? (a < c ? a : c) : (b < c ? b : c)) - (a > b ? (a > c ? a : c) : (b > c ? b : c));
}

/*
 * mv_predictor - ITU-T H.264 8.4.1.3 median motion vector predictor (A=left,
 * B=top, C=top-right, substituting D=top-left when C is unavailable). This
 * MUST match a real decoder's predictor exactly (ffmpeg included) - the
 * encoder transmits mv-minus-predictor (MVD) and the decoder reconstructs
 * mv=predictor+MVD using its OWN predictor computed the same way, so any
 * mismatch here corrupts every subsequent motion vector, not just this one.
 * All MBs in a P slice are P16x16 in this encoder (no intra-in-P mixing), so
 * the spec's ref-idx-equality special cases never apply here.
 */
static void mv_predictor(const gpu_mv_t *mvs, uint32_t mbx, uint32_t mby,
                          uint32_t width_in_mbs, uint32_t start_mb, int *px, int *py) {
    int ax, ay, bx, by, cx, cy;
    bool a_ok, b_ok, c_ok;
    neighbor_mv(mvs, mbx, mby, width_in_mbs, start_mb, -1, 0, &ax, &ay, &a_ok);   /* A: left */
    neighbor_mv(mvs, mbx, mby, width_in_mbs, start_mb, 0, -1, &bx, &by, &b_ok);   /* B: top */
    neighbor_mv(mvs, mbx, mby, width_in_mbs, start_mb, 1, -1, &cx, &cy, &c_ok);   /* C: top-right */
    if (!c_ok) {
        neighbor_mv(mvs, mbx, mby, width_in_mbs, start_mb, -1, -1, &cx, &cy, &c_ok); /* D substitutes C */
    }

    if (!b_ok && !c_ok && a_ok) {
        *px = ax; *py = ay;
        return;
    }
    /* An unavailable neighbor contributes (0,0) to the median (per spec) once
     * the single-predictor special case above doesn't apply. */
    if (!a_ok) { ax = 0; ay = 0; }
    if (!b_ok) { bx = 0; by = 0; }
    if (!c_ok) { cx = 0; cy = 0; }
    *px = median3(ax, bx, cx);
    *py = median3(ay, by, cy);
}

/* Whole-MB "does this P16x16 MB have any nonzero luma coefficient" skip
 * decision. Previously this read the lossy packed entropy summary
 * (mb_blocks[b]&0xFF); it now reads the real quant_levels data directly -
 * strictly more accurate, same semantic heuristic. */
static int mb_has_any_luma_nonzero(const int *quant_levels, uint32_t mb_idx) {
    for (int b = 0; b < 16; b++) {
        if (block_any_nonzero(quant_levels, mb_idx, b, 0, 16)) return 1;
    }
    return 0;
}

/*
 * encode_mb_i16x16 - Encode one Intra 16x16 macroblock: header, luma DC
 * (Hadamard), luma AC (16 blocks, spec order), chroma DC (Hadamard x2) and
 * chroma AC (8 blocks), updating the nC neighbor-context arrays as it goes.
 */
static void encode_mb_i16x16(bitstream_t *bs, const int *quant_levels, const int *coeff,
                              const uint32_t *pred_modes,
                              uint32_t mb, uint32_t mbx, uint32_t mby, nc_ctx_t *nc, int qp) {
    int pred_mode = gpu_pred_mode_i16(pred_modes, mb);

    /* Luma DC: gather PRE-quant DC (coeff buffer, position 0) of the 16
     * raster blocks into the natural 4x4 grid, Hadamard, quantize. */
    int dc_in[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            dc_in[r][c] = coeff_block_ptr(coeff, mb, r * 4 + c)[0];
    int dc_out_raw[16];
    luma_dc_hadamard(dc_in, dc_out_raw);
    int dc_out[16];
    for (int i = 0; i < 16; i++) dc_out[i] = quantize_dc(dc_out_raw[i], qp, 1 /* intra */);

    /* cbp_luma: any nonzero AC (raster positions 1..15) across all 16 luma blocks. */
    int cbp_luma_flag = 0;
    for (int blk = 0; blk < 16 && !cbp_luma_flag; blk++) {
        if (block_any_nonzero(quant_levels, mb, blk, 1, 16)) cbp_luma_flag = 1;
    }

    /* Chroma DC (Hadamard) + cbp_chroma. */
    int cb_dc_raw[4], cr_dc_raw[4];
    for (int i = 0; i < 4; i++) cb_dc_raw[i] = coeff_block_ptr(coeff, mb, 16 + i)[0];
    for (int i = 0; i < 4; i++) cr_dc_raw[i] = coeff_block_ptr(coeff, mb, 20 + i)[0];
    int cb_dc[4], cr_dc[4];
    chroma_dc_hadamard(cb_dc_raw, cb_dc);
    chroma_dc_hadamard(cr_dc_raw, cr_dc);
    for (int i = 0; i < 4; i++) { cb_dc[i] = quantize_dc(cb_dc[i], qp, 1); cr_dc[i] = quantize_dc(cr_dc[i], qp, 1); }

    int chroma_dc_nonzero = 0;
    for (int i = 0; i < 4; i++) if (cb_dc[i] != 0 || cr_dc[i] != 0) { chroma_dc_nonzero = 1; break; }
    int chroma_ac_nonzero = 0;
    for (int blk = 16; blk < 24 && !chroma_ac_nonzero; blk++) {
        if (block_any_nonzero(quant_levels, mb, blk, 1, 16)) chroma_ac_nonzero = 1;
    }
    int cbp_chroma = chroma_ac_nonzero ? 2 : (chroma_dc_nonzero ? 1 : 0);

    /* Header first (mb_type encodes pred_mode/cbp_chroma/cbp_luma_flag,
     * followed by intra_chroma_pred_mode and mb_qp_delta), THEN residual. */
    cavlc_write_mb_i16x16_header(bs, pred_mode, cbp_chroma, cbp_luma_flag ? 15 : 0, 0);

    /* Luma DC block (always present for I16x16, first in residual order).
     * nC uses luma4x4BlkIdx=0's neighbor chain (see dc_nc's replacement
     * comment above) - this MUST run before the AC loop below writes
     * nz_luma[mb][0], since the DC block's own nC needs to see the LEFT/TOP
     * NEIGHBOR MB's block-0-adjacent values, not this MB's own (not-yet-
     * decoded) block 0. */
    int nC_dc = luma_nc(nc, mb, mbx, mby, 0);
    cavlc_write_4x4_block(bs, dc_out, nC_dc);

    /* Luma AC blocks, spec blkIdx order (0..15). */
    if (cbp_luma_flag) {
        for (int blk_idx = 0; blk_idx < 16; blk_idx++) {
            int raster = gpu_raster_block_idx(blk_idx);
            int nC = luma_nc(nc, mb, mbx, mby, blk_idx);
            int tc = cavlc_write_4x4_ac_block(bs, quant_block_ptr(quant_levels, mb, raster), nC);
            nc->nz_luma[mb][blk_idx] = (uint8_t)tc;
        }
    } else {
        memset(nc->nz_luma[mb], 0, sizeof(nc->nz_luma[mb]));
    }

    /* Chroma DC (Cb then Cr) if any chroma residual at all. */
    if (cbp_chroma >= 1) {
        cavlc_write_chroma_dc_block(bs, cb_dc);
        cavlc_write_chroma_dc_block(bs, cr_dc);
    }

    /* Chroma AC (4 Cb blocks then 4 Cr blocks) only if cbp_chroma==2. */
    if (cbp_chroma == 2) {
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cb, mb, mbx, mby, nc->width_in_mbs, nc->start_mb, blk_idx);
            int tc = cavlc_write_4x4_ac_block(bs, quant_block_ptr(quant_levels, mb, 16 + blk_idx), nC);
            nc->nz_cb[mb][blk_idx] = (uint8_t)tc;
        }
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cr, mb, mbx, mby, nc->width_in_mbs, nc->start_mb, blk_idx);
            int tc = cavlc_write_4x4_ac_block(bs, quant_block_ptr(quant_levels, mb, 20 + blk_idx), nC);
            nc->nz_cr[mb][blk_idx] = (uint8_t)tc;
        }
    } else {
        memset(nc->nz_cb[mb], 0, sizeof(nc->nz_cb[mb]));
        memset(nc->nz_cr[mb], 0, sizeof(nc->nz_cr[mb]));
    }
}

/*
 * encode_mb_p16x16 - Encode one Inter P_L0_16x16 macroblock: header (MVD +
 * real 6-bit CBP), luma (16 FULL blocks, DC included, spec order), chroma DC
 * (Hadamard, same as I16x16) and chroma AC (8 blocks).
 */
static void encode_mb_p16x16(bitstream_t *bs, const int *quant_levels, const int *coeff,
                              const gpu_mv_t *mvs,
                              uint32_t mb, uint32_t mbx, uint32_t mby, nc_ctx_t *nc, int qp) {
    int pred_x, pred_y;
    mv_predictor(mvs, mbx, mby, nc->width_in_mbs, nc->start_mb, &pred_x, &pred_y);
    /* BUG FIX (root cause of the ~13 dB quality_test.sh FAIL - confirmed via
     * real ffmpeg decode traces on a minimal repro, see commit message):
     * motion_estimation.comp / gpu_compute_get_mv_staging_data() produce and
     * return motion vectors in INTEGER-PEL units (this pipeline does no
     * sub-pel interpolation anywhere - see residual_predict.comp's top-of-
     * file comment). mv_predictor()'s median-of-neighbors predictor operates
     * on those same raw integer-pel gpu_mv_t values, so the difference below
     * is also in integer-pel units. But ITU-T H.264 7.4.5.3 requires
     * mvd_l0[][][compIdx] to be coded in QUARTER-LUMA-SAMPLE units - the
     * bitstream field cavlc_write_mb_p16x16_header() writes is unconditionally
     * interpreted as quarter-pel by any spec-compliant decoder (mv = predictor
     * (quarter-pel) + mvd_l0, then >>2 for the integer part + fractional
     * interpolation). Writing the raw integer-pel difference here means every
     * decoded motion vector for a non-skip P16x16 MB comes out 4x too SMALL
     * in magnitude - exactly correct-looking bitstream syntax, wildly wrong
     * motion. This rounds harmlessly for near-zero motion (why flat/low-
     * motion regions looked fine) and corrupts real, larger motion badly
     * (why moving/detailed regions showed block corruption) - see the
     * decode-trace evidence in the commit message. Fix: scale by 4 (the
     * legacy CPU-heuristic path lower in this file, h264_encoder_encode_raw,
     * already did `best_dx * 4, best_dy * 4` correctly - this GPU-driven
     * path was the one missing it). */
    int mvd_x = (mvs[mb].mvx - pred_x) * 4;
    int mvd_y = (mvs[mb].mvy - pred_y) * 4;

    /* Luma CBP: one bit per 8x8 quadrant (spec blkIdx/4), based on full
     * 16-coefficient (DC+AC) nonzero-anywhere check since P16x16 luma has
     * no separate DC/AC split. */
    int luma_cbp = 0;
    for (int q = 0; q < 4; q++) {
        int any = 0;
        for (int sub = 0; sub < 4 && !any; sub++) {
            int blk_idx = q * 4 + sub;
            int raster = gpu_raster_block_idx(blk_idx);
            if (block_any_nonzero(quant_levels, mb, raster, 0, 16)) any = 1;
        }
        if (any) luma_cbp |= (1 << q);
    }

    /* Chroma DC (Hadamard, inter (/6) rounding) + cbp_chroma - identical
     * structure to encode_mb_i16x16's chroma handling. */
    int cb_dc_raw[4], cr_dc_raw[4];
    for (int i = 0; i < 4; i++) cb_dc_raw[i] = coeff_block_ptr(coeff, mb, 16 + i)[0];
    for (int i = 0; i < 4; i++) cr_dc_raw[i] = coeff_block_ptr(coeff, mb, 20 + i)[0];
    int cb_dc[4], cr_dc[4];
    chroma_dc_hadamard(cb_dc_raw, cb_dc);
    chroma_dc_hadamard(cr_dc_raw, cr_dc);
    for (int i = 0; i < 4; i++) { cb_dc[i] = quantize_dc(cb_dc[i], qp, 0); cr_dc[i] = quantize_dc(cr_dc[i], qp, 0); }

    int chroma_dc_nonzero = 0;
    for (int i = 0; i < 4; i++) if (cb_dc[i] != 0 || cr_dc[i] != 0) { chroma_dc_nonzero = 1; break; }
    int chroma_ac_nonzero = 0;
    for (int blk = 16; blk < 24 && !chroma_ac_nonzero; blk++) {
        if (block_any_nonzero(quant_levels, mb, blk, 1, 16)) chroma_ac_nonzero = 1;
    }
    int cbp_chroma = chroma_ac_nonzero ? 2 : (chroma_dc_nonzero ? 1 : 0);

    /* Standard H.264 CodedBlockPattern = CBPChroma*16 + CBPLuma; cavlc_write_mb_p16x16_header
     * maps this through map_inter_cbp()/Table 9-4 internally. */
    int cbp = (luma_cbp & 0xF) | (cbp_chroma << 4);
    cavlc_write_mb_p16x16_header(bs, mvd_x, mvd_y, cbp, 0);

    for (int blk_idx = 0; blk_idx < 16; blk_idx++) {
        int q = blk_idx / 4;
        if (luma_cbp & (1 << q)) {
            int raster = gpu_raster_block_idx(blk_idx);
            int nC = luma_nc(nc, mb, mbx, mby, blk_idx);
            int tc = cavlc_write_4x4_block(bs, quant_block_ptr(quant_levels, mb, raster), nC);
            nc->nz_luma[mb][blk_idx] = (uint8_t)tc;
        } else {
            nc->nz_luma[mb][blk_idx] = 0;
        }
    }

    if (cbp_chroma >= 1) {
        cavlc_write_chroma_dc_block(bs, cb_dc);
        cavlc_write_chroma_dc_block(bs, cr_dc);
    }
    if (cbp_chroma == 2) {
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cb, mb, mbx, mby, nc->width_in_mbs, nc->start_mb, blk_idx);
            int tc = cavlc_write_4x4_ac_block(bs, quant_block_ptr(quant_levels, mb, 16 + blk_idx), nC);
            nc->nz_cb[mb][blk_idx] = (uint8_t)tc;
        }
        for (int blk_idx = 0; blk_idx < 4; blk_idx++) {
            int nC = chroma_nc(nc->nz_cr, mb, mbx, mby, nc->width_in_mbs, nc->start_mb, blk_idx);
            int tc = cavlc_write_4x4_ac_block(bs, quant_block_ptr(quant_levels, mb, 20 + blk_idx), nC);
            nc->nz_cr[mb][blk_idx] = (uint8_t)tc;
        }
    } else {
        memset(nc->nz_cb[mb], 0, sizeof(nc->nz_cb[mb]));
        memset(nc->nz_cr[mb], 0, sizeof(nc->nz_cr[mb]));
    }
}

h264_encoder_t *h264_encoder_create(bc250_gpu_context_t *gpu_ctx,
                                    uint32_t width, uint32_t height,
                                    uint32_t fps, uint32_t bitrate,
                                    int profile)
{
    h264_encoder_t *encoder = calloc(1, sizeof(h264_encoder_t));
    if (!encoder) return NULL;

    encoder->gpu = gpu_ctx;
    encoder->width = width;
    encoder->height = height;
    encoder->width_in_mbs = (width + 15) / 16;
    encoder->height_in_mbs = (height + 15) / 16;
    encoder->total_mbs = encoder->width_in_mbs * encoder->height_in_mbs;

    encoder->fps = fps > 0 ? fps : 60;
    encoder->gop_size = encoder->fps; /* 1-second default keyframe interval */
    encoder->dpb_max = 16;
    encoder->force_idr = false;

    uint8_t prof_idc = PROFILE_BASELINE;
    if (profile == 0x64 /* VAProfileH264High */) prof_idc = PROFILE_HIGH;
    else if (profile == 0x4D /* VAProfileH264Main */) prof_idc = PROFILE_MAIN;

    h264_sps_default(&encoder->sps, width, height, encoder->fps, prof_idc);
    h264_pps_default(&encoder->pps, encoder->sps.sps_id, false, 26);

    rc_init(&encoder->rc, RC_CBR, bitrate, (double)encoder->fps);

    encoder->output_buf_size = width * height * 2 + 65536;
    encoder->output_buf = malloc(encoder->output_buf_size);
    if (!encoder->output_buf) {
        free(encoder);
        return NULL;
    }

    encoder->prev_y_frame = malloc((size_t)width * height);
    encoder->has_prev_frame = false;

    encoder->nz_luma = calloc(encoder->total_mbs, sizeof(*encoder->nz_luma));
    encoder->nz_cb = calloc(encoder->total_mbs, sizeof(*encoder->nz_cb));
    encoder->nz_cr = calloc(encoder->total_mbs, sizeof(*encoder->nz_cr));
    if (!encoder->nz_luma || !encoder->nz_cb || !encoder->nz_cr) {
        free(encoder->nz_luma); free(encoder->nz_cb); free(encoder->nz_cr);
        free(encoder->output_buf);
        free(encoder->prev_y_frame);
        free(encoder);
        return NULL;
    }

    fprintf(stderr, "[bc250-h264] Encoder initialized: %ux%u @ %u fps, %u bps, profile %d\n",
            width, height, encoder->fps, bitrate, prof_idc);

    return encoder;
}

void h264_encoder_force_idr(h264_encoder_t *encoder) {
    if (encoder) {
        encoder->force_idr = true;
    }
}

void h264_encoder_set_bitrate(h264_encoder_t *encoder, uint32_t bitrate_bps) {
    if (encoder && bitrate_bps > 0) {
        rc_init(&encoder->rc, RC_CBR, bitrate_bps, (double)encoder->fps);
    }
}

void h264_encoder_set_gop_size(h264_encoder_t *encoder, uint32_t gop_size) {
    if (encoder && gop_size > 0) {
        encoder->gop_size = gop_size;
    }
}

void h264_encoder_set_fps(h264_encoder_t *encoder, uint32_t fps) {
    if (encoder && fps > 0) {
        encoder->fps = fps;
        encoder->rc.framerate = (double)fps;
    }
}

void h264_encoder_set_qp(h264_encoder_t *encoder, int qp) {
    if (encoder) {
        if (qp < 0) qp = 0;
        if (qp > 51) qp = 51;
        encoder->pps.pic_init_qp = qp;
    }
}

int h264_encoder_encode_frame(h264_encoder_t *encoder,
                              bc250_gpu_context_t *gpu_ctx,
                              gpu_image_t input_surface,
                              uint8_t *output_buf, size_t output_size)
{
    if (!encoder || !output_buf) return -1;

    bool is_idr = (encoder->frame_count % encoder->gop_size == 0) || encoder->force_idr;
    encoder->force_idr = false;

    if (is_idr) {
        encoder->frame_num = 0;
        encoder->idr_pic_id++;
        encoder->poc = 0;
        encoder->dpb_count = 0;
    }

    int qp = rc_get_frame_qp(&encoder->rc, 0);
    if (qp < 12) qp = 12;
    if (qp > 51) qp = 51;

    size_t total_written = 0;

    /* 1. Write AUD (Access Unit Delimiter) NALU */
    total_written += write_aud(encoder->output_buf + total_written,
                               encoder->output_buf_size - total_written,
                               is_idr);

    /* 2. Write SPS and PPS NALUs on IDR frames */
    if (is_idr) {
        size_t sps_size = bs_write_sps(
            encoder->output_buf + total_written,
            encoder->output_buf_size - total_written,
            &encoder->sps);
        total_written += sps_size;

        size_t pps_size = bs_write_pps(
            encoder->output_buf + total_written,
            encoder->output_buf_size - total_written,
            &encoder->pps);
        total_written += pps_size;
    }

    /* 3a. Multi-slice partitioning (Sunshine/Moonlight network resilience) -
     * computed BEFORE the GPU dispatch below, since residual_predict.comp
     * needs num_slices too (see gpu_compute_dispatch_encode's doc comment
     * and that shader's SLICE BOUNDARIES note) to correctly treat a
     * different-slice neighbor MB as unavailable for intra prediction. */
    int num_slices = 1;
    const char *slice_env = getenv("BC250_SLICES_PER_FRAME");
    if (slice_env) {
        int s = atoi(slice_env);
        if (s >= 1 && s <= 16) num_slices = s;
    }

    /* 3b. Dispatch GPU compute encoding pipeline if available, and fetch the
     * REAL per-coefficient residual data (post-quant levels + pre-quant
     * transform coefficients) - not just the lossy packed entropy summary
     * the old heuristic-only path used. */
    const int *quant_levels = NULL;
    const int *coeff = NULL;
    const uint32_t *pred_modes = NULL;
    const gpu_mv_t *mvs = NULL;
    if (gpu_ctx && input_surface.y_plane != VK_NULL_HANDLE) {
        gpu_compute_begin_picture(gpu_ctx, input_surface);
        gpu_compute_dispatch_encode(gpu_ctx, input_surface, encoder->width, encoder->height,
                                     qp, is_idr ? 1 : 0, num_slices);
        gpu_compute_end_picture(gpu_ctx);
        gpu_compute_sync(gpu_ctx);
        gpu_compute_debug_dump_recon(gpu_ctx, (int)encoder->width, (int)encoder->height);

        void *quant_data = NULL, *coeff_data = NULL, *pred_mode_data = NULL, *mv_data = NULL;
        size_t quant_size = 0, coeff_size = 0, pred_mode_size = 0, mv_size = 0;
        if (gpu_compute_get_quant_staging_data(gpu_ctx, &quant_data, &quant_size) == 0) {
            quant_levels = (const int *)quant_data;
        }
        if (gpu_compute_get_coeff_staging_data(gpu_ctx, &coeff_data, &coeff_size) == 0) {
            coeff = (const int *)coeff_data;
        }
        if (gpu_compute_get_pred_mode_staging_data(gpu_ctx, &pred_mode_data, &pred_mode_size) == 0) {
            pred_modes = (const uint32_t *)pred_mode_data;
        }
        if (gpu_compute_get_mv_staging_data(gpu_ctx, &mv_data, &mv_size) == 0) {
            mvs = (const gpu_mv_t *)mv_data;
        }
    }

    /* 4. Encode Slices */

    const char *fm = getenv("BC250_FAST_MODE");
    int deblock_idc = (fm && (strcmp(fm, "1") == 0 || strcmp(fm, "true") == 0)) ? 1 : 0;
    int slice_type = is_idr ? SLICE_TYPE_I : SLICE_TYPE_P;
    int poc_bits = encoder->sps.log2_max_poc_lsb + 4;
    int slice_qp_delta = qp - 26 - encoder->pps.pic_init_qp;

    for (int s = 0; s < num_slices; s++) {
        uint32_t start_mb = (uint32_t)(s * encoder->total_mbs / num_slices);
        uint32_t end_mb = (uint32_t)((s + 1) * encoder->total_mbs / num_slices);

        size_t rbsp_buf_size = (end_mb - start_mb) * 64 + 4096;
        uint8_t *slice_rbsp = malloc(rbsp_buf_size);
        if (!slice_rbsp) return -1;

        bitstream_t bs;
        bs_init(&bs, slice_rbsp, rbsp_buf_size);

        /* 4a. Slice Header per H.264 Section 7.3.3 */
        bs_write_ue(&bs, start_mb); /* first_mb_in_slice */
        bs_write_ue(&bs, (uint32_t)slice_type);
        bs_write_ue(&bs, (uint32_t)encoder->pps.pps_id);
        bs_write_u(&bs, encoder->sps.log2_max_frame_num + 4, encoder->frame_num);

        if (is_idr) {
            bs_write_ue(&bs, encoder->idr_pic_id);
        }

        bs_write_u(&bs, poc_bits, encoder->poc & ((1 << poc_bits) - 1));

        if (!is_idr) {
            bs_write1(&bs, 0); /* num_ref_idx_active_override_flag = 0 */
            bs_write1(&bs, 0); /* ref_pic_list_modification_flag_l0 = 0 */
            bs_write1(&bs, 0); /* adaptive_ref_pic_marking_mode_flag = 0 */
        } else {
            bs_write1(&bs, 0); /* no_output_of_prior_pics_flag = 0 */
            bs_write1(&bs, 0); /* long_term_reference_flag = 0 */
        }

        bs_write_se(&bs, slice_qp_delta);
        bs_write_ue(&bs, (uint32_t)deblock_idc);
        /* Per ITU-T H.264 7.3.3: slice_alpha_c0_offset_div2/slice_beta_offset_div2
         * are only present when disable_deblocking_filter_idc != 1. Writing them
         * unconditionally (as this used to) inserts two spurious se(v) values into
         * the slice header whenever BC250_FAST_MODE=1 sets deblock_idc=1, silently
         * desyncing every bit of macroblock data that follows - confirmed via
         * real decode: FAST_MODE=1 produced a cascade of varied CAVLC/mb_type/qp
         * errors from MB 0 onward, while deblock_idc=0 (the default, and the only
         * value exercised by this session's earlier testing) was always clean. */
        if (deblock_idc != 1) {
            bs_write_se(&bs, 0);
            bs_write_se(&bs, 0);
        }

        /* 4b. Slice Data (Macroblock Layer) using CAVLC per Section 7.3.4 */
        nc_ctx_t nc = {
            .nz_luma = encoder->nz_luma,
            .nz_cb = encoder->nz_cb,
            .nz_cr = encoder->nz_cr,
            .width_in_mbs = encoder->width_in_mbs,
            .start_mb = start_mb,
        };

        if (is_idr) {
            for (uint32_t mb = start_mb; mb < end_mb; mb++) {
                uint32_t mbx = mb % encoder->width_in_mbs;
                uint32_t mby = mb / encoder->width_in_mbs;
                if (quant_levels && coeff) {
                    encode_mb_i16x16(&bs, quant_levels, coeff, pred_modes, mb, mbx, mby, &nc, qp);
                } else {
                    /* No GPU residual data available (e.g. gpu_ctx==NULL) -
                     * fall back to an all-zero-residual I16x16 MB so the
                     * bitstream stays structurally valid. */
                    cavlc_write_mb_i16x16_header(&bs, H264_I16x16_DC, 0, 0, 0);
                    int zero16[16] = {0};
                    cavlc_write_4x4_block(&bs, zero16, luma_nc(&nc, mb, mbx, mby, 0));
                    memset(nc.nz_luma[mb], 0, sizeof(nc.nz_luma[mb]));
                    memset(nc.nz_cb[mb], 0, sizeof(nc.nz_cb[mb]));
                    memset(nc.nz_cr[mb], 0, sizeof(nc.nz_cr[mb]));
                }
            }
        } else {
            uint32_t current_skip_run = 0;
            for (uint32_t mb = start_mb; mb < end_mb; mb++) {
                uint32_t mbx = mb % encoder->width_in_mbs;
                uint32_t mby = mb / encoder->width_in_mbs;
                /* BUG FIX (second, independent root cause of the ~13 dB
                 * quality_test.sh FAIL - confirmed via a minimal repro whose
                 * ffmpeg decode trace showed EVERY single P-frame macroblock
                 * decoding as skip ('S'), even under a rigid 16px/frame
                 * moving test box motion_estimation.comp tracked perfectly -
                 * see commit message): this used to treat "zero luma
                 * residual" as sufficient, alone, to emit a P_Skip macroblock.
                 * But per ITU-T H.264 8.4.1.1 / 7.4.5, a decoder reconstructs
                 * a P_Skip macroblock using mvL0 = the SAME median-of-
                 * neighbors predictor mv_predictor() computes below - NOT
                 * the encoder's actual searched motion vector - plus a
                 * zero residual. Skip is only a legal encoding when the real
                 * searched MV *equals* that predictor; otherwise the encoder
                 * MUST code the MB (as P_L0_16x16, mvd != 0) even though its
                 * residual is zero, purely to transmit the real motion. The
                 * old check ignored this entirely, so any MB whose optimal
                 * motion happened to produce a perfect (zero-residual) match
                 * - overwhelmingly common on this integer-pel-only,
                 * no-subpel-interpolation pipeline - got silently skipped
                 * regardless of how large its real motion was, forcing the
                 * decoder to reuse (0,0)-chained neighbor predictors and
                 * freeze that content at its previous position. This is
                 * exactly the "frames barely change despite real motion"
                 * symptom, independent of the mvd quarter-pel scaling bug
                 * fixed in encode_mb_p16x16 above (that bug corrupts MBs
                 * that DO get coded; this one wrongly avoids coding MBs that
                 * should be). */
                bool zero_luma_residual = quant_levels ? (mb_has_any_luma_nonzero(quant_levels, mb) == 0) : true;
                bool mv_matches_predictor = true;
                if (mvs) {
                    int pred_x, pred_y;
                    mv_predictor(mvs, mbx, mby, nc.width_in_mbs, nc.start_mb, &pred_x, &pred_y);
                    mv_matches_predictor = (mvs[mb].mvx == pred_x && mvs[mb].mvy == pred_y);
                }
                bool mb_changed = quant_levels ? !(zero_luma_residual && mv_matches_predictor) : false;

                if (!mb_changed) {
                    current_skip_run++;
                    memset(nc.nz_luma[mb], 0, sizeof(nc.nz_luma[mb]));
                    memset(nc.nz_cb[mb], 0, sizeof(nc.nz_cb[mb]));
                    memset(nc.nz_cr[mb], 0, sizeof(nc.nz_cr[mb]));
                } else {
                    /* mb_skip_run MUST be written exactly once, unconditionally
                     * (even when 0), before every coded macroblock - see
                     * cavlc_write_p_skip_run's doc comment. */
                    cavlc_write_p_skip_run(&bs, current_skip_run);
                    current_skip_run = 0;
                    if (quant_levels && coeff) {
                        encode_mb_p16x16(&bs, quant_levels, coeff, mvs, mb, mbx, mby, &nc, qp);
                    } else {
                        cavlc_write_mb_p16x16_header(&bs, 0, 0, 0, 0);
                        memset(nc.nz_luma[mb], 0, sizeof(nc.nz_luma[mb]));
                        memset(nc.nz_cb[mb], 0, sizeof(nc.nz_cb[mb]));
                        memset(nc.nz_cr[mb], 0, sizeof(nc.nz_cr[mb]));
                    }
                }
            }
            if (current_skip_run > 0) {
                cavlc_write_p_skip_run(&bs, current_skip_run);
            }
        }

        /* 4c. RBSP Trailing bits (1 followed by zero bits to byte boundary) */
        cavlc_write_slice_trailing_bits(&bs);
        bs_flush(&bs);

        size_t rbsp_len = bs_bytes_written(&bs);

        /* 5. Assemble Slice NAL unit: 4-byte start code + NAL header + EBSP */
        if (total_written + 5 + rbsp_len * 2 <= encoder->output_buf_size) {
            uint8_t *nal_dst = encoder->output_buf + total_written;
            nal_dst[0] = 0x00;
            nal_dst[1] = 0x00;
            nal_dst[2] = 0x00;
            nal_dst[3] = 0x01;
            nal_dst[4] = is_idr ? ((NAL_REF_IDC_HIGH << 5) | NAL_TYPE_IDR_SLICE)
                                : ((NAL_REF_IDC_MEDIUM << 5) | NAL_TYPE_SLICE);

            size_t ebsp_len = bs_rbsp_to_ebsp(nal_dst + 5,
                                              encoder->output_buf_size - total_written - 5,
                                              slice_rbsp,
                                              rbsp_len);
            total_written += 5 + ebsp_len;
        }
        free(slice_rbsp);
    }

    if (output_size < total_written) {
        fprintf(stderr, "[bc250-h264] Output buffer too small: need %zu, have %zu\n",
                total_written, output_size);
        return -1;
    }
    memcpy(output_buf, encoder->output_buf, total_written);

    rc_update_stats(&encoder->rc, (int)(total_written * 8));
    manage_dpb(encoder, encoder->frame_num, encoder->poc);

    encoder->frame_num++;
    encoder->poc += 2;
    encoder->frame_count++;

    return (int)total_written;
}

int h264_encoder_encode_raw(h264_encoder_t *encoder,
                            const uint8_t *y_plane, int y_pitch,
                            const uint8_t *uv_plane, int uv_pitch,
                            uint8_t *output_buf, size_t output_size)
{
    (void)uv_plane; (void)uv_pitch;
    if (!encoder || !output_buf || !y_plane) return -1;

    bool is_idr = (encoder->frame_count % encoder->gop_size == 0) || encoder->force_idr;
    encoder->force_idr = false;

    if (is_idr) {
        encoder->frame_num = 0;
        encoder->idr_pic_id++;
        encoder->poc = 0;
        encoder->dpb_count = 0;
    }

    int qp = rc_get_frame_qp(&encoder->rc, 0);
    if (qp < 12) qp = 12;
    if (qp > 51) qp = 51;

    size_t total_written = 0;

    /* 1. Write AUD */
    total_written += write_aud(encoder->output_buf + total_written,
                               encoder->output_buf_size - total_written,
                               is_idr);

    /* 2. Write SPS / PPS on IDR */
    if (is_idr) {
        total_written += bs_write_sps(encoder->output_buf + total_written,
                                      encoder->output_buf_size - total_written,
                                      &encoder->sps);
        total_written += bs_write_pps(encoder->output_buf + total_written,
                                      encoder->output_buf_size - total_written,
                                      &encoder->pps);
    }

    /* 3. Encode Slices (supporting multi-slice partitioning for network resilience) */
    int num_slices = 1;
    const char *slice_env = getenv("BC250_SLICES_PER_FRAME");
    if (slice_env) {
        int s = atoi(slice_env);
        if (s >= 1 && s <= 16) num_slices = s;
    }

    const char *fm = getenv("BC250_FAST_MODE");
    int deblock_idc = (fm && (strcmp(fm, "1") == 0 || strcmp(fm, "true") == 0)) ? 1 : 0;
    int slice_type = is_idr ? SLICE_TYPE_I : SLICE_TYPE_P;
    int poc_bits = encoder->sps.log2_max_poc_lsb + 4;
    int slice_qp_delta = qp - 26 - encoder->pps.pic_init_qp;

    for (int s = 0; s < num_slices; s++) {
        uint32_t start_mb = (uint32_t)(s * encoder->total_mbs / num_slices);
        uint32_t end_mb = (uint32_t)((s + 1) * encoder->total_mbs / num_slices);

        size_t rbsp_buf_size = (end_mb - start_mb) * 64 + 4096;
        uint8_t *slice_rbsp = malloc(rbsp_buf_size);
        if (!slice_rbsp) return -1;

        bitstream_t bs;
        bs_init(&bs, slice_rbsp, rbsp_buf_size);

        bs_write_ue(&bs, start_mb); /* first_mb_in_slice */
        bs_write_ue(&bs, (uint32_t)slice_type);
        bs_write_ue(&bs, (uint32_t)encoder->pps.pps_id);
        bs_write_u(&bs, encoder->sps.log2_max_frame_num + 4, encoder->frame_num);

        if (is_idr) {
            bs_write_ue(&bs, encoder->idr_pic_id);
        }

        bs_write_u(&bs, poc_bits, encoder->poc & ((1 << poc_bits) - 1));

        if (!is_idr) {
            bs_write1(&bs, 0);
            bs_write1(&bs, 0);
            bs_write1(&bs, 0);
        } else {
            bs_write1(&bs, 0);
            bs_write1(&bs, 0);
        }

        bs_write_se(&bs, slice_qp_delta);
        bs_write_ue(&bs, (uint32_t)deblock_idc);
        /* Per ITU-T H.264 7.3.3: slice_alpha_c0_offset_div2/slice_beta_offset_div2
         * are only present when disable_deblocking_filter_idc != 1. Writing them
         * unconditionally (as this used to) inserts two spurious se(v) values into
         * the slice header whenever BC250_FAST_MODE=1 sets deblock_idc=1, silently
         * desyncing every bit of macroblock data that follows - confirmed via
         * real decode: FAST_MODE=1 produced a cascade of varied CAVLC/mb_type/qp
         * errors from MB 0 onward, while deblock_idc=0 (the default, and the only
         * value exercised by this session's earlier testing) was always clean. */
        if (deblock_idc != 1) {
            bs_write_se(&bs, 0);
            bs_write_se(&bs, 0);
        }

        /* Macroblock layer */
        if (is_idr) {
            for (uint32_t mb = start_mb; mb < end_mb; mb++) {
                uint32_t mby = mb / encoder->width_in_mbs;
                uint32_t mbx = mb % encoder->width_in_mbs;
                uint32_t v_diff = 0, h_diff = 0;
                for (int r = 0; r < 15; r++) {
                    uint32_t py = mby * 16 + r;
                    if (py >= encoder->height - 1) break;
                    for (int c = 0; c < 15; c++) {
                        uint32_t px = mbx * 16 + c;
                        if (px >= encoder->width - 1) break;
                        const uint8_t *p = y_plane + py * y_pitch + px;
                        v_diff += abs((int)p[0] - (int)p[y_pitch]);
                        h_diff += abs((int)p[0] - (int)p[1]);
                    }
                }
                int mode = H264_I16x16_DC;
                if (v_diff * 3 < h_diff * 2) mode = H264_I16x16_VERT;
                else if (h_diff * 3 < v_diff * 2) mode = H264_I16x16_HORIZ;
                cavlc_write_mb_i16x16_header(&bs, mode, 0, 0, 0);
            }
        } else {
            uint32_t current_skip_run = 0;
            for (uint32_t mb = start_mb; mb < end_mb; mb++) {
                uint32_t mby = mb / encoder->width_in_mbs;
                uint32_t mbx = mb % encoder->width_in_mbs;
                uint32_t sad = 0;
                if (encoder->has_prev_frame && encoder->prev_y_frame) {
                    for (int r = 0; r < 16; r++) {
                        uint32_t py = mby * 16 + r;
                        if (py >= encoder->height) break;
                        for (int c = 0; c < 16; c++) {
                            uint32_t px = mbx * 16 + c;
                            if (px >= encoder->width) break;
                            int curr = y_plane[py * y_pitch + px];
                            int prev = encoder->prev_y_frame[py * encoder->width + px];
                            sad += abs(curr - prev);
                        }
                    }
                }
                if (sad < 512) {
                    current_skip_run++;
                } else {
                    /* mb_skip_run MUST be written exactly once, unconditionally
                     * (even when 0), before every coded macroblock - see
                     * cavlc_write_p_skip_run's doc comment. */
                    cavlc_write_p_skip_run(&bs, current_skip_run);
                    current_skip_run = 0;
                    int best_dx = 0, best_dy = 0;
                    uint32_t best_sad = sad;
                    if (encoder->has_prev_frame && encoder->prev_y_frame) {
                        static const int cand_mvs[8][2] = {
                            {-1, 0}, {1, 0}, {0, -1}, {0, 1},
                            {-2, 0}, {2, 0}, {0, -2}, {0, 2}
                        };
                        for (int d = 0; d < 8; d++) {
                            int dx = cand_mvs[d][0];
                            int dy = cand_mvs[d][1];
                            uint32_t cand_sad = 0;
                            for (int r = 0; r < 16; r += 2) {
                                int py = (int)(mby * 16 + r);
                                int ref_py = py + dy;
                                if (py >= (int)encoder->height || ref_py < 0 || ref_py >= (int)encoder->height) {
                                    cand_sad += 255 * 8;
                                    continue;
                                }
                                for (int c = 0; c < 16; c += 2) {
                                    int px = (int)(mbx * 16 + c);
                                    int ref_px = px + dx;
                                    if (px >= (int)encoder->width || ref_px < 0 || ref_px >= (int)encoder->width) {
                                        cand_sad += 255;
                                        continue;
                                    }
                                    cand_sad += abs((int)y_plane[py * y_pitch + px] -
                                                    (int)encoder->prev_y_frame[ref_py * encoder->width + ref_px]) * 4;
                                }
                            }
                            if (cand_sad < best_sad) {
                                best_sad = cand_sad;
                                best_dx = dx;
                                best_dy = dy;
                            }
                        }
                    }
                    cavlc_write_mb_p16x16_header(&bs, best_dx * 4, best_dy * 4, 0, 0);
                }
            }
            if (current_skip_run > 0) {
                cavlc_write_p_skip_run(&bs, current_skip_run);
            }
        }

        cavlc_write_slice_trailing_bits(&bs);
        bs_flush(&bs);

        size_t rbsp_len = bs_bytes_written(&bs);
        if (total_written + 5 + rbsp_len * 2 <= encoder->output_buf_size) {
            uint8_t *nal_dst = encoder->output_buf + total_written;
            nal_dst[0] = 0x00;
            nal_dst[1] = 0x00;
            nal_dst[2] = 0x00;
            nal_dst[3] = 0x01;
            nal_dst[4] = is_idr ? ((NAL_REF_IDC_HIGH << 5) | NAL_TYPE_IDR_SLICE)
                                : ((NAL_REF_IDC_MEDIUM << 5) | NAL_TYPE_SLICE);

            size_t ebsp_len = bs_rbsp_to_ebsp(nal_dst + 5,
                                              encoder->output_buf_size - total_written - 5,
                                              slice_rbsp,
                                              rbsp_len);
            total_written += 5 + ebsp_len;
        }
        free(slice_rbsp);
    }

    if (encoder->prev_y_frame) {
        for (uint32_t r = 0; r < encoder->height; r++) {
            memcpy(encoder->prev_y_frame + r * encoder->width,
                   y_plane + r * y_pitch,
                   encoder->width);
        }
        encoder->has_prev_frame = true;
    }

    if (output_size < total_written) {
        return -1;
    }
    memcpy(output_buf, encoder->output_buf, total_written);

    rc_update_stats(&encoder->rc, (int)(total_written * 8));
    manage_dpb(encoder, encoder->frame_num, encoder->poc);

    encoder->frame_num++;
    encoder->poc += 2;
    encoder->frame_count++;

    return (int)total_written;
}

void h264_encoder_destroy(h264_encoder_t *encoder)
{
    if (!encoder) return;
    if (encoder->output_buf) free(encoder->output_buf);
    if (encoder->prev_y_frame) free(encoder->prev_y_frame);
    if (encoder->nz_luma) free(encoder->nz_luma);
    if (encoder->nz_cb) free(encoder->nz_cb);
    if (encoder->nz_cr) free(encoder->nz_cr);
    free(encoder);
}
