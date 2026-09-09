/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * gpu_compute.c - Vulkan compute backend for AMD BC-250 encoding
 */
#include "gpu_compute.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define BC250_DEVICE_ID 0x13FE
#define AMD_VENDOR_ID   0x1002

/* Opt-in GPU per-stage timing (BC250_PERF_STATS=1), added for real-time
 * throughput diagnosis - see gpu_compute_dispatch_encode()'s timestamp
 * writes and gpu_compute_sync()'s readback. One VkQueryPool slot per
 * checkpoint, written in strictly increasing time order every frame
 * regardless of P/I path so the deltas are always well-defined (the path
 * NOT taken just gets a run of zero-duration slots):
 *   0 = frame start (top of command buffer)
 *   1 = after motion estimation
 *   2 = after residual_predict (P only; == 1 on I frames)
 *   3 = after DCT (P only; == 2 on I frames)
 *   4 = after quantize (P only; == 3 on I frames)
 *   5 = after reconstruct (P only; == 4 on I frames)
 *   6 = after diagonal-wavefront intra reconstruction (I only; == 5 on P frames)
 *   7 = after deblock (both paths; == 6 if BC250_FAST_MODE skipped it)
 *   8 = after entropy encode (both paths)
 *   9 = after the GPU->host staging buffer copies (frame end)
 */
#define BC250_PERF_NUM_TIMESTAMPS 10

#define VK_CHECK(x) do { \
    VkResult err = (x); \
    if (err != VK_SUCCESS) { \
        fprintf(stderr, "[bc250-gpu] Vulkan error %d at %s:%d\n", err, __FILE__, __LINE__); \
        return -1; \
    } \
} while(0)

static uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    /* Fallback to any matching type */
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if (type_filter & (1 << i)) {
            return i;
        }
    }
    return 0;
}

/* Like find_memory_type(), but tries `preferred` first (which must be a
 * superset of `required`) and only falls back to a plain `required`-only
 * match if this device exposes no type satisfying `preferred` at all.
 *
 * WHY THIS EXISTS: the GPU-readback staging buffers this driver bulk-copies
 * every frame (quant_staging_buffers/coeff_staging_buffers/pred_mode_staging_
 * buffers/mv_staging_buffers - see encoder_h264.c's shadow_copy() doc
 * comment) were being bound to a HOST_VISIBLE|HOST_COHERENT memory type
 * WITHOUT HOST_CACHED, on the reasoning that the GPU's one-shot
 * vkCmdCopyBuffer write into them doesn't care about CPU cacheability. That
 * reasoning only accounted for the write side. On real BC-250 hardware this
 * driver was measured (BC250_PERF_STATS=1, real board run, 1280x720) paying
 * ~81ms/frame - the entire real-time-throughput gap between ~1ms of actual
 * GPU compute + ~1ms of CPU CAVLC and the ~83ms real wall-clock time per
 * frame - inside shadow_copy()'s bulk memcpy() itself, i.e. the CPU
 * *reading* ~10.6MB/frame back out of that same memory. Uncached/
 * write-combined memory has notoriously poor CPU read bandwidth (routinely
 * an order of magnitude or more below normal cached RAM) even for a single
 * fully sequential streaming pass - confirmed by the shadow_copy_ms
 * diagnostic bracket landing within noise of the entire unaccounted gap.
 * vkGetPhysicalDeviceMemoryProperties() on this device confirms a
 * HOST_VISIBLE|HOST_COHERENT|HOST_CACHED type exists on the same heap as the
 * uncached one currently selected (both are system-memory-backed on this
 * APU, not a discrete-GPU BAR), so preferring it costs nothing in
 * portability: find_memory_type()'s original required-only search is kept
 * as the fallback for any device that doesn't expose a cached type at all.
 * This only changes which physical memory type backs these buffers - not
 * their VkBufferUsageFlags, not HOST_COHERENT (still required both passes,
 * so Vulkan still guarantees the CPU sees the GPU's writes after the
 * existing fence wait with no added vkInvalidateMappedMemoryRanges/
 * vkFlushMappedMemoryRanges calls needed), and not a single byte of what
 * either side reads or writes - purely a CPU-read-speed optimization. */
static uint32_t find_memory_type_preferred(VkPhysicalDevice physical_device, uint32_t type_filter,
                                            VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & preferred) == preferred) {
            return i;
        }
    }
    return find_memory_type(physical_device, type_filter, required);
}

static int create_buffer_with_memory(gpu_context_t *ctx, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer *buffer, VkDeviceMemory *memory) {
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VK_CHECK(vkCreateBuffer(ctx->device, &buffer_info, NULL, buffer));

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(ctx->device, *buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = find_memory_type(ctx->physical_device, mem_reqs.memoryTypeBits, properties)
    };
    VK_CHECK(vkAllocateMemory(ctx->device, &alloc_info, NULL, memory));
    VK_CHECK(vkBindBufferMemory(ctx->device, *buffer, *memory, 0));
    return 0;
}

/* Same as create_buffer_with_memory(), but selects the memory type via
 * find_memory_type_preferred() instead of find_memory_type() - see that
 * function's doc comment. Used only for the GPU-readback staging buffers
 * that encoder_h264.c's shadow_copy() bulk-reads every frame, where CPU read
 * bandwidth (not GPU write bandwidth) is what actually matters. */
static int create_buffer_with_memory_preferred(gpu_context_t *ctx, VkDeviceSize size, VkBufferUsageFlags usage,
                                                VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags required,
                                                VkBuffer *buffer, VkDeviceMemory *memory) {
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VK_CHECK(vkCreateBuffer(ctx->device, &buffer_info, NULL, buffer));

    VkMemoryRequirements mem_reqs;
    vkGetBufferMemoryRequirements(ctx->device, *buffer, &mem_reqs);

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = find_memory_type_preferred(ctx->physical_device, mem_reqs.memoryTypeBits, preferred, required)
    };
    VK_CHECK(vkAllocateMemory(ctx->device, &alloc_info, NULL, memory));
    VK_CHECK(vkBindBufferMemory(ctx->device, *buffer, *memory, 0));
    return 0;
}

