/* bc250-vcn-driver v0.2.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
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
 * Real per-4x4-block intra prediction over the full HEVC luma mode set
 * (Planar, DC and all 33 angular modes), chosen per PU by a rate-aware
 * coarse-then-refine search, with proper z-scan reconstruction chaining,
 * real DST-VII (luma) / DCT-II (chroma) transform + real HEVC
 * quantization, and real CABAC entropy coding are implemented in
 * hevc_intra.c and hevc_cabac.c respectively - see those files. Chroma is
 * still DC-only; that one is genuinely scoped-out, not broken.
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

#define _POSIX_C_SOURCE 200809L
#include "encoder_h265.h"
#include "bitstream.h"
#include <time.h>
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
                         uint32_t real_w, uint32_t real_h, int level_idc, int max_tb_log2) {
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
    /* MaxTb. At 4 (the old fixed value) log2TrafoSize always exceeded it
     * at an 8x8 CU, so 7.4.9.8 INFERRED split_transform_flag = 1 and the
     * transform tree had no choices at all. Raising it to 8 is what makes
     * the flag codable, i.e. what lets a CU use one 8x8 transform instead
     * of four 4x4 ones. max_transform_hierarchy_depth_intra must rise to
     * 1 to match, or the flag is still not coded (7.3.8.8 requires
     * trafoDepth < MaxTrafoDepth). */
    bs_write_ue(&bs, (uint32_t)(max_tb_log2 - 2)); /* log2_diff_max_min_transform_block_size */
    bs_write_ue(&bs, 0);                            /* max_transform_hierarchy_depth_inter */
    bs_write_ue(&bs, (uint32_t)(max_tb_log2 - 2)); /* max_transform_hierarchy_depth_intra */

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

    /* Per-4x4-luma-block intra mode SHORTLIST: the `shortlist_n` modes
     * that best predict the source block from its source neighbours,
     * produced by one frame-wide pass before any CTU is coded (see
     * encode_core). Every block in that pass is independent, so it is the
     * offloadable half of the mode decision.
     *
     * Deliberately stores the ranked mode numbers, not the 35 costs: this
     * is exactly what a GPU pass would hand back, and it matters. Keeping
     * all 35 costs meant 9 MB of scattered reads per frame plus a 35-entry
     * selection per block, both of which land in the SERIAL half and ate
     * most of the benefit (measured: 1.25x projected, against 2.4x for
     * this compact form). One byte per candidate, ranked, is 1 MB.
     *
     * Indexed [((y/4) * mode_map_stride + (x/4)) * shortlist_n].
     * NULL when the split decision is disabled. */
    uint8_t *mode_shortlist;
    int use_split_rmd;
    int shortlist_n;   /* how many of the 35 survive to the exact decision */

    /* MaxTbLog2SizeY. 4 allows an undivided 16x16 CU with one 16x16
     * transform; 2 restores the original spec-minimum behaviour. */
    int max_tb_log2;

    /* Run reconstruction on the GPU (hevc_intra_wavefront.comp) and keep
     * only CABAC on the CPU. Opt-in: bit-exact for the structure it
     * supports, but it codes every CTU as one 16x16 CU with no splits. */
    int use_gpu;

    /* CtDepth per 4x4 unit - 0 for an undivided 16x16 CU, 1 for an 8x8
     * one. split_cu_flag's context is derived from the left and above
     * neighbours' depths (9.3.4.2.2), which was a constant while every
     * CTU split unconditionally and is not any more. */
    uint8_t *cu_depth_map;

    /* Flatness threshold for the CU-size decision, as a fraction
     * flat_num/flat_den of the quantizer step. Tunable so the tradeoff
     * can be swept rather than guessed. */
    int flat_num, flat_den;

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
    enc->cu_depth_map = malloc((size_t)enc->mode_map_stride * (enc->coded_height / HEVC_PU_SIZE));

    /* Split (offloadable) mode decision. Opt-in for now: it trades a
     * small amount of decision accuracy - modes are scored against source
     * rather than reconstructed neighbours - for a search that is
     * embarrassingly parallel and can therefore move off the CPU. */
    {
        const char *e = getenv("BC250_HEVC_SPLIT_RMD");
        enc->use_split_rmd = (e && (e[0] == '1' || e[0] == 't' || e[0] == 'T'));
    }
    {
        const char *e = getenv("BC250_HEVC_GPU");
        enc->use_gpu = (e && (e[0] == '1' || e[0] == 't' || e[0] == 'T'));
    }
    enc->max_tb_log2 = 4;
    {
        const char *e = getenv("BC250_HEVC_MAX_TB");
        if (e) {
            int v = atoi(e);
            if (v == 4) enc->max_tb_log2 = 2;
            else if (v == 8) enc->max_tb_log2 = 3;
            else if (v == 16) enc->max_tb_log2 = 4;
        }
    }
    /* Merge threshold as a fraction of the quantizer step, in eighths.
     * 32 means "merge when the block's mean absolute deviation is within
     * four quantizer steps", which is far more permissive than the naive
     * RD arithmetic suggests and is what measured best: sweeping 1x, 2x,
     * 4x and 8x gave -28.6%, -32.7%, -34.8% and -34.3% BD-rate. Being
     * stricter is actively harmful - at 1/8x the mean fell to -16.1% and
     * synthetic content regressed by +26%, because every CU that does
     * NOT merge still pays the split_transform_flag bin that raising
     * MaxTbLog2SizeY makes codable, so a merge that does not happen is a
     * bin spent for nothing. */
    enc->flat_num = 32;
    enc->flat_den = 8;
    {
        const char *e = getenv("BC250_HEVC_FLAT");
        if (e) { int v = atoi(e); if (v > 0 && v <= 512) enc->flat_num = v; }
    }

    enc->shortlist_n = 4;
    {
        const char *e = getenv("BC250_HEVC_SHORTLIST");
        if (e) { int v = atoi(e); if (v >= 1 && v <= HEVC_MODE_COUNT) enc->shortlist_n = v; }
    }
    if (enc->use_split_rmd) {
        size_t blocks = (size_t)enc->mode_map_stride * (enc->coded_height / HEVC_PU_SIZE);
        enc->mode_shortlist = malloc(blocks * (size_t)enc->shortlist_n);
        if (!enc->mode_shortlist) { hevc_encoder_destroy(enc); return NULL; }
    }

    enc->dl_y = malloc((size_t)width * height);
    enc->dl_uv = malloc((size_t)(width / 2) * (height / 2) * 2);

    enc->slice_rbsp_cap = luma_size + 65536;
    enc->slice_rbsp = malloc(enc->slice_rbsp_cap);

    enc->scratch_out_cap = luma_size + 131072;
    enc->scratch_out = malloc(enc->scratch_out_cap);

    if (!enc->src_y || !enc->src_cb || !enc->src_cr || !enc->recon_y || !enc->recon_cb ||
        !enc->recon_cr || !enc->luma_mode_map || !enc->cu_depth_map || !enc->dl_y || !enc->dl_uv ||
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
    free(encoder->cu_depth_map);
    free(encoder->mode_shortlist);
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

/* Rec. ITU-T H.265 8.4.2 MPM derivation for the 4x4 luma PU at (px,py),
 * reading the neighbouring PUs' already-decided modes out of
 * enc->luma_mode_map. */
static void derive_pu_mpm(hevc_encoder_t *enc, int px, int py, int mpm_out[3]) {
    int mx = px / 4, my = py / 4;
    int left_avail = px > 0;
    /* Rec. ITU-T H.265 8.4.2: candIntraPredModeB (the "above" MPM
     * candidate) must be forced unavailable whenever the above neighbour
     * is in a different CTU row, unconditionally - not just when py==0.
     * This mirrors the picture-boundary check but for CTU rows, and is
     * easy to miss because the neighbour pixel data IS genuinely
     * available/reconstructed; the spec still mandates treating it as
     * absent for MPM derivation. Getting this wrong silently changes
     * mpm[]'s candidate ORDER (and hence what mpm_idx /
     * rem_intra_luma_pred_mode means) for one whole PU-row per CTU,
     * producing a structurally valid but wrong-meaning bitstream that
     * only misdecodes once real (non-flat/non-DC) directional content
     * exercises those modes - hence "busy directional content only" as
     * the symptom. */
    int above_avail = (py > 0) && ((py % HEVC_CTU_SIZE) != 0);
    int left_mode = left_avail ? enc->luma_mode_map[my * enc->mode_map_stride + (mx - 1)] : 0;
    int above_mode = above_avail ? enc->luma_mode_map[(my - 1) * enc->mode_map_stride + mx] : 0;
    hevc_derive_mpm(left_mode, left_avail, above_mode, above_avail, mpm_out);
}

/* One complete luma pass over a CU's four 4x4 blocks: decide (or accept a
 * forced) mode per block, predict, transform, quantize and reconstruct, in
 * z-order so each block sees its predecessors' real reconstruction. Every
 * output needed to either commit the pass or throw it away and re-run it
 * differently is captured in the struct. */
typedef struct {
    int      mode[4];
    int16_t  coeff[4][16];
    int      cbf[4];
    uint8_t  recon[64];   /* the CU's 8x8 luma block, row-major */
    long     sad;         /* summed prediction error over the 4 blocks */
    int      mode_bits;   /* summed cost of signalling those modes */
} cu_luma_pass_t;

static void save_cu_luma(const hevc_encoder_t *enc, int cu_x, int cu_y, uint8_t out[64]) {
    uint32_t cw = enc->coded_width;
    for (int y = 0; y < 8; y++)
        memcpy(out + y * 8, enc->recon_y + (size_t)(cu_y + y) * cw + cu_x, 8);
}

static void restore_cu_luma(hevc_encoder_t *enc, int cu_x, int cu_y, const uint8_t in[64]) {
    uint32_t cw = enc->coded_width;
    for (int y = 0; y < 8; y++)
        memcpy(enc->recon_y + (size_t)(cu_y + y) * cw + cu_x, in + y * 8, 8);
}


/* forced_mode < 0 searches per block (the PART_NxN shape); forced_mode >= 0
 * applies that one mode to all four blocks (the PART_2Nx2N shape, where
 * only one mode is signalled but prediction and reconstruction are still
 * per-4x4-transform-block, exactly as a decoder does it). */
static void run_luma_pass(hevc_encoder_t *enc, int cu_x, int cu_y, int forced_mode,
                           cu_luma_pass_t *p) {
    int qp = enc->qp;
    uint32_t cw = enc->coded_width, ch = enc->coded_height;

    p->sad = 0;
    p->mode_bits = 0;

    for (int pu = 0; pu < 4; pu++) {
        int px = cu_x + pu_off_x[pu], py = cu_y + pu_off_y[pu];

        /* The MPM list is derived here, before the mode is chosen, because
         * the decision is rate-aware (see hevc_choose_luma_mode()) and an
         * MPM is 3-4 bits cheaper to signal than an arbitrary mode - at
         * 4x4 granularity that side information is a large fraction of the
         * whole frame. Deriving it this early is safe and yields exactly
         * the lists step 2 below will re-derive for the actual signalling,
         * because every neighbour an MPM list reads (left and above) is
         * strictly earlier in z-order than the PU reading it: TL has no
         * in-CU neighbours, TR reads TL, BL reads TL, BR reads BL and TR. */
        int mpm[3];
        derive_pu_mpm(enc, px, py, mpm);

        /* Gather this block's reference samples ONCE and share them
         * between the mode search and the prediction of the winner. The
         * substitution scan is ~21% of encode time by profile, and doing
         * it twice per block bought nothing - the reconstruction cannot
         * have changed in between. */
        hevc_refs_t refs;
        hevc_gather_refs(enc->recon_y, (int)cw, (int)cw, (int)ch, px, py, 1, &refs);

        int mode;
        if (forced_mode >= 0) {
            mode = forced_mode;
        } else if (enc->mode_shortlist) {
            /* The frame-wide pass narrowed 35 modes to a handful; decide
             * among those against the REAL reconstructed neighbours.
             * Taking the pass's answer directly instead is measured at
             * +48% BD-rate - see hevc_rank_modes_by_cost()'s header note.
             * The MPMs are appended here rather than ranked there: they
             * are what the rate term would have promoted, and adding them
             * needs no knowledge the parallel pass could have had. */
            const uint8_t *sl = enc->mode_shortlist +
                ((size_t)(py / 4) * enc->mode_map_stride + (px / 4)) * enc->shortlist_n;
            int cands[HEVC_MODE_COUNT + 3];
            int nc = 0;
            for (int i = 0; i < enc->shortlist_n; i++) cands[nc++] = sl[i];
            for (int i = 0; i < 3; i++) cands[nc++] = mpm[i];
            mode = hevc_choose_among(&refs, enc->src_y, (int)cw, px, py, mpm, qp, cands, nc);
        } else {
            mode = hevc_choose_luma_mode_refs(&refs, enc->src_y, (int)cw, px, py, mpm, qp);
        }
        p->mode[pu] = mode;
        /* Only PU 0's signalling cost is real under PART_2Nx2N; the caller
         * accounts for that, this just reports the per-shape total. */
        p->mode_bits += hevc_mode_signal_bits(mode, mpm);

        uint8_t pred[16];
        hevc_predict_4x4_refs(&refs, mode, 1, pred);

        int16_t residual[16];
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int d = enc->src_y[(py + y) * cw + (px + x)] - pred[y * 4 + x];
                residual[y * 4 + x] = (int16_t)d;
                p->sad += d < 0 ? -d : d;
            }

        int16_t coeff[16];
        hevc_transform_quant_4x4(residual, qp, 1 /* DST for 4x4 luma intra */, coeff);
        memcpy(p->coeff[pu], coeff, sizeof(coeff));
        p->cbf[pu] = any_nonzero16(coeff);

        if (p->cbf[pu]) {
            int16_t recon_residual[16];
            hevc_dequant_itransform_4x4(coeff, qp, 1, recon_residual);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    enc->recon_y[(py + y) * cw + (px + x)] =
                        clip8i(pred[y * 4 + x] + recon_residual[y * 4 + x]);
        } else {
            /* All coefficients zero - reconstruction is the prediction. */
            for (int y = 0; y < 4; y++)
                memcpy(&enc->recon_y[(py + y) * cw + px], &pred[y * 4], 4);
        }

        enc->luma_mode_map[(py / 4) * enc->mode_map_stride + (px / 4)] = (int8_t)mode;
    }

    save_cu_luma(enc, cu_x, cu_y, p->recon);
}

