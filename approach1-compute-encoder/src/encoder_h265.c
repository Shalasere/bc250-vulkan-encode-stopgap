/* bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * encoder_h265.c - H.265/HEVC encoder supporting IDR (I-slices) and
 *                  inter-frame prediction (P-slices with zero-motion CU skip).
 *
 * ============================================================================
 * DESIGN, in one place (see docs/hevc_scope_note.md and DEVLOG.md Sec. 6/8/27
 * for the history of why this replaced a non-functional stub)
 * ============================================================================
 *
 * This encoder supports GOP structures with periodic/forced IDR frames and
 * inter-predicted P-frames. For P-frames:
 *   - Reference picture set (RPS) is configured with DeltaPOC = -1 pointing
 *     to the previous reconstructed frame in the DPB.
 *   - NAL unit type is NAL_UNIT_CODED_SLICE_TRAIL_R (1) with 4-bit POC LSB.
 *   - Each 8x8 CU evaluates temporal difference against the reference picture.
 *     Static / low-motion blocks are coded as SKIP CUs (cu_skip_flag = 1,
 *     merge_idx = 0) with zero residual and zero motion vector, yielding
 *     immense bitrate reduction on typical desktop / streaming video.
 *   - Dynamic blocks are coded with cu_skip_flag = 0 and pred_mode_flag = 1
 *     (MODE_INTRA) falling back to full intra prediction and transform coding.
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
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#define NAL_UNIT_VPS               32
#define NAL_UNIT_SPS               33
#define NAL_UNIT_PPS               34
#define NAL_UNIT_CODED_SLICE_TRAIL_R     1
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
    bs_write_ue(&bs, 1); /* vps_max_dec_pic_buffering_minus1 = 1 (1 ref + 1 current pic) */
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

/* `max_tb_log2` is log2 of the largest transform block the coder will use:
 * 2 (MaxTb = MinTb = 4) for the CPU path's all-4x4 transform tree, 4
 * (MaxTb = 16) for the GPU intra path's single 16x16 luma TU. It only
 * changes log2_diff_max_min_transform_block_size; MinTb stays 4 either way.
 * max_transform_hierarchy_depth_intra stays 0, which combined with
 * IntraSplitFlag gives MaxTrafoDepth = IntraSplitFlag - so the CPU path's
 * NxN CUs still split once to 4x4 and the GPU path's 2Nx2N CU does not
 * split at all. */
static size_t write_sps(uint8_t *buf, size_t buf_size, uint32_t coded_w, uint32_t coded_h,
                         uint32_t real_w, uint32_t real_h, int level_idc,
                         int max_tb_log2) {
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
    bs_write_ue(&bs, 1); /* sps_max_dec_pic_buffering_minus1 = 1 (1 ref + 1 current pic) */
    bs_write_ue(&bs, 0); /* sps_num_reorder_pics */
    bs_write_ue(&bs, 0); /* sps_max_latency_increase_plus1 */

    bs_write_ue(&bs, 0); /* log2_min_luma_coding_block_size_minus3 -> MinCb = 8 */
    bs_write_ue(&bs, 1); /* log2_diff_max_min_coding_block_size -> Ctb = 16 */
    bs_write_ue(&bs, 0); /* log2_min_luma_transform_block_size_minus2 -> MinTb = 4 */
    bs_write_ue(&bs, (uint32_t)(max_tb_log2 - 2)); /* log2_diff_max_min_transform_block_size */
    bs_write_ue(&bs, 0); /* max_transform_hierarchy_depth_inter */
    bs_write_ue(&bs, 0); /* max_transform_hierarchy_depth_intra (IntraSplitFlag adds +1 -> MaxTrafoDepth=1) */

    bs_write1(&bs, 0); /* scaling_list_enabled_flag */
    bs_write1(&bs, 0); /* amp_enabled_flag */
    bs_write1(&bs, 0); /* sample_adaptive_offset_enabled_flag */
    bs_write1(&bs, 0); /* pcm_enabled_flag */

    bs_write_ue(&bs, 1); /* num_short_term_ref_pic_sets = 1 */
    /* short_term_ref_pic_set(0) per Rec. ITU-T H.265 7.3.7 */
    bs_write_ue(&bs, 1); /* num_negative_pics = 1 */
    bs_write_ue(&bs, 0); /* num_positive_pics = 0 */
    bs_write_ue(&bs, 0); /* delta_poc_s0_minus1[0] = 0 -> DeltaPoc = -(0+1) = -1 */
    bs_write1(&bs, 1);   /* used_by_curr_pic_s0_flag[0] = 1 */

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
    /* Deblocking is signalled OFF, because this encoder does not simulate
     * it. Leaving it on (which is what deblocking_filter_control_present_
     * flag = 0 means - HEVC's default is ENABLED, ITU-T H.265 7.4.3.3.1)
     * costs almost nothing all-intra, but it is expensive on P-frames and
     * it compounds: the encoder's reference picture for frame N+1 is its
     * own UNFILTERED reconstruction of frame N, while the decoder's is the
     * FILTERED one, so the two reference chains walk apart a little more
     * with every P-frame and only an IDR resets them. Measured off-board
     * (testsrc2 640x480, CQP 27, 60 frames, gop 120): the encoder's own
     * reconstruction scores 46.35 dB while the real deblocking decoder
     * scores 37.23 - a 9.1 dB loss that is entirely this divergence, and
     * that a drift oracle run with -skip_loop_filter all cannot see.
     * All-intra (gop 1) loses 0.02 dB to the same filter.
     *
     * This was TRIED ONCE BEFORE (2026-09-19) and reverted after PSNR
     * collapsed to 4.85-9.78 dB on hardware; the note left behind called
     * it "a genuine bitstream syntax error of its own". It was. Two bits
     * were missing, and both are now here:
     *
     *   - deblocking_filter_override_enabled_flag. ITU-T H.265 7.3.2.3.1
     *     codes it between deblocking_filter_control_present_flag and
     *     pps_deblocking_filter_disabled_flag. Skipping it shifts every
     *     later PPS bit by one - the decoder reads a scaling list flag
     *     out of the deblocking flag, and so on to the end of the RBSP.
     *   - slice_loop_filter_across_slices_enabled_flag, in the SLICE
     *     header, whose presence condition (7.3.6.1) is
     *     `pps_loop_filter_across_slices_enabled_flag && (slice_sao_luma
     *     || slice_sao_chroma || !slice_deblocking_filter_disabled_flag)`.
     *     With SAO off and deblocking now disabled that is false, so the
     *     bit must NOT be written any more - and the slice header writers
     *     below no longer write it. Emitting it anyway desynchronises the
     *     byte alignment the CABAC engine starts from, i.e. the entire
     *     slice payload.
     *
     * The earlier attempt had no off-board oracle, so a syntax error and a
     * quality regression looked the same. Now they don't:
     * tools/hevc_host_drift.sh decodes the real bitstream and compares it
     * byte for byte against the encoder's reconstruction, and it is
     * 28/28 exact with this change - including the inter cases. Crucially
     * it no longer passes -skip_loop_filter all, so if these PPS bits did
     * not parse as intended the decoder would still be filtering and
     * every case would fail. That is the check the 2026-09-19 attempt
     * did not have. */
    bs_write1(&bs, 1);   /* deblocking_filter_control_present_flag */
    bs_write1(&bs, 0);   /* deblocking_filter_override_enabled_flag */
    bs_write1(&bs, 1);   /* pps_deblocking_filter_disabled_flag */
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
    uint32_t gop_size;
    uint32_t poc;
    bool     force_idr;
    bool     has_ref;
    int qp;
    int pps_init_qp;
    /* Last QP explicitly handed to hevc_encoder_set_qp(), or -1 if never
     * called yet. Distinct from `qp` above, which pick_frame_qp() also
     * overwrites every frame with the rate controller's own decision - see
     * hevc_encoder_set_qp()'s doc comment (docs/backlog.md D1). */
    int qp_hint_applied;
    rate_control_t rc;
    uint32_t quality_level;      /* 1..7 (1 = Quality, 4 = Balanced, 7 = Speed) */
    uint32_t max_frame_bits;     /* Maximum frame size in bits (0 = unlimited) */

    /* Source (post-download, padded/replicated to coded dimensions) and
     * reconstructed planes. Luma at coded_w x coded_h; chroma at
     * coded_w/2 x coded_h/2 (4:2:0). */
    uint8_t *src_y, *src_cb, *src_cr;
    uint8_t *recon_y, *recon_cb, *recon_cr;
    uint8_t *prev_recon_y, *prev_recon_cb, *prev_recon_cr;

    /* Per-CU skip tracking for current frame (for condL/condA context derivation).
     * Size: (width_ctu * 2) * (height_ctu * 2). */
    uint8_t *cu_skip_map;

    /* Inter prediction & motion vector maps (for spatial merge candidate derivation).
     * Size: (width_ctu * 2) * (height_ctu * 2). MVs in 1/4-pel units. */
    uint8_t *cu_is_inter;
    int16_t *mv_x_map;
    int16_t *mv_y_map;
    uint32_t last_frame_sad;

    /* GPU compute motion vector readback for acceleration */
    gpu_mv_t *gpu_mvs;
    uint32_t num_gpu_mvs;

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

    /* GPU intra path (BC250_HEVC_GPU=1), latched once at create time so a
     * mid-stream getenv() can't change the coding structure between frames -
     * it selects a DIFFERENT SPS (MaxTb 16 rather than 4), so switching after
     * the parameter sets are out would desynchronise the decoder.
     *
     * The two paths are structurally different coders, not fast/slow variants
     * of one: the CPU path splits every CTU into four 8x8 NxN-intra CUs of
     * 4x4 TUs, while the GPU path codes one 16x16 2Nx2N CU with a single
     * 16x16 luma TU and 8x8 chroma. The GPU path is all-intra only - it has
     * no inter/merge path at all, so it forces every frame to IDR. */
    bool use_gpu;

    /* Second, independent opt-in on top of use_gpu (BC250_HEVC_GPU_PFRAME=1),
     * latched at the same time and for the same reason (see use_gpu's
     * comment - this changes what the SPS says about DPB/reordering, so it
     * cannot toggle mid-stream either). Kept separate from use_gpu itself so
     * that BC250_HEVC_GPU=1 alone reproduces the exact board-validated
     * all-intra behaviour (docs/hevc-gpu-intra.md) with zero code-path
     * change - this feature (docs/notes/c7-gpu-pframes.md) has not had a
     * board run at all yet, and C5's decision to keep HEVC opt-in already
     * establishes the precedent of not changing validated default behaviour
     * for an unvalidated feature. */
    bool use_gpu_pframe;

    /* GPU-path P-frame zero-motion skip. One uint32 per CTU
     * (width_ctu * height_ctu entries), nonzero meaning "this CTU is SKIP".
     * docs/notes/c7-gpu-pframes.md's first cut decided this on the host and
     * uploaded it as the shader's skip_mask; docs/notes/
     * c7-pframe-throughput.md moved the decision itself onto the GPU
     * (hevc_pframe_skip.comp) - this array is now filled by copying back
     * gpu_compute_get_hevc_skip_data_slot()'s result after a sync, not by
     * computing it here (decide_gpu_ctu_skips() below still computes it
     * host-side, but only for the off-board hevc_encoder_encode_gpu_raw()
     * test path, which has no GPU to run that shader on). Either way it
     * doubles as the host's own bookkeeping for cu_skip_flag ctxInc and the
     * DC-for-skip-neighbour MPM rule (see encode_core_gpu()). Unlike the CPU path's
     * cu_skip_map/cu_is_inter (per-8x8-CU, 4 per CTU), this path has
     * exactly one CU per CTU, so one entry per CTU is enough - and every
     * inter CU this path ever signals is a SKIP with motion (0,0) by
     * construction (real motion compensation is out of scope, matching
     * the CPU path's own "zero-motion skip plus intra fallback" - see
     * docs/backlog.md C9's "still open after this"), so there is no
     * separate is_inter/mv map to keep: is_inter == is_skip here. */
    uint32_t *gpu_ctu_skip;

    /* Scratch for de-interleaving ctx->recon_image's packed NV12 UV plane
     * (read back via gpu_compute_hevc_download_recon_nv12()) into the
     * planar prev_recon_cb/prev_recon_cr above, coded chroma dimensions -
     * same role dl_uv plays for the source download, just for the
     * reference instead. Only touched on a P-frame candidate. */
    uint8_t *gpu_recon_uv_scratch;

    /* Set when this frame's QP has already been chosen, so the encode_core*
     * functions don't choose it a second time.
     *
     * The GPU path MUST decide QP before dispatch, because the shader
     * quantizes with the value it is handed - deciding afterwards quantized
     * at the previous frame's QP while signalling the new one, which is a
     * real decoder-visible corruption, not just a rate miss (see the call
     * site in hevc_encoder_encode_frame). And rc_get_frame_qp() is NOT a
     * pure getter: it advances current_qp and error_integral, so calling it
     * twice in one frame double-steps rate control. Hence a flag rather
     * than just calling it again. */
    bool qp_already_decided;
};

static uint32_t round_up16(uint32_t v) { return (v + 15u) & ~15u; }

