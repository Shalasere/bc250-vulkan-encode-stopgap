/* bc250-vulkan-encode-stopgap v0.4.0 - https://github.com/Shalasere/bc250-vulkan-encode-stopgap */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * encoder_h265.h - H.265/HEVC Compute Shader Encoder API
 */

#ifndef BC250_ENCODER_H265_H
#define BC250_ENCODER_H265_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gpu_compute.h"
#include "rate_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hevc_encoder hevc_encoder_t;

hevc_encoder_t *hevc_encoder_create(bc250_gpu_context_t *gpu_ctx,
                                    uint32_t width, uint32_t height,
                                    uint32_t fps, uint32_t bitrate);

void hevc_encoder_set_force_idr(hevc_encoder_t *encoder);
void hevc_encoder_set_gop_size(hevc_encoder_t *encoder, uint32_t gop_size);
uint32_t hevc_encoder_get_gop_size(const hevc_encoder_t *encoder);
void hevc_encoder_set_qp(hevc_encoder_t *encoder, int qp);
int hevc_encoder_get_qp(const hevc_encoder_t *encoder);
void hevc_encoder_set_bitrate(hevc_encoder_t *encoder, uint32_t bitrate);
uint32_t hevc_encoder_get_bitrate(const hevc_encoder_t *encoder);
void hevc_encoder_set_fps(hevc_encoder_t *encoder, uint32_t fps);
uint32_t hevc_encoder_get_fps(const hevc_encoder_t *encoder);
void hevc_encoder_set_rc_mode(hevc_encoder_t *encoder, rc_mode_t mode);
rc_mode_t hevc_encoder_get_rc_mode(const hevc_encoder_t *encoder);
uint32_t hevc_encoder_get_last_frame_sad(const hevc_encoder_t *encoder);
void hevc_encoder_set_quality_level(hevc_encoder_t *encoder, uint32_t quality_level);
uint32_t hevc_encoder_get_quality_level(const hevc_encoder_t *encoder);
void hevc_encoder_set_max_frame_size(hevc_encoder_t *encoder, uint32_t max_frame_bits);
uint32_t hevc_encoder_get_max_frame_size(const hevc_encoder_t *encoder);

/*
 * @input_memory: backing device memory of `input_surface`, needed for the
 * real gpu_compute_download_nv12() readback this encoder does (see
 * encoder_h265.c's top comment) - unlike the H.264 encoder above, this one
 * needs real host-visible pixel data because HEVC's mandatory 4x4-luma-
 * intra DST-VII transform and per-block reconstruction chaining aren't
 * something the existing GPU shaders compute (see hevc_intra.h).
 */
int hevc_encoder_encode_frame(hevc_encoder_t *encoder,
                              bc250_gpu_context_t *gpu_ctx,
                              gpu_image_t input_surface,
                              gpu_memory_t input_memory,
                              uint8_t *output_buf, size_t output_size);

/**
 * hevc_encoder_encode_raw - Encode a frame straight from host NV12 pixel
 * data, with no GPU context/surface involved at all. Exists for the same
 * reason h264_encoder_encode_raw() does (see encoder_h264.h): a
 * deterministic, GPU-free path for tests and offline validation. This
 * encoder's whole per-4x4-block intra/transform/CABAC pipeline (see
 * encoder_h265.c) already runs on the CPU against host pixel data - the
 * GPU is only ever used, via hevc_encoder_encode_frame() above, to get
 * that pixel data off an already-uploaded VA-API surface - so this
 * entry point is the same core encode logic with that one GPU readback
 * step skipped.
 */
int hevc_encoder_encode_raw(hevc_encoder_t *encoder,
                            const uint8_t *y_plane, int y_pitch,
                            const uint8_t *uv_plane, int uv_pitch,
                            uint8_t *output_buf, size_t output_size);

/**
 * hevc_encoder_encode_gpu_raw - Off-board exercise of the GPU (BC250_HEVC_
 * GPU=1) path's P-frame zero-motion-skip logic (docs/notes/
 * c7-gpu-pframes.md), with no GPU/Vulkan device involved at all - the
 * GPU-path analogue of hevc_encoder_encode_raw() above, for the same
 * reason: a deterministic, GPU-free way to exercise code that would
 * otherwise need a board. See encoder_h265.c's doc comment on this
 * function for exactly what it does and does not verify - short version:
 * real bitstream-syntax and skip-region-reconstruction correctness, NOT
 * anything about the real intra shader.
 *
 * `y_plane`/`uv_plane` (with pitches) are this frame's source, real
 * width x height, same convention as hevc_encoder_encode_raw()'s.
 * `ref_y_plane`/`ref_uv_plane` (with pitches), if non-NULL, become this
 * frame's reference (prev_recon_y/cb/cr) - CODED width x height, since
 * there is no real GPU recon_image to read back from here. Pass NULL for
 * an IDR-only test. `synth_modes` (one int32 per CTU, 0..34), `synth_
 * coeffs` (384 int32 per CTU: 256 luma + 64 Cb + 64 Cr) and `synth_cbf`
 * (one uint32 per CTU, bit0/1/2 = cbf_luma/cb/cr, bits 8+ = chroma pred
 * mode index) stand in for what hevc_intra_wavefront.comp would have
 * produced for this frame's non-skip CTUs; any may be NULL for an all-DC/
 * all-zero-residual default (a flat, decodable, but NOT quality-
 * representative CTU).
 */
int hevc_encoder_encode_gpu_raw(hevc_encoder_t *encoder,
                                const uint8_t *y_plane, int y_pitch,
                                const uint8_t *uv_plane, int uv_pitch,
                                const uint8_t *ref_y_plane, int ref_y_pitch,
                                const uint8_t *ref_uv_plane, int ref_uv_pitch,
                                const int32_t *synth_modes,
                                const int32_t *synth_coeffs,
                                const uint32_t *synth_cbf,
                                uint8_t *output_buf, size_t output_size);

void hevc_encoder_destroy(hevc_encoder_t *encoder);

#ifdef __cplusplus
}
#endif

#endif /* BC250_ENCODER_H265_H */