/* Predict, transform, quantize and reconstruct one square block of a
 * plane, in place. Returns cbf. Shared by every luma and chroma TU at
 * every size - the only size-dependent choices are DST (4x4 luma intra
 * only, 8.6.4.1) and the reference gathering, both handled here. */
static int code_tu(hevc_encoder_t *enc, uint8_t *src, uint8_t *recon, int stride,
                    int pw, int ph, int x0, int y0, int log2, int mode, int is_luma,
                    int16_t *coeff_out) {
    int n = 1 << log2;
    int qp = enc->qp;

    hevc_refs_t refs;
    hevc_gather_refs_sz(recon, stride, pw, ph, x0, y0, log2, is_luma, &refs);

    uint8_t pred[HEVC_MAX_TB_SIZE * HEVC_MAX_TB_SIZE];
    hevc_predict_refs(&refs, log2, mode, is_luma, pred);

    int16_t res[HEVC_MAX_TB_SIZE * HEVC_MAX_TB_SIZE];
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            res[y * n + x] = (int16_t)(src[(size_t)(y0 + y) * stride + x0 + x] - pred[y * n + x]);

    int use_dst = (is_luma && log2 == 2);
    hevc_transform_quant(res, log2, qp, use_dst, coeff_out);

    int cbf = 0;
    for (int i = 0; i < n * n; i++) if (coeff_out[i]) { cbf = 1; break; }

    if (!cbf) {
        /* Every coefficient quantized to zero, so the inverse transform
         * would return all zeros and the reconstruction is exactly the
         * prediction. Worth special-casing rather than computing: on the
         * flat content that now gets large CUs this is the common case,
         * and an inverse transform is O(n^3). */
        for (int y = 0; y < n; y++)
            memcpy(&recon[(size_t)(y0 + y) * stride + x0], &pred[y * n], (size_t)n);
        return 0;
    }

    int16_t rres[HEVC_MAX_TB_SIZE * HEVC_MAX_TB_SIZE];
    hevc_dequant_itransform(coeff_out, log2, qp, use_dst, rres);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            recon[(size_t)(y0 + y) * stride + x0 + x] =
                clip8i(pred[y * n + x] + rres[y * n + x]);
    return cbf;
}

