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
} gpu_image_t;

typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize size;
} gpu_memory_t;

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

    /* Pipeline layouts */
    VkPipelineLayout motion_est_layout;
    VkPipelineLayout predict_layout;
    VkPipelineLayout transform_layout;
    VkPipelineLayout quantize_layout;
    VkPipelineLayout deblock_layout;
    VkPipelineLayout entropy_layout;
    VkPipelineLayout color_convert_layout;

    /* Compute pipelines */
    VkPipeline motion_est_pipeline;
    VkPipeline predict_pipeline;
    VkPipeline transform_pipeline;
    VkPipeline quantize_pipeline;
    VkPipeline deblock_pipeline;
    VkPipeline entropy_pipeline;
    VkPipeline color_convert_pipeline;

    /* Descriptor sets */
    VkDescriptorSet me_desc_set;
    VkDescriptorSet predict_desc_set;
    VkDescriptorSet dct_desc_set;
    VkDescriptorSet quant_desc_set;
    VkDescriptorSet deblock_desc_set;
    VkDescriptorSet entropy_desc_set;
    VkDescriptorSet cc_desc_set;

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

int gpu_compute_upload_nv12(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory,
                           const uint8_t *y_plane, int y_pitch,
                           const uint8_t *uv_plane, int uv_pitch,
                           int width, int height);

int gpu_compute_download_nv12(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory,
                             uint8_t *y_plane, int y_pitch,
                             uint8_t *uv_plane, int uv_pitch,
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

#ifdef __cplusplus
}
#endif

#endif // GPU_COMPUTE_H
