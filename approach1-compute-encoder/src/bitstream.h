/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * bitstream.h - Bit-level stream writer for H.264/H.265 NAL output
 *
 * This module provides the fundamental building block for generating
 * compliant H.264 bitstreams. Every parameter set (SPS, PPS) and
 * slice header is serialized through this interface.
 */

#ifndef BC250_BITSTREAM_H
#define BC250_BITSTREAM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum NAL unit size (4 MB should be plenty for any single NAL) */
#define BS_MAX_NAL_SIZE  (4 * 1024 * 1024)

/* NAL unit types (H.264 / AVC) */
#define NAL_TYPE_SLICE        1
#define NAL_TYPE_DPA          2
#define NAL_TYPE_DPB          3
#define NAL_TYPE_DPC          4
#define NAL_TYPE_IDR_SLICE    5
#define NAL_TYPE_SEI          6
#define NAL_TYPE_SPS          7
#define NAL_TYPE_PPS          8
#define NAL_TYPE_AUD          9
#define NAL_TYPE_FILLER       12

/* NAL reference IDC values */
#define NAL_REF_IDC_NONE      0
#define NAL_REF_IDC_LOW       1
#define NAL_REF_IDC_MEDIUM    2
#define NAL_REF_IDC_HIGH      3

/* H.264 Profile IDCs */
#define PROFILE_BASELINE  66
#define PROFILE_MAIN      77
#define PROFILE_HIGH      100

/* H.264 Slice types */
#define SLICE_TYPE_P      0
#define SLICE_TYPE_B      1
#define SLICE_TYPE_I      2
#define SLICE_TYPE_SP     3
#define SLICE_TYPE_SI     4

/**
 * Bitstream writer context.
 *
 * Writes bits MSB-first into a byte buffer. Tracks current byte/bit
 * position for sequential writes. Callers should ensure the buffer
 * is large enough before writing (or use bs_bytes_remaining()).
 *
 * PERF NOTE (perf/openh264-bitwriter-fallback): bit-level writes are
 * accumulated in `accum` (an in-register bit buffer, right-justified,
 * holding exactly `bit_offset` valid pending bits, 0-7) and only stored to
 * `buffer` once a full byte is available - a single plain store, never a
 * read-modify-write. This is the same accumulate-then-batch-store technique
 * Cisco's openh264 uses in its encoder bitstream writer
 * (codec/common/inc/golomb_common.h, BsWriteBits()/SBitStringAux), adapted
 * here at byte granularity (openh264 batches to a 32-bit word) - see
 * bitstream.c's top-of-file comment for the full writeup, including why
 * byte granularity was chosen for this codebase specifically. `byte_offset`
 * and `bit_offset` keep their original external meaning/range (0-7); only
 * the internal write path changed.
 */
typedef struct bitstream {
    uint8_t *buffer;       /* Output byte buffer */
    size_t   size;         /* Total buffer capacity in bytes */
    size_t   byte_offset;  /* Bytes already physically committed to `buffer` */
    int      bit_offset;   /* Valid pending bits held in `accum`, not yet flushed to `buffer` (0-7, 0=MSB-aligned/empty) */
    bool     overflow;     /* Set if any write exceeded buffer capacity */
    uint32_t accum;        /* In-register pending-bit accumulator, right-justified low `bit_offset` bits */
} bitstream_t;

/**
 * H.264 Sequence Parameter Set (SPS) - key encoding parameters
 */
typedef struct h264_sps {
    uint8_t  profile_idc;          /* 66=Baseline, 77=Main, 100=High */
    uint8_t  level_idc;            /* e.g., 40 = Level 4.0 */
    uint8_t  sps_id;               /* SPS identifier (0-31) */
    uint8_t  chroma_format_idc;    /* 1 = 4:2:0 (standard) */
    uint8_t  bit_depth_luma;       /* 8 or 10 */
    uint8_t  bit_depth_chroma;     /* 8 or 10 */
    uint8_t  log2_max_frame_num;   /* log2(max_frame_num) - 4 */
    uint8_t  pic_order_cnt_type;   /* 0, 1, or 2 */
    uint8_t  log2_max_poc_lsb;     /* log2(max_pic_order_cnt_lsb) - 4 */
    uint32_t max_num_ref_frames;   /* Max reference frames in DPB */
    uint32_t pic_width_in_mbs;     /* Picture width in macroblocks */
    uint32_t pic_height_in_mbs;    /* Picture height in macroblocks */
    bool     frame_mbs_only;       /* true = progressive only */
    bool     direct_8x8_inference; /* Required for Level >= 3.0 */
    bool     frame_cropping;       /* Whether to signal crop offsets */
    uint32_t crop_left;
    uint32_t crop_right;
    uint32_t crop_top;
    uint32_t crop_bottom;
    /* VUI parameters */
    bool     vui_present;
    uint32_t sar_width;            /* Sample aspect ratio */
    uint32_t sar_height;
    bool     timing_info_present;
    uint32_t num_units_in_tick;    /* For framerate signaling */
    uint32_t time_scale;
} h264_sps_t;