static void encode_cu(hevc_encoder_t *enc, hevc_cabac_t *cab, int cu_x, int cu_y) {
    int qp = enc->qp;
    uint32_t cw = enc->coded_width, ch = enc->coded_height;
    uint32_t ccw = cw / 2, cch = ch / 2;

    /* ------------------------------------------------------------------
     * Step 1: luma. Choose between PART_NxN (four signalled 4x4 modes) and
     * PART_2Nx2N (one signalled mode for the whole 8x8 CU).
     *
     * This choice is worth making rather than hardcoding NxN, because at
     * this CU size intra-mode side information dominates the bitstream
     * outright: measured on a 1080p desktop frame, going from QP 27 to QP
     * 32 moved the frame size by under 7%, i.e. residual was already
     * almost nothing and essentially the entire stream was 130560 PUs'
     * worth of intra_luma_pred_mode at ~1.9 bits each. Signalling one mode
     * per CU instead of four removes up to three quarters of that.
     *
     * PART_NxN forces IntraSplitFlag = 1, so its transform tree always
     * splits into four 4x4 luma TUs. PART_2Nx2N does not, so with
     * MaxTbLog2SizeY raised to 3 it gets a real third option: one 8x8
     * transform for the whole CU, chosen below. (PART_NxN remains legal
     * only because 8x8 is MinCbSizeY.)
     * ------------------------------------------------------------------ */
    uint8_t entry_recon[64];
    save_cu_luma(enc, cu_x, cu_y, entry_recon);
    int8_t entry_modes[4];
    for (int pu = 0; pu < 4; pu++)
        entry_modes[pu] = enc->luma_mode_map[((cu_y + pu_off_y[pu]) / 4) * enc->mode_map_stride +
                                              ((cu_x + pu_off_x[pu]) / 4)];

    cu_luma_pass_t nxn;
    run_luma_pass(enc, cu_x, cu_y, -1, &nxn);

    int part_2nx2n = 0;
    cu_luma_pass_t *chosen = &nxn;
    cu_luma_pass_t sq;

    if (nxn.mode[0] == nxn.mode[1] && nxn.mode[0] == nxn.mode[2] && nxn.mode[0] == nxn.mode[3]) {
        /* All four blocks independently wanted the same mode, so the
         * PART_2Nx2N encode is bit-for-bit the same prediction and
         * reconstruction for strictly fewer signalled modes. Free win, no
         * second pass needed. */
        part_2nx2n = 1;
    } else {
        /* Otherwise it is a real trade: one mode costs less to signal but
         * predicts the dissenting blocks worse. Re-run the CU forced to the
         * most popular of the four chosen modes and compare on the same
         * SAD-vs-bits scale hevc_choose_luma_mode() already uses
         * internally, so the two decision levels cannot disagree about what
         * a bit is worth. */
        int best_m = nxn.mode[0], best_count = 0;
        for (int i = 0; i < 4; i++) {
            int count = 0;
            for (int j = 0; j < 4; j++) if (nxn.mode[j] == nxn.mode[i]) count++;
            if (count > best_count) { best_count = count; best_m = nxn.mode[i]; }
        }

        restore_cu_luma(enc, cu_x, cu_y, entry_recon);
        for (int pu = 0; pu < 4; pu++)
            enc->luma_mode_map[((cu_y + pu_off_y[pu]) / 4) * enc->mode_map_stride +
                                ((cu_x + pu_off_x[pu]) / 4)] = entry_modes[pu];

        run_luma_pass(enc, cu_x, cu_y, best_m, &sq);

        long lambda = hevc_lambda_sad_q8(qp);
        /* PART_2Nx2N signals PU 0's mode only; run_luma_pass() summed all
         * four, so take a quarter of its (identical, same-mode) total. */
        long cost_nxn = nxn.sad + ((lambda * nxn.mode_bits) >> 8);
        long cost_sq  = sq.sad  + ((lambda * (sq.mode_bits / 4)) >> 8);

        if (cost_sq <= cost_nxn) {
            part_2nx2n = 1;
            chosen = &sq;
        } else {
            restore_cu_luma(enc, cu_x, cu_y, nxn.recon);
            for (int pu = 0; pu < 4; pu++)
                enc->luma_mode_map[((cu_y + pu_off_y[pu]) / 4) * enc->mode_map_stride +
                                    ((cu_x + pu_off_x[pu]) / 4)] = (int8_t)nxn.mode[pu];
        }
    }

    /* A single 8x8 transform for a PART_2Nx2N CU, instead of four 4x4
     * ones, was built and MEASURED: +0.4% BD-rate and 0.60x speed,
     * because trialling it costs a whole extra prediction and transform
     * per CU while the four 4x4 TUs' per-block reconstruction chaining is
     * worth about as much as the larger transform's energy compaction at
     * this size. Not kept. The transform-size lever pays off by making
     * CUs bigger (fewer of them), not by making the transform inside an
     * 8x8 CU bigger - see encode_ctu(). */
    hevc_cabac_code_part_mode_intra(cab, part_2nx2n);

    int *pu_modes = chosen->mode;
    int16_t (*luma_coeff)[16] = chosen->coeff;
    int *cbf_luma = chosen->cbf;

    /* Chroma: one 4x4 Cb + one 4x4 Cr per CU, DCT-II (never DST - DST is
     * luma-4x4-intra only, per spec).
     *
     * Cb and Cr share a single signalled intra_chroma_pred_mode, so the
     * five candidate indices are scored on the summed prediction error of
     * both planes. DM_CHROMA (index 4) is a single bin against the
     * others' three, which the same lambda the luma decision uses accounts
     * for. */
    int cx = cu_x / 2, cy = cu_y / 2;
    int chroma_idx = 4, chroma_mode = pu_modes[0];

    /* Both planes' reference samples are gathered once and reused across
     * all five candidates and the final prediction - 2 gathers per CU
     * instead of 12. */
    hevc_refs_t refs_cb, refs_cr;
    hevc_gather_refs(enc->recon_cb, (int)ccw, (int)ccw, (int)cch, cx, cy, 0, &refs_cb);
    hevc_gather_refs(enc->recon_cr, (int)ccw, (int)ccw, (int)cch, cx, cy, 0, &refs_cr);
    {
        long lambda = hevc_lambda_sad_q8(qp);
        long best = -1;
        for (int idx = 0; idx <= 4; idx++) {
            int m = hevc_chroma_mode_from_idx(idx, pu_modes[0]);
            uint8_t pb[16], pr[16];
            hevc_predict_4x4_refs(&refs_cb, m, 0, pb);
            hevc_predict_4x4_refs(&refs_cr, m, 0, pr);
            long sad = 0;
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    int db = enc->src_cb[(cy + y) * ccw + (cx + x)] - pb[y * 4 + x];
                    int dr = enc->src_cr[(cy + y) * ccw + (cx + x)] - pr[y * 4 + x];
                    sad += (db < 0 ? -db : db) + (dr < 0 ? -dr : dr);
                }
            long cost = sad + ((lambda * (idx == 4 ? 1 : 3)) >> 8);
            if (best < 0 || cost < best) { best = cost; chroma_idx = idx; chroma_mode = m; }
        }
    }

    uint8_t pred_cb[16], pred_cr[16];
    hevc_predict_4x4_refs(&refs_cb, chroma_mode, 0, pred_cb);
    hevc_predict_4x4_refs(&refs_cr, chroma_mode, 0, pred_cr);

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

    /* Step 2: emit the real intra_luma_pred_mode syntax - once for
     * PART_2Nx2N, or once per PU for PART_NxN. ITU-T H.265 7.3.8.5's
     * coding_unit() codes this as TWO separate passes over the PUs - every
     * prev_intra_luma_pred_flag first, THEN every mpm_idx /
     * rem_intra_luma_pred_mode - not interleaved per PU (see
     * hevc_cabac_code_intra_luma_flag()/_data()'s comment; getting this
     * order wrong was this encoder's first real bug, caught by comparing
     * this encoder's own reconstruction - which matched the source fine -
     * against ffmpeg's actual decode of the resulting bitstream, which
     * didn't: a CABAC bit-order mistake still produces a structurally
     * valid, crash-free bitstream, just one that decodes to noise from
     * that point on).
     *
     * The MPM lists are re-derived here rather than reused from the
     * decision pass because the decision pass may have been re-run and
     * rolled back; luma_mode_map now holds the committed modes, which is
     * what a decoder will have too. */
    int npu = part_2nx2n ? 1 : 4;
    int mpm[4][3];
    int pred_idx[4];
    for (int pu = 0; pu < npu; pu++) {
        derive_pu_mpm(enc, cu_x + pu_off_x[pu], cu_y + pu_off_y[pu], mpm[pu]);
        pred_idx[pu] = hevc_cabac_code_intra_luma_flag(cab, pu_modes[pu], mpm[pu]);
    }
    for (int pu = 0; pu < npu; pu++)
        hevc_cabac_code_intra_luma_data(cab, pu_modes[pu], pred_idx[pu], mpm[pu]);

    /* Step 3: chroma mode, as the index chosen above. */
    hevc_cabac_code_intra_chroma_pred_mode(cab, chroma_idx);

    /* Step 4: transform_tree - chroma cbf BITS first (trafoDepth=0, this
     * CU's root), then the 4 luma leaves' cbf+residual, then finally the
     * chroma RESIDUAL DATA (coded once per CU, after all 4 luma leaves -
     * this specific ordering, bits-before-luma but data-after-luma, is
     * exactly what ITU-T H.265's transform_tree()/transform_unit()
     * recursion produces for a CU whose chroma has already hit the 4x4
     * floor - see this file's top comment and x265's own
     * Entropy::encodeTransform(), which this encoder's fixed two-level
     * structure is a manually-unrolled special case of).
     *
     * split_transform_flag comes FIRST, before the chroma cbfs, and only
     * when the tree actually has a choice (7.3.8.8): PART_NxN sets
     * IntraSplitFlag, which both forces the split and suppresses the
     * flag, so it is coded only for PART_2Nx2N and only when
     * MaxTbLog2SizeY allows an 8x8 transform at all. */
    if (part_2nx2n && enc->max_tb_log2 > 3)
        hevc_cabac_code_split_transform_flag(cab, 1, 3);

    hevc_cabac_code_cbf_chroma(cab, cbf_cb, 0);
    hevc_cabac_code_cbf_chroma(cab, cbf_cr, 0);

    for (int pu = 0; pu < 4; pu++) {
        hevc_cabac_code_cbf_luma(cab, cbf_luma[pu], 1);
        if (cbf_luma[pu]) {
            int scan_idx = hevc_scan_idx_for_mode(pu_modes[pu]);
            hevc_cabac_code_residual_4x4(cab, luma_coeff[pu], 1, scan_idx);
        }
    }
    /* Mode-dependent coefficient scan applies to CHROMA too at this block
     * size: ITU-T H.265 7.4.9.11 derives scanIdx from predModeIntra
     * whenever log2TrafoSize is equal to 2, with no cIdx condition (the
     * cIdx==0 condition only appears in the separate log2TrafoSize==3
     * case). Every chroma TU here is 4x4, so the scan follows
     * IntraPredModeC. This was latent while chroma was hardcoded to DC -
     * DC derives scanIdx 0, which is what was passed - and would have
     * become a real wrong-scan bug the moment chroma stopped being DC. */
    int chroma_scan = hevc_scan_idx_for_mode(chroma_mode);
    if (cbf_cb) hevc_cabac_code_residual_4x4(cab, coeff_cb, 0, chroma_scan);
    if (cbf_cr) hevc_cabac_code_residual_4x4(cab, coeff_cr, 0, chroma_scan);
}