hevc_encoder_t *hevc_encoder_create(bc250_gpu_context_t *gpu_ctx,
                                    uint32_t width, uint32_t height,
                                    uint32_t fps, uint32_t bitrate)
{
    /* A 4:2:0 picture has one chroma sample per 2x2 luma block, so below 2
     * in either axis there is no chroma plane at all: encode_core()'s
     * chroma pad_replicate() gets src_w/src_h = 0 and computes `src_h - 1`
     * on a uint32_t, which wraps to 4294967295 and reads off into space.
     * ASan SEGV at 1x1, caught while sweeping odd sizes. Refuse the size
     * rather than crash - this returns NULL like every other create-time
     * failure here, and 1x1 is not a picture anyone can encode anyway.
     * Sizes below one 16x16 CTU are fine and are covered by the drift
     * oracle (4x4): the picture is coded at 16x16 and the conformance
     * window crops the rest away. */
    if (width < 2 || height < 2) return NULL;

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
    enc->pps_init_qp = enc->qp;
    enc->qp_hint_applied = -1; /* no explicit QP hint applied yet - see hevc_encoder_set_qp() */
    rc_init(&enc->rc, RC_CQP, bitrate, (double)enc->fps, width, height);
    enc->rc.current_qp = enc->qp;
    enc->rc.base_qp = enc->qp;
    enc->quality_level = 4;
    enc->max_frame_bits = 0;

    enc->gop_size = enc->fps;
    {
        const char *gop_env = getenv("BC250_HEVC_GOP");
        if (gop_env) {
            int g = atoi(gop_env);
            if (g >= 1) enc->gop_size = (uint32_t)g;
        }
    }
    enc->poc = 0;
    enc->force_idr = false;
    enc->has_ref = false;

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
    enc->prev_recon_y = malloc(luma_size);
    enc->prev_recon_cb = malloc(chroma_size);
    enc->prev_recon_cr = malloc(chroma_size);

    size_t num_cus = (size_t)(enc->width_ctu * 2) * (enc->height_ctu * 2);
    enc->cu_skip_map = calloc(num_cus, 1);
    enc->cu_is_inter = calloc(num_cus, 1);
    enc->mv_x_map = calloc(num_cus, sizeof(int16_t));
    enc->mv_y_map = calloc(num_cus, sizeof(int16_t));

    enc->mode_map_stride = enc->coded_width / HEVC_PU_SIZE;
    enc->luma_mode_map = malloc((size_t)enc->mode_map_stride * (enc->coded_height / HEVC_PU_SIZE));

    enc->dl_y = malloc((size_t)width * height);
    /* Both writers of dl_uv (hevc_encoder_encode_raw()'s memcpy and
     * gpu_compute_download_nv12()) use a row stride of `width`, not
     * `(width/2)*2` - so for an odd width the old `(width/2)*(height/2)*2`
     * was one byte per row short and the last row's copy ran off the end of
     * the allocation. Size it from the stride that is actually used. */
    enc->dl_uv = malloc((size_t)width * ((height + 1) / 2));

    /* An 8-bit 4:2:0 picture is 12 bits per luma sample uncompressed, i.e.
     * 1.5 bytes/luma-sample, and an entropy coder is not bounded by its own
     * input: measured worst case out of this encoder is 1.53 bytes/luma-
     * sample (pseudo-random content at QP 0), and 1.10 at QP 10. The old
     * `luma_size + 65536` was ~1.03 bytes/luma-sample, so any busy frame at
     * a low QP overran it - and because nothing checked bitstream_t's
     * `overflow` flag, the result was a silently *truncated* slice that a
     * decoder happily decodes into garbage from the truncation point down.
     * That is how it presented: tools/hevc_host_drift.sh at QP 4 on
     * pattern 3 showed every resolution >= 640x360 correct down to a
     * particular row and wrong below it.
     *
     * 2.0 bytes/luma-sample is above the uncompressed bound with ~30%
     * headroom over the measured worst case; encode_core() now also fails
     * the frame outright if the slice still overflows, so exceeding this
     * can never be silent again. */
    enc->slice_rbsp_cap = luma_size * 2 + 65536;
    enc->slice_rbsp = malloc(enc->slice_rbsp_cap);

    /* Holds VPS+SPS+PPS plus the slice after RBSP->EBSP escaping. Worst-case
     * escaping expansion is 4/3 (a run of zero bytes takes an 0x03 every
     * third byte); 3/2 plus 64 KiB of parameter sets is comfortably clear. */
    enc->scratch_out_cap = enc->slice_rbsp_cap + enc->slice_rbsp_cap / 2 + 65536;
    enc->scratch_out = malloc(enc->scratch_out_cap);

    size_t num_mbs = (size_t)enc->width_ctu * enc->height_ctu;
    enc->gpu_mvs = calloc(num_mbs, sizeof(gpu_mv_t));
    enc->gpu_ctu_skip = calloc(num_mbs, sizeof(uint32_t));
    /* Same sizing as dl_uv (interleaved NV12 UV, row stride = coded_width,
     * height = coded_height/2), not width/height - this scratch is only
     * ever filled from ctx->recon_image, which is allocated at coded
     * dimensions. */
    enc->gpu_recon_uv_scratch = malloc((size_t)enc->coded_width * (enc->coded_height / 2));

    if (!enc->src_y || !enc->src_cb || !enc->src_cr ||
        !enc->recon_y || !enc->recon_cb || !enc->recon_cr ||
        !enc->prev_recon_y || !enc->prev_recon_cb || !enc->prev_recon_cr ||
        !enc->cu_skip_map || !enc->cu_is_inter || !enc->mv_x_map || !enc->mv_y_map ||
        !enc->luma_mode_map || !enc->dl_y || !enc->dl_uv ||
        !enc->slice_rbsp || !enc->scratch_out || !enc->gpu_mvs || !enc->gpu_ctu_skip ||
        !enc->gpu_recon_uv_scratch) {
        hevc_encoder_destroy(enc);
        return NULL;
    }

    /* Latched once - see the use_gpu field comment for why this must not be
     * re-read per frame. */
    {
        const char *g = getenv("BC250_HEVC_GPU");
        enc->use_gpu = (g && strcmp(g, "1") == 0);
        const char *gp = getenv("BC250_HEVC_GPU_PFRAME");
        enc->use_gpu_pframe = enc->use_gpu && gp && strcmp(gp, "1") == 0;
        if (enc->use_gpu) {
            fprintf(stderr, "[bc250-hevc] BC250_HEVC_GPU=1: GPU intra path enabled "
                            "(16x16 CU / 16x16 luma TU; P-frame zero-motion skip "
                            "%s - see docs/notes/c7-gpu-pframes.md, UNVALIDATED ON "
                            "HARDWARE)\n",
                            enc->use_gpu_pframe ? "enabled" : "disabled, all-intra");
        }
    }

    return enc;
}

void hevc_encoder_set_force_idr(hevc_encoder_t *encoder)
{
    if (encoder) encoder->force_idr = true;
}

void hevc_encoder_set_gop_size(hevc_encoder_t *encoder, uint32_t gop_size)
{
    if (encoder && gop_size >= 1) encoder->gop_size = gop_size;
}

uint32_t hevc_encoder_get_gop_size(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->gop_size : 30;
}

void hevc_encoder_set_qp(hevc_encoder_t *encoder, int qp)
{
    if (encoder) {
        if (qp < 0) qp = 0;
        if (qp > 51) qp = 51;
        /* docs/backlog.md D1: this used to stomp rc.base_qp AND
         * rc.current_qp on EVERY call, unconditionally - see
         * h264_encoder_set_qp()'s matching fix and doc comment
         * (encoder_h264.c) for the full mechanism and off-board
         * measurement (tools/rc_bench.c, docs/notes/d1-rate-control.md).
         * Short version: va_backend.c's bc250_RenderPicture() routes
         * VAEncPictureParameterBufferHEVC.pic_init_qp and
         * VAEncMiscParameterRateControl.initial_qp straight into this
         * function, PicParam is a mandatory PER-FRAME VA-API buffer, and
         * at least one real caller resends the same hint every frame - so
         * unconditionally reapplying it here reset rc_get_frame_qp()'s
         * clamped per-frame QP walk back to the hint before it ever had
         * more than one frame to move, regardless of what bitrate was
         * actually requested. Only reset rate-control state when the hint
         * genuinely changes, exactly like hevc_encoder_set_bitrate()
         * already treats a resent, unchanged bitrate. */
        if (qp != encoder->qp_hint_applied) {
            encoder->rc.base_qp = qp;
            encoder->rc.current_qp = qp;
            encoder->qp_hint_applied = qp;
        }
        encoder->qp = qp;
    }
}

int hevc_encoder_get_qp(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->qp : 27;
}

void hevc_encoder_set_bitrate(hevc_encoder_t *encoder, uint32_t bitrate)
{
    if (encoder && bitrate > 0 && bitrate != encoder->rc.target_bitrate) {
        encoder->bitrate = bitrate;
        rc_init(&encoder->rc, encoder->rc.mode, bitrate, (double)encoder->fps,
                encoder->width, encoder->height);
    }
}

uint32_t hevc_encoder_get_bitrate(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->bitrate : 0;
}

void hevc_encoder_set_fps(hevc_encoder_t *encoder, uint32_t fps)
{
    if (encoder && fps > 0 && fps != encoder->fps) {
        encoder->fps = fps;
        rc_init(&encoder->rc, encoder->rc.mode, encoder->rc.target_bitrate,
                (double)fps, encoder->width, encoder->height);
    }
}

uint32_t hevc_encoder_get_fps(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->fps : 30;
}

void hevc_encoder_set_rc_mode(hevc_encoder_t *encoder, rc_mode_t mode)
{
    if (encoder) {
        encoder->rc.mode = mode;
        if (mode == RC_LOW_LATENCY) {
            encoder->rc.buffer_size = encoder->rc.target_bits_per_frame * 2;
        } else if (mode == RC_CBR || mode == RC_VBR) {
            encoder->rc.buffer_size = encoder->rc.target_bitrate;
        }
        if (encoder->rc.buffer_size < 1000) encoder->rc.buffer_size = 1000;
        encoder->rc.buffer_fullness = encoder->rc.buffer_size / 2;
        encoder->rc.error_integral = 0;
    }
}

rc_mode_t hevc_encoder_get_rc_mode(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->rc.mode : RC_CQP;
}

uint32_t hevc_encoder_get_last_frame_sad(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->last_frame_sad : 0;
}

void hevc_encoder_set_quality_level(hevc_encoder_t *encoder, uint32_t quality_level)
{
    if (encoder) {
        if (quality_level < 1) quality_level = 1;
        if (quality_level > 7) quality_level = 7;
        encoder->quality_level = quality_level;
        rc_set_quality_level(&encoder->rc, quality_level);
    }
}

uint32_t hevc_encoder_get_quality_level(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->quality_level : 4;
}

void hevc_encoder_set_max_frame_size(hevc_encoder_t *encoder, uint32_t max_frame_bits)
{
    if (encoder) {
        encoder->max_frame_bits = max_frame_bits;
        rc_set_max_frame_size(&encoder->rc, max_frame_bits);
    }
}

uint32_t hevc_encoder_get_max_frame_size(const hevc_encoder_t *encoder)
{
    return encoder ? encoder->max_frame_bits : 0;
}

void hevc_encoder_destroy(hevc_encoder_t *encoder)
{
    if (!encoder) return;
    free(encoder->src_y); free(encoder->src_cb); free(encoder->src_cr);
    free(encoder->recon_y); free(encoder->recon_cb); free(encoder->recon_cr);
    free(encoder->prev_recon_y); free(encoder->prev_recon_cb); free(encoder->prev_recon_cr);
    free(encoder->cu_skip_map);
    free(encoder->cu_is_inter);
    free(encoder->mv_x_map);
    free(encoder->mv_y_map);
    free(encoder->luma_mode_map);
    free(encoder->dl_y); free(encoder->dl_uv);
    free(encoder->slice_rbsp);
    free(encoder->scratch_out);
    free(encoder->gpu_mvs);
    free(encoder->gpu_ctu_skip);
    free(encoder->gpu_recon_uv_scratch);
    free(encoder);
}

/* Replicate-pad a downloaded plane (real w x h) into a coded_w x coded_h
 * working buffer - only the bottom/right margin (if any) needs padding,
 * since coded dims are always >= real dims by construction. */
static void pad_replicate(uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                           const uint8_t *src, uint32_t src_stride, uint32_t src_w, uint32_t src_h) {
    /* `src_w - 1` / `src_h - 1` below are unsigned: an empty source plane
     * would wrap them to 4294967295 rather than clamp. hevc_encoder_create()
     * rejects the sizes that can produce one; this is the second line of
     * defence, because the wrap is silent (not UB, so UBSan does not see it)
     * and the read lands far out of bounds. */
    if (!src_w || !src_h) return;
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

typedef struct {
    int16_t x;
    int16_t y;
} hevc_mv_t;

static inline uint32_t compute_sad_8x8_luma(const uint8_t *src_y,
                                            const uint8_t *ref_y,
                                            uint32_t stride,
                                            int cu_x, int cu_y,
                                            int dx, int dy)
{
    const uint8_t *s = &src_y[cu_y * stride + cu_x];
    const uint8_t *r = &ref_y[(cu_y + dy) * stride + (cu_x + dx)];
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < 8; y++) {
        __m128i s_row = _mm_loadl_epi64((const __m128i *)s);
        __m128i r_row = _mm_loadl_epi64((const __m128i *)r);
        acc = _mm_add_epi32(acc, _mm_sad_epu8(s_row, r_row));
        s += stride;
        r += stride;
    }
    return (uint32_t)_mm_cvtsi128_si32(acc);
#else
    uint32_t sad = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int diff = (int)s[x] - (int)r[x];
            sad += (diff < 0) ? -diff : diff;
        }
        s += stride;
        r += stride;
    }
    return sad;
#endif
}

static inline uint32_t compute_sad_4x4_chroma(const uint8_t *src_cb,
                                              const uint8_t *src_cr,
                                              const uint8_t *ref_cb,
                                              const uint8_t *ref_cr,
                                              uint32_t cstride,
                                              int cx, int cy,
                                              int cdx, int cdy)
{
    const uint8_t *scb = &src_cb[cy * cstride + cx];
    const uint8_t *scr = &src_cr[cy * cstride + cx];
    const uint8_t *rcb = &ref_cb[(cy + cdy) * cstride + (cx + cdx)];
    const uint8_t *rcr = &ref_cr[(cy + cdy) * cstride + (cx + cdx)];
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
    __m128i acc = _mm_setzero_si128();
    for (int y = 0; y < 4; y++) {
        uint32_t scb_4, scr_4, rcb_4, rcr_4;
        memcpy(&scb_4, scb, 4);
        memcpy(&scr_4, scr, 4);
        memcpy(&rcb_4, rcb, 4);
        memcpy(&rcr_4, rcr, 4);
        uint64_t s_both = ((uint64_t)scr_4 << 32) | scb_4;
        uint64_t r_both = ((uint64_t)rcr_4 << 32) | rcb_4;
        __m128i s_vec = _mm_loadl_epi64((const __m128i *)&s_both);
        __m128i r_vec = _mm_loadl_epi64((const __m128i *)&r_both);
        acc = _mm_add_epi32(acc, _mm_sad_epu8(s_vec, r_vec));
        scb += cstride;
        scr += cstride;
        rcb += cstride;
        rcr += cstride;
    }
    return (uint32_t)_mm_cvtsi128_si32(acc);
#else
    uint32_t sad = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int dcb = (int)scb[x] - (int)rcb[x];
            int dcr = (int)scr[x] - (int)rcr[x];
            sad += (dcb < 0 ? -dcb : dcb) + (dcr < 0 ? -dcr : dcr);
        }
        scb += cstride;
        scr += cstride;
        rcb += cstride;
        rcr += cstride;
    }
    return sad;
