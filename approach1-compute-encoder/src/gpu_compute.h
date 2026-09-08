/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: MIT
 *
 * gpu_compute.h - Vulkan compute orchestration for AMD BC-250
 */
#ifndef GPU_COMPUTE_H
#define GPU_COMPUTE_H

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage y_plane;
    VkImageView y_view;
    VkImage uv_plane;
    VkImageView uv_view;
    uint32_t width;
    uint32_t height;
    /* Tracks the real current VkImageLayout of both planes (they are always
     * transitioned together). Starts at VK_IMAGE_LAYOUT_PREINITIALIZED to match
     * the images' real initialLayout (VA-API uploads pixels via host-mapped
     * memory before the GPU ever touches them) and becomes
     * VK_IMAGE_LAYOUT_GENERAL after the first compute dispatch, where it stays
     * forever since nothing transitions the image back out of GENERAL. */
    VkImageLayout current_layout;
} gpu_image_t;

typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize size;
} gpu_memory_t;

/* Real, Vulkan-derived NV12 plane layout for an already-created+bound
 * gpu_image_t/gpu_memory_t pair - i.e. exactly the addressing that
 * gpu_compute_upload_nv12()/gpu_compute_download_nv12() already use
 * internally (vkGetImageSubresourceLayout() for each plane's real row
 * pitch/offset, plus the real inter-plane bind offset from
 * vkGetImageMemoryRequirements()+alignment). Linear-tiled Vulkan images can
 * have row padding and inter-plane alignment gaps that a naive
 * tightly-packed width/height formula does not account for - any caller
 * that needs to describe this image's memory layout to something outside
 * this file (e.g. a VAImage's pitches/offsets/data_size handed to libva)
 * must use these real values, not a naive formula, whenever that
 * description will be used to address this same memory directly. */
typedef struct {
    uint32_t y_pitch;
    uint64_t y_offset;
    uint32_t uv_pitch;
    uint64_t uv_offset;
    uint64_t total_size;
} gpu_nv12_layout_t;