/**
 * H.264 Picture Parameter Set (PPS) - per-picture encoding parameters
 */
typedef struct h264_pps {
    uint8_t  pps_id;                   /* PPS identifier (0-255) */
    uint8_t  sps_id;                   /* Referenced SPS */
    bool     entropy_coding_mode;      /* 0=CAVLC, 1=CABAC */
    bool     pic_order_present;        /* Bottom field POC */
    uint8_t  num_ref_idx_l0_default;   /* Default L0 ref count - 1 */
    uint8_t  num_ref_idx_l1_default;   /* Default L1 ref count - 1 */
    bool     weighted_pred;            /* Weighted prediction for P */
    uint8_t  weighted_bipred_idc;      /* Weighted prediction for B */
    int8_t   pic_init_qp;             /* Initial QP - 26 */
    int8_t   chroma_qp_offset;        /* Cb QP offset */
    int8_t   second_chroma_qp_offset; /* Cr QP offset */
    bool     deblocking_filter_control; /* Deblocking filter control */
    bool     constrained_intra_pred;   /* Constrained intra prediction */
    bool     transform_8x8_mode;       /* 8x8 transform (High profile) */
} h264_pps_t;

/* ===== Core bitstream operations ===== */

/** Initialize a bitstream writer over the given buffer. */
void bs_init(bitstream_t *bs, uint8_t *buf, size_t size);

/**
 * Write `bits` bits of `val` into the stream (1-32 bits, MSB-first).
 *
 * PERF NOTE (perf/openh264-bitwriter-fallback): defined `static inline`
 * here, in the header, rather than out-of-line in bitstream.c. Measured on
 * real board hardware: making the accumulator rewrite in bitstream.c
 * (see that file's top-of-file comment) an out-of-line function produced
 * ~0% measured speedup end-to-end, despite a 20,000+-session differential
 * fuzz test confirming it does strictly less work per call. Root cause,
 * confirmed by this inlining change actually moving the number (see the
 * branch's commit log / final report for before/after figures): this
 * project's CMakeLists.txt does not build with -flto (measured previously
 * and found not to help - see that file's comment), so cavlc.c's
 * extremely hot per-bit call sites (cavlc_write_one_level()'s unary
 * zero-run loops, trailing-one sign bits - millions of calls per second
 * at real-time frame rates) were paying a full cross-translation-unit
 * call/return (parameter marshaling, prologue/epilogue) on every single
 * bit, which dominated over whatever arithmetic happened inside the
 * function body - so a faster function body alone was invisible until the
 * call boundary itself was removed by letting the compiler inline this
 * function directly into cavlc.c's loops. */
static inline void bs_write_u(bitstream_t *bs, int bits, uint32_t val) {
    if (bs->overflow || bits <= 0) return;
    if (bits < 32) {
        val &= (1u << bits) - 1;
    }

    uint64_t combined = ((uint64_t)bs->accum << bits) | val;
    int total_bits = bs->bit_offset + bits;
    int nbytes = total_bits >> 3;
    int rem = total_bits & 7;

    for (int i = 0; i < nbytes; i++) {
        if (bs->byte_offset >= bs->size) {
            bs->overflow = true;
            return;
        }
        int shift = (nbytes - 1 - i) * 8 + rem;
        bs->buffer[bs->byte_offset] = (uint8_t)(combined >> shift);
        bs->byte_offset++;
    }
    bs->accum = (rem == 0) ? 0u : (uint32_t)(combined & ((1u << rem) - 1));
    bs->bit_offset = rem;
}

/** Write a single bit. */
static inline void bs_write1(bitstream_t *bs, uint32_t val) {
    bs_write_u(bs, 1, val);
}

/** Write unsigned Exp-Golomb coded value (ue(v) in the spec). */
void bs_write_ue(bitstream_t *bs, uint32_t val);

/** Write signed Exp-Golomb coded value (se(v) in the spec). */
void bs_write_se(bitstream_t *bs, int32_t val);

/** Write RBSP trailing bits (stop bit + alignment zeros). */
void bs_rbsp_trailing_bits(bitstream_t *bs);

/** Get total bytes written so far (rounded up if mid-byte). */
size_t bs_bytes_written(const bitstream_t *bs);

/** Get remaining capacity in bytes. */
size_t bs_bytes_remaining(const bitstream_t *bs);

