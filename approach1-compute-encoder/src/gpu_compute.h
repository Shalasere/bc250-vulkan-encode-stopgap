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
    VkDescriptorSetLayout dct_desc_layout;
    VkDescriptorSetLayout quant_desc_layout;
    VkDescriptorSetLayout deblock_desc_layout;
    VkDescriptorSetLayout entropy_desc_layout;
    VkDescriptorSetLayout cc_desc_layout;

    /* Pipeline layouts */
    VkPipelineLayout motion_est_layout;
    VkPipelineLayout transform_layout;
    VkPipelineLayout quantize_layout;
    VkPipelineLayout deblock_layout;
    VkPipelineLayout entropy_layout;
    VkPipelineLayout color_convert_layout;

    /* Compute pipelines */
    VkPipeline motion_est_pipeline;
    VkPipeline transform_pipeline;
    VkPipeline quantize_pipeline;
    VkPipeline deblock_pipeline;
    VkPipeline entropy_pipeline;
    VkPipeline color_convert_pipeline;
    
    /* Descriptor sets */
    VkDescriptorSet me_desc_set;
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
int gpu_compute_dispatch_encode(gpu_context_t *ctx, gpu_image_t render_target, int width, int height);
int gpu_compute_end_picture(gpu_context_t *ctx);
int gpu_compute_sync(gpu_context_t *ctx);
int gpu_compute_get_staging_data(gpu_context_t *ctx, void **data, size_t *size);
int gpu_compute_release_staging_data(gpu_context_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // GPU_COMPUTE_H