static void update_storage_buffer_descriptor(VkDevice device, VkDescriptorSet set, uint32_t binding, VkBuffer buffer, VkDeviceSize size) {
    VkDescriptorBufferInfo buf_info = {
        .buffer = buffer,
        .offset = 0,
        .range = size
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buf_info
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
}

static void update_storage_image_descriptor(VkDevice device, VkDescriptorSet set, uint32_t binding, VkImageView view) {
    VkDescriptorImageInfo img_info = {
        .sampler = VK_NULL_HANDLE,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &img_info
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
}

static int allocate_encoding_buffers(gpu_context_t *ctx, uint32_t width, uint32_t height) {
    if (ctx->mv_buffer) {
        vkDestroyBuffer(ctx->device, ctx->mv_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->mv_memory, NULL);
        ctx->mv_buffer = VK_NULL_HANDLE;
    }
    if (ctx->residual_buffer) {
        vkDestroyBuffer(ctx->device, ctx->residual_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->residual_memory, NULL);
        ctx->residual_buffer = VK_NULL_HANDLE;
    }
    if (ctx->pred_buffer) {
        vkDestroyBuffer(ctx->device, ctx->pred_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->pred_memory, NULL);
        ctx->pred_buffer = VK_NULL_HANDLE;
    }
    if (ctx->coeff_buffer) {
        vkDestroyBuffer(ctx->device, ctx->coeff_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->coeff_memory, NULL);
        ctx->coeff_buffer = VK_NULL_HANDLE;
    }
    if (ctx->quant_levels_buffer) {
        vkDestroyBuffer(ctx->device, ctx->quant_levels_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->quant_levels_memory, NULL);
        ctx->quant_levels_buffer = VK_NULL_HANDLE;
    }
    if (ctx->nz_count_buffer) {
        vkDestroyBuffer(ctx->device, ctx->nz_count_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->nz_count_memory, NULL);
        ctx->nz_count_buffer = VK_NULL_HANDLE;
    }
    if (ctx->pred_mode_buffer) {
        vkDestroyBuffer(ctx->device, ctx->pred_mode_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->pred_mode_memory, NULL);
        ctx->pred_mode_buffer = VK_NULL_HANDLE;
    }
    if (ctx->entropy_buffer) {
        vkDestroyBuffer(ctx->device, ctx->entropy_buffer, NULL);
        vkFreeMemory(ctx->device, ctx->entropy_memory, NULL);
        ctx->entropy_buffer = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 2; i++) {
        if (ctx->staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->staging_memories[i]);
            ctx->staging_mapped[i] = NULL;
        }
        if (ctx->staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->staging_memories[i], NULL);
            ctx->staging_buffers[i] = VK_NULL_HANDLE;
            ctx->staging_memories[i] = VK_NULL_HANDLE;
        }
        if (ctx->quant_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->quant_staging_memories[i]);
            ctx->quant_staging_mapped[i] = NULL;
        }
        if (ctx->quant_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->quant_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->quant_staging_memories[i], NULL);
            ctx->quant_staging_buffers[i] = VK_NULL_HANDLE;
            ctx->quant_staging_memories[i] = VK_NULL_HANDLE;
        }
        if (ctx->coeff_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->coeff_staging_memories[i]);
            ctx->coeff_staging_mapped[i] = NULL;
        }
        if (ctx->coeff_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->coeff_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->coeff_staging_memories[i], NULL);
            ctx->coeff_staging_buffers[i] = VK_NULL_HANDLE;
            ctx->coeff_staging_memories[i] = VK_NULL_HANDLE;
        }
        if (ctx->pred_mode_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->pred_mode_staging_memories[i]);
            ctx->pred_mode_staging_mapped[i] = NULL;
        }
        if (ctx->pred_mode_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->pred_mode_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->pred_mode_staging_memories[i], NULL);
            ctx->pred_mode_staging_buffers[i] = VK_NULL_HANDLE;
            ctx->pred_mode_staging_memories[i] = VK_NULL_HANDLE;
        }
        if (ctx->mv_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->mv_staging_memories[i]);
            ctx->mv_staging_mapped[i] = NULL;
        }
        if (ctx->mv_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->mv_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->mv_staging_memories[i], NULL);
            ctx->mv_staging_buffers[i] = VK_NULL_HANDLE;
            ctx->mv_staging_memories[i] = VK_NULL_HANDLE;
        }
    }

    ctx->frame_width = width;
    ctx->frame_height = height;

    uint32_t width_in_mbs = (width + 15) / 16;
    uint32_t height_in_mbs = (height + 15) / 16;
    uint32_t num_mbs = width_in_mbs * height_in_mbs;

    VkDeviceSize mv_size = num_mbs * sizeof(uint32_t) * 4;
    VkDeviceSize residual_size = num_mbs * 24 * 16 * sizeof(int);
    VkDeviceSize coeff_size = residual_size;
    VkDeviceSize quant_levels_size = residual_size;
    VkDeviceSize nz_count_size = num_mbs * 24 * sizeof(uint32_t);
    VkDeviceSize pred_mode_size = num_mbs * sizeof(uint32_t);
    VkDeviceSize entropy_size = width * height * 2; /* Generous */

    ctx->staging_size = entropy_size;
    ctx->quant_staging_size = quant_levels_size;
    ctx->coeff_staging_size = coeff_size;
    ctx->pred_mode_staging_size = pred_mode_size;
    ctx->mv_staging_size = mv_size;

    create_buffer_with_memory(ctx, mv_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->mv_buffer, &ctx->mv_memory);
    create_buffer_with_memory(ctx, residual_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->residual_buffer, &ctx->residual_memory);
    create_buffer_with_memory(ctx, residual_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->pred_buffer, &ctx->pred_memory);
    create_buffer_with_memory(ctx, coeff_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->coeff_buffer, &ctx->coeff_memory);
    create_buffer_with_memory(ctx, quant_levels_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->quant_levels_buffer, &ctx->quant_levels_memory);
    create_buffer_with_memory(ctx, nz_count_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->nz_count_buffer, &ctx->nz_count_memory);
    create_buffer_with_memory(ctx, pred_mode_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->pred_mode_buffer, &ctx->pred_mode_memory);
    create_buffer_with_memory(ctx, entropy_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->entropy_buffer, &ctx->entropy_memory);
    create_buffer_with_memory(ctx, entropy_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->staging_buffers[0], &ctx->staging_memories[0]);
    create_buffer_with_memory(ctx, entropy_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->staging_buffers[1], &ctx->staging_memories[1]);

    /* These four staging-buffer pairs are the ones encoder_h264.c's
     * shadow_copy() bulk-reads from the CPU every single frame (quant_levels/
     * coeff/pred_modes/mvs) - see find_memory_type_preferred()'s doc comment
     * for why they request HOST_CACHED as a preference, not a requirement.
     * staging_buffers[]/entropy_buffer above are a separate, currently-dead
     * GPU-entropy-coding path (nothing reads gpu_compute_get_staging_data())
     * and are deliberately left on plain create_buffer_with_memory(). */
    VkMemoryPropertyFlags cached_pref = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkMemoryPropertyFlags visible_req = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    create_buffer_with_memory_preferred(ctx, quant_levels_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->quant_staging_buffers[0], &ctx->quant_staging_memories[0]);
    create_buffer_with_memory_preferred(ctx, quant_levels_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->quant_staging_buffers[1], &ctx->quant_staging_memories[1]);
    create_buffer_with_memory_preferred(ctx, coeff_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->coeff_staging_buffers[0], &ctx->coeff_staging_memories[0]);
    create_buffer_with_memory_preferred(ctx, coeff_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->coeff_staging_buffers[1], &ctx->coeff_staging_memories[1]);
    create_buffer_with_memory_preferred(ctx, pred_mode_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->pred_mode_staging_buffers[0], &ctx->pred_mode_staging_memories[0]);
    create_buffer_with_memory_preferred(ctx, pred_mode_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->pred_mode_staging_buffers[1], &ctx->pred_mode_staging_memories[1]);
    create_buffer_with_memory_preferred(ctx, mv_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->mv_staging_buffers[0], &ctx->mv_staging_memories[0]);
    create_buffer_with_memory_preferred(ctx, mv_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, cached_pref, visible_req, &ctx->mv_staging_buffers[1], &ctx->mv_staging_memories[1]);

    /* Persistently map all staging buffers to eliminate per-frame map/unmap syscall overhead */
    vkMapMemory(ctx->device, ctx->staging_memories[0], 0, entropy_size, 0, &ctx->staging_mapped[0]);
    vkMapMemory(ctx->device, ctx->staging_memories[1], 0, entropy_size, 0, &ctx->staging_mapped[1]);
    vkMapMemory(ctx->device, ctx->quant_staging_memories[0], 0, quant_levels_size, 0, &ctx->quant_staging_mapped[0]);
    vkMapMemory(ctx->device, ctx->quant_staging_memories[1], 0, quant_levels_size, 0, &ctx->quant_staging_mapped[1]);
    vkMapMemory(ctx->device, ctx->coeff_staging_memories[0], 0, coeff_size, 0, &ctx->coeff_staging_mapped[0]);
    vkMapMemory(ctx->device, ctx->coeff_staging_memories[1], 0, coeff_size, 0, &ctx->coeff_staging_mapped[1]);
    vkMapMemory(ctx->device, ctx->pred_mode_staging_memories[0], 0, pred_mode_size, 0, &ctx->pred_mode_staging_mapped[0]);
    vkMapMemory(ctx->device, ctx->pred_mode_staging_memories[1], 0, pred_mode_size, 0, &ctx->pred_mode_staging_mapped[1]);
    vkMapMemory(ctx->device, ctx->mv_staging_memories[0], 0, mv_size, 0, &ctx->mv_staging_mapped[0]);
    vkMapMemory(ctx->device, ctx->mv_staging_memories[1], 0, mv_size, 0, &ctx->mv_staging_mapped[1]);

    /* Update buffer descriptors */
    update_storage_buffer_descriptor(ctx->device, ctx->me_desc_set, 2, ctx->mv_buffer, mv_size);

    /* residual_predict.comp's buffer bindings: mv_buffer (real MVs, for P
     * motion-compensated residual), residual_buffer (its output, consumed by
     * dct_transform.comp) and pred_mode_buffer (its I16x16 mode decision,
     * consumed by encoder_h264.c's CAVLC header writer). Image bindings
     * (current Y/UV, reference Y) are updated per-dispatch in
     * gpu_compute_dispatch_encode() since they change every frame. */
    update_storage_buffer_descriptor(ctx->device, ctx->predict_desc_set, 3, ctx->mv_buffer, mv_size);
    update_storage_buffer_descriptor(ctx->device, ctx->predict_desc_set, 4, ctx->residual_buffer, residual_size);
    update_storage_buffer_descriptor(ctx->device, ctx->predict_desc_set, 5, ctx->pred_mode_buffer, pred_mode_size);
    update_storage_buffer_descriptor(ctx->device, ctx->predict_desc_set, 7, ctx->pred_buffer, residual_size);

    update_storage_buffer_descriptor(ctx->device, ctx->dct_desc_set, 0, ctx->residual_buffer, residual_size);
    update_storage_buffer_descriptor(ctx->device, ctx->dct_desc_set, 1, ctx->coeff_buffer, coeff_size);

    update_storage_buffer_descriptor(ctx->device, ctx->quant_desc_set, 0, ctx->coeff_buffer, coeff_size);
    update_storage_buffer_descriptor(ctx->device, ctx->quant_desc_set, 1, ctx->quant_levels_buffer, quant_levels_size);
    update_storage_buffer_descriptor(ctx->device, ctx->quant_desc_set, 2, ctx->nz_count_buffer, nz_count_size);

    /* deblock_filter.comp binding 1 is declared "QuantLevels" there (it used
     * to be misleadingly declared "QPMap" while never actually being read -
     * see that shader's top-of-file comment): real per-4x4-block quantized
     * coefficient levels, used for the ITU-T 8.7.2.1 nonzero-coefficient
     * boundary-strength test. Binding 2 is the real per-macroblock motion
     * vectors, used for that section's motion-vector-difference test. */
    update_storage_buffer_descriptor(ctx->device, ctx->deblock_desc_set, 1, ctx->quant_levels_buffer, quant_levels_size);
    update_storage_buffer_descriptor(ctx->device, ctx->deblock_desc_set, 2, ctx->mv_buffer, mv_size);

    update_storage_buffer_descriptor(ctx->device, ctx->entropy_desc_set, 0, ctx->quant_levels_buffer, quant_levels_size);
    update_storage_buffer_descriptor(ctx->device, ctx->entropy_desc_set, 1, ctx->entropy_buffer, entropy_size);

    /* reconstruct.comp's buffer bindings: quant_levels_buffer (post-quant AC
     * levels), coeff_buffer (pre-quant, for the I16x16/chroma DC Hadamard)
     * and pred_buffer (retained prediction). Image bindings (recon Y/UV) are
     * updated per-dispatch in gpu_compute_dispatch_encode() since recon_image
     * can be (re)created there. */
    update_storage_buffer_descriptor(ctx->device, ctx->reconstruct_desc_set, 0, ctx->quant_levels_buffer, quant_levels_size);
    update_storage_buffer_descriptor(ctx->device, ctx->reconstruct_desc_set, 1, ctx->coeff_buffer, coeff_size);
    update_storage_buffer_descriptor(ctx->device, ctx->reconstruct_desc_set, 2, ctx->pred_buffer, residual_size);

    /* intra_wavefront.comp's buffer bindings: quant_levels_buffer/coeff_buffer/
     * pred_mode_buffer (writeonly - same underlying buffers as the whole-frame
     * P-slice path, just written by this shader instead for I-slices). Image
     * bindings (current Y/UV, recon Y/UV) are updated per-dispatch in
     * gpu_compute_dispatch_encode() since recon_image can be (re)created there
     * and render_target changes every frame. */
    update_storage_buffer_descriptor(ctx->device, ctx->intra_wavefront_desc_set, 4, ctx->quant_levels_buffer, quant_levels_size);
    update_storage_buffer_descriptor(ctx->device, ctx->intra_wavefront_desc_set, 5, ctx->coeff_buffer, coeff_size);
    update_storage_buffer_descriptor(ctx->device, ctx->intra_wavefront_desc_set, 6, ctx->pred_mode_buffer, pred_mode_size);

    return 0;
}

