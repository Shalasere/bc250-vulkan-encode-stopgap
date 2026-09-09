/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * cabac.h - Spec-compliant H.264 CABAC (Context-Adaptive Binary Arithmetic
 *           Coding) entropy encoder per ITU-T H.264 / ISO/IEC 14496-10
 *           Section 9.3.
 *
 * ============================================================================
 * PROVENANCE (required disclosure - see this branch's commit messages for the
 * per-commit breakdown of what changed where)
 * ============================================================================
 * This is a genuine adaptation of the x264 project's CABAC implementation
 * (https://github.com/mirror/x264, common/cabac.c, common/cabac.h,
 * common/tables.c, encoder/cabac.c - verified GPL-2.0-or-later WITH an
 * additional commercial-license option per every x264 source file's own
 * header: "This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version. [...] This program is also available
 * under a commercial proprietary license." GPL-2.0-or-later is
 * combination-compatible with this project's GPL-3.0-only licensing (see
 * this project's top-level LICENSE and docs/relicense commit) - the
 * resulting combined work here is licensed GPL-3.0-only as a whole, per this
 * file's own SPDX line above, which is permitted by GPL-2.0-or-later's "any
 * later version" clause.
 *
 * x264 copyright notice, retained as required by the GPL and by this
 * project's own disclosure policy for adapted GPL code (see LICENSE and
 * this branch's commit messages):
 *
 *   cabac.c / cabac.h / tables.c / encoder/cabac.c:
 *   Copyright (C) 2003-2024 x264 project
 *   Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *            Loren Merritt <lorenm@u.washington.edu>
 *            Fiona Glaser <fiona@x264.com>
 *
 * WHAT IS DIRECTLY ADAPTED FROM x264 (numerically/structurally copied, not
 * independently re-derived) - see cabac.c for the exact line-level mapping:
 *   - The arithmetic coding engine itself: x264_cabac_encode_decision_c(),
 *     x264_cabac_encode_bypass_c(), x264_cabac_encode_terminal_c(),
 *     x264_cabac_encode_ue_bypass(), the cabac_putbyte()/cabac_encode_renorm()
 *     carry-propagation byte writer, and x264_cabac_encode_flush() - this is
 *     x264's specific optimized implementation choice (deferred-carry byte
 *     queue) of ITU-T 9.3.4.3's arithmetic coding engine, not just "the
 *     spec's algorithm" in the abstract; reimplemented here under new names
 *     (cabac_encode_decision/cabac_encode_bypass/etc) but numerically
 *     identical.
 *   - The context-adaptation tables: x264_cabac_range_lps[64][4] (spec Table
 *     9-46 rangeTabLPS), x264_cabac_transition[128][2] (spec Table 9-45
 *     transIdxLPS/transIdxMPS, state+valMPS packed into one byte exactly as
 *     x264 packs it), x264_cabac_renorm_shift[64] (spec Table 9-47
 *     renormalization shift). These are spec-mandated constants; x264's
 *     values were used directly (and cross-checked against the packed-state
 *     encoding convention used throughout, since that packing is x264's own
 *     representation choice, not the spec's).
 *   - The per-syntax-element context INDEX assignments (which ctxIdx offset
 *     applies to which syntax element/ctxIdxInc) and the ctxIdxInc DERIVATION
 *     formulas for: mb_type (I_16x16 binarization incl. embedded cbp),
 *     mb_skip_flag (P), coded_block_pattern (luma+chroma), mb_qp_delta,
 *     intra_chroma_pred_mode, mvd_l0 (both components), coded_block_flag
 *     (all 5 relevant ctxBlockCat categories used by this project:
 *     Intra16x16DCLevel, Intra16x16ACLevel, LumaLevel4x4, ChromaDCLevel,
 *     ChromaACLevel), significant_coeff_flag / last_significant_coeff_flag /
 *     coeff_abs_level_minus1 (including the node-context state machine
 *     coeff_abs_level1_ctx/coeff_abs_levelgt1_ctx/coeff_abs_level_transition
 *     and the per-category base-context-index tables
 *     x264_cabac_context_init_I / x264_cabac_context_init_PB[0], sliced down
 *     to only the ctxIdx range 0-275 this project's restricted macroblock
 *     type set (I_16x16 intra, P_L0_16x16 inter, single reference, no
 *     8x8 transform, no interlace, no B-slices) ever exercises). These
 *     values were cross-checked entry-by-entry against x264's real,
 *     currently-shipping source (common/tables.c) rather than reconstructed
 *     from memory of the spec text - see this branch's commit messages for
 *     the verification methodology.
 *
 * WHAT IS INDEPENDENTLY WRITTEN (this project's own code, not from x264):
 *   - This project's own bitstream integration: byte-aligning the existing
 *     bitstream_t slice header with cabac_alignment_one_bit before handing
 *     control to the CABAC engine, and splicing the engine's raw byte output
 *     back into the same slice_rbsp buffer / bs_rbsp_to_ebsp() pipeline this
 *     project's CAVLC path already uses (see encoder_h264.c).
 *   - This project's own neighbor/context-state tracking arrays (cbf_*,
 *     cbp_nb, etc. in encoder_h264.c's h264_encoder struct) - x264 uses its
 *     own internal macroblock cache (h->mb.cache.*) with a completely
 *     different data-flow (whole-frame mode decision object, not this
 *     project's streaming GPU-readback + per-MB CPU pass), so the STORAGE
 *     was written from scratch to fit this project's existing nz_luma/nz_cb/
 *     nz_cr-style per-frame persistent arrays; only the ctxIdxInc FORMULAS
 *     that read that storage are adapted from x264 (see above).
 *   - The overall per-macroblock/per-slice control flow in encoder_h264.c
 *     (encode_mb_i16x16_cabac/encode_mb_p16x16_cabac, the P-slice
 *     mb_skip_flag + end_of_slice_flag terminal-bit loop) - structured to
 *     match this project's existing encode_mb_i16x16/encode_mb_p16x16 CAVLC
 *     functions' call shape, not copied from x264's h->mb.* driven encoder
 *     loop (x264_macroblock_write_cabac / slices.c), which assumes x264's
 *     own macroblock/mode-decision object model that has no equivalent here.
 *   - BC250_USE_CABAC env-var selection and the Main/High-profile
 *     auto-selection wiring in h264_encoder_create()/va_backend.c.
 *
 * SCOPE / KNOWN LIMITATIONS (see this branch's final report for the
 * board-measured validation results): this implementation only covers the
 * macroblock types this project's encoder actually emits - I_16x16 intra
 * (all prediction modes) and P_L0_16x16 inter, both at 4:2:0, frame-only (no
 * MBAFF/interlace), single reference picture, no 8x8 transform, no B-slices,
 * no I_NxN/I_PCM, no sub-macroblock partitions. It is NOT a general-purpose
 * CABAC implementation for arbitrary H.264 content - see cabac.c's top-of-
 * file comment for the exact context-index subset (ctxIdx 0-275) this
 * implies and why that subset is sufficient for everything this project's
 * GPU pipeline can produce.
 */
#ifndef BC250_CABAC_H
#define BC250_CABAC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Total number of CABAC contexts this project ever indexes (ctxIdx 0..275).
 * Real H.264 CABAC has up to 1024 contexts (4:4:4 High profile); this
 * project never emits 4:4:4, 8x8-transform, B-slice, multi-ref, or
 * interlaced syntax, so ctxIdx 276+ (8x8 residual contexts, ref_idx,
 * sub_mb_type, B-direct, field flags, etc.) are never touched - see
 * cabac.c's context-index map comment for the full accounting. */
#define CABAC_NUM_CTX 276

typedef struct cabac_engine {
    /* Arithmetic coding engine state - directly mirrors x264's x264_cabac_t
     * (see this header's top-of-file provenance comment): i_low/i_range are
     * the codIRange/codILow of ITU-T 9.3.4.3, i_queue/i_bytes_outstanding
     * implement the same deferred-carry byte-output technique x264 uses
     * (see cabac.c's cabac_putbyte()). */
    int32_t i_low;
    int32_t i_range;
    int32_t i_queue;
    int32_t i_bytes_outstanding;

    uint8_t *p_start;
    uint8_t *p;
    uint8_t *p_end;
    bool overflow;

    /* Per-context adaptive state: bit 0 = valMPS, bits 1-7 = pStateIdx,
     * packed exactly as x264_cabac_init() packs it (see cabac.c). */
    uint8_t state[CABAC_NUM_CTX];
} cabac_engine_t;

/* Initialize the arithmetic coding engine to write into [p_data, p_end).
 * Must be called once per slice, AFTER the context state array has been
 * populated by cabac_context_init() and AFTER the slice header's
 * cabac_alignment_one_bit padding has been written to the byte immediately
 * before p_data (per ITU-T 7.3.4 / 9.3.1). */
void cabac_engine_init(cabac_engine_t *cb, uint8_t *p_data, uint8_t *p_end);

/* Populate cb->state[] for a new slice. is_intra_slice selects the
 * spec-mandated "_I" initialization table (slice_type == I/SI); otherwise
 * the "_PB" table for the given cabac_init_idc (0..2, as signaled in the
 * slice header) is used. qp is SliceQPY (26 + pic_init_qp + slice_qp_delta),
 * clamped to 0..51 by the caller. */
void cabac_context_init(cabac_engine_t *cb, bool is_intra_slice, int cabac_init_idc, int qp);

/* Encode one regular (context-modeled) bin using context ctx_idx, updating
 * that context's adaptive state. */
void cabac_encode_decision(cabac_engine_t *cb, int ctx_idx, int bit);

/* Encode one bypass-coded bin (equiprobable, no context/adaptation). */
void cabac_encode_bypass(cabac_engine_t *cb, int bit);

/* Encode a terminal bin with value 0 (ITU-T 9.3.4.5's "terminate" process,
 * continue case). Matches x264_cabac_encode_terminal_c() exactly: per spec,
 * the value-1 case ("this is the last macroblock, stop") does NOT renormalize
 * the way value-0 does - it is instead folded directly into
 * cabac_encode_flush()'s own math (see that function and
 * cabac_write_end_of_slice_flag() below), so there is deliberately no
 * bit-value parameter here - this function only ever encodes 0. */
void cabac_encode_terminal(cabac_engine_t *cb);

/* Encode a bypass-coded Exp-Golomb-like unsigned value with an exp_bits-wide
 * "kth order" prefix, used for the coeff_abs_level_minus1 and mvd escape
 * codes beyond the unary-coded short range (ITU-T 9.3.2.3). */
void cabac_encode_ue_bypass(cabac_engine_t *cb, int exp_bits, int val);

/* Flush the engine at the end of a slice (after the final end_of_slice_flag
 * terminal bit=1 call). frame_count seeds the single don't-care alignment
 * bit the same way x264 does (cosmetic - any value is spec-legal since it
 * falls after the RBSP stop bit's logical position; kept only for output
 * parity with a real encoder's byte pattern, not for correctness). Returns
 * the total number of bytes written from p_start. */
size_t cabac_encode_flush(cabac_engine_t *cb, uint32_t frame_count);

/* ===========================================================================
 * Syntax element encoders (ITU-T H.264 7.3.5 macroblock_layer() / 7.3.5.3
 * residual_block_cabac(), restricted to this project's I_16x16 / P_L0_16x16
 * macroblock types - see this file's top-of-file SCOPE note).
 * ===========================================================================
 */

/* mb_type + (for I_16x16 only) its embedded coded_block_pattern bits +
 * Intra16x16PredMode, per ITU-T Table 9-36 / x264's cabac_mb_type_intra().
 * ctx_neighbor_intra_count is the count (0..2) of {left,top} neighbor MBs
 * that are available AND are NOT I_NxN (ITU-T 9.3.3.1.1.3) - since this
 * project's I-slices are homogeneously I_16x16 (it never emits I_NxN/
 * I_PCM), this reduces in practice to simply the count of AVAILABLE
 * {left,top} neighbors (0, 1, or 2). cbp_luma_nonzero is whether ANY luma AC
 * coefficient is present; cbp_chroma is 0/1/2 per spec's
 * CodedBlockPatternChroma; i16x16_pred_mode is 0..3. */
void cabac_write_mb_type_i16x16(cabac_engine_t *cb, int ctx_neighbor_intra_count,
                                 bool cbp_luma_nonzero, int cbp_chroma,
                                 int i16x16_pred_mode);

/* intra_chroma_pred_mode (ITU-T Table 9-34, ctxIdxOffset 64). ctx_neighbor
 * is the count (0..2) of available {left,top} neighbors whose own
 * intra_chroma_pred_mode != 0 (DC). mode is 0..3. */
void cabac_write_intra_chroma_pred_mode(cabac_engine_t *cb, int ctx_neighbor, int mode);

/* mb_skip_flag for a P slice (ITU-T Table 9-34, ctxIdxOffset 11).
 * ctx_neighbor_not_skipped is the count (0..2) of available {left,top}
 * neighbors whose own mb_skip_flag == 0. */
void cabac_write_mb_skip_p(cabac_engine_t *cb, int ctx_neighbor_not_skipped, bool skip);

/* mb_type prefix + partition bits for a P_L0_16x16 macroblock, i.e. the
 * fixed 3-bin sequence "0,0,0" (not-intra, D_16x16, [unused 3rd bin only
 * distinguishes P_8x8, always 0 here since this project never emits P_8x8]
 * per x264's cabac_mb_header_p()) - this project only ever emits P_L0
 * D_16x16 partitions. */
void cabac_write_mb_type_p_l0_16x16(cabac_engine_t *cb);

/* mvd_l0[][][compIdx] for one component (ITU-T 9.3.2.3 / Table 9-34
 * ctxIdxOffset 40 horizontal / 47 vertical). ctx_amvd is the 0/1/2 context
 * derived from clip-summed abs(left mvd)+abs(top mvd) (see
 * cabac_mvd_ctx_from_neighbors() below). Returns the absolute value actually
 * coded (capped at 66, matching x264 - only used by the caller to update its
 * own neighbor-mvd tracking array, has no bitstream effect since the real
 * mvd value's exact magnitude beyond the unary-coded range is carried by the
 * Exp-Golomb escape, not by this return value). */
int cabac_write_mvd_component(cabac_engine_t *cb, int is_vertical, int ctx_amvd, int mvd);

/* Helper matching x264_cabac_mvd_sum(): given the (already-capped-at-66)
 * abs(mvd) of the left and top neighbor for one component, returns the 0/1/2
 * ctxIdxInc per ITU-T 9.3.3.1.1.7. */
static inline int cabac_mvd_ctx_from_neighbors(int left_abs, int top_abs) {
    int sum = left_abs + top_abs;
    return (sum > 2) + (sum > 32);
}

/* coded_block_pattern, luma nibble (ITU-T Table 9-34 ctxIdxOffset 73) and
 * chroma 0/1/2 value (ctxIdxOffset 77) - ONLY called for P_L0_16x16 (I_16x16
 * embeds its cbp in mb_type instead, see cabac_write_mb_type_i16x16()).
 * cbp_luma is the real 4-bit value (bit q = 1 iff 8x8 luma quadrant q has any
 * nonzero coefficient); cbp_l/cbp_t are the LEFT/TOP neighbor macroblocks'
 * OWN combined 6-bit cbp byte (cbp_chroma<<4 | cbp_luma), or -1 if that
 * neighbor is unavailable (off-picture or in an earlier slice) - matching
 * x264's exact sentinel convention, including the "unavailable defaults to
 * 1" behavior this produces for the luma bits via plain twos-complement
 * arithmetic-shift of -1 (see cabac_write_cbp_luma()'s body). */
void cabac_write_cbp_luma(cabac_engine_t *cb, int cbp_luma, int cbp_l, int cbp_t);
void cabac_write_cbp_chroma(cabac_engine_t *cb, int cbp_chroma, int cbp_l, int cbp_t);

/* mb_qp_delta (ITU-T Table 9-34 ctxIdxOffset 60). last_dqp_nonzero is
 * whether the immediately preceding macroblock in this slice (that actually
 * had mb_qp_delta present) coded a nonzero value - see cabac.c's
 * cabac_write_qp_delta() doc comment; this project always passes dqp=0 (no
 * per-MB QP variation, matching the existing CAVLC path's hardcoded
 * qp_delta=0), so in practice this always takes the single-bin "0" path with
 * ctx permanently 0, but the general (x264-faithful) unary encoding is
 * implemented for correctness/future use. Returns the new last_dqp_nonzero
 * value the caller should remember for the next macroblock. */
bool cabac_write_qp_delta(cabac_engine_t *cb, int dqp, bool last_dqp_nonzero);

/* end_of_slice_flag (ITU-T 7.3.4, coded after every macroblock regardless of
 * skip). For a non-final macroblock this codes the terminal-0 bin
 * (continue). For the final macroblock of the slice this does NOT call
 * cabac_encode_terminal() at all - per spec the value-1 termination bin is
 * folded directly into the flush math, so the caller must, immediately
 * after this returns true, call cabac_encode_flush() and nothing else for
 * this slice. Returns true iff this was the flush-pending (last-MB) case,
 * purely so the caller doesn't also have to duplicate the is_last_mb check. */
static inline bool cabac_write_end_of_slice_flag(cabac_engine_t *cb, bool is_last_mb) {
    if (!is_last_mb) {
        cabac_encode_terminal(cb);
        return false;
    }
    return true;
}

/* ctxBlockCat per ITU-T Table 9-42, restricted to the 5 categories this
 * project ever emits (no 8x8 luma, no Cb/Cr-separate 4:4:4 categories). */
typedef enum {
    CABAC_CAT_LUMA_DC   = 0,  /* Intra16x16DCLevel,  maxNumCoeff=16 */
    CABAC_CAT_LUMA_AC   = 1,  /* Intra16x16ACLevel,  maxNumCoeff=15 */
    CABAC_CAT_LUMA_4x4  = 2,  /* LumaLevel4x4,       maxNumCoeff=16 */
    CABAC_CAT_CHROMA_DC = 3,  /* ChromaDCLevel,      maxNumCoeff=4  */
    CABAC_CAT_CHROMA_AC = 4,  /* ChromaACLevel,      maxNumCoeff=15 */
} cabac_ctx_block_cat_t;

/* coded_block_flag for one block (ITU-T Table 9-34 ctxIdxOffset per
 * category: 85/89/93/97/101). nA/nB are the LEFT/TOP neighbor's own
 * coded_block_flag for the corresponding block (0 or 1), already resolved
 * for unavailability by the caller per ITU-T 9.3.3.1.1.9 (unavailable ->
 * 1 if the CURRENT macroblock is intra, else 0 - see encoder_h264.c's
 * luma_cbf_neighbors()/chroma_cbf_neighbors() helpers). */
void cabac_write_coded_block_flag(cabac_engine_t *cb, cabac_ctx_block_cat_t cat,
                                   int nA, int nB, bool cbf);

/* residual_block_cabac() for one block ALREADY KNOWN to have cbf=1 (caller
 * must have already called cabac_write_coded_block_flag() with cbf=true and
 * only call this when it returned/was true - a cbf=0 block has no residual
 * data at all, per spec). `scanned` is `count_cat_m1[cat]+1` coefficients in
 * zigzag (or, for CABAC_CAT_CHROMA_DC, raw row-major - no zigzag exists for
 * the 2x2 case) scan order, matching this project's existing CAVLC
 * convention (see cavlc.c's cavlc_write_4x4_block/_ac_block/
 * _chroma_dc_block for the identical scan/ordering contract). */
void cabac_write_residual_block(cabac_engine_t *cb, cabac_ctx_block_cat_t cat, const int *scanned);

/* Number of coefficients in a block of category `cat` (maxNumCoeff). */
int cabac_count_coeffs(cabac_ctx_block_cat_t cat);

#ifdef __cplusplus
}
#endif

#endif /* BC250_CABAC_H */