/* One undivided 16x16 CU: a single intra mode, a single 16x16 luma
 * transform and a single 8x8 chroma pair for the whole CTU.
 *
 * This is the lever the transform work exists for. Measured at MaxTb 4,
 * a 1080p desktop frame cost 9223 bytes at QP 18 and 8633 at QP 38 - a
 * 13% swing across a 20-QP range, i.e. residual was almost irrelevant
 * and the entire frame was per-CU syntax at ~2.2 bits each. Coding a
 * flat CTU as one CU instead of four (each of which may further split
 * into four PUs) removes three quarters to fifteen sixteenths of that.
 *
 * No part_mode is coded: 7.3.8.5 codes it only at MinCbLog2SizeY, so a
 * 16x16 CU is implicitly PART_2Nx2N. IntraSplitFlag is therefore 0, and
 * with MaxTbLog2SizeY == 4 the transform tree has a real choice at depth
 * 0, which is coded as "do not split". */
static void encode_cu16(hevc_encoder_t *enc, hevc_cabac_t *cab, int cu_x, int cu_y) {
    uint32_t cw = enc->coded_width, ch = enc->coded_height;
    uint32_t ccw = cw / 2, cch = ch / 2;
    int qp = enc->qp;

    int mpm[3];
    derive_pu_mpm(enc, cu_x, cu_y, mpm);

    hevc_refs_t refs;
    hevc_gather_refs_sz(enc->recon_y, (int)cw, (int)cw, (int)ch, cu_x, cu_y, 4, 1, &refs);
    int mode = hevc_choose_mode_sz(&refs, enc->src_y, (int)cw, cu_x, cu_y, 4, mpm, qp);

    int16_t luma_coeff[256];
    int cbf_luma = code_tu(enc, enc->src_y, enc->recon_y, (int)cw, (int)cw, (int)ch,
                            cu_x, cu_y, 4, mode, 1, luma_coeff);

    for (int by = 0; by < 4; by++)
        for (int bx = 0; bx < 4; bx++)
            enc->luma_mode_map[(cu_y / 4 + by) * enc->mode_map_stride + (cu_x / 4 + bx)] = (int8_t)mode;

    /* Chroma is 8x8 here. Its five candidate indices are scored the same
     * way as at 4x4, on the summed Cb+Cr prediction error. */
    int cx = cu_x / 2, cy = cu_y / 2;
    int chroma_idx = 4, chroma_mode = mode;
    {
        hevc_refs_t rcb, rcr;
        hevc_gather_refs_sz(enc->recon_cb, (int)ccw, (int)ccw, (int)cch, cx, cy, 3, 0, &rcb);
        hevc_gather_refs_sz(enc->recon_cr, (int)ccw, (int)ccw, (int)cch, cx, cy, 3, 0, &rcr);
        long lambda = hevc_lambda_sad_q8(qp);
        long best = -1;
        uint8_t pb[64], pr[64];
        for (int idx = 0; idx <= 4; idx++) {
            int m = hevc_chroma_mode_from_idx(idx, mode);
            hevc_predict_refs(&rcb, 3, m, 0, pb);
            hevc_predict_refs(&rcr, 3, m, 0, pr);
            long sad = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int db = enc->src_cb[(cy + y) * ccw + cx + x] - pb[y * 8 + x];
                    int dr = enc->src_cr[(cy + y) * ccw + cx + x] - pr[y * 8 + x];
                    sad += (db < 0 ? -db : db) + (dr < 0 ? -dr : dr);
                }
            long cost = sad + ((lambda * (idx == 4 ? 1 : 3)) >> 8);
            if (best < 0 || cost < best) { best = cost; chroma_idx = idx; chroma_mode = m; }
        }
    }

    int16_t coeff_cb[64], coeff_cr[64];
    int cbf_cb = code_tu(enc, enc->src_cb, enc->recon_cb, (int)ccw, (int)ccw, (int)cch,
                          cx, cy, 3, chroma_mode, 0, coeff_cb);
    int cbf_cr = code_tu(enc, enc->src_cr, enc->recon_cr, (int)ccw, (int)ccw, (int)cch,
                          cx, cy, 3, chroma_mode, 0, coeff_cr);

    /* ---- syntax ---- */
    int pred_idx = hevc_cabac_code_intra_luma_flag(cab, mode, mpm);
    hevc_cabac_code_intra_luma_data(cab, mode, pred_idx, mpm);
    hevc_cabac_code_intra_chroma_pred_mode(cab, chroma_idx);

    hevc_cabac_code_split_transform_flag(cab, 0, 4);
    hevc_cabac_code_cbf_chroma(cab, cbf_cb, 0);
    hevc_cabac_code_cbf_chroma(cab, cbf_cr, 0);

    hevc_cabac_code_cbf_luma(cab, cbf_luma, 0);
    /* 7.4.9.11 derives a mode-dependent scan only at log2TrafoSize 2, or
     * 3 for luma. A 16x16 luma TU and an 8x8 chroma TU are both outside
     * that, so both scan diagonally. */
    if (cbf_luma) hevc_cabac_code_residual(cab, luma_coeff, 4, 1, 0);
    if (cbf_cb) hevc_cabac_code_residual(cab, coeff_cb, 3, 0, 0);
    if (cbf_cr) hevc_cabac_code_residual(cab, coeff_cr, 3, 0, 0);
}