static VkShaderModule load_spirv_shader(VkDevice device, const char *filename) {
    const char *search_paths[] = {
        "/var/lib/bc250/shaders",
        "/usr/share/bc250/shaders",
        "/usr/local/share/bc250/shaders",
        "/usr/lib64/dri/shaders",
        "/usr/lib/dri/shaders",
        "./shaders",
        "../shaders",
        "../../approach1-compute-encoder/shaders",
        NULL
    };

    FILE *f = NULL;
    char full_path[512];

    const char *env_dir = getenv("BC250_SHADER_DIR");
    if (env_dir && env_dir[0] != '\0') {
        snprintf(full_path, sizeof(full_path), "%s/%s", env_dir, filename);
        f = fopen(full_path, "rb");
    }

    if (!f) {
        for (int i = 0; search_paths[i] != NULL; i++) {
            snprintf(full_path, sizeof(full_path), "%s/%s", search_paths[i], filename);
            f = fopen(full_path, "rb");
            if (f) break;
        }
    }

    if (!f) {
        /* Fallback: try raw filename */
        f = fopen(filename, "rb");
    }

    if (!f) {
        fprintf(stderr, "[bc250-gpu] Could not find SPIR-V shader: %s\n", filename);
        return VK_NULL_HANDLE;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t *code = malloc(size);
    if (!code) {
        fclose(f);
        return VK_NULL_HANDLE;
    }
    size_t read_bytes = fread(code, 1, size, f);
    fclose(f);

    if (read_bytes != size || size % 4 != 0) {
        free(code);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code
    };
    VkShaderModule shader;
    VkResult res = vkCreateShaderModule(device, &create_info, NULL, &shader);
    free(code);

    if (res != VK_SUCCESS) {
        fprintf(stderr, "[bc250-gpu] Failed to create shader module for %s\n", filename);
        return VK_NULL_HANDLE;
    }

    return shader;
}

static VkPipeline create_compute_pipeline(VkDevice device, VkShaderModule shader, VkPipelineLayout layout) {
    if (!shader || !layout) return VK_NULL_HANDLE;

    VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = layout
    };
    VkPipeline pipeline;
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

int bc250_gpu_init(bc250_gpu_context_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "BC-250 VCN VA-API Compute Driver",
        .apiVersion = VK_API_VERSION_1_2
    };
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info
    };
    VK_CHECK(vkCreateInstance(&inst_info, NULL, &ctx->instance));

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &dev_count, NULL);
    if (dev_count == 0) {
        fprintf(stderr, "[bc250-gpu] No Vulkan physical devices found!\n");
        return -1;
    }

    VkPhysicalDevice *devices = malloc(dev_count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(ctx->instance, &dev_count, devices);

    /* 1. Prioritize BC-250 (0x13FE) */
    for (uint32_t i = 0; i < dev_count; i++) {
        vkGetPhysicalDeviceProperties(devices[i], &ctx->dev_props);
        if (ctx->dev_props.deviceID == BC250_DEVICE_ID) {
            ctx->physical_device = devices[i];
            ctx->is_rdna2 = true;
            fprintf(stderr, "[bc250-gpu] Found AMD BC-250 APU (0x13FE) - %s\n", ctx->dev_props.deviceName);
            break;
        }
    }

    /* 2. Fallback: Any AMD device */
    if (!ctx->physical_device) {
        for (uint32_t i = 0; i < dev_count; i++) {
            vkGetPhysicalDeviceProperties(devices[i], &ctx->dev_props);
            if (ctx->dev_props.vendorID == AMD_VENDOR_ID) {
                ctx->physical_device = devices[i];
                ctx->is_rdna2 = true;
                fprintf(stderr, "[bc250-gpu] BC-250 not found, using AMD GPU: %s\n", ctx->dev_props.deviceName);
                break;
            }
        }
    }

    /* 3. Fallback: Primary compute device */
    if (!ctx->physical_device) {
        ctx->physical_device = devices[0];
        vkGetPhysicalDeviceProperties(devices[0], &ctx->dev_props);
        fprintf(stderr, "[bc250-gpu] Using primary Vulkan device: %s\n", ctx->dev_props.deviceName);
    }
    free(devices);

    /* DIAGNOSTIC ONLY (BC250_PERF_STATS=1), one-time at init: dump every
     * Vulkan memory type this device exposes, to check whether a
     * HOST_VISIBLE|HOST_COHERENT|HOST_CACHED type exists (which would let
     * the GPU-readback staging buffers - see encoder_h264.c's shadow_copy()
     * doc comment - be bulk-read by the CPU at normal cached-RAM speed
     * instead of the current uncached/write-combined type's speed). */
    if (getenv("BC250_PERF_STATS")) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            fprintf(stderr, "[bc250-gpu] memtype[%u] heap=%u flags=0x%x%s%s%s%s%s\n",
                    i, mp.memoryTypes[i].heapIndex, f,
                    (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "",
                    (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
                    (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " HOST_COHERENT" : "",
                    (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " HOST_CACHED" : "",
                    (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? " LAZILY_ALLOCATED" : "");
        }
    }

    ctx->max_workgroup_size = ctx->dev_props.limits.maxComputeWorkGroupSize[0];

    /* Find compute queue family */
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &qf_count, NULL);
    VkQueueFamilyProperties *qf_props = malloc(qf_count * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &qf_count, qf_props);

    /* 1. Prioritize dedicated hardware async compute queue (ACE on RDNA2) */
    ctx->compute_queue_family = (uint32_t)-1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if ((qf_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            !(qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            ctx->compute_queue_family = i;
            fprintf(stderr, "[bc250-gpu] Using dedicated async compute queue family %u\n", i);
            break;
        }
    }
    /* 2. Fallback to any compute-capable queue */
    if (ctx->compute_queue_family == (uint32_t)-1) {
        for (uint32_t i = 0; i < qf_count; i++) {
            if (qf_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                ctx->compute_queue_family = i;
                fprintf(stderr, "[bc250-gpu] Using general compute queue family %u\n", i);
                break;
            }
        }
    }
    free(qf_props);
    if (ctx->compute_queue_family == (uint32_t)-1) {
        fprintf(stderr, "[bc250-gpu] No compute queue family available!\n");
        return -1;
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo q_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->compute_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority
    };

    VkPhysicalDeviceVulkan12Features features12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE
    };

    VkDeviceCreateInfo dev_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &q_info
    };
    VK_CHECK(vkCreateDevice(ctx->physical_device, &dev_info, NULL, &ctx->device));
    vkGetDeviceQueue(ctx->device, ctx->compute_queue_family, 0, &ctx->compute_queue);

    /* Command Pool */
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->compute_queue_family
    };
    VK_CHECK(vkCreateCommandPool(ctx->device, &pool_info, NULL, &ctx->cmd_pool));

    VkCommandBufferAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2
    };
    VK_CHECK(vkAllocateCommandBuffers(ctx->device, &alloc_info, ctx->cmd_bufs));

    /* Fences */
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VK_CHECK(vkCreateFence(ctx->device, &fence_info, NULL, &ctx->fences[0]));
    VK_CHECK(vkCreateFence(ctx->device, &fence_info, NULL, &ctx->fences[1]));

    /* Timeline Semaphore */
    VkSemaphoreTypeCreateInfo sem_type_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0
    };
    VkSemaphoreCreateInfo sem_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &sem_type_info
    };
    VK_CHECK(vkCreateSemaphore(ctx->device, &sem_info, NULL, &ctx->timeline_sem));
    ctx->timeline_value = 0;

    /* Opt-in GPU per-stage timing (BC250_PERF_STATS=1) - see the
     * BC250_PERF_NUM_TIMESTAMPS comment above and gpu_compute_dispatch_encode()/
     * gpu_compute_sync(). Query pool creation failure or a device that
     * doesn't expose compute-queue timestamps just disables the feature;
     * it is a pure diagnostic and must never affect the encode path. */
    ctx->perf_stats_enabled = false;
    ctx->timestamp_period_ns = 0.0;
    ctx->perf_frame_counter = 0;
    const char *perf_env = getenv("BC250_PERF_STATS");
    if (perf_env && (strcmp(perf_env, "1") == 0 || strcmp(perf_env, "true") == 0)) {
        if (ctx->dev_props.limits.timestampComputeAndGraphics) {
            ctx->timestamp_period_ns = (double)ctx->dev_props.limits.timestampPeriod;
            VkQueryPoolCreateInfo qp_info = {
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = BC250_PERF_NUM_TIMESTAMPS
            };
            bool pools_ok = true;
            for (int i = 0; i < 2; i++) {
                if (vkCreateQueryPool(ctx->device, &qp_info, NULL, &ctx->timestamp_pools[i]) != VK_SUCCESS) {
                    pools_ok = false;
                    break;
                }
            }
            if (pools_ok) {
                ctx->perf_stats_enabled = true;
                fprintf(stderr, "[bc250-gpu] BC250_PERF_STATS enabled (timestampPeriod=%.4f ns/tick)\n", ctx->timestamp_period_ns);
            } else {
                fprintf(stderr, "[bc250-gpu] BC250_PERF_STATS: failed to create timestamp query pools, disabling\n");
            }
        } else {
            fprintf(stderr, "[bc250-gpu] BC250_PERF_STATS: device does not report timestampComputeAndGraphics support, disabling\n");
        }
    }

    /* Create Descriptor Set Layouts */
    VkDescriptorSetLayoutBinding me_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo me_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = me_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &me_layout_info, NULL, &ctx->me_desc_layout);

    /* residual_predict.comp: current Y/UV images, reference Y image, real
     * MVs, its residual_buffer output and its pred_mode_buffer output. See
     * that shader's top-of-file comment. */
    VkDescriptorSetLayoutBinding predict_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        /* binding 6: referenceUV - previous frame's chroma plane, for real
         * P-slice chroma motion compensation (see residual_predict.comp). */
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        /* binding 7: PredOut - retained prediction value, consumed by
         * reconstruct.comp (see that shader's top-of-file comment). */
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo predict_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 8, .pBindings = predict_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &predict_layout_info, NULL, &ctx->predict_desc_layout);

    VkDescriptorSetLayoutBinding dct_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo dct_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = dct_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &dct_layout_info, NULL, &ctx->dct_desc_layout);

    VkDescriptorSetLayoutBinding quant_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo quant_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = quant_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &quant_layout_info, NULL, &ctx->quant_desc_layout);

    VkDescriptorSetLayoutBinding deblock_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo deblock_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = deblock_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &deblock_layout_info, NULL, &ctx->deblock_desc_layout);

    VkDescriptorSetLayoutBinding entropy_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo entropy_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = entropy_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &entropy_layout_info, NULL, &ctx->entropy_desc_layout);

    VkDescriptorSetLayoutBinding cc_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo cc_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = cc_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &cc_layout_info, NULL, &ctx->cc_desc_layout);

    /* reconstruct.comp: quant_levels_buffer + coeff_buffer + pred_buffer
     * (readonly), recon Y/UV images (writeonly) - see that shader's
     * top-of-file comment. */
    VkDescriptorSetLayoutBinding reconstruct_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo reconstruct_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = reconstruct_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &reconstruct_layout_info, NULL, &ctx->reconstruct_desc_layout);

    /* intra_wavefront.comp: current Y/UV (readonly), recon Y/UV (read-write -
     * see gpu_compute.h's comment), quant_levels_buffer/coeff_buffer/
     * pred_mode_buffer (writeonly). I-slice-only, diagonal-wavefront
     * dispatch - see that shader's top-of-file comment and
     * gpu_compute_dispatch_encode() below. */
    VkDescriptorSetLayoutBinding intra_wavefront_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}
    };
    VkDescriptorSetLayoutCreateInfo intra_wavefront_layout_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 7, .pBindings = intra_wavefront_bindings };
    vkCreateDescriptorSetLayout(ctx->device, &intra_wavefront_layout_info, NULL, &ctx->intra_wavefront_desc_layout);

    /* Descriptor Pool */
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32}
    };
    VkDescriptorPoolCreateInfo pool_info_desc = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 32,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes
    };
    vkCreateDescriptorPool(ctx->device, &pool_info_desc, NULL, &ctx->desc_pool);

    /* Push constants. 9th word (num_slices) is only read by
     * residual_predict.comp (see its SLICE BOUNDARIES comment) - and is
     * separately repurposed as deblock_filter.comp's `pass` flag
     * (0=vertical edges, 1=horizontal edges - see that shader's
     * PushConstants comment and gpu_compute_dispatch_encode()'s Stage 5);
     * 10th word (diagonal) is only read by intra_wavefront.comp (see its
     * DISPATCH SHAPE comment) - every other shader still only declares the
     * first 8 (or 9) words in its own PushConstants block, which is fine,
     * they just don't read the extra tail byte range this layout now
     * allows. */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(uint32_t) * 10
    };

    /* Pipeline Layouts */
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
        .setLayoutCount = 1
    };

    layout_info.pSetLayouts = &ctx->me_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->motion_est_layout);

    layout_info.pSetLayouts = &ctx->predict_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->predict_layout);

    layout_info.pSetLayouts = &ctx->dct_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->transform_layout);

    layout_info.pSetLayouts = &ctx->quant_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->quantize_layout);

    layout_info.pSetLayouts = &ctx->deblock_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->deblock_layout);

    layout_info.pSetLayouts = &ctx->entropy_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->entropy_layout);

    layout_info.pSetLayouts = &ctx->cc_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->color_convert_layout);

    layout_info.pSetLayouts = &ctx->reconstruct_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->reconstruct_layout);

    layout_info.pSetLayouts = &ctx->intra_wavefront_desc_layout;
    vkCreatePipelineLayout(ctx->device, &layout_info, NULL, &ctx->intra_wavefront_layout);

    /* Allocate Descriptor Sets */
    VkDescriptorSetAllocateInfo alloc_set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = ctx->desc_pool,
        .descriptorSetCount = 1
    };

    alloc_set_info.pSetLayouts = &ctx->me_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->me_desc_set);

    alloc_set_info.pSetLayouts = &ctx->predict_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->predict_desc_set);

    alloc_set_info.pSetLayouts = &ctx->dct_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->dct_desc_set);

    alloc_set_info.pSetLayouts = &ctx->quant_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->quant_desc_set);

    alloc_set_info.pSetLayouts = &ctx->deblock_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->deblock_desc_set);

    alloc_set_info.pSetLayouts = &ctx->entropy_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->entropy_desc_set);

    alloc_set_info.pSetLayouts = &ctx->cc_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->cc_desc_set);

    alloc_set_info.pSetLayouts = &ctx->reconstruct_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->reconstruct_desc_set);

    alloc_set_info.pSetLayouts = &ctx->intra_wavefront_desc_layout;
    vkAllocateDescriptorSets(ctx->device, &alloc_set_info, &ctx->intra_wavefront_desc_set);

    /* Shaders & Pipelines */
    VkShaderModule me_shader = load_spirv_shader(ctx->device, "motion_estimation.comp.spv");
    if (me_shader) {
        ctx->motion_est_pipeline = create_compute_pipeline(ctx->device, me_shader, ctx->motion_est_layout);
        vkDestroyShaderModule(ctx->device, me_shader, NULL);
    }
    VkShaderModule predict_shader = load_spirv_shader(ctx->device, "residual_predict.comp.spv");
    if (predict_shader) {
        ctx->predict_pipeline = create_compute_pipeline(ctx->device, predict_shader, ctx->predict_layout);
        vkDestroyShaderModule(ctx->device, predict_shader, NULL);
    }
    VkShaderModule dct_shader = load_spirv_shader(ctx->device, "dct_transform.comp.spv");
    if (dct_shader) {
        ctx->transform_pipeline = create_compute_pipeline(ctx->device, dct_shader, ctx->transform_layout);
        vkDestroyShaderModule(ctx->device, dct_shader, NULL);
    }
    VkShaderModule quant_shader = load_spirv_shader(ctx->device, "quantize.comp.spv");
    if (quant_shader) {
        ctx->quantize_pipeline = create_compute_pipeline(ctx->device, quant_shader, ctx->quantize_layout);
        vkDestroyShaderModule(ctx->device, quant_shader, NULL);
    }
    VkShaderModule deblock_shader = load_spirv_shader(ctx->device, "deblock_filter.comp.spv");
    if (deblock_shader) {
        ctx->deblock_pipeline = create_compute_pipeline(ctx->device, deblock_shader, ctx->deblock_layout);
        vkDestroyShaderModule(ctx->device, deblock_shader, NULL);
    }
    VkShaderModule entropy_shader = load_spirv_shader(ctx->device, "entropy_encode.comp.spv");
    if (entropy_shader) {
        ctx->entropy_pipeline = create_compute_pipeline(ctx->device, entropy_shader, ctx->entropy_layout);
        vkDestroyShaderModule(ctx->device, entropy_shader, NULL);
    }
    VkShaderModule cc_shader = load_spirv_shader(ctx->device, "color_convert.comp.spv");
    if (cc_shader) {
        ctx->color_convert_pipeline = create_compute_pipeline(ctx->device, cc_shader, ctx->color_convert_layout);
        vkDestroyShaderModule(ctx->device, cc_shader, NULL);
    }
    VkShaderModule reconstruct_shader = load_spirv_shader(ctx->device, "reconstruct.comp.spv");
    if (reconstruct_shader) {
        ctx->reconstruct_pipeline = create_compute_pipeline(ctx->device, reconstruct_shader, ctx->reconstruct_layout);
        vkDestroyShaderModule(ctx->device, reconstruct_shader, NULL);
        if (!ctx->reconstruct_pipeline) {
            fprintf(stderr, "[bc250-gpu] FAILED to create reconstruct_pipeline (shader loaded but pipeline creation failed)\n");
        }
    } else {
        fprintf(stderr, "[bc250-gpu] FAILED to load reconstruct.comp.spv shader module\n");
    }
    VkShaderModule intra_wavefront_shader = load_spirv_shader(ctx->device, "intra_wavefront.comp.spv");
    if (intra_wavefront_shader) {
        ctx->intra_wavefront_pipeline = create_compute_pipeline(ctx->device, intra_wavefront_shader, ctx->intra_wavefront_layout);
        vkDestroyShaderModule(ctx->device, intra_wavefront_shader, NULL);
        if (!ctx->intra_wavefront_pipeline) {
            fprintf(stderr, "[bc250-gpu] FAILED to create intra_wavefront_pipeline (shader loaded but pipeline creation failed)\n");
        }
    } else {
        fprintf(stderr, "[bc250-gpu] FAILED to load intra_wavefront.comp.spv shader module\n");
    }

    /* Allocate device buffers for 4K maximum resolution */
    allocate_encoding_buffers(ctx, 3840, 2160);

    return 0;
}