typedef struct bc250_gpu_context {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue compute_queue;
    uint32_t compute_queue_family;
    VkCommandPool cmd_pool;
    VkDescriptorPool desc_pool;
    
    /* Descriptor set layouts */
    VkDescriptorSetLayout me_desc_layout;
    VkDescriptorSetLayout predict_desc_layout;
    VkDescriptorSetLayout dct_desc_layout;
    VkDescriptorSetLayout quant_desc_layout;
    VkDescriptorSetLayout deblock_desc_layout;
    VkDescriptorSetLayout entropy_desc_layout;
    VkDescriptorSetLayout cc_desc_layout;
    /* reconstruct.comp: quant_levels_buffer + coeff_buffer (readonly) +
     * pred_buffer (readonly) + recon Y/UV images (writeonly). See
     * reconstruct.comp's top-of-file comment. */
    VkDescriptorSetLayout reconstruct_desc_layout;

    /* intra_wavefront.comp: current Y/UV images (readonly), recon Y/UV
     * images (read-write - this frame's in-progress reconstruction, doubles
     * as the next frame's P-reference once the diagonal loop completes),
     * quant_levels_buffer/coeff_buffer/pred_mode_buffer (writeonly). Used
     * ONLY for I-slices, dispatched once per diagonal by
     * gpu_compute_dispatch_encode() - see intra_wavefront.comp's top-of-file
     * comment. */
    VkDescriptorSetLayout intra_wavefront_desc_layout;

    /* Pipeline layouts */
    VkPipelineLayout motion_est_layout;
    VkPipelineLayout predict_layout;
    VkPipelineLayout transform_layout;
    VkPipelineLayout quantize_layout;
    VkPipelineLayout deblock_layout;
    VkPipelineLayout entropy_layout;
    VkPipelineLayout color_convert_layout;
    VkPipelineLayout reconstruct_layout;
    VkPipelineLayout intra_wavefront_layout;

    /* Compute pipelines */
    VkPipeline motion_est_pipeline;
    VkPipeline predict_pipeline;
    VkPipeline transform_pipeline;
    VkPipeline quantize_pipeline;
    VkPipeline deblock_pipeline;
    VkPipeline entropy_pipeline;
    VkPipeline color_convert_pipeline;
    VkPipeline reconstruct_pipeline;
    VkPipeline intra_wavefront_pipeline;

    /* Descriptor sets */
    VkDescriptorSet me_desc_set;
    VkDescriptorSet predict_desc_set;
    VkDescriptorSet dct_desc_set;
    VkDescriptorSet quant_desc_set;
    VkDescriptorSet deblock_desc_set;
    VkDescriptorSet entropy_desc_set;
    VkDescriptorSet cc_desc_set;
    VkDescriptorSet reconstruct_desc_set;
    VkDescriptorSet intra_wavefront_desc_set;

    /* Encoding Buffers */
    VkBuffer mv_buffer;
    VkDeviceMemory mv_memory;
    
    VkBuffer residual_buffer;
    VkDeviceMemory residual_memory;
    
    VkBuffer coeff_buffer;
    VkDeviceMemory coeff_memory;
    
    VkBuffer quant_levels_buffer;
    VkDeviceMemory quant_levels_memory;
    
    VkBuffer nz_count_buffer;
    VkDeviceMemory nz_count_memory;

    VkBuffer entropy_buffer;
    VkDeviceMemory entropy_memory;

    /* Prediction value retained by residual_predict.comp (its PredOut,
     * binding 7), same size/indexing as residual_buffer - consumed by
     * reconstruct.comp so it adds the EXACT prediction value back to the
     * reconstructed residual instead of recomputing it. Device-local only;
     * no host readback needed. */
    VkBuffer pred_buffer;
    VkDeviceMemory pred_memory;

    VkBuffer staging_buffers[2];
    VkDeviceMemory staging_memories[2];
    void *staging_mapped[2];
    VkDeviceSize staging_size;

    /* Host-visible readback of the real post-quantization coefficient levels
     * (mirrors quant_levels_buffer) and pre-quantization transform
     * coefficients (mirrors coeff_buffer, needed for the I16x16 luma DC
     * Hadamard). Double-buffered the same way as staging_buffers[]/entropy_buffer. */
    VkBuffer quant_staging_buffers[2];
    VkDeviceMemory quant_staging_memories[2];
    void *quant_staging_mapped[2];
    VkDeviceSize quant_staging_size;

    VkBuffer coeff_staging_buffers[2];
    VkDeviceMemory coeff_staging_memories[2];
    void *coeff_staging_mapped[2];
    VkDeviceSize coeff_staging_size;

    /* Per-MB chosen I16x16 prediction mode (see residual_predict.comp),
     * device buffer + host-visible readback, same double-buffer contract as
     * quant_staging_buffers/coeff_staging_buffers above. */
    VkBuffer pred_mode_buffer;
    VkDeviceMemory pred_mode_memory;
    VkBuffer pred_mode_staging_buffers[2];
    VkDeviceMemory pred_mode_staging_memories[2];
    void *pred_mode_staging_mapped[2];
    VkDeviceSize pred_mode_staging_size;

    /* Host-visible readback of the real per-MB motion vectors motion_estimation.comp
     * writes to mv_buffer (mv_buffer itself is device-local only and was never
     * readable from the CPU before this). Needed so the CPU CAVLC writer can compute
     * a real spec MVD (median-of-neighbors predictor) instead of a heuristic. Same
     * double-buffer contract as the other staging buffers. */
    VkBuffer mv_staging_buffers[2];
    VkDeviceMemory mv_staging_memories[2];
    void *mv_staging_mapped[2];
    VkDeviceSize mv_staging_size;

    /* Reconstructed frame for DPB */
    gpu_image_t recon_image;
    gpu_memory_t recon_memory;
    bool has_recon_frame;

    /* Double-buffering for pipeline overlap */
    VkCommandBuffer cmd_bufs[2];
    VkFence fences[2];
    VkSemaphore timeline_sem;
    uint64_t timeline_value;
    int current_buf;
    
    /* Frame state */
    uint32_t frame_width;
    uint32_t frame_height;
    
    /* Device properties */
    VkPhysicalDeviceProperties dev_props;
    uint32_t max_workgroup_size;
    bool is_rdna2;

    /* Opt-in GPU per-stage timing (BC250_PERF_STATS=1) - see gpu_compute.c's
     * BC250_PERF_NUM_TIMESTAMPS comment and gpu_compute_dispatch_encode()/
     * gpu_compute_sync(). One VkQueryPool per double-buffered command
     * buffer, read back (and a "[BC250_PERF_GPU] ..." line printed to
     * stderr) once its frame's fence is known-signaled in gpu_compute_sync().
     * perf_is_intra[] records which prediction path (whole-frame-parallel
     * P-path vs diagonal-wavefront I-path) that buffer's frame took, since
     * the two paths write different subsets of the timestamp slots. */
    bool perf_stats_enabled;
    VkQueryPool timestamp_pools[2];
    double timestamp_period_ns;
    bool perf_is_intra[2];
    uint32_t perf_frame_counter;
    /* gpu_compute_sync() is called from more than one place per real frame
     * (h264_encoder_encode_frame()'s own EndPicture-driven encode, AND
     * va_backend.c's bc250_SyncSurface()) - both calls are cheap/correct
     * (the second just re-waits on an already-signaled fence), but without
     * this flag the perf-stats printer would read+print the same buffer's
     * still-valid query results again on every redundant call. Set true
     * right after gpu_compute_dispatch_encode() resets+writes this buffer's
     * queries; cleared after the first successful readback. */
    bool perf_result_pending[2];
} bc250_gpu_context_t;