/* Is this block flat enough that one big prediction will do?
 *
 * Merging four CUs into one saves roughly three CUs' worth of syntax -
 * measured at ~2.2 bits each - so the RD-justified distortion budget is
 * lambda_sse * ~6.6 bits, which at QP 27 is well under one grey level of
 * RMS error per pixel. In other words a merge only pays on content that
 * is genuinely flat, and the test is whether the block's deviation from
 * its own mean is small next to the quantizer step - if it is, the
 * residual quantizes to nothing either way and the big CU is free.
 *
 * This is a structure decision taken from the source picture, which is
 * safe in a way the equivalent MODE decision is not: getting it wrong
 * costs bits and a little distortion, whereas choosing a prediction mode
 * against source neighbours desynchronizes the encoder from the decoder
 * outright (measured at +48% BD-rate, see hevc_intra.h). */
static int block_is_flat(const hevc_encoder_t *enc, int x0, int y0, int n) {
    uint32_t cw = enc->coded_width;
    long sum = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            sum += enc->src_y[(size_t)(y0 + y) * cw + x0 + x];
    long mean = sum / (n * n);

    long mad = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            long d = (long)enc->src_y[(size_t)(y0 + y) * cw + x0 + x] - mean;
            mad += d < 0 ? -d : d;
        }
    mad = mad / (n * n);

    /* Quantizer step, roughly 2^((qp-4)/6), in 1/16ths to keep it in
     * integers at low QP. */
    int qp = enc->qp;
    long qstep16 = (16L << (qp / 6)) >> 1;
    if (qp % 6) qstep16 = (qstep16 * (100 + 12 * (qp % 6))) / 100;

    return (mad * 16 * enc->flat_num) <= (qstep16 * enc->flat_den);
}