void bc250_gpu_destroy(bc250_gpu_context_t *ctx) {
    if (!ctx->device) return;

    vkDeviceWaitIdle(ctx->device);

    if (ctx->motion_est_pipeline) vkDestroyPipeline(ctx->device, ctx->motion_est_pipeline, NULL);
    if (ctx->predict_pipeline) vkDestroyPipeline(ctx->device, ctx->predict_pipeline, NULL);
    if (ctx->transform_pipeline) vkDestroyPipeline(ctx->device, ctx->transform_pipeline, NULL);
    if (ctx->quantize_pipeline) vkDestroyPipeline(ctx->device, ctx->quantize_pipeline, NULL);
    if (ctx->deblock_pipeline) vkDestroyPipeline(ctx->device, ctx->deblock_pipeline, NULL);
    if (ctx->entropy_pipeline) vkDestroyPipeline(ctx->device, ctx->entropy_pipeline, NULL);
    if (ctx->color_convert_pipeline) vkDestroyPipeline(ctx->device, ctx->color_convert_pipeline, NULL);
    if (ctx->reconstruct_pipeline) vkDestroyPipeline(ctx->device, ctx->reconstruct_pipeline, NULL);
    if (ctx->intra_wavefront_pipeline) vkDestroyPipeline(ctx->device, ctx->intra_wavefront_pipeline, NULL);

    if (ctx->motion_est_layout) vkDestroyPipelineLayout(ctx->device, ctx->motion_est_layout, NULL);
    if (ctx->predict_layout) vkDestroyPipelineLayout(ctx->device, ctx->predict_layout, NULL);
    if (ctx->transform_layout) vkDestroyPipelineLayout(ctx->device, ctx->transform_layout, NULL);
    if (ctx->quantize_layout) vkDestroyPipelineLayout(ctx->device, ctx->quantize_layout, NULL);
    if (ctx->deblock_layout) vkDestroyPipelineLayout(ctx->device, ctx->deblock_layout, NULL);
    if (ctx->entropy_layout) vkDestroyPipelineLayout(ctx->device, ctx->entropy_layout, NULL);
    if (ctx->color_convert_layout) vkDestroyPipelineLayout(ctx->device, ctx->color_convert_layout, NULL);
    if (ctx->reconstruct_layout) vkDestroyPipelineLayout(ctx->device, ctx->reconstruct_layout, NULL);
    if (ctx->intra_wavefront_layout) vkDestroyPipelineLayout(ctx->device, ctx->intra_wavefront_layout, NULL);

    if (ctx->me_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->me_desc_layout, NULL);
    if (ctx->predict_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->predict_desc_layout, NULL);
    if (ctx->dct_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->dct_desc_layout, NULL);
    if (ctx->quant_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->quant_desc_layout, NULL);
    if (ctx->deblock_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->deblock_desc_layout, NULL);
    if (ctx->entropy_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->entropy_desc_layout, NULL);
    if (ctx->cc_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->cc_desc_layout, NULL);
    if (ctx->reconstruct_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->reconstruct_desc_layout, NULL);
    if (ctx->intra_wavefront_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->intra_wavefront_desc_layout, NULL);

    if (ctx->desc_pool) vkDestroyDescriptorPool(ctx->device, ctx->desc_pool, NULL);

    if (ctx->mv_buffer) { vkDestroyBuffer(ctx->device, ctx->mv_buffer, NULL); vkFreeMemory(ctx->device, ctx->mv_memory, NULL); }
    if (ctx->residual_buffer) { vkDestroyBuffer(ctx->device, ctx->residual_buffer, NULL); vkFreeMemory(ctx->device, ctx->residual_memory, NULL); }
    if (ctx->pred_buffer) { vkDestroyBuffer(ctx->device, ctx->pred_buffer, NULL); vkFreeMemory(ctx->device, ctx->pred_memory, NULL); }
    if (ctx->coeff_buffer) { vkDestroyBuffer(ctx->device, ctx->coeff_buffer, NULL); vkFreeMemory(ctx->device, ctx->coeff_memory, NULL); }
    if (ctx->quant_levels_buffer) { vkDestroyBuffer(ctx->device, ctx->quant_levels_buffer, NULL); vkFreeMemory(ctx->device, ctx->quant_levels_memory, NULL); }
    if (ctx->nz_count_buffer) { vkDestroyBuffer(ctx->device, ctx->nz_count_buffer, NULL); vkFreeMemory(ctx->device, ctx->nz_count_memory, NULL); }
    if (ctx->pred_mode_buffer) { vkDestroyBuffer(ctx->device, ctx->pred_mode_buffer, NULL); vkFreeMemory(ctx->device, ctx->pred_mode_memory, NULL); }
    if (ctx->entropy_buffer) { vkDestroyBuffer(ctx->device, ctx->entropy_buffer, NULL); vkFreeMemory(ctx->device, ctx->entropy_memory, NULL); }
    for (int i = 0; i < 2; i++) {
        if (ctx->staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->staging_memories[i]);
            ctx->staging_mapped[i] = NULL;
        }
        if (ctx->staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->staging_memories[i], NULL);
        }
        if (ctx->quant_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->quant_staging_memories[i]);
            ctx->quant_staging_mapped[i] = NULL;
        }
        if (ctx->quant_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->quant_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->quant_staging_memories[i], NULL);
        }
        if (ctx->coeff_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->coeff_staging_memories[i]);
            ctx->coeff_staging_mapped[i] = NULL;
        }
        if (ctx->coeff_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->coeff_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->coeff_staging_memories[i], NULL);
        }
        if (ctx->pred_mode_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->pred_mode_staging_memories[i]);
            ctx->pred_mode_staging_mapped[i] = NULL;
        }
        if (ctx->pred_mode_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->pred_mode_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->pred_mode_staging_memories[i], NULL);
        }
        if (ctx->mv_staging_mapped[i]) {
            vkUnmapMemory(ctx->device, ctx->mv_staging_memories[i]);
            ctx->mv_staging_mapped[i] = NULL;
        }
        if (ctx->mv_staging_buffers[i]) {
            vkDestroyBuffer(ctx->device, ctx->mv_staging_buffers[i], NULL);
            vkFreeMemory(ctx->device, ctx->mv_staging_memories[i], NULL);
        }
    }

    if (ctx->recon_image.y_plane) {
        gpu_compute_destroy_image(ctx, ctx->recon_image, ctx->recon_memory);
        ctx->recon_image.y_plane = VK_NULL_HANDLE;
    }

    if (ctx->timestamp_pools[0]) vkDestroyQueryPool(ctx->device, ctx->timestamp_pools[0], NULL);
    if (ctx->timestamp_pools[1]) vkDestroyQueryPool(ctx->device, ctx->timestamp_pools[1], NULL);
    if (ctx->timeline_sem) vkDestroySemaphore(ctx->device, ctx->timeline_sem, NULL);
    if (ctx->fences[0]) vkDestroyFence(ctx->device, ctx->fences[0], NULL);
    if (ctx->fences[1]) vkDestroyFence(ctx->device, ctx->fences[1], NULL);
    if (ctx->cmd_pool) vkDestroyCommandPool(ctx->device, ctx->cmd_pool, NULL);
    if (ctx->device) vkDestroyDevice(ctx->device, NULL);
    if (ctx->instance) vkDestroyInstance(ctx->instance, NULL);
}

