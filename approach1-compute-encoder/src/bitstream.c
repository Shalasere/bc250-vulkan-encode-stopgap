/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * bitstream.c - Bitstream NAL writer implementation
 *
 * ============================================================================
 * PERF REWRITE (branch perf/openh264-bitwriter-fallback, standby fallback -
 * see docs/../README or this branch's commit log for the sibling effort in
 * bc250-cavlc-perf diagnosing the same slowness a different way)
 * ============================================================================
 *
 * bc250's own profiling found bs_write_u()/bs_write_ue()/
 * bs_rbsp_trailing_bits() anomalously slow (~45us/macroblock of CAVLC+
 * bitstream CPU time), and -fopt-info-vec confirmed they don't
 * auto-vectorize - branchy, stateful, scalar bit manipulation is exactly
 * the class of code where hand implementation quality matters far more than
 * algorithm choice. This rewrite studies Cisco's openh264
 * (github.com/cisco/openh264, BSD-2-Clause, a real-time-oriented H.264
 * codec used in WebRTC/Chrome/Firefox) for its encoder-side bit-writer
 * technique and reimplements the same idea independently, in this file's
 * own style, WITHOUT copying its source.
 *
 * THE TECHNIQUE, AND WHAT WAS ACTUALLY WRONG WITH THE OLD CODE
 * --------------------------------------------------------------------------
 * The old bs_write_u() treated `buffer` itself as the only state: every
 * call re-derived a bit mask, then did a masked read-modify-write against
 * `buffer[byte_offset]` for every up-to-8-bit chunk it wrote - a load, an
 * AND-NOT, an OR, and a store, plus the mask/shift arithmetic to set them
 * up, EVERY TIME, even for a single bit. cavlc.c's hottest paths
 * (cavlc_write_one_level()'s level_prefix unary zero-runs, trailing-one
 * sign bits) call bs_write1()/bs_write_bit() - i.e. bs_write_u(bs,1,val) -
 * in tight per-bit loops, so this RMW-per-bit cost was being paid many
 * times per macroblock, on every macroblock, at 640x480-1920x1080
 * real-time frame rates.
 *
 * openh264's encoder bit-writer (codec/common/inc/golomb_common.h,
 * BsWriteBits() operating on an SBitStringAux) does not touch memory at
 * all for most calls. It keeps a small in-register accumulator
 * (`uiCurBits`, a uint32_t) plus a count of how many more bits currently
 * fit in it before it's full (`iLeftBits`). Writing N bits is: shift the
 * accumulator left by N and OR in the new bits - two register ops, no
 * branch beyond checking whether the accumulator would overflow, no
 * memory access. Only when the accumulator actually fills up does it do a
 * single unconditional 4-byte big-endian store to memory (never a
 * read-modify-write, since that memory was never partially written
 * before) and carry the remainder into a fresh accumulator. Exp-Golomb
 * (BsWriteUE) resolves the whole ue(v) codeword length in O(1) (a
 * lookup table for small values, a couple of shifts for large ones)
 * instead of a bit-by-bit unary scan, and issues it as a single
 * BsWriteBits() call - never a per-bit loop.
 *
 * This file reimplements that same idea - accumulate bits in a register,
 * defer the store, batch it, and resolve ue(v)'s length in O(1) - but
 * independently: no openh264 source was copied. Two deliberate
 * differences from openh264's own code, both because this codebase's
 * calling contract differs from openh264's internal one:
 *
 *   1. Batch granularity is one BYTE, not openh264's 4-byte word. Several
 *      places in this file (bs_write_sps()/bs_write_pps()) write a NAL
 *      header through this API and then immediately read the resulting
 *      bytes back out of the raw `buf` pointer directly (bs_rbsp_to_ebsp()),
 *      bypassing bitstream_t entirely, expecting every byte up to
 *      bs_bytes_written()'s return value to already be physically resident
 *      in memory. bs_bytes_written() is (and stays) a pure/const query with
 *      no side effects, so it cannot itself force a flush. Batching to a
 *      byte - rather than a 4-byte word - means every write that completes
 *      a byte commits it to memory immediately as part of that same call,
 *      so this invariant holds automatically with no extra synchronization
 *      calls anywhere, and `bit_offset`'s external meaning/range (0-7,
 *      "bits pending in the current not-yet-complete byte") is preserved
 *      exactly, unchanged from the pre-rewrite struct doc comment. This is
 *      a smaller memory-store reduction than openh264's own 4x-per-word
 *      batching, but it removes the same dominant cost (the masked RMW and
 *      its branchy setup arithmetic) that was actually responsible for the
 *      measured slowness, while sidestepping a real buffer-aliasing hazard
 *      a straight 4-byte port would have introduced into this codebase's
 *      call pattern (see this branch's commit log for the full analysis).
 *   2. bs_write_ue()'s O(1) codeword-length step uses a leading-zero-count
 *      (__builtin_clz, with a portable fallback) instead of openh264's
 *      256-entry g_kuiGolombUELength[] lookup table - exact for every
 *      value rather than only the <256 table-covered range, and needs no
 *      generated data table to reproduce.
 *
 * cavlc.c is completely unmodified by this rewrite (out of scope for this
 * branch, and not touched) - its per-bit bs_write_bit()/bs_write_bits()
 * calls hit this file's new, cheap accumulator path unchanged, with no
 * caller-visible difference in bitstream.h's function signatures or
 * behavior.
 */
#include "bitstream.h"
#include <string.h>

/* Count leading zero bits in a nonzero 32-bit value (bit 31 = MSB). Used
 * by bs_write_ue() to resolve the Exp-Golomb "zeros" prefix length in O(1)
 * instead of the previous while()-loop that right-shifted one bit at a
 * time until reaching 1. `x` is always >= 1 at every call site below. */
static inline int bc250_clz32(uint32_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(x);
#else
    int n = 0;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
#endif
}

void bs_init(bitstream_t *bs, uint8_t *buf, size_t size) {
    bs->buffer = buf;
    bs->size = size;
    bs->byte_offset = 0;
    bs->bit_offset = 0;
    bs->overflow = false;
    bs->accum = 0;
    /* No proactive buf[0]=0 here (unlike the pre-rewrite version): that
     * existed only to support the old read-modify-write into `buffer`.
     * This implementation never reads `buffer` back - every byte it writes
     * is a freshly computed, fully-formed value written exactly once - so
     * there is nothing to pre-zero. */
}

/* bs_write_u() itself now lives in bitstream.h as a `static inline`
 * function - see that header's comment on it for why (cross-TU call
 * overhead, without LTO, was masking the accumulator rewrite's actual
 * per-call savings on cavlc.c's hot per-bit loops). */

void bs_write_ue(bitstream_t *bs, uint32_t val) {
    /* code_num = val+1, computed in 64-bit so val==UINT32_MAX can't wrap
     * uint32_t arithmetic (matches the previous implementation's own
     * uint64_t temp for the same reason). */
    uint64_t code_num = (uint64_t)val + 1;

    if (code_num <= 0xFFFFFFFFULL) {
        /* The common case, and the only one any real CAVLC caller in this
         * codebase ever hits (levels, runs, coded_block_pattern, mvd,
         * skip_run, qp_delta are all small, spec-bounded values). The
         * Exp-Golomb "zeros" prefix length is exactly the bit position of
         * code_num's highest set bit - one clz instead of a variable-trip
         * shift-until-1 loop - then the whole codeword (zeros 0-bits
         * followed by the (zeros+1)-bit binary form of code_num) is issued
         * as two bs_write_u() calls, same as before, just without the loop
         * to get there. */
        uint32_t cn = (uint32_t)code_num;
        int zeros = 31 - bc250_clz32(cn);
        bs_write_u(bs, zeros, 0);
        bs_write_u(bs, zeros + 1, cn);
    } else {
        /* val == UINT32_MAX: code_num == 2^32, a 33-bit quantity that
         * doesn't fit bs_write_u()'s documented 1-32-bit-per-call
         * contract. No real H.264 syntax element this project emits ever
         * takes this value - this branch exists solely so
         * test_bitstream.c's "must not crash, must produce output"
         * extreme-edge-case check still terminates cleanly and
         * deterministically (32 zero bits, a single 1 bit, then 32 more
         * zero bits: a valid-shaped, if enormous, Exp-Golomb-style code),
         * via well-defined <=32-bit calls rather than one out-of-contract
         * 33-bit call. */
        bs_write_u(bs, 32, 0);
        bs_write1(bs, 1);
        bs_write_u(bs, 32, 0);
    }
}

void bs_write_se(bitstream_t *bs, int32_t val) {
    uint32_t uval;
    if (val <= 0) {
        uval = (uint32_t)(-(int64_t)val * 2);
    } else {
        uval = (uint32_t)((uint32_t)val * 2 - 1);
    }
    bs_write_ue(bs, uval);
}

void bs_rbsp_trailing_bits(bitstream_t *bs) {
    if (!bs) return;
    /* Old implementation: write the '1' stop bit, then loop bs_write1(0)
     * one bit at a time until byte-aligned (with an explicit overflow
     * check to avoid spinning forever at end-of-buffer - see this
     * function's pre-rewrite comment). bs_flush() below now does exactly
     * "zero-pad and commit the pending partial byte" as a single O(1)
     * operation (already overflow-safe, and with no loop to spin in the
     * first place), so trailing_bits is just: stop bit, then flush. */
    bs_write1(bs, 1);
    bs_flush(bs);
}

size_t bs_bytes_written(const bitstream_t *bs) {
    return bs->byte_offset + (bs->bit_offset > 0 ? 1 : 0);
}

size_t bs_bytes_remaining(const bitstream_t *bs) {
    if (bs->size <= bs_bytes_written(bs)) return 0;
    return bs->size - bs_bytes_written(bs);
}

void bs_flush(bitstream_t *bs) {
    if (!bs || bs->overflow) return;
    if (bs->bit_offset == 0) return; /* already byte-aligned, nothing pending */
    if (bs->byte_offset >= bs->size) {
        bs->overflow = true;
        return;
    }
    /* Left-justify the pending bits to the top of the byte and zero-fill
     * the rest - the same content the old per-bit zero-padding loop
     * produced, written in one store instead of up to 7. */
    bs->buffer[bs->byte_offset] = (uint8_t)(bs->accum << (8 - bs->bit_offset));
    bs->byte_offset++;
    bs->accum = 0;
    bs->bit_offset = 0;
}

size_t bs_write_nal_header(bitstream_t *bs, int nal_ref_idc, int nal_type) {
    bs_write_u(bs, 32, 0x00000001);
    uint32_t header = (0 << 7) | ((nal_ref_idc & 3) << 5) | (nal_type & 0x1f);
    bs_write_u(bs, 8, header);
    return bs_bytes_written(bs);
}

size_t bs_write_nal_header_hevc(bitstream_t *bs, int nal_unit_type) {
    bs_write_u(bs, 32, 0x00000001);
    bs_write1(bs, 0);                       /* forbidden_zero_bit */
    bs_write_u(bs, 6, (uint32_t)nal_unit_type & 0x3f);
    bs_write_u(bs, 6, 0);                   /* nuh_layer_id */
    bs_write_u(bs, 3, 1);                   /* nuh_temporal_id_plus1 */
    return bs_bytes_written(bs);
}

size_t bs_rbsp_to_ebsp(uint8_t *dst, size_t dst_size, const uint8_t *src, size_t src_size) {
    size_t i, j = 0;
    int zero_count = 0;
    for (i = 0; i < src_size; i++) {
        if (j >= dst_size) break;
        if (zero_count == 2 && src[i] <= 0x03) {
            dst[j++] = 0x03;
            zero_count = 0;
            if (j >= dst_size) break;
        }
        dst[j++] = src[i];
        if (src[i] == 0x00) {
            zero_count++;
        } else {
            zero_count = 0;
        }
    }
    return j;
}

size_t bs_write_sps(uint8_t *buf, size_t buf_size, const h264_sps_t *sps) {
    if (!buf || !sps) return 0;
    uint8_t rbsp[1024];
    bitstream_t bs;
    bs_init(&bs, rbsp, sizeof(rbsp));
    
    bs_write_u(&bs, 8, sps->profile_idc);
    bs_write1(&bs, 0); // constraint_set0_flag
    bs_write1(&bs, 0); // constraint_set1_flag
    bs_write1(&bs, 0); // constraint_set2_flag
    bs_write1(&bs, 0); // constraint_set3_flag
    bs_write1(&bs, 0); // constraint_set4_flag
    bs_write1(&bs, 0); // constraint_set5_flag
    bs_write_u(&bs, 2, 0); // reserved
    bs_write_u(&bs, 8, sps->level_idc);
    bs_write_ue(&bs, sps->sps_id);
    
    if (sps->profile_idc == PROFILE_HIGH) {
        bs_write_ue(&bs, sps->chroma_format_idc);
        bs_write_ue(&bs, sps->bit_depth_luma - 8);
        bs_write_ue(&bs, sps->bit_depth_chroma - 8);
        bs_write1(&bs, 0); // qpprime_y_zero_transform_bypass_flag
        bs_write1(&bs, 0); // seq_scaling_matrix_present_flag
    }
    
    bs_write_ue(&bs, sps->log2_max_frame_num);
    bs_write_ue(&bs, sps->pic_order_cnt_type);
    if (sps->pic_order_cnt_type == 0) {
        bs_write_ue(&bs, sps->log2_max_poc_lsb);
    }
    bs_write_ue(&bs, sps->max_num_ref_frames);
    bs_write1(&bs, 0); // gaps_in_frame_num_value_allowed_flag
    bs_write_ue(&bs, sps->pic_width_in_mbs - 1);
    bs_write_ue(&bs, sps->pic_height_in_mbs - 1);
    bs_write1(&bs, sps->frame_mbs_only ? 1 : 0);
    if (!sps->frame_mbs_only) {
        bs_write1(&bs, 0); // mb_adaptive_frame_field_flag
    }
    bs_write1(&bs, sps->direct_8x8_inference ? 1 : 0);
    bs_write1(&bs, sps->frame_cropping ? 1 : 0);
    if (sps->frame_cropping) {
        bs_write_ue(&bs, sps->crop_left);
        bs_write_ue(&bs, sps->crop_right);
        bs_write_ue(&bs, sps->crop_top);
        bs_write_ue(&bs, sps->crop_bottom);
    }
    bs_write1(&bs, sps->vui_present ? 1 : 0);
    if (sps->vui_present) {
        bs_write1(&bs, 1); // aspect_ratio_info_present_flag
        bs_write_u(&bs, 8, 255); // Extended_SAR
        bs_write_u(&bs, 16, sps->sar_width);
        bs_write_u(&bs, 16, sps->sar_height);
        bs_write1(&bs, 0); // overscan_info_present_flag
        bs_write1(&bs, 0); // video_signal_type_present_flag
        bs_write1(&bs, 0); // chroma_loc_info_present_flag
        bs_write1(&bs, sps->timing_info_present ? 1 : 0);
        if (sps->timing_info_present) {
            bs_write_u(&bs, 32, sps->num_units_in_tick);
            bs_write_u(&bs, 32, sps->time_scale);
            bs_write1(&bs, 0); // fixed_frame_rate_flag
        }
        bs_write1(&bs, 0); // nal_hrd_parameters_present_flag
        bs_write1(&bs, 0); // vcl_hrd_parameters_present_flag
        bs_write1(&bs, 0); // pic_struct_present_flag
        bs_write1(&bs, 0); // bitstream_restriction_flag
    }
    
    bs_rbsp_trailing_bits(&bs);
    
    bitstream_t out_bs;
    bs_init(&out_bs, buf, buf_size);
    bs_write_nal_header(&out_bs, NAL_REF_IDC_HIGH, NAL_TYPE_SPS);
    size_t payload_offset = bs_bytes_written(&out_bs);
    if (payload_offset >= buf_size) return 0;
    size_t ebsp_size = bs_rbsp_to_ebsp(buf + payload_offset, buf_size - payload_offset, rbsp, bs_bytes_written(&bs));
    
    return payload_offset + ebsp_size;
}

size_t bs_write_pps(uint8_t *buf, size_t buf_size, const h264_pps_t *pps) {
    if (!buf || !pps) return 0;
    uint8_t rbsp[1024];
    bitstream_t bs;
    bs_init(&bs, rbsp, sizeof(rbsp));
    
    bs_write_ue(&bs, pps->pps_id);
    bs_write_ue(&bs, pps->sps_id);
    bs_write1(&bs, pps->entropy_coding_mode ? 1 : 0);
    bs_write1(&bs, pps->pic_order_present ? 1 : 0);
    bs_write_ue(&bs, 0); // num_slice_groups_minus1
    bs_write_ue(&bs, pps->num_ref_idx_l0_default);
    bs_write_ue(&bs, pps->num_ref_idx_l1_default);
    bs_write1(&bs, pps->weighted_pred ? 1 : 0);
    bs_write_u(&bs, 2, pps->weighted_bipred_idc);
    bs_write_se(&bs, pps->pic_init_qp);
    bs_write_se(&bs, 0); // pic_init_qs
    bs_write_se(&bs, pps->chroma_qp_offset);
    bs_write1(&bs, pps->deblocking_filter_control ? 1 : 0);
    bs_write1(&bs, pps->constrained_intra_pred ? 1 : 0);
    bs_write1(&bs, 0); // redundant_pic_cnt_present_flag
    if (pps->transform_8x8_mode) {
        bs_write1(&bs, 1);
        bs_write1(&bs, 0); // pic_scaling_matrix_present_flag
        bs_write_se(&bs, pps->second_chroma_qp_offset);
    }
    
    bs_rbsp_trailing_bits(&bs);
    
    bitstream_t out_bs;
    bs_init(&out_bs, buf, buf_size);
    bs_write_nal_header(&out_bs, NAL_REF_IDC_HIGH, NAL_TYPE_PPS);
    size_t payload_offset = bs_bytes_written(&out_bs);
    if (payload_offset >= buf_size) return 0;
    size_t ebsp_size = bs_rbsp_to_ebsp(buf + payload_offset, buf_size - payload_offset, rbsp, bs_bytes_written(&bs));
    
    return payload_offset + ebsp_size;
}

void h264_sps_default(h264_sps_t *sps, uint32_t width, uint32_t height, uint32_t fps, uint8_t profile) {
    memset(sps, 0, sizeof(h264_sps_t));
    sps->profile_idc = profile;
    sps->level_idc = 40;
    sps->sps_id = 0;
    sps->chroma_format_idc = 1;
    sps->bit_depth_luma = 8;
    sps->bit_depth_chroma = 8;
    sps->log2_max_frame_num = 4;
    sps->pic_order_cnt_type = 0;
    sps->log2_max_poc_lsb = 4;
    sps->max_num_ref_frames = 1;
    sps->pic_width_in_mbs = (width + 15) / 16;
    sps->pic_height_in_mbs = (height + 15) / 16;
    sps->frame_mbs_only = true;
    sps->direct_8x8_inference = true;
    sps->frame_cropping = (width % 16 != 0 || height % 16 != 0);
    sps->crop_left = 0;
    sps->crop_right = (sps->pic_width_in_mbs * 16 - width) / 2;
    sps->crop_top = 0;
    sps->crop_bottom = (sps->pic_height_in_mbs * 16 - height) / 2;
    sps->vui_present = true;
    sps->sar_width = 1;
    sps->sar_height = 1;
    sps->timing_info_present = true;
    sps->num_units_in_tick = 1;
    sps->time_scale = fps * 2;
}

void h264_pps_default(h264_pps_t *pps, uint8_t sps_id, bool cabac, int32_t qp) {
    memset(pps, 0, sizeof(h264_pps_t));
    pps->pps_id = 0;
    pps->sps_id = sps_id;
    pps->entropy_coding_mode = cabac;
    pps->pic_order_present = false;
    pps->num_ref_idx_l0_default = 0;
    pps->num_ref_idx_l1_default = 0;
    pps->weighted_pred = false;
    pps->weighted_bipred_idc = 0;
    pps->pic_init_qp = qp - 26;
    pps->chroma_qp_offset = 0;
    pps->second_chroma_qp_offset = 0;
    pps->deblocking_filter_control = true;
    pps->constrained_intra_pred = false;
    pps->transform_8x8_mode = false;
}
