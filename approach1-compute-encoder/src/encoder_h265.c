/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * encoder_h265.c - real intra-only H.265/HEVC encoder.
 *
 * ============================================================================
 * DESIGN, in one place (see docs/hevc_scope_note.md and DEVLOG.md Sec. 6/8
 * for the history of why this replaced a non-functional stub)
 * ============================================================================
 *
 * This is an INTRA-ONLY (every frame is an IDR I-slice) encoder. There is no
 * inter prediction, no multi-slice, no SAO, no deblocking-filter control (the
 * decoder applies its own default in-loop deblocking automatically - that is
 * fully decoder-normative and needs no encoder-side work), no cu_qp_delta,
 * no scaling lists, no tiles/WPP, and no VUI. All of that is real, scoped-out
 * future work, not silently-broken coverage - see this file's final report /
 * DEVLOG.md for the honest list.
 *
 * Picture structure, chosen to keep every stage genuinely simple AND
 * genuinely spec-correct at the same time (see hevc_intra.h's top comment
 * for why this is NOT a case of reusing the existing GPU DCT/quantize
 * shaders - HEVC's mandatory 4x4-luma-intra DST-VII transform and its own
 * QP-to-quantizer-step mapping make that numerically wrong, not just a
 * block-size mismatch):
 *
 *   - CTU size = 16x16 (the minimum ITU-T H.265 allows - CtbLog2SizeY must
 *     be 4..6). One split_cu_flag=1 per CTU (always forced - condL/condA
 *     context still real, computed from real neighbor availability), giving
 *     exactly four 8x8 CUs per CTU, in z-order (TL,TR,BL,BR).
 *   - Every CU is intra, PartMode=PART_NxN (legal only at minimum CU size,
 *     which 8x8 always is here) - four independent 4x4 luma PUs per CU, each
 *     with its own real intra_luma_pred_mode. PartMode=NxN makes
 *     IntraSplitFlag=1, which per 7.4.9.8 FORCES (infers, no bit spent) the
 *     transform tree to split once at trafoDepth==0, landing exactly on the
 *     four 4x4 luma PUs as their own leaf TUs - no separate transform-size
 *     decision needed anywhere in this encoder.
 *   - Chroma (4:2:0) is one 4x4 Cb + one 4x4 Cr block per CU (8x8 luma / 1
 *     chroma shift = 4x4 chroma, coded once at the CU's own transform-tree
 *     root per the spec's "chroma stops splitting at the 4x4 floor" rule -
 *     see encode_cu()'s comment).
 *   - Every 4x4 TU (luma AND chroma) is therefore always exactly one
 *     coefficient group - no sig_coeff_group_flag/coded_sub_block_flag
 *     complexity anywhere (see hevc_cabac.h's scope note).
 *
 * Real per-4x4-block intra prediction (Planar/DC/Horizontal/Vertical, the
 * same four candidates the GPU's own I16x16 SAD decision already knows how
 * to choose between, conceptually) with proper z-scan reconstruction
 * chaining, real DST-VII (luma) / DCT-II (chroma) transform + real HEVC
 * quantization, and real CABAC entropy coding are implemented in
 * hevc_intra.c and hevc_cabac.c respectively - see those files.
 *
 * The one piece of the existing GPU/Vulkan infrastructure this file DOES
 * reuse unmodified is gpu_compute_download_nv12() - the real, already-
 * uploaded picture is read back from the GPU surface into host memory once
 * per frame, exactly the way va_backend.c's own CPU-side surface access
 * (bc250_MapBuffer et al) already does, and the existing
 * gpu_compute_begin_picture/dispatch_encode/end_picture/sync() sequence is
 * still called first (with its result discarded) purely to preserve the
 * exact same Vulkan image layout transitions and fence/staging-buffer
 * bookkeeping the rest of this driver (va_backend.c's EndPicture) already
 * depends on - see that call site's comment below.
 */

#include "encoder_h265.h"
#include "bitstream.h"
#include "hevc_cabac.h"
#include "hevc_intra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NAL_UNIT_VPS               32
#define NAL_UNIT_SPS               33
#define NAL_UNIT_PPS               34
#define NAL_UNIT_CODED_SLICE_IDR_W_RADL 19

#define HEVC_CTU_SIZE 16
#define HEVC_CU_SIZE   8
#define HEVC_PU_SIZE   4

static inline uint8_t clip8i(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* ============================================================================
 * Level selection (Annex A.3 MaxLumaPs table, picture-size-only heuristic -
 * a real encoder would also check bitrate/CPB constraints; this project's
 * one-QP-for-the-whole-stream design has no rate-control loop to check
 * against, so this picks the smallest level whose MaxLumaPs covers the
 * picture, which is what every simple/embedded HEVC encoder does in
 * practice for a "just make it playable" level tag).
 * ==========================================================================*/
static int hevc_pick_level_idc(uint32_t width, uint32_t height) {
    static const struct { uint64_t max_luma_ps; int level_idc; } table[] = {
        { 36864UL,        30 },
        { 122880UL,       60 },
        { 245760UL,       63 },
        { 552960UL,       90 },
        { 983040UL,       93 },
        { 2228224UL,     120 },
        { 2228224UL,     123 },
        { 8912896UL,     150 },
        { 8912896UL,     153 },
        { 8912896UL,     156 },
        { 35651584UL,    180 },
        { 35651584UL,    183 },
        { 35651584UL,    186 },
    };
    uint64_t pic_size = (uint64_t)width * (uint64_t)height;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (pic_size <= table[i].max_luma_ps) return table[i].level_idc;
    return 186;
}

/* ============================================================================
 * VPS / SPS / PPS
 * ==========================================================================*/

static void write_profile_tier_level(bitstream_t *bs, int level_idc) {
    bs_write_u(bs, 2, 0);   /* general_profile_space */
    bs_write1(bs, 0);       /* general_tier_flag (Main tier) */
    bs_write_u(bs, 5, 1);   /* general_profile_idc = 1 (Main) */
    for (int i = 0; i < 32; i++)
        bs_write1(bs, i == 1 ? 1 : 0); /* general_profile_compatibility_flag[1] = Main */
    bs_write1(bs, 1); /* general_progressive_source_flag */
    bs_write1(bs, 0); /* general_interlaced_source_flag */
    bs_write1(bs, 0); /* general_non_packed_constraint_flag */
    bs_write1(bs, 1); /* general_frame_only_constraint_flag */
    bs_write_u(bs, 16, 0);
    bs_write_u(bs, 16, 0);
    bs_write_u(bs, 12, 0); /* reserved_zero_44bits */
    bs_write_u(bs, 8, level_idc);
}

static size_t write_vps(uint8_t *buf, size_t buf_size) {
    uint8_t rbsp[128];
    bitstream_t bs;
    bs_init(&bs, rbsp, sizeof(rbsp));

    bs_write_u(&bs, 4, 0);   /* vps_video_parameter_set_id */
    bs_write_u(&bs, 2, 3);   /* vps_base_layer_internal/available_flag */
    bs_write_u(&bs, 6, 0);   /* vps_max_layers_minus1 */
    bs_write_u(&bs, 3, 0);   /* vps_max_sub_layers_minus1 */
    bs_write1(&bs, 1);       /* vps_temporal_id_nesting_flag */
    bs_write_u(&bs, 16, 0xffff);

    write_profile_tier_level(&bs, 30); /* level is irrelevant here; SPS carries the real one */

    bs_write1(&bs, 1); /* vps_sub_layer_ordering_info_present_flag */
    bs_write_ue(&bs, 0); /* vps_max_dec_pic_buffering_minus1 */
    bs_write_ue(&bs, 0); /* vps_num_reorder_pics */
    bs_write_ue(&bs, 0); /* vps_max_latency_increase_plus1 */

    bs_write_u(&bs, 6, 0); /* vps_max_nuh_reserved_zero_layer_id */
    bs_write_ue(&bs, 0);   /* vps_max_op_sets_minus1 */
    bs_write1(&bs, 0);     /* vps_timing_info_present_flag */
    bs_write1(&bs, 0);     /* vps_extension_flag */

    bs_rbsp_trailing_bits(&bs);

    bitstream_t out_bs;
    bs_init(&out_bs, buf, buf_size);
    bs_write_nal_header_hevc(&out_bs, NAL_UNIT_VPS);
    size_t off = bs_bytes_written(&out_bs);
    if (off >= buf_size) return 0;
    return off + bs_rbsp_to_ebsp(buf + off, buf_size - off, rbsp, bs_bytes_written(&bs));
}

static size_t write_sps(uint8_t *buf, size_t buf_size, uint32_t coded_w, uint32_t coded_h,
                         uint32_t real_w, uint32_t real_h, int level_idc) {
    uint8_t rbsp[256];
    bitstream_t bs;
    bs_init(&bs, rbsp, sizeof(rbsp));

    bs_write_u(&bs, 4, 0);  /* sps_video_parameter_set_id */
    bs_write_u(&bs, 3, 0);  /* sps_max_sub_layers_minus1 */
    bs_write1(&bs, 1);      /* sps_temporal_id_nesting_flag */

    write_profile_tier_level(&bs, level_idc);

    bs_write_ue(&bs, 0); /* sps_seq_parameter_set_id */
    bs_write_ue(&bs, 1); /* chroma_format_idc = 1 (4:2:0) */

    bs_write_ue(&bs, coded_w);
    bs_write_ue(&bs, coded_h);

    int need_crop = (coded_w != real_w) || (coded_h != real_h);
    bs_write1(&bs, need_crop ? 1 : 0);
    if (need_crop) {
        bs_write_ue(&bs, 0);
        bs_write_ue(&bs, (coded_w - real_w) / 2);
        bs_write_ue(&bs, 0);
        bs_write_ue(&bs, (coded_h - real_h) / 2);
    }

    bs_write_ue(&bs, 0); /* bit_depth_luma_minus8 */
    bs_write_ue(&bs, 0); /* bit_depth_chroma_minus8 */
    bs_write_ue(&bs, 0); /* log2_max_pic_order_cnt_lsb_minus4 (log2=4; unused - every frame is IDR) */

    bs_write1(&bs, 1); /* sps_sub_layer_ordering_info_present_flag */
    bs_write_ue(&bs, 0); /* sps_max_dec_pic_buffering_minus1 - all-IDR, no DPB needed */
    bs_write_ue(&bs, 0); /* sps_num_reorder_pics */
    bs_write_ue(&bs, 0); /* sps_max_latency_increase_plus1 */

    bs_write_ue(&bs, 0); /* log2_min_luma_coding_block_size_minus3 -> MinCb = 8 */
    bs_write_ue(&bs, 1); /* log2_diff_max_min_coding_block_size -> Ctb = 16 */
    bs_write_ue(&bs, 0); /* log2_min_luma_transform_block_size_minus2 -> MinTb = 4 */
    bs_write_ue(&bs, 0); /* log2_diff_max_min_transform_block_size -> MaxTb = MinTb = 4 */
    bs_write_ue(&bs, 0); /* max_transform_hierarchy_depth_inter (unused, no inter) */
    bs_write_ue(&bs, 0); /* max_transform_hierarchy_depth_intra (IntraSplitFlag adds +1 -> MaxTrafoDepth=1) */

    bs_write1(&bs, 0); /* scaling_list_enabled_flag */
    bs_write1(&bs, 0); /* amp_enabled_flag */
    bs_write1(&bs, 0); /* sample_adaptive_offset_enabled_flag */
    bs_write1(&bs, 0); /* pcm_enabled_flag */

    bs_write_ue(&bs, 0); /* num_short_term_ref_pic_sets */
    bs_write1(&bs, 0);   /* long_term_ref_pics_present_flag */
    bs_write1(&bs, 0);   /* sps_temporal_mvp_enable_flag */
    bs_write1(&bs, 0);   /* sps_strong_intra_smoothing_enable_flag */
    bs_write1(&bs, 0);   /* vui_parameters_present_flag */
    bs_write1(&bs, 0);   /* sps_extension_present_flag */

    bs_rbsp_trailing_bits(&bs);

    bitstream_t out_bs;
    bs_init(&out_bs, buf, buf_size);
    bs_write_nal_header_hevc(&out_bs, NAL_UNIT_SPS);
    size_t off = bs_bytes_written(&out_bs);
    if (off >= buf_size) return 0;
    return off + bs_rbsp_to_ebsp(buf + off, buf_size - off, rbsp, bs_bytes_written(&bs));
}

static size_t write_pps(uint8_t *buf, size_t buf_size, int init_qp) {
    uint8_t rbsp[64];
    bitstream_t bs;
    bs_init(&bs, rbsp, sizeof(rbsp));

    bs_write_ue(&bs, 0); /* pps_pic_parameter_set_id */
    bs_write_ue(&bs, 0); /* pps_seq_parameter_set_id */
    bs_write1(&bs, 0);   /* dependent_slice_segments_enabled_flag */
    bs_write1(&bs, 0);   /* output_flag_present_flag */
    bs_write_u(&bs, 3, 0); /* num_extra_slice_header_bits */
    bs_write1(&bs, 0);   /* sign_data_hiding_flag */
    bs_write1(&bs, 0);   /* cabac_init_present_flag */
    bs_write_ue(&bs, 0); /* num_ref_idx_l0_default_active_minus1 */
    bs_write_ue(&bs, 0); /* num_ref_idx_l1_default_active_minus1 */
    bs_write_se(&bs, init_qp - 26); /* init_qp_minus26 */
    bs_write1(&bs, 0);   /* constrained_intra_pred_flag */
    bs_write1(&bs, 0);   /* transform_skip_enabled_flag */
    bs_write1(&bs, 0);   /* cu_qp_delta_enabled_flag */
    bs_write_se(&bs, 0); /* pps_cb_qp_offset */
    bs_write_se(&bs, 0); /* pps_cr_qp_offset */
    bs_write1(&bs, 0);   /* pps_slice_chroma_qp_offsets_present_flag */
    bs_write1(&bs, 0);   /* weighted_pred_flag */
    bs_write1(&bs, 0);   /* weighted_bipred_flag */
    bs_write1(&bs, 0);   /* transquant_bypass_enable_flag */
    bs_write1(&bs, 0);   /* tiles_enabled_flag */
    bs_write1(&bs, 0);   /* entropy_coding_sync_enabled_flag */
    bs_write1(&bs, 1);   /* pps_loop_filter_across_slices_enabled_flag */
    bs_write1(&bs, 0);   /* deblocking_filter_control_present_flag (defaults apply: enabled, offsets 0) */
    bs_write1(&bs, 0);   /* pps_scaling_list_data_present_flag */
    bs_write1(&bs, 0);   /* lists_modification_present_flag */
    bs_write_ue(&bs, 0); /* log2_parallel_merge_level_minus2 */
    bs_write1(&bs, 0);   /* slice_segment_header_extension_present_flag */
    bs_write1(&bs, 0);   /* pps_extension_present_flag */

    bs_rbsp_trailing_bits(&bs);

    bitstream_t out_bs;
    bs_init(&out_bs, buf, buf_size);
    bs_write_nal_header_hevc(&out_bs, NAL_UNIT_PPS);
    size_t off = bs_bytes_written(&out_bs);
    if (off >= buf_size) return 0;
    return off + bs_rbsp_to_ebsp(buf + off, buf_size - off, rbsp, bs_bytes_written(&bs));
}

/* ============================================================================
 * Encoder state
 * ==========================================================================*/

struct hevc_encoder {
    bc250_gpu_context_t *gpu;
    uint32_t width, height;               /* real (as requested by libva) */
    uint32_t coded_width, coded_height;   /* rounded up to a 16px CTU multiple */
    uint32_t width_ctu, height_ctu;
    uint32_t fps, bitrate;
    uint32_t frame_count;
    int qp;

    /* Source (post-download, padded/replicated to coded dimensions) and
     * reconstructed planes. Luma at coded_w x coded_h; chroma at
     * coded_w/2 x coded_h/2 (4:2:0). */
    uint8_t *src_y, *src_cb, *src_cr;
    uint8_t *recon_y, *recon_cb, *recon_cr;

    /* Real per-4x4-luma-PU intra mode, for MPM derivation - one entry per
     * 4x4 position, persistent scratch (positional availability checks
     * gate every read, so stale cross-frame content is never read - see
     * hevc_derive_mpm() call sites below). */
    int8_t *luma_mode_map;
    uint32_t mode_map_stride;

    /* Raw NV12 download scratch, real width x height. */
    uint8_t *dl_y;
    uint8_t *dl_uv;

    uint8_t *slice_rbsp;
    size_t   slice_rbsp_cap;

    uint8_t *scratch_out;
    size_t   scratch_out_cap;
};

static uint32_t round_up16(uint32_t v) { return (v + 15u) & ~15u; }

hevc_encoder_t *hevc_encoder_create(bc250_gpu_context_t *gpu_ctx,
                                    uint32_t width, uint32_t height,
                                    uint32_t fps, uint32_t bitrate)
{
    hevc_encoder_t *enc = calloc(1, sizeof(hevc_encoder_t));
    if (!enc) return NULL;

    enc->gpu = gpu_ctx;
    enc->width = width;
    enc->height = height;
    enc->fps = fps ? fps : 30;
    enc->bitrate = bitrate;
    enc->qp = 27;
    {
        const char *qp_env = getenv("BC250_HEVC_QP");
        if (qp_env) {
            int q = atoi(qp_env);
            if (q >= 1 && q <= 51) enc->qp = q;
        }
    }

    enc->coded_width = round_up16(width);
    enc->coded_height = round_up16(height);
    enc->width_ctu = enc->coded_width / HEVC_CTU_SIZE;
    enc->height_ctu = enc->coded_height / HEVC_CTU_SIZE;

    size_t luma_size = (size_t)enc->coded_width * enc->coded_height;
    size_t chroma_size = (size_t)(enc->coded_width / 2) * (enc->coded_height / 2);

    enc->src_y = malloc(luma_size);
    enc->src_cb = malloc(chroma_size);
    enc->src_cr = malloc(chroma_size);
    enc->recon_y = malloc(luma_size);
    enc->recon_cb = malloc(chroma_size);
    enc->recon_cr = malloc(chroma_size);

    enc->mode_map_stride = enc->coded_width / HEVC_PU_SIZE;
    enc->luma_mode_map = malloc((size_t)enc->mode_map_stride * (enc->coded_height / HEVC_PU_SIZE));

    enc->dl_y = malloc((size_t)width * height);
    enc->dl_uv = malloc((size_t)(width / 2) * (height / 2) * 2);

    enc->slice_rbsp_cap = luma_size + 65536;
    enc->slice_rbsp = malloc(enc->slice_rbsp_cap);

    enc->scratch_out_cap = luma_size + 131072;
    enc->scratch_out = malloc(enc->scratch_out_cap);

    if (!enc->src_y || !enc->src_cb || !enc->src_cr || !enc->recon_y || !enc->recon_cb ||
        !enc->recon_cr || !enc->luma_mode_map || !enc->dl_y || !enc->dl_uv ||
        !enc->slice_rbsp || !enc->scratch_out) {
        hevc_encoder_destroy(enc);
        return NULL;
    }

    return enc;
}

void hevc_encoder_destroy(hevc_encoder_t *encoder)
{
    if (!encoder) return;
    free(encoder->src_y); free(encoder->src_cb); free(encoder->src_cr);
    free(encoder->recon_y); free(encoder->recon_cb); free(encoder->recon_cr);
    free(encoder->luma_mode_map);
    free(encoder->dl_y); free(encoder->dl_uv);
    free(encoder->slice_rbsp);
    free(encoder->scratch_out);
    free(encoder);
}

/* Replicate-pad a downloaded plane (real w x h) into a coded_w x coded_h
 * working buffer - only the bottom/right margin (if any) needs padding,
 * since coded dims are always >= real dims by construction. */
static void pad_replicate(uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                           const uint8_t *src, uint32_t src_stride, uint32_t src_w, uint32_t src_h) {
    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t sy = y < src_h ? y : src_h - 1;
        const uint8_t *srow = src + (size_t)sy * src_stride;
        uint8_t *drow = dst + (size_t)y * dst_w;
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t sx = x < src_w ? x : src_w - 1;
            drow[x] = srow[sx];
        }
    }
}

/* ============================================================================
 * Per-CU encoding
 * ==========================================================================*/

static const int pu_off_x[4] = { 0, 4, 0, 4 };
static const int pu_off_y[4] = { 0, 0, 4, 4 };

static int any_nonzero16(const int16_t *c) {
    for (int i = 0; i < 16; i++) if (c[i]) return 1;
    return 0;
}

static void encode_cu(hevc_encoder_t *enc, hevc_cabac_t *cab, int cu_x, int cu_y) {
    int qp = enc->qp;
    uint32_t cw = enc->coded_width, ch = enc->coded_height;
    uint32_t ccw = cw / 2, cch = ch / 2;

    hevc_cabac_code_part_mode_intra(cab, 0 /* PART_NxN */);

    int pu_modes[4];
    int16_t luma_coeff[4][16];
    int cbf_luma[4];

    /* Step 1: decide + reconstruct all 4 luma PUs in z-order (needed so
     * each later PU's neighbor gathering sees real reconstructed samples
     * from the earlier PUs of the SAME CU, exactly like a real decoder). */
    for (int pu = 0; pu < 4; pu++) {
        int px = cu_x + pu_off_x[pu], py = cu_y + pu_off_y[pu];
        int mode = hevc_choose_luma_mode(enc->src_y, enc->recon_y, (int)cw, (int)cw, (int)ch, px, py);
        pu_modes[pu] = mode;

        uint8_t pred[16];
        hevc_predict_4x4(enc->recon_y, cw, cw, ch, px, py, mode, 1, pred);

        int16_t residual[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                residual[y * 4 + x] = (int16_t)(enc->src_y[(py + y) * cw + (px + x)] - pred[y * 4 + x]);

        int16_t coeff[16];
        hevc_transform_quant_4x4(residual, qp, 1 /* DST for 4x4 luma intra */, coeff);
        memcpy(luma_coeff[pu], coeff, sizeof(coeff));
        cbf_luma[pu] = any_nonzero16(coeff);

        int16_t recon_residual[16];
        hevc_dequant_itransform_4x4(coeff, qp, 1, recon_residual);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                enc->recon_y[(py + y) * cw + (px + x)] = clip8i(pred[y * 4 + x] + recon_residual[y * 4 + x]);

        enc->luma_mode_map[(py / 4) * enc->mode_map_stride + (px / 4)] = (int8_t)mode;
    }

    /* Chroma: one 4x4 Cb + one 4x4 Cr per CU, DC prediction only (matching
     * this codebase's existing H.264 "chroma directional modes not
     * implemented" precedent), DCT-II (never DST - DST is luma-4x4-intra
     * only, per spec). */
    int cx = cu_x / 2, cy = cu_y / 2;
    uint8_t pred_cb[16], pred_cr[16];
    hevc_predict_4x4(enc->recon_cb, ccw, ccw, cch, cx, cy, HEVC_MODE_DC, 0, pred_cb);
    hevc_predict_4x4(enc->recon_cr, ccw, ccw, cch, cx, cy, HEVC_MODE_DC, 0, pred_cr);

    int16_t res_cb[16], res_cr[16];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            res_cb[y * 4 + x] = (int16_t)(enc->src_cb[(cy + y) * ccw + (cx + x)] - pred_cb[y * 4 + x]);
            res_cr[y * 4 + x] = (int16_t)(enc->src_cr[(cy + y) * ccw + (cx + x)] - pred_cr[y * 4 + x]);
        }

    int16_t coeff_cb[16], coeff_cr[16];
    hevc_transform_quant_4x4(res_cb, qp, 0, coeff_cb);
    hevc_transform_quant_4x4(res_cr, qp, 0, coeff_cr);
    int cbf_cb = any_nonzero16(coeff_cb);
    int cbf_cr = any_nonzero16(coeff_cr);

    int16_t rres_cb[16], rres_cr[16];
    hevc_dequant_itransform_4x4(coeff_cb, qp, 0, rres_cb);
    hevc_dequant_itransform_4x4(coeff_cr, qp, 0, rres_cr);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            enc->recon_cb[(cy + y) * ccw + (cx + x)] = clip8i(pred_cb[y * 4 + x] + rres_cb[y * 4 + x]);
            enc->recon_cr[(cy + y) * ccw + (cx + x)] = clip8i(pred_cr[y * 4 + x] + rres_cr[y * 4 + x]);
        }

    /* Step 2: emit the 4 PUs' real intra_luma_pred_mode syntax. ITU-T
     * H.265 7.3.8.5's coding_unit() codes this as TWO separate passes over
     * all 4 PUs - every prev_intra_luma_pred_flag first, THEN every
     * mpm_idx/rem_intra_luma_pred_mode - not interleaved per PU (see
     * hevc_cabac_code_intra_luma_flag()/_data()'s comment; getting this
     * order wrong was this encoder's first real bug, caught by comparing
     * this encoder's own reconstruction - which matched the source fine -
     * against ffmpeg's actual decode of the resulting bitstream, which
     * didn't: a CABAC bit-order mistake still produces a structurally
     * valid, crash-free bitstream, just one that decodes to noise from
     * that point on). */
    int mpm[4][3];
    int pred_idx[4];
    for (int pu = 0; pu < 4; pu++) {
        int px = cu_x + pu_off_x[pu], py = cu_y + pu_off_y[pu];
        int mx = px / 4, my = py / 4;
        int left_avail = px > 0;
        int above_avail = py > 0;
        int left_mode = left_avail ? enc->luma_mode_map[my * enc->mode_map_stride + (mx - 1)] : 0;
        int above_mode = above_avail ? enc->luma_mode_map[(my - 1) * enc->mode_map_stride + mx] : 0;
        hevc_derive_mpm(left_mode, left_avail, above_mode, above_avail, mpm[pu]);
        pred_idx[pu] = hevc_cabac_code_intra_luma_flag(cab, pu_modes[pu], mpm[pu]);
    }
    for (int pu = 0; pu < 4; pu++)
        hevc_cabac_code_intra_luma_data(cab, pu_modes[pu], pred_idx[pu], mpm[pu]);

    /* Step 3: chroma mode (always DC; luma_mode_pu0 decides whether that's
     * signaled as index-3-of-candidate-list or as the derived/DM mode -
     * see hevc_cabac_code_intra_chroma_pred_mode()'s comment). */
    hevc_cabac_code_intra_chroma_pred_mode(cab, pu_modes[0]);

    /* Step 4: transform_tree - chroma cbf BITS first (trafoDepth=0, this
     * CU's root), then the 4 luma leaves' cbf+residual, then finally the
     * chroma RESIDUAL DATA (coded once per CU, after all 4 luma leaves -
     * this specific ordering, bits-before-luma but data-after-luma, is
     * exactly what ITU-T H.265's transform_tree()/transform_unit()
     * recursion produces for a CU whose chroma has already hit the 4x4
     * floor - see this file's top comment and x265's own
     * Entropy::encodeTransform(), which this encoder's fixed two-level
     * structure is a manually-unrolled special case of). */
    hevc_cabac_code_cbf_chroma(cab, cbf_cb, 0);
    hevc_cabac_code_cbf_chroma(cab, cbf_cr, 0);

    for (int pu = 0; pu < 4; pu++) {
        hevc_cabac_code_cbf_luma(cab, cbf_luma[pu], 1);
        if (cbf_luma[pu]) {
            int scan_idx = hevc_scan_idx_for_mode(pu_modes[pu]);
            hevc_cabac_code_residual_4x4(cab, luma_coeff[pu], 1, scan_idx);
        }
    }
    if (cbf_cb) hevc_cabac_code_residual_4x4(cab, coeff_cb, 0, 0 /* chroma always diagonal in 4:2:0 */);
    if (cbf_cr) hevc_cabac_code_residual_4x4(cab, coeff_cr, 0, 0);
}

static void encode_ctu(hevc_encoder_t *enc, hevc_cabac_t *cab, int ctu_col, int ctu_row) {
    int ctu_x = ctu_col * HEVC_CTU_SIZE, ctu_y = ctu_row * HEVC_CTU_SIZE;
    int cond_l = ctu_col > 0 ? 1 : 0;
    int cond_a = ctu_row > 0 ? 1 : 0;
    hevc_cabac_code_split_cu_flag(cab, 1, cond_l + cond_a);

    static const int cu_off_x[4] = { 0, 8, 0, 8 };
    static const int cu_off_y[4] = { 0, 0, 8, 8 };
    for (int i = 0; i < 4; i++)
        encode_cu(enc, cab, ctu_x + cu_off_x[i], ctu_y + cu_off_y[i]);
}

/* ============================================================================
 * Frame entry point
 * ==========================================================================*/

/* Core encode: assumes encoder->dl_y / encoder->dl_uv (real width x height,
 * NV12: dl_y row-pitch == width, dl_uv row-pitch == width with Cb/Cr
 * interleaved) are already populated. Both hevc_encoder_encode_frame()
 * (GPU-surface readback) and hevc_encoder_encode_raw() (direct host
 * pointers, no GPU involved - see encoder_h265.h) fill those in their own
 * way and then share everything from here on. */
static int encode_core(hevc_encoder_t *encoder, uint8_t *output_buf, size_t output_size)
{
    pad_replicate(encoder->src_y, encoder->coded_width, encoder->coded_height,
                  encoder->dl_y, encoder->width, encoder->width, encoder->height);

    uint32_t cw2 = encoder->width / 2, ch2 = encoder->height / 2;
    uint32_t ccw = encoder->coded_width / 2, cch = encoder->coded_height / 2;
    for (uint32_t y = 0; y < ch2; y++) {
        const uint8_t *uvrow = encoder->dl_uv + (size_t)y * encoder->width;
        for (uint32_t x = 0; x < cw2; x++) {
            encoder->src_cb[y * ccw + x] = uvrow[x * 2 + 0];
            encoder->src_cr[y * ccw + x] = uvrow[x * 2 + 1];
        }
    }
    pad_replicate(encoder->src_cb, ccw, cch, encoder->src_cb, ccw, cw2, ch2);
    pad_replicate(encoder->src_cr, ccw, cch, encoder->src_cr, ccw, cw2, ch2);

    memset(encoder->luma_mode_map, 0, (size_t)encoder->mode_map_stride * (encoder->coded_height / HEVC_PU_SIZE));

    bitstream_t slice_bs;
    bs_init(&slice_bs, encoder->slice_rbsp, encoder->slice_rbsp_cap);

    bs_write1(&slice_bs, 1); /* first_slice_segment_in_pic_flag */
    bs_write1(&slice_bs, 1); /* no_output_of_prior_pics_flag (RAP picture) */
    bs_write_ue(&slice_bs, 0); /* slice_pic_parameter_set_id */
    bs_write_ue(&slice_bs, 2); /* slice_type = I */
    bs_write_se(&slice_bs, 0); /* slice_qp_delta (init_qp already == encoder->qp) */
    bs_write1(&slice_bs, 1);   /* slice_loop_filter_across_slices_enabled_flag */

    /* ITU-T H.265 7.3.6.1: slice_segment_header() ends with byte_alignment()
     * - alignment_bit_equal_to_one (a mandatory '1') followed by
     * alignment_bit_equal_to_zero padding up to the next byte boundary -
     * unconditionally, even if the header already happens to end on a byte
     * boundary (in which case this still emits a full padding byte, per
     * 7.3.2.11's own "while(!byte_aligned())" loop). slice_segment_data()
     * (CABAC) starts only after this. Missing this was a real bug: it
     * silently shifted every CABAC bit by however many bits were missing,
     * which a hand-written decoder sharing this same omission would never
     * catch (it agrees with the encoder's own - wrong - assumption); found
     * by cross-checking against ffmpeg's `trace_headers` bitstream filter,
     * which parses against the real spec independently of anything in
     * this codebase. bs_rbsp_trailing_bits() below is bit-for-bit the same
     * construct (rbsp_trailing_bits() and byte_alignment() are defined
     * identically), reused here rather than duplicated. */
    bs_rbsp_trailing_bits(&slice_bs);

    hevc_cabac_t cab;
    hevc_cabac_init(&cab, &slice_bs);
    hevc_cabac_reset_contexts(&cab, encoder->qp);
    hevc_cabac_start(&cab);

    /* ITU-T H.265 7.3.8.1 slice_segment_data(): end_of_slice_segment_flag
     * is coded via encodeBinTrm() after EVERY coding_tree_unit(), not just
     * once at the end of the slice - it's 0 for every CTU but the last,
     * 1 for the last. encodeBinTrm() narrows the arithmetic coder's range
     * even when its bin is 0 (see hevc_cabac_encode_terminate()), so
     * skipping the 0-valued calls does real, silent damage to the coder's
     * low/range state from the very first CTU onward - this was found by
     * comparing this encoder's own (CABAC-free) reconstruction, which
     * matched real source content fine, against a real decoder's actual
     * output, which didn't even for a trivial flat/near-zero-residual
     * frame - see git history for the full debugging note. */
    uint32_t total_ctus = encoder->width_ctu * encoder->height_ctu;
    uint32_t ctu_idx = 0;
    for (uint32_t row = 0; row < encoder->height_ctu; row++) {
        for (uint32_t col = 0; col < encoder->width_ctu; col++) {
            encode_ctu(encoder, &cab, (int)col, (int)row);
            ctu_idx++;
            hevc_cabac_encode_terminate(&cab, ctu_idx == total_ctus ? 1 : 0);
        }
    }

    hevc_cabac_finish(&cab);
    bs_rbsp_trailing_bits(&slice_bs);

    size_t total = 0;
    total += write_vps(encoder->scratch_out + total, encoder->scratch_out_cap - total);
    total += write_sps(encoder->scratch_out + total, encoder->scratch_out_cap - total,
                        encoder->coded_width, encoder->coded_height,
                        encoder->width, encoder->height,
                        hevc_pick_level_idc(encoder->coded_width, encoder->coded_height));
    total += write_pps(encoder->scratch_out + total, encoder->scratch_out_cap - total, encoder->qp);

    {
        bitstream_t out_bs;
        bs_init(&out_bs, encoder->scratch_out + total, encoder->scratch_out_cap - total);
        bs_write_nal_header_hevc(&out_bs, NAL_UNIT_CODED_SLICE_IDR_W_RADL);
        size_t off = bs_bytes_written(&out_bs);
        size_t ebsp = bs_rbsp_to_ebsp(encoder->scratch_out + total + off, encoder->scratch_out_cap - total - off,
                                       encoder->slice_rbsp, bs_bytes_written(&slice_bs));
        total += off + ebsp;
    }

    if (total > output_size) return -1;
    memcpy(output_buf, encoder->scratch_out, total);

    /* Debug-only: dump this encoder's own idea of the reconstructed picture
     * (i.e. what a bug-free decoder given this exact bitstream SHOULD
     * reproduce) - lets a diff against a real decoder's actual output
     * localize whether a mismatch is in the prediction/transform/quant
     * math (this dump would ALSO look wrong) or in CABAC/bitstream framing
     * (this dump looks right, but a real decoder's output doesn't). */
    if (getenv("BC250_HEVC_DEBUG_RECON")) {
        FILE *fy = fopen("bc250_hevc_debug_recon_y.raw", "wb");
        if (fy) { fwrite(encoder->recon_y, 1, (size_t)encoder->coded_width * encoder->coded_height, fy); fclose(fy); }
    }

    encoder->frame_count++;
    return (int)total;
}

int hevc_encoder_encode_frame(hevc_encoder_t *encoder,
                              bc250_gpu_context_t *gpu_ctx,
                              gpu_image_t input_surface,
                              gpu_memory_t input_memory,
                              uint8_t *output_buf, size_t output_size)
{
    if (!encoder || !output_buf) return -1;

    /* Preserve the existing driver's Vulkan image-layout-transition and
     * staging/fence contract (see va_backend.c's bc250_EndPicture() comment
     * on why this call must still happen even though this encoder discards
     * its coefficient/motion output entirely - HEVC's own transform/quant
     * math is done independently in encode_core() above, see this file's
     * top comment). */
    if (gpu_ctx && input_surface.y_plane != VK_NULL_HANDLE) {
        gpu_compute_begin_picture(gpu_ctx, input_surface);
        gpu_compute_dispatch_encode(gpu_ctx, input_surface, encoder->width, encoder->height,
                                     encoder->qp, 1, 1);
        gpu_compute_end_picture(gpu_ctx);
        gpu_compute_sync(gpu_ctx);

        gpu_compute_download_nv12(gpu_ctx, &input_surface, input_memory,
                                   encoder->dl_y, (int)encoder->width,
                                   encoder->dl_uv, (int)encoder->width,
                                   (int)encoder->width, (int)encoder->height);
    } else {
        memset(encoder->dl_y, 128, (size_t)encoder->width * encoder->height);
        memset(encoder->dl_uv, 128, (size_t)(encoder->width / 2) * (encoder->height / 2) * 2);
    }

    return encode_core(encoder, output_buf, output_size);
}

int hevc_encoder_encode_raw(hevc_encoder_t *encoder,
                            const uint8_t *y_plane, int y_pitch,
                            const uint8_t *uv_plane, int uv_pitch,
                            uint8_t *output_buf, size_t output_size)
{
    if (!encoder || !output_buf || !y_plane || !uv_plane) return -1;

    for (uint32_t y = 0; y < encoder->height; y++)
        memcpy(encoder->dl_y + (size_t)y * encoder->width, y_plane + (size_t)y * y_pitch, encoder->width);
    for (uint32_t y = 0; y < encoder->height / 2; y++)
        memcpy(encoder->dl_uv + (size_t)y * encoder->width, uv_plane + (size_t)y * uv_pitch, encoder->width);

    return encode_core(encoder, output_buf, output_size);
}