int gpu_compute_init(gpu_context_t *ctx) {
    return bc250_gpu_init(ctx);
}

void gpu_compute_terminate(gpu_context_t *ctx) {
    bc250_gpu_destroy(ctx);
}

int gpu_compute_create_image(gpu_context_t *ctx, int width, int height, int format, gpu_image_t *image, gpu_memory_t *memory) {
    (void)format;
    image->width = width;
    image->height = height;
    /* Matches the real initialLayout used below for both y_plane and uv_plane. */
    image->current_layout = VK_IMAGE_LAYOUT_PREINITIALIZED;

    VkImageCreateInfo y_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED
    };
    VK_CHECK(vkCreateImage(ctx->device, &y_info, NULL, &image->y_plane));

    VkImageCreateInfo uv_info = y_info;
    uv_info.format = VK_FORMAT_R8G8_UNORM;
    uv_info.extent.width = width / 2;
    uv_info.extent.height = height / 2;
    VK_CHECK(vkCreateImage(ctx->device, &uv_info, NULL, &image->uv_plane));

    VkMemoryRequirements y_req, uv_req;
    vkGetImageMemoryRequirements(ctx->device, image->y_plane, &y_req);
    vkGetImageMemoryRequirements(ctx->device, image->uv_plane, &uv_req);

    VkDeviceSize align = uv_req.alignment > y_req.alignment ? uv_req.alignment : y_req.alignment;
    VkDeviceSize uv_offset = (y_req.size + align - 1) & ~(align - 1);
    VkDeviceSize total_size = uv_offset + uv_req.size;

    memory->size = total_size;

    uint32_t mem_bits = y_req.memoryTypeBits & uv_req.memoryTypeBits;
    if (mem_bits == 0) mem_bits = y_req.memoryTypeBits | uv_req.memoryTypeBits;

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = total_size,
        .memoryTypeIndex = find_memory_type(ctx->physical_device, mem_bits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };
    VK_CHECK(vkAllocateMemory(ctx->device, &alloc_info, NULL, &memory->memory));
    VK_CHECK(vkBindImageMemory(ctx->device, image->y_plane, memory->memory, 0));
    VK_CHECK(vkBindImageMemory(ctx->device, image->uv_plane, memory->memory, uv_offset));

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image->y_plane,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}
    };
    VK_CHECK(vkCreateImageView(ctx->device, &view_info, NULL, &image->y_view));

    view_info.image = image->uv_plane;
    view_info.format = VK_FORMAT_R8G8_UNORM;
    VK_CHECK(vkCreateImageView(ctx->device, &view_info, NULL, &image->uv_view));

    return 0;
}

void gpu_compute_destroy_image(gpu_context_t *ctx, gpu_image_t image, gpu_memory_t memory) {
    if (image.y_view) vkDestroyImageView(ctx->device, image.y_view, NULL);
    if (image.uv_view) vkDestroyImageView(ctx->device, image.uv_view, NULL);
    if (image.y_plane) vkDestroyImage(ctx->device, image.y_plane, NULL);
    if (image.uv_plane) vkDestroyImage(ctx->device, image.uv_plane, NULL);
    if (memory.memory) vkFreeMemory(ctx->device, memory.memory, NULL);
}

/* Test-harness instrumentation (tools/quality_test.sh): dump raw NV12 frame
 * bytes to disk when BC250_DUMP_INPUT_FRAMES=1 is set, building a
 * byte-exact ground-truth reference of what the driver actually received
 * from libva/ffmpeg for later PSNR/SSIM comparison against encoder output.
 * Called from both known upload paths — gpu_compute_upload_nv12()
 * (vaPutImage) and bc250_UnmapBuffer() in va_backend.c (the zero-copy
 * vaDeriveImage+vaMapBuffer path some ffmpeg versions use instead) — so
 * whichever path a given ffmpeg build takes, the frame gets captured.
 * Compiled in unconditionally but a no-op (single getenv check) unless the
 * env var is set, so it costs nothing in normal operation. */
void bc250_debug_dump_nv12_frame(const uint8_t *y_plane, int y_pitch,
                                  const uint8_t *uv_plane, int uv_pitch,
                                  int width, int height) {
    if (!getenv("BC250_DUMP_INPUT_FRAMES")) return;
    if (!y_plane || !uv_plane || width <= 0 || height <= 0) return;

    static int dump_frame_index = 0;
    const char *dump_dir = getenv("BC250_DUMP_DIR");
    if (!dump_dir || dump_dir[0] == '\0') dump_dir = "/tmp/bc250_dump_frames";
    char dump_path[600];
    snprintf(dump_path, sizeof(dump_path), "%s/frame_%05d.nv12", dump_dir, dump_frame_index);
    FILE *dumpf = fopen(dump_path, "wb");
    if (dumpf) {
        for (int r = 0; r < height; r++) {
            fwrite(y_plane + (size_t)r * y_pitch, 1, (size_t)width, dumpf);
        }
        for (int r = 0; r < height / 2; r++) {
            fwrite(uv_plane + (size_t)r * uv_pitch, 1, (size_t)width, dumpf);
        }
        fclose(dumpf);
    } else {
        fprintf(stderr, "[bc250-gpu] BC250_DUMP_INPUT_FRAMES: failed to open %s: %s\n", dump_path, strerror(errno));
    }
    dump_frame_index++;
}

int gpu_compute_get_nv12_layout(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory, gpu_nv12_layout_t *layout) {
    if (!ctx || !image || !memory.memory || !layout) return -1;

    /* Same math gpu_compute_upload_nv12()/gpu_compute_download_nv12() use
     * to address this image's real memory: per-plane row pitch + offset via
     * vkGetImageSubresourceLayout(), and the real inter-plane bind offset
     * via vkGetImageMemoryRequirements() + alignment (must match the bind
     * performed in gpu_compute_create_image() exactly, since that's the
     * memory layout actually being described). */
    VkImageSubresource subresource_y = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout_y;
    vkGetImageSubresourceLayout(ctx->device, image->y_plane, &subresource_y, &layout_y);

    VkMemoryRequirements y_req, uv_req;
    vkGetImageMemoryRequirements(ctx->device, image->y_plane, &y_req);
    vkGetImageMemoryRequirements(ctx->device, image->uv_plane, &uv_req);
    VkDeviceSize align = uv_req.alignment > y_req.alignment ? uv_req.alignment : y_req.alignment;
    VkDeviceSize uv_bind_offset = (y_req.size + align - 1) & ~(align - 1);

    VkImageSubresource subresource_uv = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout_uv;
    vkGetImageSubresourceLayout(ctx->device, image->uv_plane, &subresource_uv, &layout_uv);

    layout->y_pitch = (uint32_t)layout_y.rowPitch;
    layout->y_offset = (uint64_t)layout_y.offset;
    layout->uv_pitch = (uint32_t)layout_uv.rowPitch;
    layout->uv_offset = (uint64_t)(uv_bind_offset + layout_uv.offset);
    layout->total_size = (uint64_t)memory.size;
    return 0;
}

int gpu_compute_upload_nv12(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory,
                           const uint8_t *y_plane, int y_pitch,
                           const uint8_t *uv_plane, int uv_pitch,
                           int width, int height) {
    if (!ctx || !image || !memory.memory || !y_plane || !uv_plane) return -1;

    /* Test-harness instrumentation (tools/quality_test.sh): capture the
     * exact raw NV12 bytes libva handed us via the vaPutImage upload path,
     * before any GPU work touches them. See bc250_debug_dump_nv12_frame()
     * for the other upload path (zero-copy vaDeriveImage+vaMapBuffer) this
     * doesn't cover. */
    bc250_debug_dump_nv12_frame(y_plane, y_pitch, uv_plane, uv_pitch, width, height);

    VkImageSubresource subresource_y = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout_y;
    vkGetImageSubresourceLayout(ctx->device, image->y_plane, &subresource_y, &layout_y);

    VkMemoryRequirements y_req, uv_req;
    vkGetImageMemoryRequirements(ctx->device, image->y_plane, &y_req);
    vkGetImageMemoryRequirements(ctx->device, image->uv_plane, &uv_req);
    VkDeviceSize align = uv_req.alignment > y_req.alignment ? uv_req.alignment : y_req.alignment;
    VkDeviceSize uv_offset = (y_req.size + align - 1) & ~(align - 1);

    VkImageSubresource subresource_uv = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout_uv;
    vkGetImageSubresourceLayout(ctx->device, image->uv_plane, &subresource_uv, &layout_uv);

    uint8_t *mapped = NULL;
    if (vkMapMemory(ctx->device, memory.memory, 0, memory.size, 0, (void **)&mapped) != VK_SUCCESS) {
        return -1;
    }

    uint8_t *dst_y = mapped + layout_y.offset;
    for (int r = 0; r < height; r++) {
        memcpy(dst_y + (size_t)r * layout_y.rowPitch, y_plane + (size_t)r * y_pitch, width);
    }

    uint8_t *dst_uv = mapped + uv_offset + layout_uv.offset;
    for (int r = 0; r < height / 2; r++) {
        memcpy(dst_uv + (size_t)r * layout_uv.rowPitch, uv_plane + (size_t)r * uv_pitch, width);
    }

    vkUnmapMemory(ctx->device, memory.memory);
    return 0;
}