typedef bc250_gpu_context_t gpu_context_t;

/* Core lifecycle */
int bc250_gpu_init(bc250_gpu_context_t *ctx);
void bc250_gpu_destroy(bc250_gpu_context_t *ctx);

int gpu_compute_init(gpu_context_t *ctx);
void gpu_compute_terminate(gpu_context_t *ctx);

/* Image allocation & transfers */
int gpu_compute_create_image(gpu_context_t *ctx, int width, int height, int format, gpu_image_t *image, gpu_memory_t *memory);
void gpu_compute_destroy_image(gpu_context_t *ctx, gpu_image_t image, gpu_memory_t memory);

/* Queries the real layout described above for `image`/`memory` (both must
 * already be created and bound, e.g. via gpu_compute_create_image()).
 * Returns 0 on success, -1 if ctx/image/layout is NULL or memory is
 * unbound. */
int gpu_compute_get_nv12_layout(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory, gpu_nv12_layout_t *layout);

int gpu_compute_upload_nv12(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory,
                           const uint8_t *y_plane, int y_pitch,
                           const uint8_t *uv_plane, int uv_pitch,
                           int width, int height);

int gpu_compute_download_nv12(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory,
                             uint8_t *y_plane, int y_pitch,
                             uint8_t *uv_plane, int uv_pitch,
                             int width, int height);

/* Test-harness instrumentation (tools/quality_test.sh): dumps raw NV12
 * frame bytes to BC250_DUMP_DIR (default /tmp/bc250_dump_frames) when
 * BC250_DUMP_INPUT_FRAMES=1 is set in the environment; a no-op otherwise.
 * Shared by every known VA-API upload path so the harness catches whichever
 * one a given libva/ffmpeg build actually uses. See gpu_compute.c. */
void bc250_debug_dump_nv12_frame(const uint8_t *y_plane, int y_pitch,
                                  const uint8_t *uv_plane, int uv_pitch,
                                  int width, int height);

/* Picture encoding orchestration */
int gpu_compute_begin_picture(gpu_context_t *ctx, gpu_image_t render_target);
/* num_slices: threaded through to residual_predict.comp so its I16x16
 * neighbor-availability check can correctly treat a different-slice
 * neighbor MB as unavailable - see that shader's SLICE BOUNDARIES comment.
 * Must match the num_slices the caller will actually partition the CAVLC
 * bitstream into (encoder_h264.c's BC250_SLICES_PER_FRAME). */
int gpu_compute_dispatch_encode(gpu_context_t *ctx, gpu_image_t render_target, int width, int height, int qp, int is_intra, int num_slices);
int gpu_compute_end_picture(gpu_context_t *ctx);
int gpu_compute_sync(gpu_context_t *ctx);
int gpu_compute_get_staging_data(gpu_context_t *ctx, void **data, size_t *size);
int gpu_compute_release_staging_data(gpu_context_t *ctx);

/* Real per-coefficient residual readback (see quant_staging_buffers/coeff_staging_buffers
 * above). Both follow the same double-buffer contract as gpu_compute_get_staging_data():
 * call after gpu_compute_sync(), data points at the buffer that was written by the
 * frame BEFORE the one just submitted (fence-safe to read from the CPU). Layout is
 * num_mbs*24*16 ints, int index = (mb_idx*24+block_idx)*16+pos (raster position within
 * the 4x4 block, NOT zigzag). */
int gpu_compute_get_quant_staging_data(gpu_context_t *ctx, void **data, size_t *size);
int gpu_compute_get_coeff_staging_data(gpu_context_t *ctx, void **data, size_t *size);

/* Real per-MB I16x16 prediction mode (see residual_predict.comp), one uint32
 * per MB, values match cavlc.h's H264_I16x16_* constants. Only meaningful for
 * I-slices. Same fence-safe double-buffer contract as above. */
int gpu_compute_get_pred_mode_staging_data(gpu_context_t *ctx, void **data, size_t *size);

/* Real per-MB motion vectors (see motion_estimation.comp's OutputMV), laid
 * out as num_mbs entries of {int32_t mvx, mvy; uint32_t sad; uint32_t pad;}
 * (16 bytes/entry, matching the GPU's std430 MotionVector struct). Only
 * meaningful for P-slices. Same fence-safe double-buffer contract as above. */
int gpu_compute_get_mv_staging_data(gpu_context_t *ctx, void **data, size_t *size);

/* TEMPORARY debug instrumentation for Part A (reconstruction) verification -
 * see gpu_compute.c for details. No-op unless BC250_DUMP_RECON_FRAMES=1. */
void gpu_compute_debug_dump_recon(gpu_context_t *ctx, int width, int height);

#ifdef __cplusplus
}
#endif

#endif // GPU_COMPUTE_H