#endif
}

/* Derives the merge candidate list matching ITU-T H.265 Section 8.5.3.2.2.
 * Output cand_mvs has exactly 5 candidates (padded with (0,0)), in 1/4-pel
 * units.
 *
 * THIS LIST MUST BE EXACTLY THE ONE THE DECODER DERIVES, because the only
 * thing transmitted is merge_idx - an index into a list the decoder builds
 * for itself, from the current picture's already-decoded neighbours, with
 * no help from the bitstream. A candidate the encoder can see and the
 * decoder cannot is not a candidate; it is a corrupt index.
 *
 * It used to append the GPU's motion-estimation vector here as a sixth
 * source, after the spatial candidates. That is not a merge candidate in
 * any HEVC profile. With sps_temporal_mvp_enabled_flag = 0 (write_sps)
 * there is no temporal candidate either, so the decoder's list is
 * "spatials, then zero-motion padding" - and the GPU vector sat exactly on
 * the first padding slot. Every time a CU picked it, the encoder built a
 * motion-compensated block while the decoder built a co-located copy, with
 * no residual to correct the difference (these CUs are SKIP) and the error
 * carried into the reference picture for every later P-frame.
 *
 * Measured with BC250_HEVC_FAKE_GPU_MV (see hevc_encoder_encode_raw), which
 * exists so this is reachable without a GPU: at 64x64 / QP 27 / gop 6, a
 * GPU MV of (2,0) put 17271 of 24576 luma samples wrong against ffmpeg,
 * starting at the first P-frame. (0,0) was exact, which is why no oracle in
 * this repo had caught it - on a dev machine there is no GPU readback, so
 * every merge candidate was zero and every index selected the same vector.
 *
 * Consequence, stated plainly because it is easy to miss: the spatial
 * candidates are read back from mv_x_map/mv_y_map, which only SKIP CUs
 * write, and a SKIP CU can only write a vector it took from this list. With
 * the GPU vector gone the list has no way to ever contain a non-zero
 * vector, so every P-frame motion vector is (0,0). This function is a
 * fixpoint at zero.
 *
 * That killed the motion search. hevc_motion_search_diamond_8x8() - a
 * hierarchical diamond over up to ~39 SAD positions per 8x8 CU, seeded from
 * the GPU MV and the spatial predictors - used to run here and could not
 * affect one bit of output, because the only vector this encoder can signal
 * is the one at the merge index it picks out of the all-zero list above. It
 * was removed (docs/notes/dead-motion-search.md) after an empirical check,
 * not a reading of this comment: perturbing its result arbitrarily left all
 * 38 tools/hevc_host_drift.sh bitstreams byte-identical. Rate control now
 * gets the zero-MV SAD that encode_cu() computes for the skip decision
 * anyway - see there for why that is the right number and not merely the
 * cheap one.
 *
 * Giving this encoder real motion needs explicit MVD signalling (merge_flag
 * = 0 + AMVP + mvd_coding + rqt_root_cbf), not a sixth entry in this list.
 * Whoever does that re-introduces a motion search, and should re-read
 * hevc_encoder_encode_raw()'s BC250_HEVC_FAKE_GPU_MV comment first: that
 * hook and the six drift cases using it are the only way a non-zero vector
 * reaches this code on a machine with no GPU. */
static int derive_merge_candidates(const hevc_encoder_t *enc,
                                   int cux, int cuy,
                                   hevc_mv_t cand_mvs[5])
{
    int num_cand = 0;
    uint32_t w_cu = enc->width_ctu * 2;
    uint32_t h_cu = enc->height_ctu * 2;
    int cu_in_ctu = (cuy & 1) * 2 + (cux & 1); /* 0=TL, 1=TR, 2=BL, 3=BR */

    hevc_mv_t spatial_cand[5];
    int num_spatial = 0;

    /* 1. Candidate A1 (Left): (cu_x - 1, cu_y + 7) -> CU (cux - 1, cuy) */
    bool a1_avail = false;
    hevc_mv_t mv_a1 = {0, 0};
    if (cux > 0) {
        uint32_t a1_idx = (uint32_t)cuy * w_cu + (uint32_t)(cux - 1);
        if (enc->cu_is_inter[a1_idx]) {
            a1_avail = true;
            mv_a1.x = enc->mv_x_map[a1_idx];
            mv_a1.y = enc->mv_y_map[a1_idx];
            spatial_cand[num_spatial++] = mv_a1;
        }
    }

    /* 2. Candidate B1 (Above): (cu_x + 7, cu_y - 1) -> CU (cux, cuy - 1) */
    bool b1_avail = false;
    hevc_mv_t mv_b1 = {0, 0};
    if (cuy > 0) {
        uint32_t b1_idx = (uint32_t)(cuy - 1) * w_cu + (uint32_t)cux;
        if (enc->cu_is_inter[b1_idx]) {
            mv_b1.x = enc->mv_x_map[b1_idx];
            mv_b1.y = enc->mv_y_map[b1_idx];
            /* Pruning: B1 against A1 */
            if (!a1_avail || mv_b1.x != mv_a1.x || mv_b1.y != mv_a1.y) {
                b1_avail = true;
                spatial_cand[num_spatial++] = mv_b1;
            }
        }
    }

    /* 3. Candidate B0 (Above-Right): (cu_x + 8, cu_y - 1) -> CU (cux + 1, cuy - 1) */
    hevc_mv_t mv_b0 = {0, 0};
    bool b0_pos_avail = false;
    if (cuy > 0 && (cux + 1) < (int)w_cu) {
        if (cu_in_ctu != 3) {
            b0_pos_avail = true;
        }
    }
    if (b0_pos_avail) {
        uint32_t b0_idx = (uint32_t)(cuy - 1) * w_cu + (uint32_t)(cux + 1);
        if (enc->cu_is_inter[b0_idx]) {
            mv_b0.x = enc->mv_x_map[b0_idx];
            mv_b0.y = enc->mv_y_map[b0_idx];
            /* Pruning: B0 against B1 */
            if (!b1_avail || mv_b0.x != mv_b1.x || mv_b0.y != mv_b1.y) {
                spatial_cand[num_spatial++] = mv_b0;
            }
        }
    }

    /* 4. Candidate A0 (Below-Left): (cu_x - 1, cu_y + 8) -> CU (cux - 1, cuy + 1) */
    hevc_mv_t mv_a0 = {0, 0};
    bool a0_pos_avail = false;
    if (cux > 0 && (cuy + 1) < (int)h_cu) {
        if (cu_in_ctu == 0) {
            a0_pos_avail = true;
        }
    }
    if (a0_pos_avail) {
        uint32_t a0_idx = (uint32_t)(cuy + 1) * w_cu + (uint32_t)(cux - 1);
        if (enc->cu_is_inter[a0_idx]) {
            mv_a0.x = enc->mv_x_map[a0_idx];
            mv_a0.y = enc->mv_y_map[a0_idx];
            /* Pruning: A0 against A1 */
            if (!a1_avail || mv_a0.x != mv_a1.x || mv_a0.y != mv_a1.y) {
                spatial_cand[num_spatial++] = mv_a0;
            }
        }
    }

    /* 5. Candidate B2 (Above-Left): (cu_x - 1, cu_y - 1) -> CU (cux - 1, cuy - 1) */
    if (num_spatial < 4 && cux > 0 && cuy > 0) {
        uint32_t b2_idx = (uint32_t)(cuy - 1) * w_cu + (uint32_t)(cux - 1);
        if (enc->cu_is_inter[b2_idx]) {
            hevc_mv_t mv_b2;
            mv_b2.x = enc->mv_x_map[b2_idx];
            mv_b2.y = enc->mv_y_map[b2_idx];
            /* Pruning: B2 against A1 and B1 */
            if ((!a1_avail || mv_b2.x != mv_a1.x || mv_b2.y != mv_a1.y) &&
                (!b1_avail || mv_b2.x != mv_b1.x || mv_b2.y != mv_b1.y)) {
                spatial_cand[num_spatial++] = mv_b2;
            }
        }
    }

    /* 6. No temporal (Col) candidate: sps_temporal_mvp_enabled_flag is 0,
     * so the decoder does not derive one either. Anything appended past
     * this point would land on a slot the decoder fills with zero motion -
     * see this function's header comment. */

    for (int i = 0; i < num_spatial && num_cand < 5; i++) {
        cand_mvs[num_cand++] = spatial_cand[i];
    }

    while (num_cand < 5) {
        cand_mvs[num_cand].x = 0;
        cand_mvs[num_cand].y = 0;
        num_cand++;
    }

    return num_cand;
}