static void encode_ctu(hevc_encoder_t *enc, hevc_cabac_t *cab, int ctu_col, int ctu_row) {
    int ctu_x = ctu_col * HEVC_CTU_SIZE, ctu_y = ctu_row * HEVC_CTU_SIZE;
    int cond_l = ctu_col > 0 ? 1 : 0;
    int cond_a = ctu_row > 0 ? 1 : 0;

    int split = 1;
    if (enc->max_tb_log2 >= 4 && block_is_flat(enc, ctu_x, ctu_y, HEVC_CTU_SIZE))
        split = 0;

    /* 9.3.4.2.2: ctxInc counts neighbours coded at a GREATER depth than
     * this node. At the CTU root cqtDepth is 0, so that means neighbours
     * that were themselves split. This was a constant while every CTU
     * split unconditionally; now it has to read the real depth map. */
    int mx = ctu_x / 4, my = ctu_y / 4;
    int cl = cond_l && enc->cu_depth_map[my * enc->mode_map_stride + mx - 1] > 0;
    int ca = cond_a && enc->cu_depth_map[(my - 1) * enc->mode_map_stride + mx] > 0;
    hevc_cabac_code_split_cu_flag(cab, split, cl + ca);

    /* Record this CTU's depth for the neighbours that will read it. */
    for (int by = 0; by < HEVC_CTU_SIZE / 4; by++)
        memset(&enc->cu_depth_map[(my + by) * enc->mode_map_stride + mx],
               (uint8_t)split, HEVC_CTU_SIZE / 4);

    if (!split) {
        encode_cu16(enc, cab, ctu_x, ctu_y);
        return;
    }

    static const int cu_off_x[4] = { 0, 8, 0, 8 };
    static const int cu_off_y[4] = { 0, 0, 8, 8 };
    for (int i = 0; i < 4; i++)
        encode_cu(enc, cab, ctu_x + cu_off_x[i], ctu_y + cu_off_y[i]);
}

/* ============================================================================
 * GPU path: entropy-code decisions the shader already made
 *
 * hevc_intra_wavefront.comp does prediction, transform, quantization and
 * reconstruction for every CTU and hands back, per CTU: the luma mode,
 * 384 coefficients (256 luma, 64 Cb, 64 Cr) and a flags word packing
 * cbf_luma / cbf_cb / cbf_cr and the chroma mode index. All that is left
 * is the one part that cannot be parallelized - CABAC - which measured
 * at ~0.7% of the CPU frame.
 *
 * The syntax emitted here is exactly encode_cu16()'s, so the two paths
 * produce the same bitstream shape: every CTU is one undivided 16x16 CU,
 * which is why split_cu_flag is 0, part_mode is absent (7.3.8.5 codes it
 * only at MinCbLog2SizeY) and split_transform_flag is 0.
 * ==========================================================================*/