int gpu_compute_download_nv12(gpu_context_t *ctx, gpu_image_t *image, gpu_memory_t memory,
                             uint8_t *y_plane, int y_pitch,
                             uint8_t *uv_plane, int uv_pitch,
                             int width, int height) {
    if (!ctx || !image || !memory.memory || !y_plane || !uv_plane) return -1;

    VkImageSubresource subresource_y = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout_y;
    vkGetImageSubresourceLayout(ctx->device, image->y_plane, &subresource_y, &layout_y);

    VkMemoryRequirements y_req, uv_req;
    vkGetImageMemoryRequirements(ctx->device, image->y_plane, &y_req);
    vkGetImageMemoryRequirements(ctx->device, image->uv_plane, &uv_req);
    VkDeviceSize align = uv_req.alignment > y_req.alignment ? uv_req.alignment : y_req.alignment;
    VkDeviceSize uv_offset = (y_req.size + align - 1) & ~(align - 1);

    VkImageSubresource subresource_uv = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout_uv;
    vkGetImageSubresourceLayout(ctx->device, image->uv_plane, &subresource_uv, &layout_uv);

    uint8_t *mapped = NULL;
    if (vkMapMemory(ctx->device, memory.memory, 0, memory.size, 0, (void **)&mapped) != VK_SUCCESS) {
        return -1;
    }

    const uint8_t *src_y = mapped + layout_y.offset;
    for (int r = 0; r < height; r++) {
        memcpy(y_plane + (size_t)r * y_pitch, src_y + (size_t)r * layout_y.rowPitch, width);
    }

    const uint8_t *src_uv = mapped + uv_offset + layout_uv.offset;
    for (int r = 0; r < height / 2; r++) {
        memcpy(uv_plane + (size_t)r * uv_pitch, src_uv + (size_t)r * layout_uv.rowPitch, width);
    }

    vkUnmapMemory(ctx->device, memory.memory);
    return 0;
}

static void transition_image_layout(VkCommandBuffer cmd_buf, VkImage image,
                                    VkImageLayout old_layout, VkImageLayout new_layout) {
    if (!image) return;
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dst_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_PREINITIALIZED || old_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    }
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd_buf, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void insert_compute_barrier(VkCommandBuffer cmd_buf) {
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    };
    vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* Diagnostic-only (BC250_PERF_STATS=1): millisecond delta between two
 * CLOCK_MONOTONIC timespecs. Used below to isolate vkQueueSubmit() and
 * vkWaitForFences() as their own real wall-clock brackets - see the
 * "[BC250_PERF_SUBMIT]"/"[BC250_PERF_WAIT]" lines this enables. This exists
 * because the pre-existing GPU-timestamp-query instrumentation
 * (BC250_PERF_NUM_TIMESTAMPS / "[BC250_PERF_GPU]") only measures GPU
 * *execution* time between the first and last command in a submitted
 * command buffer; it cannot see time spent before the GPU starts executing
 * that command buffer at all (scheduling/dispatch latency between
 * vkQueueSubmit returning and the GPU actually beginning the work), which
 * is exactly the gap this diagnostic was added to find. */
static double bc250_diag_delta_ms(const struct timespec *t0, const struct timespec *t1) {
    return (double)(t1->tv_sec - t0->tv_sec) * 1000.0 +
           (double)(t1->tv_nsec - t0->tv_nsec) / 1e6;
}

int gpu_compute_begin_picture(gpu_context_t *ctx, gpu_image_t render_target) {
    (void)render_target;
    struct timespec w0, w1;
    if (ctx->perf_stats_enabled) clock_gettime(CLOCK_MONOTONIC, &w0);
    vkWaitForFences(ctx->device, 1, &ctx->fences[ctx->current_buf], VK_TRUE, UINT64_MAX);
    if (ctx->perf_stats_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &w1);
        fprintf(stderr, "[BC250_PERF_WAIT] site=begin_picture buf=%d wait_ms=%.3f\n",
                ctx->current_buf, bc250_diag_delta_ms(&w0, &w1));
    }
    vkResetFences(ctx->device, 1, &ctx->fences[ctx->current_buf]);

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(ctx->cmd_bufs[ctx->current_buf], &begin_info);

    return 0;
}

