/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: MIT
 *
 * gpu_compute.c - Vulkan compute backend for AMD BC-250 encoding
 */
#include "gpu_compute.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define BC250_DEVICE_ID 0x13FE
#define AMD_VENDOR_ID   0x1002

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

    create_buffer_with_memory(ctx, quant_levels_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->quant_staging_buffers[0], &ctx->quant_staging_memories[0]);
    create_buffer_with_memory(ctx, quant_levels_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->quant_staging_buffers[1], &ctx->quant_staging_memories[1]);
    create_buffer_with_memory(ctx, coeff_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->coeff_staging_buffers[0], &ctx->coeff_staging_memories[0]);
    create_buffer_with_memory(ctx, coeff_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->coeff_staging_buffers[1], &ctx->coeff_staging_memories[1]);
    create_buffer_with_memory(ctx, pred_mode_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->pred_mode_staging_buffers[0], &ctx->pred_mode_staging_memories[0]);
    create_buffer_with_memory(ctx, pred_mode_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->pred_mode_staging_buffers[1], &ctx->pred_mode_staging_memories[1]);
    create_buffer_with_memory(ctx, mv_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->mv_staging_buffers[0], &ctx->mv_staging_memories[0]);
    create_buffer_with_memory(ctx, mv_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ctx->mv_staging_buffers[1], &ctx->mv_staging_memories[1]);

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
     * residual_predict.comp (see its SLICE BOUNDARIES comment) - every other
     * shader still only declares the first 8 words in its own PushConstants
     * block, which is fine, they just don't read the extra tail byte range
     * this layout now allows. */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(uint32_t) * 9
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

    if (ctx->motion_est_layout) vkDestroyPipelineLayout(ctx->device, ctx->motion_est_layout, NULL);
    if (ctx->predict_layout) vkDestroyPipelineLayout(ctx->device, ctx->predict_layout, NULL);
    if (ctx->transform_layout) vkDestroyPipelineLayout(ctx->device, ctx->transform_layout, NULL);
    if (ctx->quantize_layout) vkDestroyPipelineLayout(ctx->device, ctx->quantize_layout, NULL);
    if (ctx->deblock_layout) vkDestroyPipelineLayout(ctx->device, ctx->deblock_layout, NULL);
    if (ctx->entropy_layout) vkDestroyPipelineLayout(ctx->device, ctx->entropy_layout, NULL);
    if (ctx->color_convert_layout) vkDestroyPipelineLayout(ctx->device, ctx->color_convert_layout, NULL);
    if (ctx->reconstruct_layout) vkDestroyPipelineLayout(ctx->device, ctx->reconstruct_layout, NULL);

    if (ctx->me_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->me_desc_layout, NULL);
    if (ctx->predict_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->predict_desc_layout, NULL);
    if (ctx->dct_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->dct_desc_layout, NULL);
    if (ctx->quant_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->quant_desc_layout, NULL);
    if (ctx->deblock_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->deblock_desc_layout, NULL);
    if (ctx->entropy_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->entropy_desc_layout, NULL);
    if (ctx->cc_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->cc_desc_layout, NULL);
    if (ctx->reconstruct_desc_layout) vkDestroyDescriptorSetLayout(ctx->device, ctx->reconstruct_desc_layout, NULL);

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

int gpu_compute_begin_picture(gpu_context_t *ctx, gpu_image_t render_target) {
    (void)render_target;
    vkWaitForFences(ctx->device, 1, &ctx->fences[ctx->current_buf], VK_TRUE, UINT64_MAX);
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
        update_storage_image_descriptor(ctx->device, ctx->deblock_desc_set, 0, render_target.y_view);
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

    /* Stage 2.5: Real intra/inter prediction + residual generation (see
     * residual_predict.comp) - consumes the real MVs Stage 2 just wrote, for
     * P-slice motion-compensated residual. */
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

    /* Stage 3: DCT */
    if (ctx->transform_pipeline) {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->transform_pipeline);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->transform_layout, 0, 1, &ctx->dct_desc_set, 0, NULL);
        vkCmdPushConstants(cmd_buf, ctx->transform_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);
    }

    /* Stage 4: Quantize */
    if (ctx->quantize_pipeline) {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->quantize_pipeline);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->quantize_layout, 0, 1, &ctx->quant_desc_set, 0, NULL);
        vkCmdPushConstants(cmd_buf, ctx->quantize_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);
    }

    /* Stage 4.5: Reconstruct (see reconstruct.comp's top-of-file comment) -
     * dequantizes+inverse-transforms this frame's real quantized residual
     * (quant_levels_buffer, plus coeff_buffer for the I16x16/chroma DC
     * Hadamard) and adds it back to the retained prediction (pred_buffer,
     * written by Stage 2.5 above), writing the clipped result directly into
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

    /* Stage 5: Deblock (Skipped in BC250_FAST_MODE to maximize gaming framerates) */
    const char *fm = getenv("BC250_FAST_MODE");
    int fast_mode = (fm && (strcmp(fm, "1") == 0 || strcmp(fm, "true") == 0)) ? 1 : 0;

    if (!fast_mode && ctx->deblock_pipeline) {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->deblock_pipeline);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->deblock_layout, 0, 1, &ctx->deblock_desc_set, 0, NULL);
        vkCmdPushConstants(cmd_buf, ctx->deblock_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);
    }

    /* Stage 6: Entropy */
    if (ctx->entropy_pipeline) {
        vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->entropy_pipeline);
        vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->entropy_layout, 0, 1, &ctx->entropy_desc_set, 0, NULL);
        vkCmdPushConstants(cmd_buf, ctx->entropy_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cmd_buf, width_mbs, height_mbs, 1);
        insert_compute_barrier(cmd_buf);
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

    return 0;
}

int gpu_compute_end_picture(gpu_context_t *ctx) {
    vkEndCommandBuffer(ctx->cmd_bufs[ctx->current_buf]);

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &ctx->cmd_bufs[ctx->current_buf]
    };
    vkQueueSubmit(ctx->compute_queue, 1, &submit_info, ctx->fences[ctx->current_buf]);

    ctx->current_buf = (ctx->current_buf + 1) % 2;
    return 0;
}

int gpu_compute_sync(gpu_context_t *ctx) {
    int prev_buf = (ctx->current_buf + 1) % 2;
    vkWaitForFences(ctx->device, 1, &ctx->fences[prev_buf], VK_TRUE, UINT64_MAX);
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

/* TEMPORARY debug instrumentation for Part A verification (reconstruction
 * pipeline) - dumps ctx->recon_image's current contents as raw NV12 bytes,
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