static void encode_cu(hevc_encoder_t *enc, hevc_cabac_t *cab, int cu_x, int cu_y, bool is_idr) {
    int qp = enc->qp;
    uint32_t cw = enc->coded_width, ch = enc->coded_height;
    uint32_t ccw = cw / 2, cch = ch / 2;
    int cux = cu_x / HEVC_CU_SIZE;
    int cuy = cu_y / HEVC_CU_SIZE;
    uint32_t cu_stride = enc->width_ctu * 2;
    uint32_t cu_idx = (uint32_t)cuy * cu_stride + (uint32_t)cux;

    int cond_l = (cux > 0 && enc->cu_skip_map[cu_idx - 1]) ? 1 : 0;
    int cond_a = (cuy > 0 && enc->cu_skip_map[cu_idx - cu_stride]) ? 1 : 0;
    int skip_ctx_inc = cond_l + cond_a;

    bool is_skip = false;
    int chosen_merge_idx = 0;
    int chosen_dx = 0, chosen_dy = 0;

    if (!is_idr && enc->has_ref) {
        hevc_mv_t cand_mvs[5];
        derive_merge_candidates(enc, cux, cuy, cand_mvs);

        int cx = cu_x / 2, cy = cu_y / 2;

        /* SAD of the co-located block of the reference - the prediction this
         * encoder can actually emit, since derive_merge_candidates() is a
         * fixpoint at zero (see its comment). Computed once here and reused
         * by the candidate loop below, which used to recompute it up to five
         * times per CU because all five candidates are (0,0).
         *
         * This is also what rate control now gets. The number it used to get
         * was hevc_motion_search_diamond_8x8()'s best SAD, which is <= this
         * one by construction (the search starts at (0,0) and only ever
         * accepts an improvement) and describes a motion-compensated block
         * the bitstream has no syntax to ask for. Rate control's only use of
         * it (rate_control.c, RC_VBR) is est_sad / prev_frame_sad as a
         * temporal-complexity ratio, so feeding it a residual the encoder
         * cannot realise systematically under-states how hard the frame is.
         * The zero-MV SAD is the residual energy the encoder will really
         * face, it is exactly the number the skip decision three lines down
         * is already making, and it costs nothing extra. Measured effect on
         * output: of the 38 tools/hevc_host_drift.sh cases, the 35 CQP ones
         * are byte-identical (RC_CQP ignores est_sad entirely) and of the
         * three VBR ones only 128x128 p2 gop8 @4000 kbps changes - one QP
         * step on one frame. docs/notes/dead-motion-search.md. */
        uint32_t sad_zero = compute_sad_8x8_luma(enc->src_y, enc->prev_recon_y, cw, cu_x, cu_y, 0, 0) +
                            compute_sad_4x4_chroma(enc->src_cb, enc->src_cr,
                                                   enc->prev_recon_cb, enc->prev_recon_cr,
                                                   ccw, cx, cy, 0, 0);
        enc->last_frame_sad += sad_zero;

        uint32_t threshold = 96 * (1 + (enc->qp / 8));
        if (enc->quality_level >= 5) {
            threshold = threshold * 3 / 2;
        }
        static int s_skip_override = -2;
        if (s_skip_override == -2) {
            const char *env = getenv("BC250_HEVC_SKIP_THRESHOLD");
            s_skip_override = env ? atoi(env) : -1;
        }
        if (s_skip_override >= 0) {
            threshold = (uint32_t)s_skip_override;
        }

        /* Evaluate candidates in cand_mvs to find the best merge candidate.
         * Every candidate is (0,0) today, so this loop costs nothing beyond
         * the sad_zero above - but it is left general on purpose: it is the
         * piece that stays correct if a future AMVP/MVD path ever puts a
         * real vector in the list. */
        int best_cand_idx = -1;
        uint32_t best_cand_sad = UINT32_MAX;

        for (int i = 0; i < 5; i++) {
            int c_dx = cand_mvs[i].x / 4;
            int c_dy = cand_mvs[i].y / 4;
            if (cu_x + c_dx >= 0 && cu_x + c_dx + 8 <= (int)cw &&
                cu_y + c_dy >= 0 && cu_y + c_dy + 8 <= (int)ch) {
                uint32_t c_sad;
                if (c_dx == 0 && c_dy == 0) {
                    c_sad = sad_zero;
                } else {
                    c_sad = compute_sad_8x8_luma(enc->src_y, enc->prev_recon_y, cw, cu_x, cu_y, c_dx, c_dy) +
                            compute_sad_4x4_chroma(enc->src_cb, enc->src_cr,
                                                   enc->prev_recon_cb, enc->prev_recon_cr,
                                                   ccw, cx, cy, c_dx / 2, c_dy / 2);
                }
                if (c_sad < best_cand_sad) {
                    best_cand_sad = c_sad;
                    best_cand_idx = i;
                }
            }
        }

        if (best_cand_idx >= 0 && best_cand_sad <= threshold) {
            is_skip = true;
            chosen_merge_idx = best_cand_idx;
            chosen_dx = cand_mvs[best_cand_idx].x / 4;
            chosen_dy = cand_mvs[best_cand_idx].y / 4;
        }
    }

    if (is_skip) {
        enc->cu_skip_map[cu_idx] = 1;
        enc->cu_is_inter[cu_idx] = 1;
        enc->mv_x_map[cu_idx] = (int16_t)(chosen_dx * 4);
        enc->mv_y_map[cu_idx] = (int16_t)(chosen_dy * 4);
        hevc_cabac_code_cu_skip_flag(cab, 1, skip_ctx_inc);
        hevc_cabac_code_merge_idx(cab, chosen_merge_idx);

        for (int y = 0; y < HEVC_CU_SIZE; y++) {
            memcpy(&enc->recon_y[(cu_y + y) * cw + cu_x],
                   &enc->prev_recon_y[(cu_y + chosen_dy + y) * cw + (cu_x + chosen_dx)],
                   HEVC_CU_SIZE);
        }
        int cx = cu_x / 2, cy = cu_y / 2;
        int cdx = chosen_dx / 2, cdy = chosen_dy / 2;
        for (int y = 0; y < HEVC_PU_SIZE; y++) {
            memcpy(&enc->recon_cb[(cy + y) * ccw + cx],
                   &enc->prev_recon_cb[(cy + cdy + y) * ccw + (cx + cdx)],
                   HEVC_PU_SIZE);
            memcpy(&enc->recon_cr[(cy + y) * ccw + cx],
                   &enc->prev_recon_cr[(cy + cdy + y) * ccw + (cx + cdx)],
                   HEVC_PU_SIZE);
        }
        for (int pu = 0; pu < 4; pu++) {
            int px = cu_x + pu_off_x[pu], py = cu_y + pu_off_y[pu];
            enc->luma_mode_map[(py / 4) * enc->mode_map_stride + (px / 4)] = HEVC_MODE_DC;
        }
        return;
    }

    enc->cu_skip_map[cu_idx] = 0;
    enc->cu_is_inter[cu_idx] = 0;
    enc->mv_x_map[cu_idx] = 0;
    enc->mv_y_map[cu_idx] = 0;
    if (!is_idr) {
        hevc_cabac_code_cu_skip_flag(cab, 0, skip_ctx_inc);
        hevc_cabac_code_pred_mode_flag(cab, 1 /* MODE_INTRA */);
    }

    /* A6 piece (2): PART_2Nx2N, one undivided 8x8 luma PU/TU per CU,
     * replacing the old PART_NxN four-4x4-PU/TU structure - see this
     * function's callers (write_sps()'s MaxTb comment) and
     * docs/notes/a6-cu-tu-structure.md for the full reasoning on why this
     * (not a bare "add bigger transforms alongside the old 4x4 structure")
     * is what "all-TU-size transforms" needs at this fixed 8x8 CU size:
     * with MinCbLog2SizeY == log2CbSize == 3, PART_NxN forces
     * IntraSplitFlag=1, which per 7.3.8.8's transform_tree() FORCES the
     * transform tree to split at depth 0 regardless of anything this
     * encoder decides - there is no bitstream-legal way to reach an 8x8
     * luma transform while keeping the old four-PU structure at this CU
     * size. PART_2Nx2N (IntraSplitFlag=0) is what makes an undivided 8x8
     * transform reachable at all. This is the correctness-only "always use
     * the largest transform the CU size allows, never split" first cut the
     * task asked for - here that largest transform is dictated by the CU
     * size, which piece (3) (not attempted this session) is what would
     * ever make bigger. */
    hevc_cabac_code_part_mode_intra(cab, 1 /* PART_2Nx2N */);

    int mx = cu_x / 4, my = cu_y / 4;
    int left_avail = cu_x > 0;
    /* Same CtbLog2SizeY-crossing rule as before (8.4.2's candIntraPredModeB),
     * now evaluated once at the CU's own top-left corner instead of once
     * per PU - there is only one PU now, and it always starts at the CU's
     * own corner. */
    int above_avail = (cu_y > 0) && ((cu_y % HEVC_CTU_SIZE) != 0);
    int left_mode = left_avail ? enc->luma_mode_map[my * enc->mode_map_stride + (mx - 1)] : 0;
    int above_mode = above_avail ? enc->luma_mode_map[(my - 1) * enc->mode_map_stride + mx] : 0;
    int mpm[3];
    hevc_derive_mpm(left_mode, left_avail, above_mode, above_avail, mpm);

    uint8_t pred[HEVC_CU_SIZE * HEVC_CU_SIZE];
    int mode = hevc_choose_luma_mode_nxn(enc->src_y, enc->recon_y, (int)cw,
                                         (int)cw, (int)ch, cu_x, cu_y,
                                         3 /* log2_size: 8x8 */, mpm, pred);

    int16_t residual[HEVC_CU_SIZE * HEVC_CU_SIZE];
    for (int y = 0; y < HEVC_CU_SIZE; y++)
        for (int x = 0; x < HEVC_CU_SIZE; x++)
            residual[y * HEVC_CU_SIZE + x] =
                (int16_t)(enc->src_y[(cu_y + y) * cw + (cu_x + x)] - pred[y * HEVC_CU_SIZE + x]);

    int16_t luma_coeff[HEVC_CU_SIZE * HEVC_CU_SIZE];
    /* use_dst=0: DST-VII is the 4x4-luma-intra-only alternative transform
     * (8.6.4.1) - an 8x8 luma TU is DCT-II unconditionally. */
    hevc_transform_quant(residual, qp, 3, 0, luma_coeff);
    int cbf_luma = 0;
    for (int i = 0; i < HEVC_CU_SIZE * HEVC_CU_SIZE; i++) if (luma_coeff[i]) { cbf_luma = 1; break; }

    int16_t recon_residual[HEVC_CU_SIZE * HEVC_CU_SIZE];
    hevc_dequant_itransform(luma_coeff, qp, 3, 0, recon_residual);
    for (int y = 0; y < HEVC_CU_SIZE; y++)
        for (int x = 0; x < HEVC_CU_SIZE; x++)
            enc->recon_y[(cu_y + y) * cw + (cu_x + x)] =
                clip8i(pred[y * HEVC_CU_SIZE + x] + recon_residual[y * HEVC_CU_SIZE + x]);

    /* One mode now covers the whole CU: mark all 4 of its 4x4 mode-map
     * cells with it, same convention the skip-CU path already uses above
     * for the same reason (future CUs' MPM derivation reads this map at
     * 4x4 granularity regardless of how the mode that produced it was
     * signaled). */
    for (int pu = 0; pu < 4; pu++) {
        int px = cu_x + pu_off_x[pu], py = cu_y + pu_off_y[pu];
        enc->luma_mode_map[(py / 4) * enc->mode_map_stride + (px / 4)] = (int8_t)mode;
    }
    /* BC250_HEVC_DEBUG_MODES=1: one line per luma CU now (was one per 4x4
     * PU) - see the original comment on this hook, same diagnostic intent. */
    if (getenv("BC250_HEVC_DEBUG_MODES"))
        fprintf(stderr, "[MODE] x=%d y=%d mode=%d\n", cu_x, cu_y, mode);

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

    /* Chroma quantizes at QpC, not QpY - Table 8-10. Passing qp here (as
     * this did) matches a decoder only below QP 30; above it the encoder's
     * reconstruction and the decoder's diverge, growing with QP. */
    int cqp = hevc_chroma_qp_from_luma(qp);

    int16_t coeff_cb[16], coeff_cr[16];
    hevc_transform_quant_4x4(res_cb, cqp, 0, coeff_cb);
    hevc_transform_quant_4x4(res_cr, cqp, 0, coeff_cr);
    int cbf_cb = any_nonzero16(coeff_cb);
    int cbf_cr = any_nonzero16(coeff_cr);

    int16_t rres_cb[16], rres_cr[16];
    hevc_dequant_itransform_4x4(coeff_cb, cqp, 0, rres_cb);
    hevc_dequant_itransform_4x4(coeff_cr, cqp, 0, rres_cr);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            enc->recon_cb[(cy + y) * ccw + (cx + x)] = clip8i(pred_cb[y * 4 + x] + rres_cb[y * 4 + x]);
            enc->recon_cr[(cy + y) * ccw + (cx + x)] = clip8i(pred_cr[y * 4 + x] + rres_cr[y * 4 + x]);
        }

    /* Step 2: emit intra_luma_pred_mode syntax for this CU's one PU. ITU-T
     * H.265 7.3.8.5's coding_unit() codes this as TWO separate passes over
     * all PUs in the CU - every prev_intra_luma_pred_flag first, THEN every
     * mpm_idx/rem_intra_luma_pred_mode (see
     * hevc_cabac_code_intra_luma_flag()/_data()'s comment for why getting
     * that order wrong was this encoder's first real bug). With PART_2Nx2N
     * there is exactly one PU, so the two "passes" are each one call - the
     * two-pass STRUCTURE still matters for a decoder reading a general
     * bitstream, it just has nothing left to interleave here. */
    int pred_idx = hevc_cabac_code_intra_luma_flag(cab, mode, mpm);
    hevc_cabac_code_intra_luma_data(cab, mode, pred_idx, mpm);

    /* Step 3: chroma mode (always DC; the luma mode decides whether that's
     * signaled as index-3-of-candidate-list or as the derived/DM mode -
     * see hevc_cabac_code_intra_chroma_pred_mode()'s comment). Only one
     * luma mode exists per CU now, so this needs no "which PU" caveat. */
    hevc_cabac_code_intra_chroma_pred_mode(cab, mode);

    /* Step 4: transform_tree - chroma cbf BITS first (trafoDepth=0, this
     * CU's root), then the luma leaf's cbf+residual, then finally the
     * chroma RESIDUAL DATA - same ordering as before (chroma cbf bits
     * ahead of luma, chroma residual data after it), now with ONE luma
     * leaf coded at trafoDepth=0 (ctx 1, via hevc_cabac_code_cbf_luma()'s
     * own trafo_depth==0 test) instead of four leaves at trafoDepth=1
     * (ctx 0) - MaxTrafoDepth is 0 for a PART_2Nx2N CU (see write_sps()'s
     * comment above), so trafoDepth=0 IS this CU's one and only transform
     * leaf, not an approximation of "the root before splitting". */
    hevc_cabac_code_cbf_chroma(cab, cbf_cb, 0);
    hevc_cabac_code_cbf_chroma(cab, cbf_cr, 0);

    hevc_cabac_code_cbf_luma(cab, cbf_luma, 0);
    if (cbf_luma) {
        int scan_idx = hevc_scan_idx_for_mode(mode);
        hevc_cabac_code_residual(cab, luma_coeff, 3 /* log2_size: 8x8 */, 1 /* luma */, scan_idx);
    }
    if (cbf_cb) hevc_cabac_code_residual_4x4(cab, coeff_cb, 0, 0 /* chroma always diagonal in 4:2:0 */);
    if (cbf_cr) hevc_cabac_code_residual_4x4(cab, coeff_cr, 0, 0);
}