/** Flush any partial byte (zero-pad remaining bits in current byte). */
void bs_flush(bitstream_t *bs);

/* ===== NAL unit framing ===== */

/**
 * Write a NAL start code (0x00 0x00 0x00 0x01) and NAL header.
 * Returns the byte offset where the NAL payload begins.
 */
size_t bs_write_nal_header(bitstream_t *bs, int nal_ref_idc, int nal_type);

/**
 * Perform RBSP-to-EBSP emulation prevention (stuffs 0x03 bytes).
 * Takes raw RBSP data, outputs EBSP. Returns output size.
 */
size_t bs_rbsp_to_ebsp(uint8_t *dst, size_t dst_size,
                       const uint8_t *src, size_t src_size);

/**
 * A filler_data_rbsp() NAL (see bs_write_filler()'s doc comment) with zero
 * 0xFF payload bytes is still 4 (start code) + 1 (NAL header) + 1
 * (rbsp_trailing_bits' single stop-bit byte) = 6 bytes. A caller wanting to
 * close a real, positive shortfall smaller than this can't do so with a
 * filler NAL without overshooting the target - see encoder_h264.c's
 * maybe_append_filler().
 */
#define BS_FILLER_MIN_NAL_SIZE 6

/**
 * bs_write_filler - write one filler_data_rbsp() NAL unit (ITU-T H.264
 * SS7.3.2.7 / SS7.4.2.7, nal_unit_type 12) directly into `buf`.
 *
 * Spec syntax is: repeated ff_byte (each == 0xFF) for as many bytes as the
 * caller wants, then rbsp_trailing_bits() (a single '1' stop bit, then
 * zero-padding to the next byte boundary - since the ff_byte run already
 * ends byte-aligned, this is exactly one more byte, 0x80). A real decoder
 * is required (7.4.2.7) to parse and discard this NAL without it affecting
 * any decoded picture - it exists purely to let an encoder manufacture
 * bytes it has no coded content for, to hit a genuine constant-bitrate
 * target. x264 (GPL-2.0-or-later, compatible with this project's
 * GPL-3.0-only license per "or any later version") does exactly this in
 * encoder/set.c's x264_filler_write(): a loop of bs_write(s, 8, 0xff)
 * followed by bs_rbsp_trailing(s) - confirming this is the real mechanism
 * production encoders use, not an invented alternative. This function was
 * written independently against the spec text and that confirmation, not
 * by copying x264's code (its bitstream writer has a different internal
 * contract than this project's bitstream_t).
 *
 * Unlike bs_write_sps()/bs_write_pps()/a coded slice, this NAL's RBSP is
 * never passed through bs_rbsp_to_ebsp(): every payload byte is either
 * 0xFF or (the final trailing-bits byte) 0x80, so the "two zero bytes
 * followed by 0x00-0x03" pattern bs_rbsp_to_ebsp() escapes can never occur
 * here - RBSP and EBSP are byte-identical for this specific payload shape,
 * by construction, not by omission.
 *
 * @param buf              Destination (start code onward).
 * @param buf_size         Bytes available at `buf`.
 * @param filler_ff_count  Number of 0xFF payload bytes to emit (0 is legal:
 *                         a bare 6-byte filler NAL). Total bytes written is
 *                         exactly BS_FILLER_MIN_NAL_SIZE + filler_ff_count,
 *                         or less if `buf_size` is too small (silently
 *                         truncated the same way every other bs_write_*
 *                         NAL helper in this file behaves on overflow -
 *                         callers that can't tolerate truncation must
 *                         check buf_size themselves first).
 * @return Total bytes written (may be 0 if buf/buf_size can't even hold
 *         the NAL header).
 */
size_t bs_write_filler(uint8_t *buf, size_t buf_size, size_t filler_ff_count);

/* ===== H.264 parameter set serialization ===== */

/** Write a complete SPS NAL unit. Returns bytes written. */
size_t bs_write_sps(uint8_t *buf, size_t buf_size, const h264_sps_t *sps);

/** Write a complete PPS NAL unit. Returns bytes written. */
size_t bs_write_pps(uint8_t *buf, size_t buf_size, const h264_pps_t *pps);

/* ===== Utility ===== */

/** Fill an SPS struct with sensible defaults for given resolution/framerate. */
void h264_sps_default(h264_sps_t *sps, uint32_t width, uint32_t height,
                      uint32_t fps, uint8_t profile);

/** Fill a PPS struct with sensible defaults. */
void h264_pps_default(h264_pps_t *pps, uint8_t sps_id, bool cabac, int32_t qp);

#ifdef __cplusplus
}
#endif

#endif /* BC250_BITSTREAM_H */