static int encode_core_gpu(hevc_encoder_t *enc,
                            const int32_t *gmodes, const int32_t *gcoeffs,
                            const uint32_t *gflags,
                            uint8_t *output_buf, size_t output_size)
{
    uint32_t wc = enc->width_ctu, hc = enc->height_ctu;

    memset(enc->luma_mode_map, 0,
           (size_t)enc->mode_map_stride * (enc->coded_height / HEVC_PU_SIZE));
    memset(enc->cu_depth_map, 0,
           (size_t)enc->mode_map_stride * (enc->coded_height / HEVC_PU_SIZE));

    bitstream_t slice_bs;
    bs_init(&slice_bs, enc->slice_rbsp, enc->slice_rbsp_cap);
    bs_write1(&slice_bs, 1);            /* first_slice_segment_in_pic_flag */
    bs_write1(&slice_bs, 1);            /* no_output_of_prior_pics_flag */
    bs_write_ue(&slice_bs, 0);          /* slice_pic_parameter_set_id */
    bs_write_ue(&slice_bs, 2);          /* slice_type = I */
    bs_write_se(&slice_bs, 0);          /* slice_qp_delta */
    bs_write1(&slice_bs, 1);            /* slice_loop_filter_across_slices_enabled_flag */
    bs_rbsp_trailing_bits(&slice_bs);   /* byte_alignment() - see encode_core */

    hevc_cabac_t cab;
    hevc_cabac_init(&cab, &slice_bs);
    hevc_cabac_reset_contexts(&cab, enc->qp);
    hevc_cabac_start(&cab);

    int16_t coef[256];
    uint32_t total_ctus = wc * hc;
    uint32_t idx = 0;

    for (uint32_t row = 0; row < hc; row++) {
        for (uint32_t col = 0; col < wc; col++) {
            uint32_t ci = row * wc + col;
            int cu_x = (int)col * HEVC_CTU_SIZE, cu_y = (int)row * HEVC_CTU_SIZE;
            int mode = gmodes[ci];
            if (mode < 0 || mode >= HEVC_MODE_COUNT) mode = HEVC_MODE_DC;
            uint32_t flags = gflags[ci];
            int cbf_luma = (int)(flags & 1u);
            int cbf_cb   = (int)((flags >> 1) & 1u);
            int cbf_cr   = (int)((flags >> 2) & 1u);
            int chroma_idx = (int)((flags >> 8) & 0xffu);
            if (chroma_idx > 4) chroma_idx = 4;
            int chroma_mode = hevc_chroma_mode_from_idx(chroma_idx, mode);

            /* Every CTU is one 16x16 CU, so depth 0 everywhere and the
             * split_cu_flag context is always 0 + 0. */
            hevc_cabac_code_split_cu_flag(&cab, 0, 0);

            int mpm[3];
            derive_pu_mpm(enc, cu_x, cu_y, mpm);
            int pred_idx = hevc_cabac_code_intra_luma_flag(&cab, mode, mpm);
            hevc_cabac_code_intra_luma_data(&cab, mode, pred_idx, mpm);
            hevc_cabac_code_intra_chroma_pred_mode(&cab, chroma_idx);

            /* The mode map has to be updated as we go: the next CTU's MPM
             * list reads it. */
            for (int by = 0; by < HEVC_CTU_SIZE / 4; by++)
                for (int bx = 0; bx < HEVC_CTU_SIZE / 4; bx++)
                    enc->luma_mode_map[(cu_y / 4 + by) * enc->mode_map_stride + (cu_x / 4 + bx)] =
                        (int8_t)mode;

            hevc_cabac_code_split_transform_flag(&cab, 0, 4);
            hevc_cabac_code_cbf_chroma(&cab, cbf_cb, 0);
            hevc_cabac_code_cbf_chroma(&cab, cbf_cr, 0);
            hevc_cabac_code_cbf_luma(&cab, cbf_luma, 0);

            const int32_t *src = &gcoeffs[(size_t)ci * 384];
            /* 16x16 luma and 8x8 chroma are both outside the sizes
             * 7.4.9.11 gives a mode-dependent scan, so all three are
             * diagonal. */
            if (cbf_luma) {
                for (int i = 0; i < 256; i++) coef[i] = (int16_t)src[i];
                hevc_cabac_code_residual(&cab, coef, 4, 1, 0);
            }
            if (cbf_cb) {
                for (int i = 0; i < 64; i++) coef[i] = (int16_t)src[256 + i];
                hevc_cabac_code_residual(&cab, coef, 3, 0, 0);
            }
            if (cbf_cr) {
                for (int i = 0; i < 64; i++) coef[i] = (int16_t)src[320 + i];
                hevc_cabac_code_residual(&cab, coef, 3, 0, 0);
            }
            (void)chroma_mode;

            idx++;
            hevc_cabac_encode_terminate(&cab, idx == total_ctus ? 1 : 0);
        }
    }

    hevc_cabac_finish(&cab);
    bs_rbsp_trailing_bits(&slice_bs);

    size_t total = 0;
    total += write_vps(enc->scratch_out + total, enc->scratch_out_cap - total);
    total += write_sps(enc->scratch_out + total, enc->scratch_out_cap - total,
                        enc->coded_width, enc->coded_height, enc->width, enc->height,
                        hevc_pick_level_idc(enc->coded_width, enc->coded_height),
                        enc->max_tb_log2);
    total += write_pps(enc->scratch_out + total, enc->scratch_out_cap - total, enc->qp);
    {
        bitstream_t out_bs;
        bs_init(&out_bs, enc->scratch_out + total, enc->scratch_out_cap - total);
        bs_write_nal_header_hevc(&out_bs, NAL_UNIT_CODED_SLICE_IDR_W_RADL);
        size_t off = bs_bytes_written(&out_bs);
        size_t ebsp = bs_rbsp_to_ebsp(enc->scratch_out + total + off,
                                       enc->scratch_out_cap - total - off,
                                       enc->slice_rbsp, bs_bytes_written(&slice_bs));
        total += off + ebsp;
    }

    if (total > output_size) return -1;
    memcpy(output_buf, enc->scratch_out, total);
    enc->frame_count++;
    return (int)total;
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
    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

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

    /* ------------------------------------------------------------------
     * Frame-wide intra distortion pass.
     *
     * THIS LOOP IS THE OFFLOAD TARGET. Every iteration is independent -
     * it reads only the source picture and writes only its own block's
     * 35 costs - so it can run in any order, or all at once on the GPU as
     * one dispatch of (coded_width/4) x (coded_height/4) invocations. It
     * is written here as a plain CPU loop so the decision it produces can
     * be validated against the serial encoder before any shader exists.
     *
     * By profile this is ~64% of the frame's CPU time (prediction, the
     * neighbour substitution scan, and SAD), and it is the only large
     * part of an intra HEVC encode that is not inherently serial: the
     * reconstruction chain that follows genuinely cannot be parallelized
     * at 4x4 granularity, so ~2.8x is the honest Amdahl ceiling for this
     * decomposition, not "make H.265 fast".
     * ------------------------------------------------------------------ */
    double rmd_ms = 0.0;
    if (encoder->mode_shortlist) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int cw = (int)encoder->coded_width, chh = (int)encoder->coded_height;
        int nsl = encoder->shortlist_n;
        for (int by = 0; by < chh / HEVC_PU_SIZE; by++) {
            for (int bx = 0; bx < cw / HEVC_PU_SIZE; bx++) {
                /* The 35 costs live and die inside this iteration - they
                 * never reach memory, which is the whole point. Only the
                 * ranked shortlist is written out. */
                uint16_t costs[HEVC_MODE_COUNT];
                hevc_block_mode_costs(encoder->src_y, encoder->src_y, cw, cw, chh,
                                       bx * HEVC_PU_SIZE, by * HEVC_PU_SIZE, costs);
                uint8_t *sl = encoder->mode_shortlist +
                    ((size_t)by * encoder->mode_map_stride + bx) * nsl;
                /* Pure distortion ranking - no rate term, because the MPM
                 * list a rate term needs depends on neighbouring blocks'
                 * decisions and would reintroduce exactly the serial
                 * dependency this pass exists to avoid. The caller adds
                 * the MPMs to the candidate set itself, which costs
                 * nothing and covers what the rate term would have
                 * promoted. */
                hevc_rank_modes_by_cost(costs, nsl, sl);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        rmd_ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    }

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
                        hevc_pick_level_idc(encoder->coded_width, encoder->coded_height),
                        encoder->max_tb_log2);
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

    /* Reports how much of the frame is the offloadable distortion pass
     * versus the serial remainder, which is the only number that says
     * what moving that pass to the GPU can actually buy. */
    if (getenv("BC250_HEVC_PROFILE")) {
        struct timespec tend;
        clock_gettime(CLOCK_MONOTONIC, &tend);
        double total_ms = (tend.tv_sec - t_start.tv_sec) * 1e3 +
                          (tend.tv_nsec - t_start.tv_nsec) / 1e6;
        fprintf(stderr, "[hevc-profile] total=%.1fms parallel_rmd=%.1fms serial=%.1fms (%.0f%% offloadable)\n",
                total_ms, rmd_ms, total_ms - rmd_ms,
                total_ms > 0 ? 100.0 * rmd_ms / total_ms : 0.0);
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
    /* GPU reconstruction path. Opt-in for now: it is bit-exact against
     * the CPU for the structure it supports (one undivided 16x16 CU per
     * CTU) but does not yet implement the split cases, so it trades some
     * compression for a large amount of speed. */
    if (encoder->use_gpu && gpu_ctx && input_surface.y_plane != VK_NULL_HANDLE) {
        if (gpu_compute_begin_picture(gpu_ctx, input_surface) == 0 &&
            gpu_compute_hevc_dispatch_intra(gpu_ctx, input_surface,
                                             (int)encoder->coded_width, (int)encoder->coded_height,
                                             (int)encoder->width, (int)encoder->height,
                                             encoder->qp) == 0) {
            gpu_compute_end_picture(gpu_ctx);
            gpu_compute_sync(gpu_ctx);

            void *mp = NULL, *cp = NULL, *fp = NULL;
            size_t mn = 0, cn = 0, fn = 0;
            if (gpu_compute_get_quant_staging_data(gpu_ctx, &mp, &mn) == 0 &&
                gpu_compute_get_coeff_staging_data(gpu_ctx, &cp, &cn) == 0 &&
                gpu_compute_get_pred_mode_staging_data(gpu_ctx, &fp, &fn) == 0) {
                return encode_core_gpu(encoder, (const int32_t *)mp, (const int32_t *)cp,
                                        (const uint32_t *)fp, output_buf, output_size);
            }
            fprintf(stderr, "[bc250-h265] GPU readback failed, falling back to CPU\n");
        } else {
            fprintf(stderr, "[bc250-h265] GPU dispatch unavailable, falling back to CPU\n");
        }
        /* Fall through to the CPU path below rather than failing the
         * frame - a missing shader must not take the encoder down. */
    }

    if (gpu_ctx && input_surface.y_plane != VK_NULL_HANDLE) {
        gpu_compute_begin_picture(gpu_ctx, input_surface);
        gpu_compute_dispatch_encode(gpu_ctx, input_surface, encoder->width, encoder->height,
                                     encoder->qp, 1, 1);
        gpu_compute_end_picture(gpu_ctx);
        gpu_compute_sync(gpu_ctx);

        /* input_surface/input_memory is the live VA-API surface a real
         * Sunshine session writes into directly via its own GL blit, into
         * this surface's exported DMA-BUF - a completely separate GPU
         * context/API/process from this driver's Vulkan one, with nothing
         * shared between them to order this CPU read against that write
         * (see gpu_compute.h's gpu_compute_dmabuf_sync_start() doc comment,
         * and gpu_compute_debug_dump_real_input(), whose dumps are what
         * first confirmed this race as content-dependent block corruption
         * in the raw captured frame). This is the real, on-the-encode-path
         * equivalent of that debug dump: without the sync bracket here,
         * whatever HEVC actually encodes is subject to the exact same race,
         * not just a diagnostic capture of it. */
        gpu_compute_dmabuf_sync_start(gpu_ctx, input_memory);
        gpu_compute_download_nv12(gpu_ctx, &input_surface, input_memory,
                                   encoder->dl_y, (int)encoder->width,
                                   encoder->dl_uv, (int)encoder->width,
                                   (int)encoder->width, (int)encoder->height);
        gpu_compute_dmabuf_sync_end(gpu_ctx, input_memory);
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