int gpu_compute_dispatch_encode(gpu_context_t *ctx, gpu_image_t render_target, int width, int height, int qp, int is_intra, int num_slices) {
    if (!ctx) return -1;
    if (num_slices < 1) num_slices = 1;

    /* Ensure pipeline buffers are allocated for current dimensions */
    if (ctx->staging_buffers[0] == VK_NULL_HANDLE || ctx->frame_width != (uint32_t)width || ctx->frame_height != (uint32_t)height) {
        allocate_encoding_buffers(ctx, (uint32_t)width, (uint32_t)height);
    }

    /* Ensure reconstructed frame buffer is allocated for DPB / reference */
    if (ctx->recon_image.y_plane == VK_NULL_HANDLE || ctx->recon_image.width != (uint32_t)width || ctx->recon_image.height != (uint32_t)height) {
        if (ctx->recon_image.y_plane != VK_NULL_HANDLE) {
            gpu_compute_destroy_image(ctx, ctx->recon_image, ctx->recon_memory);
        }
        gpu_compute_create_image(ctx, width, height, 0, &ctx->recon_image, &ctx->recon_memory);
        ctx->has_recon_frame = false;
    }

    VkCommandBuffer cmd_buf = ctx->cmd_bufs[ctx->current_buf];

    /* Opt-in GPU per-stage timing (BC250_PERF_STATS=1) - see the
     * BC250_PERF_NUM_TIMESTAMPS comment near the top of this file.
     * perf_buf mirrors ctx->current_buf at the moment this frame's commands
     * are being recorded into it (gpu_compute_end_picture() toggles
     * ctx->current_buf AFTER submission, so this is stable for the whole
     * function); is_intra is recorded now since gpu_compute_sync() reads it
     * back later without visibility into this call's parameters. */
    int perf_buf = ctx->current_buf;
    if (ctx->perf_stats_enabled) {
        ctx->perf_is_intra[perf_buf] = is_intra ? true : false;
        ctx->perf_result_pending[perf_buf] = true;
        vkCmdResetQueryPool(cmd_buf, ctx->timestamp_pools[perf_buf], 0, BC250_PERF_NUM_TIMESTAMPS);
        vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 0);
    }

    uint32_t width_mbs = (width + 15) / 16;
    uint32_t height_mbs = (height + 15) / 16;
    /* qp/is_intra used to be hardcoded (26, 0) here regardless of the actual
     * per-frame rate-control QP or slice type, which silently defeated rate
     * control (the AC quantizer always ran at QP 26, no matter what QP the
     * slice header declared) and always used the inter (/6) quantizer
     * rounding offset even on IDR/I-slices. Now threaded through from the
     * caller so the shader's quantize.comp actually uses the QP/frame-type
     * the encoder decided on - this also keeps the CPU-side luma/chroma DC
     * Hadamard quantization (encoder_h264.c) numerically consistent with
     * what quantize.comp did for the AC coefficients of the same frame. */
    uint32_t pc[9] = { (uint32_t)width, (uint32_t)height, width_mbs, height_mbs,
                       (uint32_t)qp, (uint32_t)(is_intra ? 1 : 0), 0, 5, (uint32_t)num_slices };

    /* Transition image layout to GENERAL for compute storage access.
     *
     * render_target is a copy of the caller's persistent gpu_image_t (e.g.
     * bc250_surface.image in va_backend.c), so render_target.current_layout
     * reflects the image's real layout at the start of this call: PREINITIALIZED
     * on the very first dispatch for this surface (VA-API uploads pixels via
     * host-mapped memory before the GPU touches them), or GENERAL on every
     * dispatch after that, since nothing ever transitions the image back out of
     * GENERAL. Using the tracked real layout here (instead of hardcoding
     * VK_IMAGE_LAYOUT_UNDEFINED) is required by the Vulkan spec: claiming
     * UNDEFINED when the real layout is GENERAL permits the implementation to
     * discard the image's prior contents. Updating render_target.current_layout
     * below only affects this local copy; the caller is responsible for
     * persisting VK_IMAGE_LAYOUT_GENERAL back onto its own stored gpu_image_t
     * (see va_backend.c's bc250_EndPicture) so the next dispatch call passes in
     * the correct real layout. */
    if (render_target.y_plane) {
        transition_image_layout(cmd_buf, render_target.y_plane, render_target.current_layout, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (render_target.uv_plane) {
        transition_image_layout(cmd_buf, render_target.uv_plane, render_target.current_layout, VK_IMAGE_LAYOUT_GENERAL);
    }
    render_target.current_layout = VK_IMAGE_LAYOUT_GENERAL;

    /* Reference image for ME: use recon frame if available, otherwise self */
    VkImageView ref_view = render_target.y_view;
    if (ctx->has_recon_frame && ctx->recon_image.y_view != VK_NULL_HANDLE) {
        ref_view = ctx->recon_image.y_view;
    }
    /* Same idea for chroma - see residual_predict.comp's referenceUV /
     * P-slice chroma motion compensation. */
    VkImageView ref_uv_view = render_target.uv_view;
    if (ctx->has_recon_frame && ctx->recon_image.uv_view != VK_NULL_HANDLE) {
        ref_uv_view = ctx->recon_image.uv_view;
    }

    /* Update image descriptors */
    if (render_target.y_view && render_target.uv_view) {
        update_storage_image_descriptor(ctx->device, ctx->me_desc_set, 0, render_target.y_view);
        update_storage_image_descriptor(ctx->device, ctx->me_desc_set, 1, ref_view);
    }
    /*
     * BUG FIX (reconciled from fix/gradient-boundary-mc-v2's independent
     * finding): deblock_desc_set binding 0 (deblock_filter.comp's
     * `frameImage`) was bound to render_target.y_view - the CURRENT INPUT
     * SURFACE's own pixels, already fully consumed by ME/residual
     * generation earlier in this same dispatch and about to be discarded -
     * instead of ctx->recon_image.y_view, the actual reconstruction buffer
     * Stage 4.5 (reconstruct.comp) / the intra-wavefront loop populates a
     * few lines below and that becomes next frame's ME reference (ref_view
     * above) and what BC250_DUMP_RECON_FRAMES reads back. Net effect: the
     * real ITU-T deblocking filter (see deblock_filter.comp's top-of-file
     * comment) ran on a dead buffer nothing downstream ever reads -
     * provably inert, since recon_image is fully finalized by Stage 4.5/the
     * wavefront loop before Stage 5 (below) even dispatches, and nothing
     * after Stage 5 copies its output anywhere. A real H.264 decoder DOES
     * deblock its reference every frame (disable_deblocking_filter_idc=0
     * here by default), so the encoder's own assumed reference silently
     * diverged from the decoder's actual one from the second frame of every
     * GOP onward - invisible on near-zero-residual (flat/static) content,
     * real and compounding wherever genuine per-frame residual energy
     * exists. frameImage (binding 0) is a plain read-write image2D with no
     * semantic dependency on which buffer backs it, so retargeting is a
     * drop-in change - the real bS/alpha-beta/tc0 algorithm itself is
     * unchanged, it just now actually reaches the reference chain.
     * ctx->recon_image.y_view is a stable handle allocated once at context
     * creation (only its CONTENTS become valid at Stage 4.5/the wavefront
     * loop below), so binding it here, before either has run this frame,
     * is safe - Vulkan descriptor updates only need a valid image view
     * handle, not populated contents, at update time. */
    if (ctx->recon_image.y_view != VK_NULL_HANDLE) {
        update_storage_image_descriptor(ctx->device, ctx->deblock_desc_set, 0, ctx->recon_image.y_view);
    }

    /* Transition recon_image to GENERAL up front, before reconstruct.comp's
     * imageStore writes into it below (Stage 4.5). Like render_target, it
     * starts VK_IMAGE_LAYOUT_PREINITIALIZED (see gpu_compute_create_image())
     * and is never host-written afterwards - only ever written by
     * reconstruct.comp's compute-shader stores, so its tracked layout is
     * updated here using the same real-old-layout pattern render_target
     * uses above. ctx owns recon_image directly (not a by-value copy), so
     * this persists correctly across dispatches. */
    if (ctx->recon_image.y_plane) {
        transition_image_layout(cmd_buf, ctx->recon_image.y_plane, ctx->recon_image.current_layout, VK_IMAGE_LAYOUT_GENERAL);
    }
    if (ctx->recon_image.uv_plane) {
        transition_image_layout(cmd_buf, ctx->recon_image.uv_plane, ctx->recon_image.current_layout, VK_IMAGE_LAYOUT_GENERAL);
    }
    ctx->recon_image.current_layout = VK_IMAGE_LAYOUT_GENERAL;

    /* Stage 1: Color Convert (Skipped: inputs in VA-API are already NV12) */

    /* Stage 2: Motion Estimation */
    if (ctx->motion_est_pipeline) {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->motion_est_pipeline);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->motion_est_layout, 0, 1, &ctx->me_desc_set, 0, NULL);
        vkCmdPushConstants(cmd_buf, ctx->motion_est_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);
    }
    if (ctx->perf_stats_enabled) {
        vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 1);
    }

    if (!is_intra) {
        /* P-slice path - UNCHANGED (whole-frame-parallel). See
         * residual_predict.comp's top-of-file comment: P-slices are always
         * inter-coded in this encoder and only depend on the PREVIOUS frame
         * (already fully reconstructed via recon_image by the time this
         * frame starts), so there is no same-frame macroblock-ordering
         * problem here - only I-slices (the `else` branch below) need
         * diagonal-wavefront dispatch. */

        /* Stage 2.5: Real intra/inter prediction + residual generation (see
         * residual_predict.comp) - consumes the real MVs Stage 2 just wrote,
         * for P-slice motion-compensated residual. */
        if (ctx->predict_pipeline && render_target.y_view && render_target.uv_view) {
            update_storage_image_descriptor(ctx->device, ctx->predict_desc_set, 0, render_target.y_view);
            update_storage_image_descriptor(ctx->device, ctx->predict_desc_set, 1, render_target.uv_view);
            update_storage_image_descriptor(ctx->device, ctx->predict_desc_set, 2, ref_view);
            update_storage_image_descriptor(ctx->device, ctx->predict_desc_set, 6, ref_uv_view);

            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->predict_pipeline);
            vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->predict_layout, 0, 1, &ctx->predict_desc_set, 0, NULL);
            vkCmdPushConstants(cmd_buf, ctx->predict_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
            insert_compute_barrier(cmd_buf);
        }
        if (ctx->perf_stats_enabled) {
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 2);
        }

        /* Stage 3: DCT */
        if (ctx->transform_pipeline) {
            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->transform_pipeline);
            vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->transform_layout, 0, 1, &ctx->dct_desc_set, 0, NULL);
            vkCmdPushConstants(cmd_buf, ctx->transform_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
            insert_compute_barrier(cmd_buf);
        }
        if (ctx->perf_stats_enabled) {
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 3);
        }

        /* Stage 4: Quantize */
        if (ctx->quantize_pipeline) {
            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->quantize_pipeline);
            vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->quantize_layout, 0, 1, &ctx->quant_desc_set, 0, NULL);
            vkCmdPushConstants(cmd_buf, ctx->quantize_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
            insert_compute_barrier(cmd_buf);
        }
        if (ctx->perf_stats_enabled) {
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 4);
        }

        /* Stage 4.5: Reconstruct (see reconstruct.comp's top-of-file comment) -
         * dequantizes+inverse-transforms this frame's real quantized residual
         * (quant_levels_buffer, plus coeff_buffer for the chroma DC Hadamard)
         * and adds it back to the retained prediction (pred_buffer, written
         * by Stage 2.5 above), writing the clipped result directly into
         * ctx->recon_image - this REPLACES the old raw vkCmdCopyImage-from-source
         * population of recon_image, so the NEXT frame's P-slice inter
         * prediction (referenceImage/referenceUV, set up via ref_view/ref_uv_view
         * above) sees real reconstructed pixels instead of source pixels. Must
         * run after Stage 4 (quantize) and Stage 2.5 (predict, for pred_buffer);
         * ordering relative to deblock/entropy below doesn't matter since it
         * only needs quantized coefficients + retained prediction. */
        if (ctx->reconstruct_pipeline && ctx->recon_image.y_view != VK_NULL_HANDLE && ctx->recon_image.uv_view != VK_NULL_HANDLE) {
            update_storage_image_descriptor(ctx->device, ctx->reconstruct_desc_set, 3, ctx->recon_image.y_view);
            update_storage_image_descriptor(ctx->device, ctx->reconstruct_desc_set, 4, ctx->recon_image.uv_view);

            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->reconstruct_pipeline);
            vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->reconstruct_layout, 0, 1, &ctx->reconstruct_desc_set, 0, NULL);
            vkCmdPushConstants(cmd_buf, ctx->reconstruct_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
            vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
            insert_compute_barrier(cmd_buf);
            ctx->has_recon_frame = true;
        }
        if (ctx->perf_stats_enabled) {
            /* Slot 6 (wavefront) is I-only; write it here as a zero-duration
             * duplicate of slot 5 so the downstream delta is well-defined on
             * P frames (see the BC250_PERF_NUM_TIMESTAMPS comment). */
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 5);
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 6);
        }
    } else {
        /* I-slice path - diagonal-wavefront intra reconstruction (see
         * intra_wavefront.comp's top-of-file comment for the full
         * rationale). REPLACES, for I-slices only, the whole-frame-parallel
         * predict->dct->quantize->reconstruct chain above: real I16x16 intra
         * prediction has a genuine same-frame spatial dependency (macroblock
         * (mbx,mby) needs macroblocks (mbx-1,mby)/(mbx,mby-1) to be truly
         * reconstructed FIRST), which a single whole-frame-parallel dispatch
         * cannot provide. Dispatched one anti-diagonal (d = mbx+mby) at a
         * time, with an explicit compute memory barrier between diagonals,
         * so every macroblock on diagonal d can safely read diagonal d-1's
         * (and earlier's) already-reconstructed neighbor pixels out of
         * ctx->recon_image (bound as intra_wavefront_desc_set's reconY/
         * reconUV, read-write - see gpu_compute.h's comment on that
         * descriptor set for why reusing recon_image here is safe). */
        if (ctx->perf_stats_enabled) {
            /* Slots 2-5 (predict/dct/quant/reconstruct) are P-only; write
             * them here as zero-duration duplicates of slot 1 so the
             * downstream delta is well-defined on I frames (see the
             * BC250_PERF_NUM_TIMESTAMPS comment). Slot 6 (wavefront) is
             * written below, after the diagonal loop completes. */
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 2);
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 3);
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 4);
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 5);
        }
        if (ctx->intra_wavefront_pipeline && render_target.y_view && render_target.uv_view &&
            ctx->recon_image.y_view != VK_NULL_HANDLE && ctx->recon_image.uv_view != VK_NULL_HANDLE) {
            update_storage_image_descriptor(ctx->device, ctx->intra_wavefront_desc_set, 0, render_target.y_view);
            update_storage_image_descriptor(ctx->device, ctx->intra_wavefront_desc_set, 1, render_target.uv_view);
            update_storage_image_descriptor(ctx->device, ctx->intra_wavefront_desc_set, 2, ctx->recon_image.y_view);
            update_storage_image_descriptor(ctx->device, ctx->intra_wavefront_desc_set, 3, ctx->recon_image.uv_view);

            vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->intra_wavefront_pipeline);
            vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->intra_wavefront_layout, 0, 1, &ctx->intra_wavefront_desc_set, 0, NULL);

            /* One dispatch per anti-diagonal d in [0, width_mbs+height_mbs-2].
             * Diagonal d's macroblocks are exactly those (mbx,mby) with
             * mbx+mby==d, 0<=mbx<width_mbs, 0<=mby<height_mbs - i.e.
             * mbx in [mbx_start, mbx_end] below. count = mbx_end-mbx_start+1
             * equals min(d+1, width_mbs, height_mbs, width_mbs+height_mbs-1-d),
             * the real number of macroblocks on that diagonal - no more, no
             * fewer. intra_wavefront.comp independently recomputes the same
             * mbx_start (and its own workgroup's mbx/mby) from pcs.diagonal +
             * pcs.width_in_mbs/height_in_mbs - see its DISPATCH SHAPE comment -
             * so nothing else needs to be threaded through push constants
             * beyond the diagonal index itself. */
            uint32_t num_diagonals = width_mbs + height_mbs - 1;
            for (uint32_t d = 0; d < num_diagonals; d++) {
                uint32_t mbx_start = (d + 1 > height_mbs) ? (d + 1 - height_mbs) : 0;
                uint32_t mbx_end = (d < width_mbs) ? d : (width_mbs - 1);
                uint32_t count = mbx_end - mbx_start + 1;

                uint32_t pcw[10] = { (uint32_t)width, (uint32_t)height, width_mbs, height_mbs,
                                      (uint32_t)qp, 1u, 0, 5, (uint32_t)num_slices, d };
                vkCmdPushConstants(cmd_buf, ctx->intra_wavefront_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcw), pcw);
                vkCmdDispatch(cmd_buf, count, 1, 1);
                insert_compute_barrier(cmd_buf);
            }
            ctx->has_recon_frame = true;
        }
        if (ctx->perf_stats_enabled) {
            vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 6);
        }
    }

    /* Stage 5: Deblock (Skipped in BC250_FAST_MODE to maximize gaming framerates) */
    const char *fm = getenv("BC250_FAST_MODE");
    int fast_mode = (fm && (strcmp(fm, "1") == 0 || strcmp(fm, "true") == 0)) ? 1 : 0;

    if (!fast_mode && ctx->deblock_pipeline) {
        /* Two whole-frame dispatches - all vertical edges, THEN (after a
         * real vkCmdPipelineBarrier via insert_compute_barrier(), not just
         * an intra-workgroup barrier()) all horizontal edges - matching
         * ITU-T H.264 8.7's required "vertical edges of the whole picture
         * before any horizontal edge" ordering. See deblock_filter.comp's
         * top-of-file comment for the full race-condition rationale: a
         * single dispatch cannot guarantee this ordering across different
         * macroblocks' independently-scheduled workgroups, since the
         * horizontal pass for one macroblock reads pixels a NEIGHBORING
         * macroblock's vertical pass may or may not have written yet.
         * pc[8] (num_slices for other stages, unused by this shader) is
         * repurposed here as pcs.pass (0=vertical, 1=horizontal) - see
         * deblock_filter.comp's PushConstants comment. */
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->deblock_pipeline);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->deblock_layout, 0, 1, &ctx->deblock_desc_set, 0, NULL);

        uint32_t pc_deblock[9];
        memcpy(pc_deblock, pc, sizeof(uint32_t) * 8);

        pc_deblock[8] = 0; /* pass 0: vertical edges */
        vkCmdPushConstants(cmd_buf, ctx->deblock_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc_deblock), pc_deblock);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);

        pc_deblock[8] = 1; /* pass 1: horizontal edges */
        vkCmdPushConstants(cmd_buf, ctx->deblock_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc_deblock), pc_deblock);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);
    }
    if (ctx->perf_stats_enabled) {
        vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 7);
    }

    /* Stage 6: Entropy */
    if (ctx->entropy_pipeline) {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->entropy_pipeline);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->entropy_layout, 0, 1, &ctx->entropy_desc_set, 0, NULL);
        vkCmdPushConstants(cmd_buf, ctx->entropy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);
    }
    if (ctx->perf_stats_enabled) {
        vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 8);
    }

    /* Copy entropy output buffer to current staging buffer for overlapped CPU readback */
    VkDeviceSize copy_size = width * height;
    if (copy_size > ctx->staging_size) copy_size = ctx->staging_size;
    VkBufferCopy copy_region = { .srcOffset = 0, .dstOffset = 0, .size = copy_size };
    vkCmdCopyBuffer(cmd_buf, ctx->entropy_buffer, ctx->staging_buffers[ctx->current_buf], 1, &copy_region);

    /* Copy the REAL post-quantization coefficient levels and pre-quantization
     * transform coefficients to their host-visible staging buffers, full size
     * each time (unlike the entropy copy above, which is deliberately capped
     * to width*height - that cap is specific to entropy_buffer's oversized
     * allocation and must not be carried forward here). */
    VkBufferCopy quant_copy_region = { .srcOffset = 0, .dstOffset = 0, .size = ctx->quant_staging_size };
    vkCmdCopyBuffer(cmd_buf, ctx->quant_levels_buffer, ctx->quant_staging_buffers[ctx->current_buf], 1, &quant_copy_region);
    VkBufferCopy coeff_copy_region = { .srcOffset = 0, .dstOffset = 0, .size = ctx->coeff_staging_size };
    vkCmdCopyBuffer(cmd_buf, ctx->coeff_buffer, ctx->coeff_staging_buffers[ctx->current_buf], 1, &coeff_copy_region);

    /* Same for the real per-MB I16x16 pred mode and motion vectors residual_predict.comp
     * / motion_estimation.comp computed this frame - see gpu_compute_get_pred_mode_staging_data()
     * / gpu_compute_get_mv_staging_data(). */
    VkBufferCopy pred_mode_copy_region = { .srcOffset = 0, .dstOffset = 0, .size = ctx->pred_mode_staging_size };
    vkCmdCopyBuffer(cmd_buf, ctx->pred_mode_buffer, ctx->pred_mode_staging_buffers[ctx->current_buf], 1, &pred_mode_copy_region);
    VkBufferCopy mv_copy_region = { .srcOffset = 0, .dstOffset = 0, .size = ctx->mv_staging_size };
    vkCmdCopyBuffer(cmd_buf, ctx->mv_buffer, ctx->mv_staging_buffers[ctx->current_buf], 1, &mv_copy_region);

    /* ctx->recon_image is now populated directly by Stage 4.5 (Reconstruct)
     * above via real dequant+IDCT+add-back+clip - see that stage's comment.
     * This replaces the old raw vkCmdCopyImage-from-render_target (source
     * pixels) that used to run here; has_recon_frame is set by Stage 4.5. */

    if (ctx->perf_stats_enabled) {
        vkCmdWriteTimestamp(cmd_buf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ctx->timestamp_pools[perf_buf], 9);
    }

    return 0;
}

