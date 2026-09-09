/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project Contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * encoder_h264.h - H.264/AVC Compute Shader Encoder API
 */

#ifndef BC250_ENCODER_H264_H
#define BC250_ENCODER_H264_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "gpu_compute.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h264_encoder h264_encoder_t;

/**
 * h264_encoder_create - Allocate and configure an H.264 encoder
 * @gpu_ctx: Vulkan compute context
 * @width: Frame width in pixels
 * @height: Frame height in pixels
 * @fps: Framerate (e.g. 30 or 60)
 * @bitrate: Target bitrate in bits per second
 * @profile: VAProfile (Baseline, Main, High)
 */
h264_encoder_t *h264_encoder_create(bc250_gpu_context_t *gpu_ctx,
                                    uint32_t width, uint32_t height,
                                    uint32_t fps, uint32_t bitrate,
                                    int profile);

/**
 * h264_encoder_encode_frame - Encodes a frame to H.264 Annex B byte stream
 * @encoder: Encoder context
 * @gpu_ctx: Vulkan compute context
 * @input_surface: Input GPU surface
 * @output_buf: Destination buffer for NALUs
 * @output_size: Size of output buffer
 *
 * Returns number of bytes written, or -1 on error.
 */
int h264_encoder_encode_frame(h264_encoder_t *encoder,
                              bc250_gpu_context_t *gpu_ctx,
                              gpu_image_t input_surface,
                              uint8_t *output_buf, size_t output_size);

/**
 * h264_encoder_force_idr - Request next frame to be an instantaneous decoder refresh (IDR)
 */
void h264_encoder_force_idr(h264_encoder_t *encoder);

/**
 * h264_encoder_set_bitrate - Dynamically adjust target bitrate
 */
void h264_encoder_set_bitrate(h264_encoder_t *encoder, uint32_t bitrate_bps);

/**
 * h264_encoder_set_gop_size - Configure keyframe (IDR) interval
 */
void h264_encoder_set_gop_size(h264_encoder_t *encoder, uint32_t gop_size);

/**
 * h264_encoder_set_fps - Dynamically update framerate
 */
void h264_encoder_set_fps(h264_encoder_t *encoder, uint32_t fps);

/**
 * h264_encoder_set_qp - Set constant/base quantization parameter (0..51)
 */
void h264_encoder_set_qp(h264_encoder_t *encoder, int qp);

/**
 * h264_encoder_encode_raw - Encodes a raw NV12 image frame with pattern/content analysis
 * @encoder: Encoder context
 * @y_plane: Host pointer to Y plane data
 * @y_pitch: Row pitch of Y plane in bytes
 * @uv_plane: Host pointer to interleaved UV plane data
 * @uv_pitch: Row pitch of UV plane in bytes
 * @output_buf: Destination buffer for NALUs
 * @output_size: Size of output buffer
 *
 * Returns number of bytes written, or -1 on error.
 */
int h264_encoder_encode_raw(h264_encoder_t *encoder,
                            const uint8_t *y_plane, int y_pitch,
                            const uint8_t *uv_plane, int uv_pitch,
                            uint8_t *output_buf, size_t output_size);

/**
 * h264_encoder_destroy - Teardown and free resources
 */
void h264_encoder_destroy(h264_encoder_t *encoder);

/**
 * h264_intra16_luma_dc_transform - forward Hadamard transform, quantize, and
 * transpose the 16 luma DC coefficients of an Intra16x16 macroblock into the
 * row/column layout cavlc_write_4x4_block() (and a real decoder) expect.
 *
 * This is the exact pure-math DC path used by the encoder's Intra16x16
 * macroblock encoding (see encoder_h264.c's encode_mb_i16x16, its only
 * production caller). It is exposed here (rather than kept static) purely
 * so it can be unit-tested in isolation without a GPU/Vulkan context - see
 * tests/test_encode.c's test_intra16_dc_transpose() regression test for the
 * transpose bug fixed in commit d95b840.
 *
 * @param dc_in                16 pre-quant luma DC values (one per luma 4x4
 *                             sub-block of the macroblock), raster
 *                             (row*4+col) order.
 * @param qp                   Quantization parameter for this macroblock.
 * @param dc_out               Output: quantized DC array in the natural
 *                             row/column order CAVLC/a real decoder expect.
 * @param dc_out_pretranspose  Optional (may be NULL): if non-NULL, filled
 *                             with the quantized array BEFORE the transpose
 *                             fix is applied. For regression testing only -
 *                             no production caller needs this.
 */
void h264_intra16_luma_dc_transform(const int dc_in[4][4], int qp,
                                     int dc_out[16], int dc_out_pretranspose[16]);

#ifdef __cplusplus
}
#endif

#endif /* BC250_ENCODER_H264_H */