static void encode_ctu(hevc_encoder_t *enc, hevc_cabac_t *cab, int ctu_col, int ctu_row, bool is_idr) {
    int ctu_x = ctu_col * HEVC_CTU_SIZE, ctu_y = ctu_row * HEVC_CTU_SIZE;
    int cond_l = ctu_col > 0 ? 1 : 0;
    int cond_a = ctu_row > 0 ? 1 : 0;
    hevc_cabac_code_split_cu_flag(cab, 1, cond_l + cond_a);

    /* No per-CTU GPU motion vector is looked up any more. enc->gpu_mvs is
     * still filled (motion_estimation.comp's readback in
     * hevc_encoder_encode_frame(), or BC250_HEVC_FAKE_GPU_MV off-board) but
     * nothing downstream can use it: the merge list is a fixpoint at zero,
     * so the only vector this encoder can signal is (0,0) whatever the GPU
     * found. See derive_merge_candidates()'s comment and
     * docs/notes/dead-motion-search.md. */
    static const int cu_off_x[4] = { 0, 8, 0, 8 };
    static const int cu_off_y[4] = { 0, 0, 8, 8 };
    for (int i = 0; i < 4; i++)
        encode_cu(enc, cab, ctu_x + cu_off_x[i], ctu_y + cu_off_y[i], is_idr);
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
/* Choose this frame's QP, exactly once. Idempotent per frame via
 * qp_already_decided, because rc_get_frame_qp() advances rate-control state
 * (current_qp and error_integral) rather than just reporting it. */
static void pick_frame_qp(hevc_encoder_t *encoder, uint64_t est_sad)
{
    if (encoder->qp_already_decided) return;
    if (encoder->rc.mode != RC_CQP) {
        int target_qp = rc_get_frame_qp(&encoder->rc, est_sad);
        if (target_qp >= 1 && target_qp <= 51) encoder->qp = target_qp;
    }
    encoder->qp_already_decided = true;
}

static int encode_core(hevc_encoder_t *encoder, uint8_t *output_buf, size_t output_size)
{
    bool is_idr = (encoder->frame_count % encoder->gop_size == 0) || encoder->force_idr || !encoder->has_ref;
    encoder->force_idr = false;
    if (is_idr) {
        encoder->poc = 0;
    }

    /* In VBR/CBR/LOW_LATENCY mode, update QP via rate control model.
     * A no-op if the GPU path already chose it before dispatching. */
    pick_frame_qp(encoder, is_idr ? 0 : encoder->last_frame_sad);
    encoder->qp_already_decided = false;
    encoder->last_frame_sad = 0;

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

    size_t num_cus = (size_t)(encoder->width_ctu * 2) * (encoder->height_ctu * 2);
    memset(encoder->luma_mode_map, 0, (size_t)encoder->mode_map_stride * (encoder->coded_height / HEVC_PU_SIZE));
    memset(encoder->cu_skip_map, 0, num_cus);
    memset(encoder->cu_is_inter, 0, num_cus);
    memset(encoder->mv_x_map, 0, num_cus * sizeof(int16_t));
    memset(encoder->mv_y_map, 0, num_cus * sizeof(int16_t));

    bool write_param_sets = is_idr;
    if (getenv("BC250_HEVC_REPEAT_HEADERS")) {
        write_param_sets = true;
    }
    if (write_param_sets) {
        encoder->pps_init_qp = encoder->qp;
    }
    int slice_qp_delta = encoder->qp - encoder->pps_init_qp;

    bitstream_t slice_bs;
    bs_init(&slice_bs, encoder->slice_rbsp, encoder->slice_rbsp_cap);

    bs_write1(&slice_bs, 1); /* first_slice_segment_in_pic_flag */
    if (is_idr) {
        bs_write1(&slice_bs, 1); /* no_output_of_prior_pics_flag (only for IRAP) */
    }
    bs_write_ue(&slice_bs, 0); /* slice_pic_parameter_set_id */
    bs_write_ue(&slice_bs, is_idr ? 2 : 1); /* slice_type: 2 = I, 1 = P */

    if (!is_idr) {
        bs_write_u(&slice_bs, 4, encoder->poc & 0xF); /* slice_pic_order_cnt_lsb */
        bs_write1(&slice_bs, 1);                       /* short_term_ref_pic_set_sps_flag = 1 */
        bs_write1(&slice_bs, 0);                       /* num_ref_idx_active_override_flag = 0 */
        bs_write_ue(&slice_bs, 0);                      /* five_minus_max_num_merge_cand = 0 */
    }

    bs_write_se(&slice_bs, slice_qp_delta); /* slice_qp_delta relative to active PPS */
    /* slice_loop_filter_across_slices_enabled_flag is NOT present: its
     * 7.3.6.1 condition needs SAO or deblocking enabled, and the PPS now
     * disables both. See write_pps()'s comment. */

    bs_rbsp_trailing_bits(&slice_bs);

    hevc_cabac_t cab;
    hevc_cabac_init(&cab, &slice_bs);
    hevc_cabac_reset_contexts(&cab, encoder->qp, is_idr ? 2 : 1);
    hevc_cabac_start(&cab);

    uint32_t total_ctus = encoder->width_ctu * encoder->height_ctu;
    uint32_t ctu_idx = 0;
    for (uint32_t row = 0; row < encoder->height_ctu; row++) {
        for (uint32_t col = 0; col < encoder->width_ctu; col++) {
            encode_ctu(encoder, &cab, (int)col, (int)row, is_idr);
            ctu_idx++;
            hevc_cabac_encode_terminate(&cab, ctu_idx == total_ctus ? 1 : 0);
        }
    }

    hevc_cabac_finish(&cab);
    bs_rbsp_trailing_bits(&slice_bs);

    /* A slice that did not fit is a failed frame, never a short one. See the
     * slice_rbsp_cap comment in hevc_encoder_create(). */
    if (bs_overflowed(&slice_bs)) return -1;

    /* BC250_HEVC_DEBUG_STATS=1: one line per frame - slice type, QP, how
     * many of the 8x8 CUs were coded as inter SKIP rather than intra, and
     * this frame's own reconstruction error against its source. Without
     * this, "the inter path is bad" cannot be separated from "the inter
     * path is never taken": a P-frame whose CUs are all intra passes every
     * byte-exactness oracle in this repo while exercising nothing. The
     * recon error is the encoder's OWN (pre-deblocking) view - compare it
     * against a real decode to see the in-loop-filter divergence, which is
     * exactly what a drift run with -skip_loop_filter all cannot see. */
    if (getenv("BC250_HEVC_DEBUG_STATS")) {
        uint32_t n_skip = 0;
        for (size_t i = 0; i < num_cus; i++) n_skip += encoder->cu_skip_map[i] ? 1u : 0u;
        uint64_t abserr = 0;
        for (uint32_t y = 0; y < encoder->height; y++)
            for (uint32_t x = 0; x < encoder->width; x++) {
                int d = (int)encoder->recon_y[(size_t)y * encoder->coded_width + x] -
                        (int)encoder->src_y[(size_t)y * encoder->coded_width + x];
                abserr += (uint64_t)(d < 0 ? -d : d);
            }
        fprintf(stderr, "[HEVC_STATS] frame=%u %s qp=%d cu_skip=%u/%zu recon_mad=%.3f\n",
                encoder->frame_count, is_idr ? "I" : "P", encoder->qp,
                n_skip, num_cus,
                (double)abserr / ((double)encoder->width * encoder->height));
    }

    size_t total = 0;
    if (write_param_sets) {
        total += write_vps(encoder->scratch_out + total, encoder->scratch_out_cap - total);
        total += write_sps(encoder->scratch_out + total, encoder->scratch_out_cap - total,
                            encoder->coded_width, encoder->coded_height,
                            encoder->width, encoder->height,
                            hevc_pick_level_idc(encoder->coded_width, encoder->coded_height),
                            /* A6 piece (2): MaxTb = 8. encode_cu() now emits
                             * PART_2Nx2N with one undivided 8x8 luma TU per
                             * CU (see its own comment) instead of the old
                             * PART_NxN four-4x4-TU structure, so MinCb(8)'s
                             * own transform tree needs 8x8 to be SPS-legal.
                             * max_transform_hierarchy_depth_intra stays 0
                             * (write_sps()'s own comment on this parameter),
                             * so with IntraSplitFlag now 0 too
                             * (PART_2Nx2N), MaxTrafoDepth = 0: split_transform
                             * _flag still isn't explicitly coded (same as
                             * before, and same as the GPU path's own 2Nx2N
                             * CU) - it is now inferred to "don't split" at
                             * the CU's one 8x8 TU instead of the old
                             * "forced split to four 4x4 TUs". */
                            3);
        total += write_pps(encoder->scratch_out + total, encoder->scratch_out_cap - total, encoder->qp);
    }

    {
        bitstream_t out_bs;
        bs_init(&out_bs, encoder->scratch_out + total, encoder->scratch_out_cap - total);
        bs_write_nal_header_hevc(&out_bs, is_idr ? NAL_UNIT_CODED_SLICE_IDR_W_RADL : NAL_UNIT_CODED_SLICE_TRAIL_R);
        size_t off = bs_bytes_written(&out_bs);
        size_t rbsp = bs_bytes_written(&slice_bs);
        /* bs_rbsp_to_ebsp() stops at the destination end and reports only how
         * much it wrote, so a short destination is indistinguishable from a
         * complete conversion. Check the worst case up front instead. */
        if (bs_overflowed(&out_bs) ||
            encoder->scratch_out_cap - total - off < ebsp_worst_case(rbsp)) return -1;
        size_t ebsp = bs_rbsp_to_ebsp(encoder->scratch_out + total + off, encoder->scratch_out_cap - total - off,
                                       encoder->slice_rbsp, rbsp);
        total += off + ebsp;
    }

    if (total > output_size) return -1;
    memcpy(output_buf, encoder->scratch_out, total);

    if (total > 0 && encoder->rc.mode != RC_CQP) {
        rc_update_stats(&encoder->rc, (int)(total * 8));
    }

    /* Update reference buffers for subsequent P-frames */
    size_t luma_size = (size_t)encoder->coded_width * encoder->coded_height;
    size_t chroma_size = (size_t)(encoder->coded_width / 2) * (encoder->coded_height / 2);
    memcpy(encoder->prev_recon_y, encoder->recon_y, luma_size);
    memcpy(encoder->prev_recon_cb, encoder->recon_cb, chroma_size);
    memcpy(encoder->prev_recon_cr, encoder->recon_cr, chroma_size);
    encoder->has_ref = true;
    encoder->poc++;

    /* Debug-only: dump this encoder's own idea of the reconstructed picture
     * (i.e. what a bug-free decoder given this exact bitstream SHOULD
     * reproduce) - lets a diff against a real decoder's actual output
     * localize whether a mismatch is in the prediction/transform/quant
     * math (this dump would ALSO look wrong) or in CABAC/bitstream framing
     * (this dump looks right, but a real decoder's output doesn't). */
    if (getenv("BC250_HEVC_DEBUG_RECON")) {
        size_t ysz = (size_t)encoder->coded_width * encoder->coded_height;
        size_t csz = (size_t)(encoder->coded_width / 2) * (encoder->coded_height / 2);
        FILE *fy = fopen("bc250_hevc_debug_recon_y.raw", "wb");
        if (fy) { fwrite(encoder->recon_y, 1, ysz, fy); fclose(fy); }
        /* Chroma too, as planar I420 alongside the luma, appended per frame.
         * Luma alone cannot answer the question this dump exists for once a
         * defect is chroma-only - which is exactly what the GPU path turned
         * out to have (docs/hevc-gpu-intra.md, chroma QP and Table 8-10).
         * One file per plane, frames appended, so frame N is at N*plane_size. */
        FILE *fa = fopen("bc250_hevc_debug_recon_i420.raw", "ab");
        if (fa) {
            fwrite(encoder->recon_y,  1, ysz, fa);
            fwrite(encoder->recon_cb, 1, csz, fa);
            fwrite(encoder->recon_cr, 1, csz, fa);
            fclose(fa);
        }
    }

    encoder->frame_count++;
    return (int)total;
}

/* ============================================================================
 * GPU intra path (BC250_HEVC_GPU=1)
 * ==========================================================================*/

/* GPU-path P-frame zero-motion-skip decision (BC250_HEVC_GPU_PFRAME=1,
 * docs/notes/c7-gpu-pframes.md). Host-side only - nothing here touches the
 * GPU. Requires encoder->src_y/cb/cr and encoder->prev_recon_y/cb/cr to
 * already hold this frame's source and the previous frame's reconstruction
 * (the caller's job: hevc_encoder_encode_frame() downloads both from the
 * GPU before calling this; hevc_encoder_encode_gpu_raw() below takes them
 * as host pointers directly, for off-board testing). */

/* One CTU's zero-motion SAD is the sum of four 8x8-luma + 4x4-chroma
 * zero-motion SADs - the exact same per-block primitives encode_cu() (CPU
 * path) already uses for its own 8x8-CU skip decision, just called four
 * times each to cover this path's 16x16 luma / 8x8 chroma CTU instead of
 * once. No new SAD code, and nothing here uses the GPU's own SAD (this
 * shader has no SAD output binding). */
static uint32_t compute_sad_ctu_zero(const hevc_encoder_t *enc, int ctu_x, int ctu_y) {
    uint32_t cw = enc->coded_width, ccw = cw / 2;
    static const int off_x[4] = { 0, 8, 0, 8 };
    static const int off_y[4] = { 0, 0, 8, 8 };
    uint32_t sad = 0;
    for (int i = 0; i < 4; i++) {
        int lx = ctu_x + off_x[i], ly = ctu_y + off_y[i];
        sad += compute_sad_8x8_luma(enc->src_y, enc->prev_recon_y, cw, lx, ly, 0, 0);
        int cx = ctu_x / 2 + off_x[i] / 2, cy = ctu_y / 2 + off_y[i] / 2;
        sad += compute_sad_4x4_chroma(enc->src_cb, enc->src_cr,
                                      enc->prev_recon_cb, enc->prev_recon_cr,
                                      ccw, cx, cy, 0, 0);
    }
    return sad;
}

/* CPU path's own per-8x8-CU threshold (encode_cu()) is 96 * (1 + qp/8). One
 * GPU-path CTU is the SUM of four such CU-sized zero-motion SADs
 * (compute_sad_ctu_zero() below, and hevc_pframe_skip.comp's GPU
 * equivalent - docs/notes/c7-pframe-throughput.md), so scaling that same
 * formula by 4 asks "would each of the four sub-blocks individually have
 * passed the CPU path's own threshold" - a reasonable starting point, not
 * a derived constant. BC250_HEVC_GPU_SKIP_THRESHOLD overrides it outright,
 * same convention as the CPU path's own BC250_HEVC_SKIP_THRESHOLD.
 *
 * Factored out to ONE place (used by decide_gpu_ctu_skips() below, for the
 * off-board hevc_encoder_encode_gpu_raw() test path, and by
 * hevc_encoder_encode_frame()'s real GPU dispatch, which now hands this
 * value to hevc_pframe_skip.comp as a push constant instead of running the
 * SAD loop on the CPU) so the formula cannot drift between the two
 * callers - moving WHERE the decision runs must not risk two different
 * answers to WHAT it decides. */
static uint32_t hevc_gpu_pframe_skip_threshold(int qp) {
    uint32_t threshold = 4u * 96u * (1u + (uint32_t)(qp / 8));
    static int s_override = -2;
    if (s_override == -2) {
        const char *env = getenv("BC250_HEVC_GPU_SKIP_THRESHOLD");
        s_override = env ? atoi(env) : -1;
    }
    if (s_override >= 0) threshold = (uint32_t)s_override;
    return threshold;
}

/* Fills enc->gpu_ctu_skip[] (one uint32 per CTU, nonzero = skip) and
 * enc->last_frame_sad (this frame's total zero-motion SAD, fed to
 * pick_frame_qp() as the temporal-complexity estimate for the NEXT frame -
 * same convention as encode_cu()'s enc->last_frame_sad accumulation on the
 * CPU path). Also marks every skip CTU's luma_mode_map footprint DC, for
 * MPM correctness if this decision is consumed before any later memset
 * re-zeroes the map (encode_core_gpu() re-does this marking itself, from
 * this same gpu_ctu_skip[], after its own memset - see there).
 *
 * CPU-only: used by the off-board hevc_encoder_encode_gpu_raw() test path
 * (docs/notes/c7-gpu-pframes.md), which has no GPU to dispatch
 * hevc_pframe_skip.comp on. The real board path (hevc_encoder_encode_
 * frame()) no longer calls this - see docs/notes/c7-pframe-throughput.md.
 *
 * The threshold decides RATE/QUALITY, not CORRECTNESS: a CTU marked skip
 * always reconstructs as an exact copy of the reference (see
 * hevc_intra_wavefront.comp's early return), so no value of this threshold
 * can make the bitstream non-conforming - only worse-compressed or worse-
 * looking. That is deliberate slack: this heuristic has had no board
 * measurement at all (see this file's top-level status note), so getting
 * its exact value right is explicitly not a correctness requirement here,
 * only a tuning one for later. */
/* Refreshes enc->prev_recon_y/cb/cr from ctx->recon_image (docs/notes/
 * c7-pframe-throughput.md). docs/notes/c7-gpu-pframes.md's first cut did
 * this download unconditionally, before EVERY P-frame-candidate dispatch,
 * as a side effect of needing the reference pixels on the host for its own
 * (now-removed) CPU SAD loop - which is exactly the "full reference
 * download" c7-pframe-throughput.md's whole point is to stop paying for on
 * the common path. It has exactly one remaining reason to exist: if a GPU
 * P-frame candidate's dispatch or readback fails and hevc_encoder_encode_
 * frame() falls through to the CPU path (encode_core()) for that SAME
 * frame, encode_core()'s own zero-motion skip decision (encode_cu()) reads
 * enc->prev_recon_* as the actual previous-frame reference - normally kept
 * fresh by encode_core()'s own end-of-frame memcpy, which never runs while
 * the GPU path is handling frames. Called ONLY from that fallback branch
 * now, so the common (dispatch succeeds) path never pays for this download
 * at all - matching this feature's own prior acknowledgment that the
 * cross-path interaction "should be consistent by construction... but has
 * not been exercised" (still true here: this relocates an existing,
 * already-untested defensive measure, it does not add a new one).
 *
 * A failure here does not by itself make the CPU path's output wrong: a
 * stale/garbage prev_recon_* will generally fail encode_cu()'s own SAD
 * threshold against the real current-frame source (an actual previous
 * reconstruction and an arbitrary stale buffer are unlikely to look
 * alike), routing those CTUs to intra instead of a wrong skip - but this
 * is "generally", not a guarantee, which is exactly why this refresh is
 * still attempted rather than skipped. Returns 0 on success (matching
 * gpu_compute_hevc_download_recon_nv12()'s own contract), -1 if there is
 * no usable recon_image yet. */
static int hevc_refresh_prev_recon_from_gpu(hevc_encoder_t *encoder, bc250_gpu_context_t *gpu_ctx) {
    if (gpu_compute_hevc_download_recon_nv12(gpu_ctx,
            encoder->prev_recon_y, (int)encoder->coded_width,
            encoder->gpu_recon_uv_scratch, (int)encoder->coded_width,
            (int)encoder->coded_width, (int)encoder->coded_height) != 0) {
        return -1;
    }
    uint32_t ccw = encoder->coded_width / 2, cch = encoder->coded_height / 2;
    for (uint32_t y = 0; y < cch; y++) {
        const uint8_t *uvrow = encoder->gpu_recon_uv_scratch + (size_t)y * encoder->coded_width;
        for (uint32_t x = 0; x < ccw; x++) {
            encoder->prev_recon_cb[y * ccw + x] = uvrow[x * 2 + 0];
            encoder->prev_recon_cr[y * ccw + x] = uvrow[x * 2 + 1];
        }
    }
    return 0;
}

static void decide_gpu_ctu_skips(hevc_encoder_t *enc, int qp) {
    uint32_t threshold = hevc_gpu_pframe_skip_threshold(qp);
    uint32_t total_sad = 0;
    for (uint32_t row = 0; row < enc->height_ctu; row++) {
        for (uint32_t col = 0; col < enc->width_ctu; col++) {
            uint32_t idx = row * enc->width_ctu + col;
            int ctu_x = (int)col * HEVC_CTU_SIZE, ctu_y = (int)row * HEVC_CTU_SIZE;
            uint32_t sad = compute_sad_ctu_zero(enc, ctu_x, ctu_y);
            total_sad += sad;
            bool skip = sad <= threshold;
            enc->gpu_ctu_skip[idx] = skip ? 1u : 0u;
            if (skip) {
                int mx = ctu_x / HEVC_PU_SIZE, my = ctu_y / HEVC_PU_SIZE;
                for (int py = 0; py < HEVC_CTU_SIZE / HEVC_PU_SIZE; py++)
                    for (int px = 0; px < HEVC_CTU_SIZE / HEVC_PU_SIZE; px++)
                        enc->luma_mode_map[(my + py) * enc->mode_map_stride + (mx + px)] = HEVC_MODE_DC;
            }
        }
    }
    enc->last_frame_sad = total_sad;

    if (getenv("BC250_HEVC_GPU_DEBUG_STATS")) {
        size_t nctu = (size_t)enc->width_ctu * enc->height_ctu;
        size_t n_skip = 0;
        for (size_t i = 0; i < nctu; i++) n_skip += enc->gpu_ctu_skip[i] ? 1u : 0u;
        fprintf(stderr, "[HEVC_GPU_STATS] frame=%u qp=%d threshold=%u total_sad=%u skip=%zu/%zu\n",
                enc->frame_count, qp, threshold, total_sad, n_skip, nctu);
    }
}

/* Entropy-code one frame from hevc_intra_wavefront.comp's output. The shader
 * has already done mode decision, transform, quantization and reconstruction
 * for every CTU; nothing here recomputes any of it, and nothing here touches
 * encoder->src_* / recon_* / luma_mode_map's CPU-path meaning beyond the mode
 * map, which this path fills from the GPU's decisions so MPM derivation sees
 * what the decoder will see.
 *
 * Coding structure, fixed for every CTU (see the use_gpu field comment):
 *   split_cu_flag = 0        -> one 16x16 CU (MinCb is 8, so this IS coded)
 *   part_mode                -> not coded, log2CbSize != MinCbLog2SizeY,
 *                               so PART_2Nx2N is inferred: one PU
 *   split_transform_flag     -> not coded, MaxTrafoDepth = 0 + IntraSplitFlag
 *                               = 0, so a single 16x16 luma TU is inferred
 *                               (this is what needs SPS MaxTb = 16)
 *   scanIdx                  -> 0 (diagonal) for every block here. The
 *                               mode-dependent scan of 7.4.9.11 only applies
 *                               at log2TrafoSize 2, or 3 for luma; this path
 *                               has 16x16 luma and 8x8 chroma, neither of
 *                               which qualifies.
 *
 * The PPS this shares with the CPU path already has sign_data_hiding,
 * transform_skip and cu_qp_delta all disabled, so transform_unit() carries
 * no syntax beyond the cbf flags and the residuals. */
static int encode_core_gpu(hevc_encoder_t *encoder, uint8_t *output_buf, size_t output_size,
                           bool is_idr,
                           const int32_t *gmodes, const int32_t *gcoeffs, const uint32_t *gcbf)
{
    /* `is_idr` is the caller's decision, not this function's - see
     * hevc_encoder_encode_frame() and docs/notes/c7-gpu-pframes.md. It is
     * NOT simply "frame_count % gop_size == 0": the caller must also force
     * it whenever gpu_compute_hevc_dispatch_intra() reports the reference
     * image was just (re)created (out_recon_was_reset), since this
     * function has no way to see that on its own. */
    encoder->force_idr = false;
    if (is_idr) {
        encoder->poc = 0;
        /* No CTU is a skip on an IDR. The dispatch that already ran ignored
         * gpu_ctu_skip for this frame too (see gpu_compute_hevc_dispatch_
         * intra()'s NULL/recon-reset handling) - this keeps the CPU-side
         * skip bookkeeping in agreement with what was actually dispatched,
         * which is exactly the kind of encoder/decoder disagreement this
         * project's history says to take seriously (docs/hevc-gpu-intra.md's
         * split_cu_flag ctxInc bug). */
        memset(encoder->gpu_ctu_skip, 0,
               (size_t)encoder->width_ctu * encoder->height_ctu * sizeof(uint32_t));
    }

    /* Already chosen before the dispatch - the shader quantized with it.
     * Re-deriving here would signal a QP the coefficients were not
     * quantized at. */
    pick_frame_qp(encoder, 0);
    encoder->qp_already_decided = false;

    /* This memset zeroes luma_mode_map back to Planar (mode 0) for every
     * position, including the ones decide_gpu_ctu_skips() already marked
     * HEVC_MODE_DC before the dispatch that used it. Re-mark them below,
     * from gpu_ctu_skip (the authoritative decision this function also
     * signals from), rather than trusting whatever the pre-dispatch pass
     * left behind. */
    memset(encoder->luma_mode_map, 0,
           (size_t)encoder->mode_map_stride * (encoder->coded_height / HEVC_PU_SIZE));

    bool write_param_sets = is_idr;
    if (write_param_sets) {
        encoder->pps_init_qp = encoder->qp;
    }
    int slice_qp_delta = encoder->qp - encoder->pps_init_qp;

    bitstream_t slice_bs;
    bs_init(&slice_bs, encoder->slice_rbsp, encoder->slice_rbsp_cap);

    bs_write1(&slice_bs, 1); /* first_slice_segment_in_pic_flag */
    if (is_idr) {
        bs_write1(&slice_bs, 1); /* no_output_of_prior_pics_flag (IRAP) */
    }
    bs_write_ue(&slice_bs, 0); /* slice_pic_parameter_set_id */
    bs_write_ue(&slice_bs, is_idr ? 2 : 1); /* slice_type: 2 = I, 1 = P */

    if (!is_idr) {
        /* Byte-for-byte the same P-slice header syntax as encode_core()'s
         * (CPU path) - same SPS short-term RPS entry (DeltaPoc = -1), same
         * PPS, so a decoder cannot tell which encoder path produced this
         * from the header alone. */
        bs_write_u(&slice_bs, 4, encoder->poc & 0xF);  /* slice_pic_order_cnt_lsb */
        bs_write1(&slice_bs, 1);                        /* short_term_ref_pic_set_sps_flag = 1 */
        bs_write1(&slice_bs, 0);                        /* num_ref_idx_active_override_flag = 0 */
        bs_write_ue(&slice_bs, 0);                       /* five_minus_max_num_merge_cand = 0 */
    }

    bs_write_se(&slice_bs, slice_qp_delta); /* slice_qp_delta relative to active PPS */
    /* slice_loop_filter_across_slices_enabled_flag omitted - see the CPU
     * path's slice header and write_pps()'s comment. Both paths share one
     * PPS, so this bit's presence condition is false for both. */
    bs_rbsp_trailing_bits(&slice_bs);

    hevc_cabac_t cab;
    hevc_cabac_init(&cab, &slice_bs);
    hevc_cabac_reset_contexts(&cab, encoder->qp, is_idr ? 2 : 1);
    hevc_cabac_start(&cab);

    int16_t cl[256], ccb[64], ccr[64];

    uint32_t total_ctus = encoder->width_ctu * encoder->height_ctu;
    uint32_t ctu_idx = 0;
    for (uint32_t row = 0; row < encoder->height_ctu; row++) {
        for (uint32_t col = 0; col < encoder->width_ctu; col++) {
            uint32_t ctu = row * encoder->width_ctu + col;
            int ctu_x = (int)col * HEVC_CTU_SIZE, ctu_y = (int)row * HEVC_CTU_SIZE;
            int mx = ctu_x / HEVC_PU_SIZE, my = ctu_y / HEVC_PU_SIZE;

            bool is_skip = (!is_idr) && (encoder->gpu_ctu_skip[ctu] != 0);

            /* split_cu_flag is a coding_QUADTREE element, coded before
             * coding_unit() is ever entered - i.e. before cu_skip_flag,
             * unconditionally, for EVERY CTU regardless of slice type or
             * what coding_unit() will turn out to contain. This was
             * previously coded only on the non-skip path below (after an
             * early `continue` for skip CTUs that never reached it at
             * all) - a real bitstream bug found by this file's own
             * off-board test harness (docs/notes/c7-gpu-pframes.md): every
             * skip CTU was missing this bin entirely, and every non-skip
             * P-slice CTU had cu_skip_flag/pred_mode_flag coded BEFORE it
             * instead of after, densely reordering every following bin for
             * the rest of the slice. All-intra frames never had this bug -
             * is_idr never took the is_skip branch and pred_mode_flag is
             * never coded on that path either, so split_cu_flag was already
             * the first thing written per CTU there; it only broke once a
             * P-slice (is_idr == false) existed to code cu_skip_flag/
             * pred_mode_flag ahead of it. ctxInc is 0 always here - see the
             * comment that used to sit directly above this call, kept
             * below at its new call site's rationale. */
            hevc_cabac_code_split_cu_flag(&cab, 0, 0);

            if (!is_idr) {
                /* cu_skip_flag ctxInc (9.3.4.2.2): condL + condA, condX = 1
                 * iff that neighbour CTU exists and was ITSELF coded skip.
                 * Same formula as the CPU path's encode_cu() - explicitly
                 * checked and ruled out as a bug source there
                 * (docs/hevc_scope_note.md's "what was ruled out") - just
                 * indexed per-CTU instead of per-8x8-CU, since this path
                 * has exactly one CU per CTU. Unlike split_cu_flag's ctxInc
                 * just above (which IS 0 here, for a different, depth-based
                 * reason - see its own comment), this one keeps the CPU
                 * path's own left/above-existence formula because
                 * cu_skip_flag's ctxInc really is about existence, not
                 * depth (9.3.4.2.2 vs 9.3.4.2.1). */
                int cond_l = (col > 0 && encoder->gpu_ctu_skip[ctu - 1]) ? 1 : 0;
                int cond_a = (row > 0 && encoder->gpu_ctu_skip[ctu - encoder->width_ctu]) ? 1 : 0;
                hevc_cabac_code_cu_skip_flag(&cab, is_skip ? 1 : 0, cond_l + cond_a);
            }

            if (is_skip) {
                /* merge_idx = 0, unconditionally - not a shortcut around
                 * building the real 8.5.3.2.2 candidate list, but exact,
                 * because every one of that list's five entries is
                 * provably (0,0) on this path: every spatial neighbour
                 * this path can ever mark "inter" is itself a zero-motion
                 * SKIP (real motion compensation is out of scope - see the
                 * gpu_ctu_skip field comment and docs/backlog.md C9's
                 * "still open after this"), sps_temporal_mvp_enabled_flag
                 * is 0 (write_sps() - no temporal candidate), and a
                 * P-slice's zero-candidate padding (8.5.3.2.1) is (0,0)/
                 * refIdx 0 by construction. A list that can only ever
                 * contain (0,0) in all five slots makes merge_idx's VALUE
                 * unobservable in the reconstructed picture - any index a
                 * decoder derives decodes to the same motion - so this
                 * signals the cheapest legal one instead of deriving a
                 * list whose content could never change the outcome. This
                 * is NOT the CPU path's derive_merge_candidates(): that
                 * function's z-scan/availability geometry is specific to
                 * four 8x8 NxN CUs per CTU and does not describe this
                 * path's one-CU-per-CTU structure - see this file's top
                 * comment on why the two paths are different coders. */
                hevc_cabac_code_merge_idx(&cab, 0);

                /* DC for MPM purposes - ITU-T H.265 8.4.2, CuPredMode !=
                 * MODE_INTRA forces candIntraPredModeX to INTRA_DC.
                 * Re-marked here (decide_gpu_ctu_skips() already did this
                 * once, before dispatch) because the luma_mode_map memset
                 * above re-zeroed the whole map to Planar after that ran. */
                for (int py = 0; py < HEVC_CTU_SIZE / HEVC_PU_SIZE; py++)
                    for (int px = 0; px < HEVC_CTU_SIZE / HEVC_PU_SIZE; px++)
                        encoder->luma_mode_map[(my + py) * encoder->mode_map_stride + (mx + px)] = HEVC_MODE_DC;

                ctu_idx++;
                hevc_cabac_encode_terminate(&cab, ctu_idx == total_ctus ? 1 : 0);
                continue;
            }

            if (!is_idr) {
                hevc_cabac_code_pred_mode_flag(&cab, 1 /* MODE_INTRA */);
            }

            /* Clamp both GPU-supplied decisions. These cross a device->host
             * staging boundary, and a stale or partially-written buffer
             * would otherwise index the MPM tables or the chroma
             * candidate list out of range. */
            /* HEVC_MODE_COUNT (35) intra modes, 0..34 (Planar, DC, and 33
             * angular). Added to hevc_intra.h by backlog A6, which also
             * gave the CPU path (hevc_choose_luma_mode()) this same full
             * range - this clamp predates that and needed no other
             * change. */
            int mode = gmodes[ctu];
            if (mode < 0 || mode >= HEVC_MODE_COUNT) mode = HEVC_MODE_DC;
            /* BC250_HEVC_DEBUG_MODES=1: same diagnostic the CPU path already
             * has (its own call site above logs one line per 4x4 luma PU) -
             * this path has one PU per CTU (undivided 16x16), so one line
             * per CTU is the equivalent granularity. Added for
             * docs/hevc-shader-audit.md's "prove coverage first" experiment:
             * the GPU path had gmodes[ctu] read here and never logged, so
             * there was no way to confirm which of the 35 modes real content
             * actually selects on this path, off the strength of a mode
             * histogram, the way A5/A6's off-board mode-coverage tables
             * already did for the CPU path. Diagnostic only. */
            if (getenv("BC250_HEVC_DEBUG_MODES"))
                fprintf(stderr, "[GPU_MODE] ctu=%u x=%u y=%u mode=%d\n",
                        ctu, col * 16u, row * 16u, mode);
            uint32_t flags = gcbf[ctu];
            int cbf_luma = (int)(flags & 1u);
            int cbf_cb   = (int)((flags >> 1) & 1u);
            int cbf_cr   = (int)((flags >> 2) & 1u);
            int chroma_idx = (int)((flags >> 8) & 0xFFu);
            if (chroma_idx > 4) chroma_idx = 4;

            /* split_cu_flag for THIS CTU was already coded above, before
             * cu_skip_flag - see that call site for both the ctxInc-is-
             * always-0 rationale (a depth argument, 9.3.4.2.1) and the
             * historical intra-only bug it documents (a DIFFERENT ctxInc
             * mistake, 9.3.4.2.2, that a real board run caught: 5.0 dB PSNR,
             * decoded without a single ffmpeg error). Nothing else in
             * coding_quadtree() belongs between that flag and here. */

            /* MPM. candIntraPredModeB is unconditionally INTRA_DC here: this
             * CU starts at a CTU boundary, so yCb-1 always crosses into the
             * CTU row above, which 8.4.2 forces to DC as a normative rule
             * rather than an availability test (see the long comment on the
             * CPU path's equivalent - getting this wrong was a real bug).
             * candIntraPredModeA (left) now legitimately reads a SKIP
             * neighbour's DC marking here too, when the left CTU is a
             * P-frame skip - that is the same rule, applied to the new
             * case this feature introduces. */
            int left_avail = ctu_x > 0;
            int left_mode = left_avail ? encoder->luma_mode_map[my * encoder->mode_map_stride + (mx - 1)] : 0;
            int mpm[3];
            hevc_derive_mpm(left_mode, left_avail, 0, 0, mpm);
            int pred_idx = hevc_cabac_code_intra_luma_flag(&cab, mode, mpm);
            hevc_cabac_code_intra_luma_data(&cab, mode, pred_idx, mpm);

            hevc_cabac_code_intra_chroma_pred_mode_idx(&cab, chroma_idx);

            /* transform_tree at trafoDepth 0: chroma cbf bits, then the
             * single luma leaf, then the chroma residuals. */
            hevc_cabac_code_cbf_chroma(&cab, cbf_cb, 0);
            hevc_cabac_code_cbf_chroma(&cab, cbf_cr, 0);
            hevc_cabac_code_cbf_luma(&cab, cbf_luma, 0);

            const int32_t *cc = gcoeffs + (size_t)ctu * 384;
            if (cbf_luma) {
                for (int i = 0; i < 256; i++) cl[i] = (int16_t)cc[i];
                hevc_cabac_code_residual(&cab, cl, 4, 1, 0);
            }
            if (cbf_cb) {
                for (int i = 0; i < 64; i++) ccb[i] = (int16_t)cc[256 + i];
                hevc_cabac_code_residual(&cab, ccb, 3, 0, 0);
            }
            if (cbf_cr) {
                for (int i = 0; i < 64; i++) ccr[i] = (int16_t)cc[320 + i];
                hevc_cabac_code_residual(&cab, ccr, 3, 0, 0);
            }

            /* Record the mode across this CU's 4x4 grid for the next CTU's
             * MPM. Only the rightmost column is ever read back (the row above
             * is DC-forced), but filling all of it keeps the map's meaning
             * the same as the CPU path's. */
            for (int py = 0; py < HEVC_CTU_SIZE / HEVC_PU_SIZE; py++)
                for (int px = 0; px < HEVC_CTU_SIZE / HEVC_PU_SIZE; px++)
                    encoder->luma_mode_map[(my + py) * encoder->mode_map_stride + (mx + px)] = (int8_t)mode;

            ctu_idx++;
            hevc_cabac_encode_terminate(&cab, ctu_idx == total_ctus ? 1 : 0);
        }
    }

    hevc_cabac_finish(&cab);
    bs_rbsp_trailing_bits(&slice_bs);

    if (bs_overflowed(&slice_bs)) return -1;

    size_t total = 0;
    if (write_param_sets) {
        total += write_vps(encoder->scratch_out + total, encoder->scratch_out_cap - total);
        total += write_sps(encoder->scratch_out + total, encoder->scratch_out_cap - total,
                           encoder->coded_width, encoder->coded_height,
                           encoder->width, encoder->height,
                           hevc_pick_level_idc(encoder->coded_width, encoder->coded_height),
                           4 /* MaxTb = 16: one 16x16 luma TU per CU */);
        total += write_pps(encoder->scratch_out + total, encoder->scratch_out_cap - total, encoder->qp);
    }

    {
        bitstream_t out_bs;
        bs_init(&out_bs, encoder->scratch_out + total, encoder->scratch_out_cap - total);
        bs_write_nal_header_hevc(&out_bs, is_idr ? NAL_UNIT_CODED_SLICE_IDR_W_RADL : NAL_UNIT_CODED_SLICE_TRAIL_R);
        size_t off = bs_bytes_written(&out_bs);
        size_t rbsp = bs_bytes_written(&slice_bs);
        if (bs_overflowed(&out_bs) ||
            encoder->scratch_out_cap - total - off < ebsp_worst_case(rbsp)) return -1;
        size_t ebsp = bs_rbsp_to_ebsp(encoder->scratch_out + total + off, encoder->scratch_out_cap - total - off,
                                      encoder->slice_rbsp, rbsp);
        total += off + ebsp;
    }

    if (total > output_size) return -1;
    memcpy(output_buf, encoder->scratch_out, total);

    if (total > 0 && encoder->rc.mode != RC_CQP) rc_update_stats(&encoder->rc, (int)(total * 8));

    /* This frame's own reconstruction (in ctx->recon_image - the CTUs the
     * shader wrote fresh, plus the CTUs left untouched because
     * decide_gpu_ctu_skips()/the shader's skip early-return copied last
     * frame's forward instead - see docs/notes/c7-gpu-pframes.md) is now a
     * complete, valid reference for the next P-frame, whether this frame
     * was itself an IDR or a P. Unlike the all-intra version of this
     * function, has_ref must become true here - the whole point of this
     * feature is giving the next frame something to reference. */
    encoder->has_ref = true;
    encoder->poc++;
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

    encoder->num_gpu_mvs = 0;

    /* GPU intra path. Everything it needs happens on the GPU, so it skips
     * the NV12 download the CPU path below depends on entirely. Any failure
     * - no pipeline, allocation refused, a staging pointer that is still
     * NULL because this is the first frame - falls through to the CPU path
     * rather than producing a broken frame. */
    if (encoder->use_gpu && gpu_ctx && input_surface.y_plane != VK_NULL_HANDLE) {
        /* Same is_idr formula as encode_core() (CPU path) - EXCEPT this
         * path only ever considers a P-frame at all when
         * BC250_HEVC_GPU_PFRAME=1 was latched at create time
         * (docs/notes/c7-gpu-pframes.md). With that flag unset, is_idr is
         * unconditionally true here and every line below that touches
         * gpu_ctu_skip, prev_recon buffers or decide_gpu_ctu_skips() is
         * skipped, which reproduces this function's exact pre-existing
         * (all-intra, board-validated) behaviour byte for byte - see
         * use_gpu_pframe's field comment for why that matters. */
        bool is_idr_candidate = !encoder->use_gpu_pframe ||
            (encoder->frame_count % encoder->gop_size == 0) || encoder->force_idr || !encoder->has_ref;

        /* Decide the QP BEFORE dispatching: the shader quantizes with the
         * value handed to it here, and encode_core_gpu() then signals that
         * same value in the slice header. Deciding afterwards (as this
         * originally did) quantized every frame at the PREVIOUS frame's QP
         * while signalling the new one, so the decoder dequantized
         * coefficients against the wrong step size. Measured symptom: the
         * GPU path's output size was pinned near 520 KB whether the
         * requested bitrate was 1M or 8M - rate control could not move it
         * at all - and ffmpeg reported CABAC_MAX_BIN errors once the two
         * QPs diverged far enough that the coded levels no longer matched
         * the signalled step. Same is_idr-gated estimate encode_core() uses
         * for the same reason (rate control's temporal-complexity ratio). */
        pick_frame_qp(encoder, is_idr_candidate ? 0 : encoder->last_frame_sad);

        /* P-frame candidate: no host download of any kind here any more.
         * docs/notes/c7-gpu-pframes.md's first cut downloaded this frame's
         * source AND the previous frame's reconstruction to host memory and
         * ran the zero-motion SAD decision on the CPU - board-measured
         * (docs/backlog.md C7) at roughly HALF the intra-only throughput at
         * 720p, dominated by two ~1.66 MB NV12 readbacks (each a GPU-CPU
         * sync point) plus the CPU SAD loop, every P-frame. hevc_pframe_
         * skip.comp (docs/notes/c7-pframe-throughput.md) now makes that
         * same decision on the GPU, reading srcY/srcUV/reconY/reconUV where
         * they already live and writing the mask directly into the buffer
         * hevc_intra_wavefront.comp already reads - so all that is needed
         * here is the threshold value, which is pure arithmetic on `qp`. */
        uint32_t skip_threshold = is_idr_candidate ? 0u
            : hevc_gpu_pframe_skip_threshold((int)encoder->qp);

        gpu_compute_begin_picture(gpu_ctx, input_surface);
        int recon_was_reset = 0;
        int rc = gpu_compute_hevc_dispatch_intra(gpu_ctx, input_surface,
                                                 (int)encoder->coded_width, (int)encoder->coded_height,
                                                 (int)encoder->width, (int)encoder->height,
                                                 encoder->qp,
                                                 !is_idr_candidate, skip_threshold,
                                                 &recon_was_reset);
        gpu_compute_end_picture(gpu_ctx);
        int slot = gpu_compute_submitted_slot(gpu_ctx);
        gpu_compute_sync_slot(gpu_ctx, slot);

        /* A reset recon_image means gpu_compute_hevc_dispatch_intra() ignored
         * want_pframe_skip (see that function's doc comment) and coded every
         * CTU intra regardless - encode_core_gpu() must be told the same
         * thing, or its entropy loop would signal cu_skip_flag for CTUs the
         * shader never actually skipped. */
        bool is_idr_final = is_idr_candidate || (recon_was_reset != 0);

        /* Read the GPU's skip decision back - one tiny buffer
         * (width_ctu*height_ctu uint32s, e.g. 3600 bytes at 1280x720)
         * instead of the ~3.3 MB of raw pixels the old CPU-side decision
         * needed. Still needed on the host: encode_core_gpu()'s CABAC stage
         * (cu_skip_flag's ctxInc, merge_idx, the MPM DC-for-skip-neighbour
         * rule) reads enc->gpu_ctu_skip[] directly - that bookkeeping is
         * inherently host-side entropy coding, not something this task
         * moves to the GPU (see docs/notes/c7-pframe-throughput.md). */
        if (!is_idr_final) {
            void *skip_data = NULL;
            size_t skip_sz = 0;
            size_t nctu = (size_t)encoder->width_ctu * encoder->height_ctu;
            if (gpu_compute_get_hevc_skip_data_slot(gpu_ctx, slot, &skip_data, &skip_sz) == 0 &&
                skip_data && skip_sz >= nctu * sizeof(uint32_t)) {
                memcpy(encoder->gpu_ctu_skip, skip_data, nctu * sizeof(uint32_t));
                /* last_frame_sad is a rate-control heuristic only (fed to
                 * the NEXT frame's pick_frame_qp() as a temporal-complexity
                 * estimate - see that call above) - not a correctness value,
                 * per this function's own established convention. The exact
                 * CPU-computed total SAD is no longer available without the
                 * pixel downloads this change removes; every non-skip CTU
                 * failed the threshold by definition, so (non-skip count) *
                 * threshold is a deliberate, order-of-magnitude-correct
                 * proxy in the same units, not the same number decide_gpu_
                 * ctu_skips() would have produced. */
                size_t n_skip = 0;
                for (size_t i = 0; i < nctu; i++) n_skip += encoder->gpu_ctu_skip[i] ? 1u : 0u;
                encoder->last_frame_sad = (uint32_t)((nctu - n_skip) * (size_t)skip_threshold);
            } else {
                /* Readback unavailable (shader failed to load, or a
                 * transient mapping issue) - no CTU skips this frame rather
                 * than trusting whatever was last in the buffer. */
                memset(encoder->gpu_ctu_skip, 0, nctu * sizeof(uint32_t));
                encoder->last_frame_sad = 0;
            }
        }

        /* BC250_DUMP_RECON_FRAMES=1: the encoder's OWN reconstruction, as the
         * shader left it. The drift oracle - decode the resulting bitstream
         * with the loop filter disabled (ffmpeg -skip_loop_filter all; SAO is
         * off in our SPS) and it must match this byte for byte, because a
         * conforming decoder derives exactly the picture the encoder
         * predicted from. Anything else means the bitstream does not describe
         * what the encoder actually built, which is the failure mode a
         * silent decode and a good PSNR can both miss.
         *
         * CODED dimensions, not display: recon_image is allocated at the
         * padded size (1088 for 1080p), and the decoder's output is cropped
         * by the SPS conformance window, so the comparison is against the
         * top `height` rows of this dump. */
        gpu_compute_debug_dump_recon(gpu_ctx, (int)encoder->coded_width,
                                     (int)encoder->coded_height);

        if (rc == 0) {
            void *md = NULL, *cd = NULL, *bd = NULL;
            size_t ms = 0, cs = 0, bs_sz = 0;
            if (gpu_compute_get_hevc_mode_staging_data_slot(gpu_ctx, slot, &md, &ms) == 0 &&
                gpu_compute_get_hevc_coeff_staging_data_slot(gpu_ctx, slot, &cd, &cs) == 0 &&
                gpu_compute_get_hevc_cbf_staging_data_slot(gpu_ctx, slot, &bd, &bs_sz) == 0 &&
                md && cd && bd) {
                size_t nctu = (size_t)encoder->width_ctu * encoder->height_ctu;
                if (ms >= nctu * sizeof(int32_t) &&
                    cs >= nctu * 384 * sizeof(int32_t) &&
                    bs_sz >= nctu * sizeof(uint32_t)) {
                    return encode_core_gpu(encoder, output_buf, output_size, is_idr_final,
                                           (const int32_t *)md, (const int32_t *)cd,
                                           (const uint32_t *)bd);
                }
            }
        }
        /* Fall through to the CPU path. The dispatch above already consumed
         * this frame's begin/end picture pair, so re-running the H.264
         * dispatch here would be a second submission of the same surface;
         * instead just download and encode on the CPU. Sync-bracketed for
         * the same reason as the main path below. */
        if (!is_idr_candidate) {
            /* This frame was meant to be a GPU P-frame candidate - see
             * hevc_refresh_prev_recon_from_gpu()'s comment for why the CPU
             * path about to run needs this. */
            hevc_refresh_prev_recon_from_gpu(encoder, gpu_ctx);
        }
        gpu_compute_dmabuf_sync_start(gpu_ctx, input_memory);
        gpu_compute_download_nv12(gpu_ctx, &input_surface, input_memory,
                                  encoder->dl_y, (int)encoder->width,
                                  encoder->dl_uv, (int)encoder->width,
                                  (int)encoder->width, (int)encoder->height);
        gpu_compute_dmabuf_sync_end(gpu_ctx, input_memory);
        return encode_core(encoder, output_buf, output_size);
    }

    bool is_idr = (encoder->frame_count % encoder->gop_size == 0) || encoder->force_idr || !encoder->has_ref;

    /* Preserve the existing driver's Vulkan image-layout-transition and
     * staging/fence contract (see va_backend.c's bc250_EndPicture() comment).
     * By passing is_intra = (is_idr ? 1 : 0), the GPU computes motion
     * estimation for each 16x16 CTU on P-slices across its 40 CUs! */
    if (gpu_ctx && input_surface.y_plane != VK_NULL_HANDLE) {
        gpu_compute_begin_picture(gpu_ctx, input_surface);
        gpu_compute_dispatch_encode(gpu_ctx, input_surface, encoder->width, encoder->height,
                                     encoder->qp, is_idr ? 1 : 0, 1);
        gpu_compute_end_picture(gpu_ctx);
        gpu_compute_sync(gpu_ctx);

        /* Unconsumed since the motion search was removed - see
         * derive_merge_candidates() and docs/notes/dead-motion-search.md.
         * Deliberately NOT deleted here: it is the tail of the GPU ME
         * dispatch above, that dispatch is shared with the H.264 path and
         * is kept for its Vulkan layout/fence bookkeeping (this file's top
         * comment), and neither half can be validated without the board.
         * Removing the pair is a board-run item, not an off-board one. The
         * cost is one memcpy per P-frame against a search that was ~39 SADs
         * per 8x8 CU, so leaving it costs approximately nothing. */
        if (!is_idr && encoder->has_ref && encoder->gpu_mvs) {
            void *mv_data = NULL;
            size_t mv_size = 0;
            if (gpu_compute_get_mv_staging_data(gpu_ctx, &mv_data, &mv_size) == 0 && mv_data) {
                size_t max_bytes = (size_t)encoder->width_ctu * encoder->height_ctu * sizeof(gpu_mv_t);
                size_t copy_bytes = (mv_size < max_bytes) ? mv_size : max_bytes;
                memcpy(encoder->gpu_mvs, mv_data, copy_bytes);
                encoder->num_gpu_mvs = (uint32_t)(copy_bytes / sizeof(gpu_mv_t));
            }
        }

        /* input_surface/input_memory is the live VA-API surface a real
         * Sunshine session writes into directly via its own GL blit, into
         * this surface's exported DMA-BUF - a separate GPU context, API and
         * process from this driver's Vulkan one, with nothing shared to
         * order this CPU read against that write. See gpu_compute.h's
         * gpu_compute_dmabuf_sync_start() doc comment. Without the bracket,
         * what HEVC actually encodes is subject to the same torn-read race
         * that BC250_DUMP_REAL_INPUT dumps first exposed - not just a
         * diagnostic capture of it. */
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

    encoder->num_gpu_mvs = 0;

    /* BC250_HEVC_FAKE_GPU_MV="<dx>,<dy>" (INTEGER pel) stands in for
     * motion_estimation.comp's per-CTU output on a machine with no GPU.
     *
     * This exists because the inter path's only source of a NON-ZERO
     * motion vector is that GPU readback: with no GPU, every spatial merge
     * candidate is seeded from CUs whose MV is zero, so the merge list is
     * all-zero, every SKIP is a plain co-located copy, and the parts of
     * derive_merge_candidates() that actually pick between different
     * vectors never run. Without this hook tools/hevc_host_drift.sh can
     * confirm the P-frame *syntax* but cannot reach the MV-selection logic
     * at all - and that is where the real defect turned out to be.
     *
     * Say the current state plainly: since the motion search was removed
     * (docs/notes/dead-motion-search.md) NOTHING reads encoder->gpu_mvs, so
     * this hook has no effect on the output and the six drift cases that
     * set it are byte-identical to the same cases without it. It is kept,
     * with the readback in hevc_encoder_encode_frame() it stands in for,
     * because it is the seam a real MVD/AMVP path would reconnect to, and
     * because deleting the only off-board source of a non-zero vector is
     * how the injection bug above stayed invisible the first time. */
    if (encoder->gpu_mvs && encoder->has_ref) {
        const char *fake = getenv("BC250_HEVC_FAKE_GPU_MV");
        if (fake) {
            int fdx = atoi(fake);
            const char *comma = strchr(fake, ',');
            int fdy = comma ? atoi(comma + 1) : 0;
            uint32_t n = encoder->width_ctu * encoder->height_ctu;
            for (uint32_t i = 0; i < n; i++) {
                encoder->gpu_mvs[i].mvx = fdx * 4;  /* quarter-pel, as the shader emits */
                encoder->gpu_mvs[i].mvy = fdy * 4;
                encoder->gpu_mvs[i].sad = 0;
            }
            encoder->num_gpu_mvs = n;
        }
    }

    for (uint32_t y = 0; y < encoder->height; y++)
        memcpy(encoder->dl_y + (size_t)y * encoder->width, y_plane + (size_t)y * y_pitch, encoder->width);
    for (uint32_t y = 0; y < encoder->height / 2; y++)
        memcpy(encoder->dl_uv + (size_t)y * encoder->width, uv_plane + (size_t)y * uv_pitch, encoder->width);

    return encode_core(encoder, output_buf, output_size);
}

/* Off-board exercise of the GPU path's P-frame logic (docs/notes/
 * c7-gpu-pframes.md), with the same "stand in for the GPU readback" role
 * BC250_HEVC_FAKE_GPU_MV plays for the CPU path above. There is no board
 * and no working Vulkan device available in this environment - this is the
 * only way the new encode_core_gpu() P-slice code (RPS/slice-header
 * signalling, cu_skip_flag ctxInc, merge_idx, the DC-for-skip-neighbour MPM
 * rule) runs at all in this pass, or ever ran before a board picks it up.
 *
 * Unlike hevc_encoder_encode_raw() above, the "reference picture" is an
 * explicit argument (`ref_y_plane`/`ref_uv_plane`) rather than carried-over
 * encoder state, because there is no real shader run here to produce one:
 * the real GPU path's reference is whatever hevc_intra_wavefront.comp left
 * in ctx->recon_image last frame, and this function has no such image to
 * read forward from. Passing the SAME content as both `y_plane`/`uv_plane`
 * (this frame's source) and the reference is how to force a deterministic,
 * fully-skipped P-frame for the strongest check this function can do
 * off-board (see below); passing a genuinely different reference exercises
 * the skip-decision threshold and a real mix of skip/non-skip CTUs, but at
 * that point `synth_modes`/`synth_coeffs`/`synth_cbf` for the non-skip CTUs
 * are still not a claim about real intra coding - see below.
 *
 * What this DOES verify, when driven through tools/hevc_host_drift.sh-style
 * decode: the bitstream syntax (RPS, POC, P-slice header, skip signalling)
 * parses and decodes silently, and - critically, since a skip CTU's
 * decoded picture is byte-determined regardless of what "shader" produced
 * the non-skip CTUs - that a fully-skipped P-frame decodes to a bit-exact
 * copy of the reference frame. That is a real, spec-grounded correctness
 * check on the part of this feature that is pure CPU-side signalling.
 *
 * What this does NOT verify: anything about hevc_intra_wavefront.comp
 * itself (the skip_mask early return, the recon_image reuse, the
 * descriptor/buffer plumbing in gpu_compute.c). `synth_modes`/
 * `synth_coeffs`/`synth_cbf` (any may be NULL for an all-DC/all-zero-
 * residual default) are NOT a claim about what the real shader would
 * produce for a non-skip CTU - they only need to be A valid, decodable
 * intra CTU so the surrounding P-slice signalling can be exercised on a
 * realistic mix of skip and non-skip CTUs. Treat any PSNR/quality number
 * out of this function as meaningless; only decode-silence and skip-region
 * byte-exactness are real signal. */
int hevc_encoder_encode_gpu_raw(hevc_encoder_t *encoder,
                                const uint8_t *y_plane, int y_pitch,
                                const uint8_t *uv_plane, int uv_pitch,
                                const uint8_t *ref_y_plane, int ref_y_pitch,
                                const uint8_t *ref_uv_plane, int ref_uv_pitch,
                                const int32_t *synth_modes,
                                const int32_t *synth_coeffs,
                                const uint32_t *synth_cbf,
                                uint8_t *output_buf, size_t output_size)
{
    if (!encoder || !output_buf || !y_plane || !uv_plane) return -1;
    /* This path's SPS (MaxTb=16, one CU per CTU) only exists when use_gpu
     * was latched at create time - see that field's comment. */
    if (!encoder->use_gpu) return -1;

    for (uint32_t y = 0; y < encoder->height; y++)
        memcpy(encoder->dl_y + (size_t)y * encoder->width, y_plane + (size_t)y * y_pitch, encoder->width);
    for (uint32_t y = 0; y < encoder->height / 2; y++)
        memcpy(encoder->dl_uv + (size_t)y * encoder->width, uv_plane + (size_t)y * uv_pitch, encoder->width);
    pad_replicate(encoder->src_y, encoder->coded_width, encoder->coded_height,
                  encoder->dl_y, encoder->width, encoder->width, encoder->height);
    {
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
    }

    bool is_idr = !encoder->use_gpu_pframe ||
        (encoder->frame_count % encoder->gop_size == 0) || encoder->force_idr || !encoder->has_ref;

    if (!is_idr && ref_y_plane && ref_uv_plane) {
        /* Reference goes straight into prev_recon_y/cb/cr at CODED
         * dimensions - the caller is expected to already hand this at
         * coded_width x coded_height (e.g. a previous call's own `src_y`-
         * shaped buffer), so no pad_replicate step is needed or done here,
         * matching how the real GPU path's recon_image readback (coded
         * dims, no replication - see hevc_encoder_encode_frame()) behaves. */
        for (uint32_t y = 0; y < encoder->coded_height; y++)
            memcpy(encoder->prev_recon_y + (size_t)y * encoder->coded_width,
                   ref_y_plane + (size_t)y * ref_y_pitch, encoder->coded_width);
        uint32_t ccw = encoder->coded_width / 2, cch = encoder->coded_height / 2;
        for (uint32_t y = 0; y < cch; y++) {
            const uint8_t *uvrow = ref_uv_plane + (size_t)y * ref_uv_pitch;
            for (uint32_t x = 0; x < ccw; x++) {
                encoder->prev_recon_cb[y * ccw + x] = uvrow[x * 2 + 0];
                encoder->prev_recon_cr[y * ccw + x] = uvrow[x * 2 + 1];
            }
        }
    }

    pick_frame_qp(encoder, is_idr ? 0 : encoder->last_frame_sad);

    if (!is_idr) {
        decide_gpu_ctu_skips(encoder, encoder->qp);
    } else {
        memset(encoder->gpu_ctu_skip, 0,
               (size_t)encoder->width_ctu * encoder->height_ctu * sizeof(uint32_t));
        encoder->last_frame_sad = 0;
    }

    size_t nctu = (size_t)encoder->width_ctu * encoder->height_ctu;
    int32_t *modes = malloc(nctu * sizeof(int32_t));
    int32_t *coeffs = calloc(nctu * 384, sizeof(int32_t));
    uint32_t *cbf = calloc(nctu, sizeof(uint32_t));
    int ret = -1;
    if (modes && coeffs && cbf) {
        for (size_t i = 0; i < nctu; i++)
            modes[i] = synth_modes ? synth_modes[i] : HEVC_MODE_DC;
        if (synth_coeffs) memcpy(coeffs, synth_coeffs, nctu * 384 * sizeof(int32_t));
        for (size_t i = 0; i < nctu; i++)
            cbf[i] = synth_cbf ? synth_cbf[i] : 0u; /* cbf=0 -> no residual bits, flat DC block */

        ret = encode_core_gpu(encoder, output_buf, output_size, is_idr, modes, coeffs, cbf);
    }
    free(modes);
    free(coeffs);
    free(cbf);
    return ret;
}