int gpu_compute_end_picture(gpu_context_t *ctx) {
    struct timespec e0, e1, s0, s1;
    if (ctx->perf_stats_enabled) clock_gettime(CLOCK_MONOTONIC, &e0);
    vkEndCommandBuffer(ctx->cmd_bufs[ctx->current_buf]);
    if (ctx->perf_stats_enabled) clock_gettime(CLOCK_MONOTONIC, &e1);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &ctx->cmd_bufs[ctx->current_buf]
    };
    if (ctx->perf_stats_enabled) clock_gettime(CLOCK_MONOTONIC, &s0);
    vkQueueSubmit(ctx->compute_queue, 1, &submit_info, ctx->fences[ctx->current_buf]);
    if (ctx->perf_stats_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &s1);
        fprintf(stderr, "[BC250_PERF_SUBMIT] buf=%d end_cmdbuf_ms=%.3f queue_submit_ms=%.3f\n",
                ctx->current_buf, bc250_diag_delta_ms(&e0, &e1), bc250_diag_delta_ms(&s0, &s1));
    }

    ctx->current_buf = (ctx->current_buf + 1) % 2;
    return 0;
}

int gpu_compute_sync(gpu_context_t *ctx) {
    int prev_buf = (ctx->current_buf + 1) % 2;
    struct timespec w0, w1;
    if (ctx->perf_stats_enabled) clock_gettime(CLOCK_MONOTONIC, &w0);
    vkWaitForFences(ctx->device, 1, &ctx->fences[prev_buf], VK_TRUE, UINT64_MAX);
    if (ctx->perf_stats_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &w1);
        fprintf(stderr, "[BC250_PERF_WAIT] site=sync buf=%d wait_ms=%.3f\n",
                prev_buf, bc250_diag_delta_ms(&w0, &w1));
    }

    /* Opt-in GPU per-stage timing readback (BC250_PERF_STATS=1). Safe to
     * read now: the fence above just confirmed this exact command buffer's
     * submission (the one gpu_compute_dispatch_encode()/gpu_compute_end_picture()
     * most recently recorded into buffer `prev_buf`) has completed on the
     * GPU, so every vkCmdWriteTimestamp in it is guaranteed available -
     * VK_QUERY_RESULT_WAIT_BIT is added only as defense-in-depth. */
    if (ctx->perf_stats_enabled && ctx->timestamp_pools[prev_buf] && ctx->perf_result_pending[prev_buf]) {
        ctx->perf_result_pending[prev_buf] = false;
        uint64_t ts[BC250_PERF_NUM_TIMESTAMPS];
        VkResult qres = vkGetQueryPoolResults(ctx->device, ctx->timestamp_pools[prev_buf], 0, BC250_PERF_NUM_TIMESTAMPS,
                                               sizeof(ts), ts, sizeof(uint64_t),
                                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (qres == VK_SUCCESS) {
            double p = ctx->timestamp_period_ns;
            double me_ms        = (double)(ts[1] - ts[0]) * p / 1e6;
            double predict_ms   = (double)(ts[2] - ts[1]) * p / 1e6;
            double dct_ms       = (double)(ts[3] - ts[2]) * p / 1e6;
            double quant_ms     = (double)(ts[4] - ts[3]) * p / 1e6;
            double reconstr_ms  = (double)(ts[5] - ts[4]) * p / 1e6;
            double wavefront_ms = (double)(ts[6] - ts[5]) * p / 1e6;
            double deblock_ms   = (double)(ts[7] - ts[6]) * p / 1e6;
            double entropy_ms   = (double)(ts[8] - ts[7]) * p / 1e6;
            double copy_ms      = (double)(ts[9] - ts[8]) * p / 1e6;
            double total_ms     = (double)(ts[9] - ts[0]) * p / 1e6;
            fprintf(stderr,
                "[BC250_PERF_GPU] frame=%u type=%s total_ms=%.3f me_ms=%.3f predict_ms=%.3f dct_ms=%.3f "
                "quant_ms=%.3f reconstruct_ms=%.3f wavefront_ms=%.3f deblock_ms=%.3f entropy_ms=%.3f copy_ms=%.3f\n",
                ctx->perf_frame_counter, ctx->perf_is_intra[prev_buf] ? "I" : "P",
                total_ms, me_ms, predict_ms, dct_ms, quant_ms, reconstr_ms, wavefront_ms, deblock_ms, entropy_ms, copy_ms);
            ctx->perf_frame_counter++;
        }
    }

    return 0;
}

int gpu_compute_get_staging_data(gpu_context_t *ctx, void **data, size_t *size) {
    if (!ctx || !data || !size) return -1;
    int prev_buf = (ctx->current_buf + 1) % 2;
    *size = ctx->staging_size;
    *data = ctx->staging_mapped[prev_buf];
    return (*data != NULL) ? 0 : -1;
}

int gpu_compute_release_staging_data(gpu_context_t *ctx) {
    (void)ctx;
    /* Persistently mapped: zero syscall overhead */
    return 0;
}

int gpu_compute_get_quant_staging_data(gpu_context_t *ctx, void **data, size_t *size) {
    if (!ctx || !data || !size) return -1;
    int prev_buf = (ctx->current_buf + 1) % 2;
    *size = ctx->quant_staging_size;
    *data = ctx->quant_staging_mapped[prev_buf];
    return (*data != NULL) ? 0 : -1;
}

int gpu_compute_get_coeff_staging_data(gpu_context_t *ctx, void **data, size_t *size) {
    if (!ctx || !data || !size) return -1;
    int prev_buf = (ctx->current_buf + 1) % 2;
    *size = ctx->coeff_staging_size;
    *data = ctx->coeff_staging_mapped[prev_buf];
    return (*data != NULL) ? 0 : -1;
}

int gpu_compute_get_pred_mode_staging_data(gpu_context_t *ctx, void **data, size_t *size) {
    if (!ctx || !data || !size) return -1;
    int prev_buf = (ctx->current_buf + 1) % 2;
    *size = ctx->pred_mode_staging_size;
    *data = ctx->pred_mode_staging_mapped[prev_buf];
    return (*data != NULL) ? 0 : -1;
}

int gpu_compute_get_mv_staging_data(gpu_context_t *ctx, void **data, size_t *size) {
    if (!ctx || !data || !size) return -1;
    int prev_buf = (ctx->current_buf + 1) % 2;
    *size = ctx->mv_staging_size;
    *data = ctx->mv_staging_mapped[prev_buf];
    return (*data != NULL) ? 0 : -1;
}

/* Opt-in debug instrumentation, originally added for Part A verification
 * (reconstruction pipeline) and kept as a permanent low-risk diagnostic -
 * dumps ctx->recon_image's current contents as raw NV12 bytes,
 * same file-naming convention as bc250_debug_dump_nv12_frame(), gated by
 * BC250_DUMP_RECON_FRAMES=1 (BC250_DUMP_DIR for the directory, same as that
 * function). Call after gpu_compute_sync() so the frame's GPU writes are
 * guaranteed visible on the host. */
void gpu_compute_debug_dump_recon(gpu_context_t *ctx, int width, int height) {
    if (!getenv("BC250_DUMP_RECON_FRAMES")) return;
    if (!ctx || ctx->recon_image.y_plane == VK_NULL_HANDLE || width <= 0 || height <= 0) return;

    static int dump_frame_index = 0;
    const char *dump_dir = getenv("BC250_DUMP_DIR");
    if (!dump_dir || dump_dir[0] == '\0') dump_dir = "/tmp/bc250_dump_frames";

    size_t y_size = (size_t)width * height;
    size_t uv_size = (size_t)width * (height / 2);
    uint8_t *y_buf = malloc(y_size);
    uint8_t *uv_buf = malloc(uv_size);
    if (!y_buf || !uv_buf) { free(y_buf); free(uv_buf); return; }

    if (gpu_compute_download_nv12(ctx, &ctx->recon_image, ctx->recon_memory,
                                   y_buf, width, uv_buf, width, width, height) == 0) {
        char dump_path[600];
        snprintf(dump_path, sizeof(dump_path), "%s/recon_%05d.nv12", dump_dir, dump_frame_index);
        FILE *dumpf = fopen(dump_path, "wb");
        if (dumpf) {
            fwrite(y_buf, 1, y_size, dumpf);
            fwrite(uv_buf, 1, uv_size, dumpf);
            fclose(dumpf);
        }
    }
    free(y_buf);
    free(uv_buf);
    dump_frame_index++;
}
