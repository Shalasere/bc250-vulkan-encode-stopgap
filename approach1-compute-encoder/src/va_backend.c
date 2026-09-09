/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * va_backend.c - Complete VA-API Backend Driver Implementation for AMD BC-250
 */
#include "va_backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define BC250_MAX_WIDTH 3840
#define BC250_MAX_HEIGHT 2160

static bc250_driver_data* get_driver_data(VADriverContextP ctx) {
    return (bc250_driver_data*)ctx->pDriverData;
}

VAStatus bc250_QueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list, int *num_profiles) {
    if (!ctx || !num_profiles) return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!profile_list) {
        *num_profiles = 4;
        return VA_STATUS_SUCCESS;
    }

    int i = 0;
    profile_list[i++] = VAProfileH264ConstrainedBaseline;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    profile_list[i++] = VAProfileH264Baseline;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    profile_list[i++] = VAProfileH264Main;
    profile_list[i++] = VAProfileH264High;

    *num_profiles = i;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_QueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint *entrypoint_list, int *num_entrypoints) {
    if (!ctx || !num_entrypoints) return VA_STATUS_ERROR_INVALID_PARAMETER;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    int is_supported_profile = (profile == VAProfileH264ConstrainedBaseline ||
                                profile == VAProfileH264Baseline ||
                                profile == VAProfileH264Main ||
                                profile == VAProfileH264High);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    if (!is_supported_profile) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (!entrypoint_list) {
        *num_entrypoints = 1;
        return VA_STATUS_SUCCESS;
    }

    entrypoint_list[0] = VAEntrypointEncSlice;
    *num_entrypoints = 1;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_GetConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib *attrib_list, int num_attribs) {
    (void)ctx; (void)profile; (void)entrypoint;
    if (!attrib_list) return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
            case VAConfigAttribRTFormat:
                attrib_list[i].value = VA_RT_FORMAT_YUV420;
                break;
            case VAConfigAttribRateControl:
                attrib_list[i].value = VA_RC_CBR | VA_RC_VBR | VA_RC_CQP;
                break;
            case VAConfigAttribEncPackedHeaders:
                /* bc250_RenderPicture() below treats VAEncPackedHeaderParameterBufferType
                 * and VAEncPackedHeaderDataBufferType as a silent no-op - whatever SPS/PPS/
                 * slice-header/SEI bytes a caller (e.g. ffmpeg's h264_vaapi) hands us via
                 * those buffers are discarded, and encoder_h264.c always emits its own
                 * AUD/SPS/PPS/slice headers instead. Previously this advertised SEQUENCE |
                 * PICTURE | SLICE (0x7), which told libva callers we would splice in their
                 * own header bytes verbatim. That's not true, and it isn't just cosmetic:
                 * ffmpeg only builds AVCodecContext.extradata from its self-authored SPS/PPS
                 * when VA_ENC_PACKED_HEADER_SEQUENCE is (falsely) reported present, so an
                 * MP4/avcC mux could end up with an extradata SPS/PPS that disagrees with
                 * the in-band one this driver actually writes. Advertise NONE until/unless
                 * RenderPicture is changed to genuinely consume these buffers.
                 */
                attrib_list[i].value = VA_ENC_PACKED_HEADER_NONE;
                break;
            case VAConfigAttribEncMaxRefFrames:
                attrib_list[i].value = 1;
                break;
            case VAConfigAttribMaxPictureWidth:
                attrib_list[i].value = BC250_MAX_WIDTH;
                break;
            case VAConfigAttribMaxPictureHeight:
                attrib_list[i].value = BC250_MAX_HEIGHT;
                break;
            default:
                attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
                break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_CreateConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib *attrib_list, int num_attribs, VAConfigID *config_id) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !config_id) return VA_STATUS_ERROR_INVALID_PARAMETER;

    /* data->configs[i].attribs is a fixed-size array (see bc250_config in
     * va_backend.h). Without this check a caller-supplied num_attribs larger
     * than that capacity would memcpy past the end of the attribs array and
     * corrupt adjacent bc250_config fields / neighboring array entries. */
    if (num_attribs < 0 || (size_t)num_attribs > (sizeof(((bc250_config *)0)->attribs) / sizeof(VAConfigAttrib))) {
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    for (int i = 0; i < MAX_CONFIGS; i++) {
        if (!data->configs[i].allocated) {
            data->configs[i].allocated = 1;
            data->configs[i].profile = profile;
            data->configs[i].entrypoint = entrypoint;
            data->configs[i].num_attribs = num_attribs;
            if (num_attribs > 0 && attrib_list) {
                memcpy(data->configs[i].attribs, attrib_list, num_attribs * sizeof(VAConfigAttrib));
            }
            *config_id = i;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

VAStatus bc250_DestroyConfig(VADriverContextP ctx, VAConfigID config_id) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!VALID_ID(config_id, MAX_CONFIGS) || !data->configs[config_id].allocated) return VA_STATUS_ERROR_INVALID_CONFIG;
    data->configs[config_id].allocated = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_QueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id, VAProfile *profile, VAEntrypoint *entrypoint, VAConfigAttrib *attrib_list, int *num_attribs) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!VALID_ID(config_id, MAX_CONFIGS) || !data->configs[config_id].allocated) return VA_STATUS_ERROR_INVALID_CONFIG;

    if (profile) *profile = data->configs[config_id].profile;
    if (entrypoint) *entrypoint = data->configs[config_id].entrypoint;
    if (num_attribs) *num_attribs = data->configs[config_id].num_attribs;
    if (attrib_list && data->configs[config_id].num_attribs > 0) {
        memcpy(attrib_list, data->configs[config_id].attribs, data->configs[config_id].num_attribs * sizeof(VAConfigAttrib));
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib *attrib_list, unsigned int *num_attribs) {
    (void)ctx; (void)config;
    if (!num_attribs) return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!attrib_list) {
        *num_attribs = 3;
        return VA_STATUS_SUCCESS;
    }

    int i = 0;
    attrib_list[i].type = VASurfaceAttribPixelFormat;
    attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[i].value.type = VAGenericValueTypeInteger;
    attrib_list[i].value.value.i = VA_FOURCC_NV12;
    i++;

    attrib_list[i].type = VASurfaceAttribMaxWidth;
    attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[i].value.type = VAGenericValueTypeInteger;
    attrib_list[i].value.value.i = BC250_MAX_WIDTH;
    i++;

    attrib_list[i].type = VASurfaceAttribMaxHeight;
    attrib_list[i].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[i].value.type = VAGenericValueTypeInteger;
    attrib_list[i].value.value.i = BC250_MAX_HEIGHT;
    i++;

    *num_attribs = i;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_CreateSurfaces(VADriverContextP ctx, int width, int height, int format, int num_surfaces, VASurfaceID *surfaces) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !surfaces) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (width > data->max_width || height > data->max_height) return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;

    int allocated = 0;
    for (int i = 0; i < MAX_SURFACES && allocated < num_surfaces; i++) {
        if (!data->surfaces[i].allocated) {
            bc250_surface *surf = &data->surfaces[i];
            memset(surf, 0, sizeof(*surf));

            /* gpu_compute_create_image() can fail (e.g. Vulkan allocation
             * failure) and returns nonzero in that case (see VK_CHECK in
             * gpu_compute.c). The return value was previously discarded, so
             * a failed surface was still marked allocated and handed back
             * to the caller as a "valid" surface backed by a
             * partially-initialized/invalid gpu_image_t - any later use
             * (encode, GetImage/PutImage, DestroySurfaces) would operate on
             * garbage Vulkan handles. Skip publishing this surface on
             * failure instead.
             *
             * Stop the whole loop here rather than `continue`-ing to the
             * next slot: confirmed on-hardware (gdb) that under real GPU
             * contention (a live desktop compositor also driving this
             * GPU), once one vkBindImageMemory call has already failed
             * with VK_ERROR_UNKNOWN, an immediate retry on the very next
             * surface segfaults *inside* radv_BindImageMemory2() itself -
             * i.e. the failure leaves RADV's own allocator state for this
             * memory type in a condition that a same-loop-iteration retry
             * cannot safely probe further. Bailing out immediately and
             * surfacing VA_STATUS_ERROR_MAX_NUM_EXCEEDED to the caller
             * (via the `allocated < num_surfaces` check below) is the
             * failure this driver can actually recover from; hammering
             * the allocator again cannot be made safe from here. */
            if (gpu_compute_create_image(&data->gpu, width, height, format, &surf->image, &surf->memory) != 0) {
                memset(surf, 0, sizeof(*surf));
                break;
            }

            surf->allocated = 1;
            surf->width = width;
            surf->height = height;
            surf->format = format;
            surf->ref_count = 1;

            surfaces[allocated++] = i;
        }
    }

    if (allocated < num_surfaces) {
        bc250_DestroySurfaces(ctx, surfaces, allocated);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_CreateSurfaces2(VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height,
                              VASurfaceID *surfaces, unsigned int num_surfaces,
                              VASurfaceAttrib *attrib_list, unsigned int num_attribs) {
    (void)attrib_list; (void)num_attribs;
    return bc250_CreateSurfaces(ctx, width, height, format, num_surfaces, surfaces);
}

/* Drop one reference on surface `id` (data->surfaces[id] must already be
 * known valid/allocated by the caller). The surface's Vulkan image/memory
 * are only actually freed once ref_count reaches zero - i.e. once both the
 * original vaCreateSurfaces() reference AND every vaDeriveImage()-derived
 * image's reference have been released. This is the single place that
 * performs the real Vulkan teardown, shared by bc250_DestroySurfaces()
 * (releasing the app's own reference) and bc250_DestroyBuffer()
 * (releasing a derived image's reference on the surface it aliases). */
static void bc250_surface_unref(bc250_driver_data *data, VASurfaceID id) {
    bc250_surface *surf = &data->surfaces[id];
    if (surf->ref_count > 0) {
        surf->ref_count--;
    }
    if (surf->ref_count <= 0) {
        gpu_compute_destroy_image(&data->gpu, surf->image, surf->memory);
        surf->allocated = 0;
        surf->pending_destroy = 0;
    }
}

VAStatus bc250_DestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list, int num_surfaces) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !surface_list) return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (int i = 0; i < num_surfaces; i++) {
        VASurfaceID id = surface_list[i];
        if (!VALID_ID(id, MAX_SURFACES) || !data->surfaces[id].allocated) continue;

        bc250_surface *surf = &data->surfaces[id];
        /* Idempotent: an application that (incorrectly) destroys the same
         * surface twice must not decrement ref_count twice for a single
         * app-held reference - only the first vaDestroySurfaces() call on
         * a given surface releases that reference. */
        if (surf->pending_destroy) continue;

        /* From here on this VASurfaceID is invalid for the application to
         * use in any other VA call (vaBeginPicture, vaDeriveImage,
         * vaGetImage/vaPutImage, vaSyncSurface, ...), regardless of
         * whether the underlying Vulkan resources are freed immediately
         * below or kept alive a while longer for an outstanding derived
         * image - see bc250_surface.pending_destroy in va_backend.h. */
        surf->pending_destroy = 1;
        bc250_surface_unref(data, id);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_CreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width, int picture_height, int flag, VASurfaceID *render_targets, int num_render_targets, VAContextID *context) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(config_id, MAX_CONFIGS) || !data->configs[config_id].allocated || !context) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }

    for (int i = 0; i < MAX_CONTEXTS; i++) {
        if (!data->contexts[i].allocated) {
            bc250_context *c = &data->contexts[i];
            memset(c, 0, sizeof(*c));
            c->allocated = 1;
            c->config_id = config_id;
            c->width = picture_width;
            c->height = picture_height;
            c->flag = flag;
            c->num_render_targets = num_render_targets;
            c->current_render_target = VA_INVALID_SURFACE;
            c->coded_buf_id = VA_INVALID_ID;

            if (num_render_targets > 0 && render_targets) {
                c->render_targets = malloc(num_render_targets * sizeof(VASurfaceID));
                memcpy(c->render_targets, render_targets, num_render_targets * sizeof(VASurfaceID));
            }

            VAProfile prof = data->configs[config_id].profile;
            VAEntrypoint entry = data->configs[config_id].entrypoint;

            if (entry == VAEntrypointEncSlice) {
                if (prof == VAProfileHEVCMain) {
                    c->hevc_enc = hevc_encoder_create(&data->gpu, picture_width, picture_height, 30, 4000000);
                } else {
                    c->h264_enc = h264_encoder_create(&data->gpu, picture_width, picture_height, 30, 4000000, prof);
                }
            } else if (entry == VAEntrypointVLD) {
                c->h264_dec = h264_decoder_create(&data->gpu, picture_width, picture_height);
            }

            *context = i;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

VAStatus bc250_DestroyContext(VADriverContextP ctx, VAContextID context) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(context, MAX_CONTEXTS) || !data->contexts[context].allocated) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    bc250_context *c = &data->contexts[context];
    if (c->h264_enc) {
        h264_encoder_destroy(c->h264_enc);
        c->h264_enc = NULL;
    }
    if (c->hevc_enc) {
        hevc_encoder_destroy(c->hevc_enc);
        c->hevc_enc = NULL;
    }
    if (c->h264_dec) {
        h264_decoder_destroy(c->h264_dec);
        c->h264_dec = NULL;
    }
    if (c->render_targets) {
        free(c->render_targets);
        c->render_targets = NULL;
    }
    c->allocated = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_CreateBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned int size, unsigned int num_elements, void *data_ptr, VABufferID *buf_id) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !buf_id) return VA_STATUS_ERROR_INVALID_PARAMETER;
    (void)context;

    for (int i = 0; i < MAX_BUFFERS; i++) {
        if (!data->buffers[i].allocated) {
            bc250_buffer *b = &data->buffers[i];
            b->type = type;
            b->size = size;
            b->num_elements = num_elements;
            b->mapped = 0;
            b->is_derived = 0;
            b->gpu_mem = VK_NULL_HANDLE;
            b->derived_surface = VA_INVALID_SURFACE;

            size_t total_alloc = (size_t)size * num_elements;
            if (type == VAEncCodedBufferType) {
                total_alloc += sizeof(VACodedBufferSegment);
            }

            b->data = calloc(1, total_alloc > 0 ? total_alloc : 1);
            if (!b->data) {
                /* Leave the slot free (allocated stays 0) so this failure
                 * doesn't permanently strand a buffer slot with no backing
                 * memory - a caller that ignored this error and later called
                 * vaMapBuffer/vaDestroyBuffer on buf_id would otherwise
                 * dereference/free a NULL data pointer or operate on a slot
                 * that looks valid but never had memory. */
                return VA_STATUS_ERROR_ALLOCATION_FAILED;
            }
            b->allocated = 1;
            if (data_ptr) {
                memcpy(b->data, data_ptr, (size_t)size * num_elements);
            } else if (type == VAEncCodedBufferType) {
                VACodedBufferSegment *seg = (VACodedBufferSegment *)b->data;
                seg->size = 0;
                seg->bit_offset = 0;
                seg->status = 0;
                seg->reserved = 0;
                seg->buf = ((uint8_t *)b->data) + sizeof(VACodedBufferSegment);
                seg->next = NULL;
            }
            *buf_id = i;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

VAStatus bc250_BufferSetNumElements(VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(buf_id, MAX_BUFFERS) || !data->buffers[buf_id].allocated) return VA_STATUS_ERROR_INVALID_BUFFER;
    data->buffers[buf_id].num_elements = num_elements;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_MapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(buf_id, MAX_BUFFERS) || !data->buffers[buf_id].allocated || !pbuf) {
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    data->buffers[buf_id].mapped = 1;
    *pbuf = data->buffers[buf_id].data;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(buf_id, MAX_BUFFERS) || !data->buffers[buf_id].allocated) return VA_STATUS_ERROR_INVALID_BUFFER;

    /* Test-harness instrumentation (tools/quality_test.sh): a derived-image
     * buffer (see bc250_DeriveImage) is a direct mapping of GPU surface
     * memory, so a caller (e.g. ffmpeg's vaapi hwupload doing a zero-copy
     * upload) writes frame data straight into it via vaMapBuffer without
     * ever calling vaPutImage / gpu_compute_upload_nv12(). Capture the
     * frame here, at unmap time, so that upload path is covered too. Only
     * active when BC250_DUMP_INPUT_FRAMES=1 (see bc250_debug_dump_nv12_frame).
     */
    if (data->buffers[buf_id].is_derived && data->buffers[buf_id].data && getenv("BC250_DUMP_INPUT_FRAMES")) {
        for (int i = 0; i < MAX_IMAGES; i++) {
            bc250_image *img = &data->images[i];
            if (img->allocated && img->buffer_id == buf_id) {
                if (VALID_ID(img->surface_id, MAX_SURFACES) && data->surfaces[img->surface_id].allocated) {
                    bc250_surface *surf = &data->surfaces[img->surface_id];
                    const uint8_t *base = (const uint8_t *)data->buffers[buf_id].data;
                    const uint8_t *y_plane = base + img->image.offsets[0];
                    const uint8_t *uv_plane = base + img->image.offsets[1];
                    int y_pitch = img->image.pitches[0] > 0 ? (int)img->image.pitches[0] : surf->width;
                    int uv_pitch = img->image.pitches[1] > 0 ? (int)img->image.pitches[1] : surf->width;
                    bc250_debug_dump_nv12_frame(y_plane, y_pitch, uv_plane, uv_pitch, surf->width, surf->height);
                }
                break;
            }
        }
    }

    data->buffers[buf_id].mapped = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_DestroyBuffer(VADriverContextP ctx, VABufferID buffer_id) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data) return VA_STATUS_ERROR_INVALID_BUFFER;
    /* A genuinely out-of-range ID is a real caller bug - keep erroring on
     * that. An in-range ID that's simply not currently allocated (already
     * destroyed, or never allocated) is treated as a harmless no-op instead
     * of an error: observed in practice (ffmpeg's vaapi_encode.c, e.g.
     * "Failed to destroy param buffer 0x1: invalid VABufferID" on the very
     * first frame) calling vaDestroyBuffer a second time on an ID it
     * believes it owns - this driver's own CreateBuffer/RenderPicture/
     * DestroyContext never proactively frees a buffer out from under the
     * caller (checked directly: RenderPicture only reads param data,
     * DestroyContext doesn't touch the buffer table at all), so the double
     * call is on the caller's side, most likely tied to how this driver
     * advertises VA_ENC_PACKED_HEADER_NONE (see bc250_GetConfigAttributes).
     * Several real VA-API drivers (including Mesa's) treat a destroy-again
     * on an already-gone buffer as success for the same reason - the
     * resource the caller wanted gone is, in fact, gone. */
    if (!VALID_ID(buffer_id, MAX_BUFFERS)) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!data->buffers[buffer_id].allocated) return VA_STATUS_SUCCESS;
    bc250_buffer *b = &data->buffers[buffer_id];
    if (b->is_derived) {
        if (b->gpu_mem) {
            /* Unmap while the surface's VkDeviceMemory is still guaranteed
             * alive (it can't have been freed yet: this buffer's own
             * reference, taken in bc250_DeriveImage(), is still held at
             * this point and keeps the surface's ref_count above zero). */
            vkUnmapMemory(data->gpu.device, b->gpu_mem);
        }
        /* Release this derived image's reference on the surface it
         * aliases. If the application already called vaDestroySurfaces()
         * on that surface while this image was still alive, this is what
         * finally lets the surface's Vulkan resources be freed - safely,
         * now that nothing is mapping them anymore. */
        if (VALID_ID(b->derived_surface, MAX_SURFACES) && data->surfaces[b->derived_surface].allocated) {
            bc250_surface_unref(data, b->derived_surface);
        }
        b->derived_surface = VA_INVALID_SURFACE;
    } else {
        free(b->data);
    }
    b->data = NULL;
    b->allocated = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_BeginPicture(VADriverContextP ctx, VAContextID context, VASurfaceID render_target) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(context, MAX_CONTEXTS) || !data->contexts[context].allocated) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!VALID_ID(render_target, MAX_SURFACES) || !data->surfaces[render_target].allocated ||
        data->surfaces[render_target].pending_destroy) return VA_STATUS_ERROR_INVALID_SURFACE;

    bc250_context *c = &data->contexts[context];
    c->current_render_target = render_target;
    c->coded_buf_id = VA_INVALID_ID;

    c->h264_state.has_seq = 0;
    c->h264_state.has_pic = 0;
    c->h264_state.has_slice = 0;

    return VA_STATUS_SUCCESS;
}

VAStatus bc250_RenderPicture(VADriverContextP ctx, VAContextID context, VABufferID *buffers, int num_buffers) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(context, MAX_CONTEXTS) || !data->contexts[context].allocated || !buffers) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    bc250_context *c = &data->contexts[context];

    for (int i = 0; i < num_buffers; i++) {
        VABufferID buf_id = buffers[i];
        if (!VALID_ID(buf_id, MAX_BUFFERS) || !data->buffers[buf_id].allocated) continue;

        bc250_buffer *b = &data->buffers[buf_id];
        switch (b->type) {
            case VAEncSequenceParameterBufferType:
                if (b->size >= sizeof(VAEncSequenceParameterBufferH264)) {
                    memcpy(&c->h264_state.seq_param, b->data, sizeof(VAEncSequenceParameterBufferH264));
                    c->h264_state.has_seq = 1;
                    if (c->h264_enc) {
                        VAEncSequenceParameterBufferH264 *seq = &c->h264_state.seq_param;
                        if (seq->intra_period > 0) {
                            h264_encoder_set_gop_size(c->h264_enc, seq->intra_period);
                        }
                        if (seq->bits_per_second > 0) {
                            h264_encoder_set_bitrate(c->h264_enc, seq->bits_per_second);
                        }
                    }
                }
                break;
            case VAEncPictureParameterBufferType:
                if (b->size >= sizeof(VAEncPictureParameterBufferH264)) {
                    VAEncPictureParameterBufferH264 *pic = (VAEncPictureParameterBufferH264*)b->data;
                    memcpy(&c->h264_state.pic_param, pic, sizeof(VAEncPictureParameterBufferH264));
                    c->h264_state.has_pic = 1;
                    c->coded_buf_id = pic->coded_buf;
                    if (c->h264_enc) {
                        if (pic->pic_fields.bits.idr_pic_flag) {
                            h264_encoder_force_idr(c->h264_enc);
                        }
                        if (pic->pic_init_qp > 0) {
                            h264_encoder_set_qp(c->h264_enc, pic->pic_init_qp);
                        }
                    }
                }
                break;
            case VAEncMiscParameterBufferType:
                if (b->size >= sizeof(VAEncMiscParameterBuffer)) {
                    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer*)b->data;
                    if (misc->type == VAEncMiscParameterTypeRateControl && c->h264_enc) {
                        VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl*)misc->data;
                        if (rc->bits_per_second > 0) {
                            /* docs/rate_control_audit.md section 2: ffmpeg's actual
                             * default h264_vaapi invocation (-b:v X, no -rc_mode) is
                             * VBR with target_percentage=50 and bits_per_second=2X -
                             * i.e. the real intended target is X, encoded as "50% of
                             * 2X". Previously this only ever read bits_per_second and
                             * ignored target_percentage entirely, so the driver was
                             * handed 2X and treated it as if it were the real target -
                             * a 2x error before rate_control.c even runs. Apply the
                             * percentage here, falling back to 100% when it's unset/
                             * out of range (0 or >100), matching common VA-API driver
                             * convention for an absent/invalid percentage field. */
                            unsigned int pct = rc->target_percentage;
                            if (pct == 0 || pct > 100) pct = 100;
                            uint32_t target_bps = (uint32_t)(((uint64_t)rc->bits_per_second * pct) / 100);
                            if (target_bps == 0) target_bps = rc->bits_per_second;
                            h264_encoder_set_bitrate(c->h264_enc, target_bps);

                            /* CBR-intent signal for filler/padding (see
                             * h264_encoder_set_cbr_intent's doc comment and
                             * docs/rate_control_audit.md's "no filler data"
                             * finding). This driver has no code path today
                             * that reads back the VAConfigAttribRateControl
                             * value an application chose at vaCreateConfig()
                             * time (bc250_CreateConfig stores the attrib
                             * list, but bc250_CreateContext never reads it
                             * back out - docs/rate_control_audit.md section
                             * 4 point 5, still open, out of this change's
                             * scope), so that isn't available here as a
                             * signal. What *is* already real, already read,
                             * and already board-confirmed (this buffer's own
                             * handling above, and the audit's ffmpeg -v
                             * verbose logs) is target_percentage itself:
                             * real CBR (`-rc_mode CBR`) sends exactly 100
                             * ("RC target: 100% of X bps"); ffmpeg's actual
                             * VBR default sends 50 ("RC target: 50% of
                             * 2X bps"). The VA-API spec text for this field
                             * (va.h) even says as much: "In CBR mode this
                             * value is ignored (treated as 100%)" - a raw,
                             * unclamped 100 is specifically the CBR
                             * signature, not just a coincidentally-loose
                             * VBR ceiling. Require the RAW field (not the
                             * `pct` fallback above, which also maps 0/
                             * out-of-range to 100 for the arithmetic above -
                             * an absent field is not an explicit CBR
                             * request, so it must not enable padding).
                             * Also honor rc_flags.bits.disable_bit_stuffing,
                             * the VA-API's own explicit "don't insert
                             * filler" signal, when the caller sets it. */
                            bool cbr_intent = (rc->target_percentage == 100) &&
                                              !rc->rc_flags.bits.disable_bit_stuffing;
                            h264_encoder_set_cbr_intent(c->h264_enc, cbr_intent);
                        }
                    } else if (misc->type == VAEncMiscParameterTypeFrameRate && c->h264_enc) {
                        VAEncMiscParameterFrameRate *fr = (VAEncMiscParameterFrameRate*)misc->data;
                        uint32_t num = fr->framerate & 0xFFFF;
                        uint32_t den = (fr->framerate >> 16) & 0xFFFF;
                        if (den == 0) den = 1;
                        if (num > 0) {
                            h264_encoder_set_fps(c->h264_enc, num / den);
                        }
                    }
                }
                break;
            case VAEncSliceParameterBufferType:
                if (b->size >= sizeof(VAEncSliceParameterBufferH264)) {
                    memcpy(&c->h264_state.slice_param, b->data, sizeof(VAEncSliceParameterBufferH264));
                    c->h264_state.has_slice = 1;
                }
                break;
            case VAEncPackedHeaderParameterBufferType:
            case VAEncPackedHeaderDataBufferType:
                /* Packed headers passed by Sunshine / OBS / FFmpeg - handled gracefully */
                break;
            case VAEncCodedBufferType:
                c->coded_buf_id = buf_id;
                break;
            default:
                break;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_EndPicture(VADriverContextP ctx, VAContextID context) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(context, MAX_CONTEXTS) || !data->contexts[context].allocated) return VA_STATUS_ERROR_INVALID_CONTEXT;

    bc250_context *c = &data->contexts[context];
    if (!VALID_ID(c->current_render_target, MAX_SURFACES) || !data->surfaces[c->current_render_target].allocated ||
        data->surfaces[c->current_render_target].pending_destroy) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    bc250_surface *surf = &data->surfaces[c->current_render_target];

    if ((c->h264_enc || c->hevc_enc) && VALID_ID(c->coded_buf_id, MAX_BUFFERS) && data->buffers[c->coded_buf_id].allocated) {
        bc250_buffer *coded_buf = &data->buffers[c->coded_buf_id];
        uint8_t *dest = ((uint8_t *)coded_buf->data) + sizeof(VACodedBufferSegment);
        size_t max_payload = (size_t)coded_buf->size * coded_buf->num_elements;

        int written = -1;
        if (c->h264_enc) {
            written = h264_encoder_encode_frame(c->h264_enc, &data->gpu, surf->image, dest, max_payload);
        } else if (c->hevc_enc) {
            written = hevc_encoder_encode_frame(c->hevc_enc, &data->gpu, surf->image, surf->memory, dest, max_payload);
        }

        if (written > 0) {
            VACodedBufferSegment *seg = (VACodedBufferSegment *)coded_buf->data;
            seg->size = (unsigned int)written;
            seg->bit_offset = 0;
            seg->status = 0;
            seg->reserved = 0;
            seg->buf = dest;
            seg->next = NULL;
        }
    } else {
        gpu_compute_begin_picture(&data->gpu, surf->image);
        gpu_compute_dispatch_encode(&data->gpu, surf->image, c->width, c->height, 26, 0, 1);
        gpu_compute_end_picture(&data->gpu);
    }

    /* gpu_compute_dispatch_encode() (called above, either directly or via
     * h264_encoder_encode_frame()/hevc_encoder_encode_frame()) always
     * transitions the render target's image layout to VK_IMAGE_LAYOUT_GENERAL.
     * It receives gpu_image_t by value, so that transition only affects its
     * local copy -- surf here is a real pointer into data->surfaces[], so we
     * persist the real post-encode layout onto the surface's stored image
     * state ourselves. This is what lets the next bc250_EndPicture() call for
     * this surface pass the correct real old layout (GENERAL, not a hardcoded
     * and spec-incorrect UNDEFINED) into gpu_compute_dispatch_encode(). */
    surf->image.current_layout = VK_IMAGE_LAYOUT_GENERAL;

    return VA_STATUS_SUCCESS;
}

VAStatus bc250_SyncSurface(VADriverContextP ctx, VASurfaceID render_target) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(render_target, MAX_SURFACES) || !data->surfaces[render_target].allocated ||
        data->surfaces[render_target].pending_destroy) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    gpu_compute_sync(&data->gpu);
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_QuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status) {
    (void)ctx; (void)render_target;
    if (!status) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *status = VASurfaceReady;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_QueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list, int *num_formats) {
    (void)ctx;
    if (!num_formats) return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!format_list) {
        *num_formats = 2;
        return VA_STATUS_SUCCESS;
    }

    int i = 0;
    format_list[i].fourcc = VA_FOURCC_NV12;
    format_list[i].byte_order = VA_LSB_FIRST;
    format_list[i].bits_per_pixel = 12;
    i++;

    format_list[i].fourcc = VA_FOURCC_RGBA;
    format_list[i].byte_order = VA_LSB_FIRST;
    format_list[i].bits_per_pixel = 32;
    i++;

    *num_formats = i;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_CreateImage(VADriverContextP ctx, VAImageFormat *format, int width, int height, VAImage *image) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !format || !image) return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (int i = 0; i < MAX_IMAGES; i++) {
        if (!data->images[i].allocated) {
            bc250_image *img = &data->images[i];
            memset(img, 0, sizeof(*img));
            img->allocated = 1;
            /* Not derived from any surface unless bc250_DeriveImage() below
             * says otherwise. memset() above already zeroed this field, but
             * 0 is a valid VASurfaceID (surface slot 0) - make the "no
             * surface" state unambiguous instead of relying on that. */
            img->surface_id = VA_INVALID_SURFACE;

            image->image_id = i;
            image->format = *format;
            image->width = width;
            image->height = height;

            if (format->fourcc == VA_FOURCC_NV12) {
                image->num_planes = 2;
                image->pitches[0] = width;
                image->offsets[0] = 0;
                image->data_size = width * height * 3 / 2;
                image->pitches[1] = width;
                image->offsets[1] = width * height;
            } else {
                image->num_planes = 1;
                image->pitches[0] = width * 4;
                image->offsets[0] = 0;
                image->data_size = width * height * 4;
            }

            VABufferID buf_id;
            VAStatus buf_status = bc250_CreateBuffer(ctx, 0, VAImageBufferType, image->data_size, 1, NULL, &buf_id);
            if (buf_status != VA_STATUS_SUCCESS) {
                /* Roll back: without this, buf_id is left uninitialized and
                 * gets stored as img->buffer_id / image->buf. A later
                 * vaDestroyImage() would then call bc250_DestroyBuffer() on
                 * that garbage id, which - if it happens to fall in range
                 * and alias a live, unrelated buffer slot - would corrupt or
                 * free memory that belongs to something else entirely. */
                img->allocated = 0;
                return buf_status;
            }
            image->buf = buf_id;
            img->image = *image;
            img->buffer_id = buf_id;

            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

VAStatus bc250_DestroyImage(VADriverContextP ctx, VAImageID image) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(image, MAX_IMAGES) || !data->images[image].allocated) return VA_STATUS_ERROR_INVALID_IMAGE;

    bc250_DestroyBuffer(ctx, data->images[image].buffer_id);
    data->images[image].allocated = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_DeriveImage(VADriverContextP ctx, VASurfaceID surface, VAImage *image) {
    bc250_driver_data *data = get_driver_data(ctx);
    /* A surface with pending_destroy set has already been handed back to
     * vaDestroySurfaces() by the application - it must be rejected here
     * exactly like any other invalid surface, even if its Vulkan
     * resources happen to still be alive internally pending an earlier
     * derived image's teardown (see bc250_surface.pending_destroy). */
    if (!data || !VALID_ID(surface, MAX_SURFACES) || !data->surfaces[surface].allocated ||
        data->surfaces[surface].pending_destroy || !image) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    bc250_surface *surf = &data->surfaces[surface];

    VAImageFormat fmt = {
        .fourcc = VA_FOURCC_NV12,
        .byte_order = VA_LSB_FIRST,
        .bits_per_pixel = 12
    };
    VAStatus status = bc250_CreateImage(ctx, &fmt, surf->width, surf->height, image);
    if (status != VA_STATUS_SUCCESS) return status;

    bc250_image *img = &data->images[image->image_id];
    bc250_buffer *buf = &data->buffers[img->buffer_id];

    /* bc250_CreateImage() above just filled in `image`/`img->image` with a
     * naive, tightly-packed pitches/offsets/data_size (pitches[0]=width,
     * offsets[1]=width*height, data_size=width*height*3/2) - correct for a
     * plain vaCreateImage() whose VAImageBufferType buffer is a private,
     * non-aliased malloc(). But below, this derived image's buffer is about
     * to be replaced with a *direct mapping of the surface's own real
     * Vulkan memory* (buf->data = mapped). A consumer of this VAImage (e.g.
     * ffmpeg's hwupload -> av_frame_copy -> av_image_copy, which does a
     * plain memcpy straight into this buffer using exactly these
     * pitches/offsets/data_size) will address that real memory using the
     * naive numbers, which do not account for the real per-row pitch
     * padding and inter-plane alignment gap that
     * gpu_compute_create_image() actually bound Y/UV to (confirmed
     * on-hardware: 854x480 real data_size=737280B vs naive 614880B;
     * 1920x1080 real=3317760B vs naive=3110400B; the two happen to coincide
     * exactly at 1280x720 because 1280 and 640*2=1280 are already multiples
     * of this hardware's apparent 256-byte row-pitch alignment, so the
     * mismatch is resolution-dependent, not universal). Overwrite the
     * geometry with the real, Vulkan-derived layout (the same
     * vkGetImageSubresourceLayout()/vkGetImageMemoryRequirements() math
     * gpu_compute_upload_nv12()/gpu_compute_download_nv12() already use to
     * address this same memory) before handing it back, so the caller's
     * view of this buffer always matches the real allocation exactly. */
    gpu_nv12_layout_t real_layout;
    if (gpu_compute_get_nv12_layout(&data->gpu, &surf->image, surf->memory, &real_layout) == 0) {
        image->pitches[0] = real_layout.y_pitch;
        image->offsets[0] = (unsigned int)real_layout.y_offset;
        image->pitches[1] = real_layout.uv_pitch;
        image->offsets[1] = (unsigned int)real_layout.uv_offset;
        image->data_size = (unsigned int)real_layout.total_size;
        img->image = *image;
    }

    if (buf && surf->memory.memory) {
        void *mapped = NULL;
        if (vkMapMemory(data->gpu.device, surf->memory.memory, 0, surf->memory.size, 0, &mapped) == VK_SUCCESS) {
            if (buf->data) free(buf->data);
            buf->data = mapped;
            buf->mapped = 1;
            buf->is_derived = 1;
            buf->gpu_mem = surf->memory.memory;
            /* This derived image now aliases the surface's own Vulkan
             * memory directly (buf->data / buf->gpu_mem above). Take a
             * reference on the surface so vaDestroySurfaces() cannot free
             * that memory out from under this still-live mapping - see
             * bc250_surface_unref() / bc250_DestroyBuffer() for the
             * matching release. This is the fix for the use-after-free:
             * previously ref_count was only ever set to 1 at
             * vaCreateSurfaces() and never incremented here, so a
             * vaDestroySurfaces() call while a derived image was still
             * alive would free the surface's VkImage/VkDeviceMemory
             * immediately, and a later vaDestroyImage() -> vkUnmapMemory()
             * on that freed VkDeviceMemory handle would segfault
             * (confirmed on-hardware: radv_UnmapMemory2 SIGSEGV via
             * bc250_DestroyBuffer at va_backend.c, called from
             * bc250_DestroyImage). */
            surf->ref_count++;
            buf->derived_surface = surface;
            img->surface_id = surface;
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_GetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y, unsigned int width, unsigned int height, VAImageID image) {
    (void)x; (void)y; (void)width; (void)height;
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(surface, MAX_SURFACES) || !data->surfaces[surface].allocated ||
        data->surfaces[surface].pending_destroy) return VA_STATUS_ERROR_INVALID_SURFACE;
    if (!VALID_ID(image, MAX_IMAGES) || !data->images[image].allocated) return VA_STATUS_ERROR_INVALID_IMAGE;

    bc250_surface *surf = &data->surfaces[surface];
    bc250_image *img = &data->images[image];
    bc250_buffer *buf = &data->buffers[img->buffer_id];

    if (buf && buf->data && surf->memory.memory) {
        uint8_t *dst_y = (uint8_t *)buf->data + img->image.offsets[0];
        uint8_t *dst_uv = (uint8_t *)buf->data + img->image.offsets[1];
        int y_pitch = img->image.pitches[0] > 0 ? (int)img->image.pitches[0] : surf->width;
        int uv_pitch = img->image.pitches[1] > 0 ? (int)img->image.pitches[1] : surf->width;

        /* Copy extent must be the image's OWN allocated width/height
         * (img->image.width/height - what bc250_CreateImage() actually
         * sized buf->data for), not the surface's: surf->width/height is
         * this driver's internal macroblock-padded encode size (e.g. 1088
         * for a 1080-tall frame), which is >= the real display size a
         * plain vaCreateImage()+vaGetImage() caller asked for. Using
         * surf->height here walked this copy past the end of buf->data's
         * real allocation (confirmed on-hardware via gdb: SIGSEGV in the
         * UV-plane memcpy at r=540 for a 1080-tall image, where
         * height/2=544 from surf->height=1088 overran a buffer sized for
         * only 1080/2=540 UV rows). bc250_DeriveImage() is unaffected -
         * there, img->image.width/height are set to surf->width/height by
         * construction (see bc250_DeriveImage() above), so this is the
         * same value in that case, not a behavior change. */
        int copy_width = img->image.width > 0 ? (int)img->image.width : surf->width;
        int copy_height = img->image.height > 0 ? (int)img->image.height : surf->height;
        if (copy_width > surf->width) copy_width = surf->width;
        if (copy_height > surf->height) copy_height = surf->height;

        gpu_compute_download_nv12(&data->gpu, &surf->image, surf->memory,
                                  dst_y, y_pitch,
                                  dst_uv, uv_pitch,
                                  copy_width, copy_height);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_PutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image, int src_x, int src_y, unsigned int src_width, unsigned int src_height, int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height) {
    (void)src_x; (void)src_y; (void)src_width; (void)src_height;
    (void)dest_x; (void)dest_y; (void)dest_width; (void)dest_height;
    bc250_driver_data *data = get_driver_data(ctx);
    if (!data || !VALID_ID(surface, MAX_SURFACES) || !data->surfaces[surface].allocated ||
        data->surfaces[surface].pending_destroy) return VA_STATUS_ERROR_INVALID_SURFACE;
    if (!VALID_ID(image, MAX_IMAGES) || !data->images[image].allocated) return VA_STATUS_ERROR_INVALID_IMAGE;

    bc250_surface *surf = &data->surfaces[surface];
    bc250_image *img = &data->images[image];
    bc250_buffer *buf = &data->buffers[img->buffer_id];

    if (buf && buf->data && surf->memory.memory) {
        const uint8_t *src_y = (const uint8_t *)buf->data + img->image.offsets[0];
        const uint8_t *src_uv = (const uint8_t *)buf->data + img->image.offsets[1];
        int y_pitch = img->image.pitches[0] > 0 ? (int)img->image.pitches[0] : surf->width;
        int uv_pitch = img->image.pitches[1] > 0 ? (int)img->image.pitches[1] : surf->width;

        /* Same fix as bc250_GetImage() above, mirrored: the copy extent
         * must be img->image.width/height (what buf->data was actually
         * allocated for), not surf->width/height (this driver's internal
         * macroblock-padded encode size) - otherwise this reads past the
         * end of buf->data whenever the surface's padded height exceeds
         * the image's real height (e.g. 1088 vs 1080). */
        int copy_width = img->image.width > 0 ? (int)img->image.width : surf->width;
        int copy_height = img->image.height > 0 ? (int)img->image.height : surf->height;
        if (copy_width > surf->width) copy_width = surf->width;
        if (copy_height > surf->height) copy_height = surf->height;

        gpu_compute_upload_nv12(&data->gpu, &surf->image, surf->memory,
                                src_y, y_pitch,
                                src_uv, uv_pitch,
                                copy_width, copy_height);
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_SetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette) {
    (void)ctx; (void)image; (void)palette;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* Subpictures are not supported by this driver. Query returns zero available
 * formats (the standard way to report "unsupported"); the rest are stubs to
 * satisfy libva's vtable completeness check. */
VAStatus bc250_QuerySubpictureFormats(VADriverContextP ctx, VAImageFormat *format_list, unsigned int *flags, unsigned int *num_formats) {
    (void)ctx; (void)format_list; (void)flags;
    if (!num_formats) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_CreateSubpicture(VADriverContextP ctx, VAImageID image, VASubpictureID *subpicture) {
    (void)ctx; (void)image; (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus bc250_DestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture) {
    (void)ctx; (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus bc250_SetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image) {
    (void)ctx; (void)subpicture; (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus bc250_SetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture, unsigned int chromakey_min, unsigned int chromakey_max, unsigned int chromakey_mask) {
    (void)ctx; (void)subpicture; (void)chromakey_min; (void)chromakey_max; (void)chromakey_mask;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus bc250_SetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha) {
    (void)ctx; (void)subpicture; (void)global_alpha;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus bc250_AssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces,
                                   short src_x, short src_y, unsigned short src_width, unsigned short src_height,
                                   short dest_x, short dest_y, unsigned short dest_width, unsigned short dest_height,
                                   unsigned int flags) {
    (void)ctx; (void)subpicture; (void)target_surfaces; (void)num_surfaces;
    (void)src_x; (void)src_y; (void)src_width; (void)src_height;
    (void)dest_x; (void)dest_y; (void)dest_width; (void)dest_height; (void)flags;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus bc250_DeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture, VASurfaceID *target_surfaces, int num_surfaces) {
    (void)ctx; (void)subpicture; (void)target_surfaces; (void)num_surfaces;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* Display attributes are not supported. Query/Get report zero/no-op success
 * (the standard way to report "unsupported"); Set is unimplemented since
 * nothing was ever exposed to set. */
VAStatus bc250_QueryDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list, int *num_attributes) {
    (void)ctx; (void)attr_list;
    if (!num_attributes) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_GetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list, int num_attributes) {
    (void)ctx; (void)attr_list; (void)num_attributes;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_SetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list, int num_attributes) {
    (void)ctx; (void)attr_list; (void)num_attributes;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus bc250_QueryVideoProcFilters(VADriverContextP ctx, VAContextID context, VAProcFilterType *filters, unsigned int *num_filters) {
    (void)ctx; (void)context;
    if (!num_filters) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!filters) {
        *num_filters = 0;
        return VA_STATUS_SUCCESS;
    }
    *num_filters = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_QueryVideoProcFilterCaps(VADriverContextP ctx, VAContextID context, VAProcFilterType type, void *filter_caps, unsigned int *num_filter_caps) {
    (void)ctx; (void)context; (void)type; (void)filter_caps;
    if (!num_filter_caps) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *num_filter_caps = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_QueryVideoProcPipelineCaps(VADriverContextP ctx, VAContextID context, VABufferID *filters, unsigned int num_filters, VAProcPipelineCaps *pipeline_caps) {
    (void)ctx; (void)context; (void)filters; (void)num_filters;
    if (!pipeline_caps) return VA_STATUS_ERROR_INVALID_PARAMETER;
    memset(pipeline_caps, 0, sizeof(*pipeline_caps));
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_Terminate(VADriverContextP ctx) {
    bc250_driver_data *data = get_driver_data(ctx);
    if (data) {
        /* The VA-API contract expects callers to have destroyed every
         * config/context/buffer/image/surface before vaTerminate(), but a
         * driver should not silently leak GPU memory and heap allocations
         * if a caller doesn't. Free anything still outstanding here, before
         * tearing down the GPU context, by reusing the existing Destroy*
         * paths. Order matters: contexts (which hold encoder/decoder state
         * and reference surfaces/buffers by id, but don't own them) must go
         * before the buffers/images/surfaces they reference; images (which
         * own a buffer each) before the remaining plain buffers; and
         * surfaces last, since gpu_compute_terminate() below invalidates the
         * VkDevice that surface/derived-image teardown still needs. */
        for (int i = 0; i < MAX_CONTEXTS; i++) {
            if (data->contexts[i].allocated) {
                bc250_DestroyContext(ctx, i);
            }
        }
        for (int i = 0; i < MAX_IMAGES; i++) {
            if (data->images[i].allocated) {
                bc250_DestroyImage(ctx, i);
            }
        }
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (data->buffers[i].allocated) {
                bc250_DestroyBuffer(ctx, i);
            }
        }
        for (int i = 0; i < MAX_SURFACES; i++) {
            if (data->surfaces[i].allocated) {
                VASurfaceID id = (VASurfaceID)i;
                bc250_DestroySurfaces(ctx, &id, 1);
            }
        }

        gpu_compute_terminate(&data->gpu);
        free(data);
        ctx->pDriverData = NULL;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus bc250_Initialize(VADriverContextP ctx, int *major_version, int *minor_version) {
    if (!ctx) return VA_STATUS_ERROR_INVALID_CONTEXT;

    bc250_driver_data *data = calloc(1, sizeof(bc250_driver_data));
    if (!data) return VA_STATUS_ERROR_ALLOCATION_FAILED;

    if (gpu_compute_init(&data->gpu) != 0) {
        fprintf(stderr, "[bc250-drv] Failed to initialize Vulkan compute backend!\n");
        free(data);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    data->max_width = BC250_MAX_WIDTH;
    data->max_height = BC250_MAX_HEIGHT;
    ctx->pDriverData = data;
    ctx->str_vendor = "AMD BC-250 RDNA2 Compute VA-API Driver";

    /* libva's core vaInitialize() validates these counts and the vtable
     * completeness before returning control to the driver's caller - both
     * are mandatory, not just documentation. */
    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = MAX_ENTRYPOINTS;
    ctx->max_attributes = MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats = MAX_IMAGE_FORMATS;
    /* libva requires these positive even though we report zero actual
     * subpicture formats / display attributes at query time - they only
     * size libva's internal arrays, they aren't a "supported" flag. */
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;

    /* Wire complete vtable */
    ctx->vtable->vaTerminate = bc250_Terminate;
    ctx->vtable->vaQueryConfigProfiles = bc250_QueryConfigProfiles;
    ctx->vtable->vaQueryConfigEntrypoints = bc250_QueryConfigEntrypoints;
    ctx->vtable->vaGetConfigAttributes = bc250_GetConfigAttributes;
    ctx->vtable->vaCreateConfig = bc250_CreateConfig;
    ctx->vtable->vaDestroyConfig = bc250_DestroyConfig;
    ctx->vtable->vaQueryConfigAttributes = bc250_QueryConfigAttributes;
    ctx->vtable->vaCreateSurfaces = bc250_CreateSurfaces;
    ctx->vtable->vaCreateSurfaces2 = bc250_CreateSurfaces2;
    ctx->vtable->vaDestroySurfaces = bc250_DestroySurfaces;
    ctx->vtable->vaCreateContext = bc250_CreateContext;
    ctx->vtable->vaDestroyContext = bc250_DestroyContext;
    ctx->vtable->vaCreateBuffer = bc250_CreateBuffer;
    ctx->vtable->vaBufferSetNumElements = bc250_BufferSetNumElements;
    ctx->vtable->vaMapBuffer = bc250_MapBuffer;
    ctx->vtable->vaUnmapBuffer = bc250_UnmapBuffer;
    ctx->vtable->vaDestroyBuffer = bc250_DestroyBuffer;
    ctx->vtable->vaBeginPicture = bc250_BeginPicture;
    ctx->vtable->vaRenderPicture = bc250_RenderPicture;
    ctx->vtable->vaEndPicture = bc250_EndPicture;
    ctx->vtable->vaSyncSurface = bc250_SyncSurface;
    ctx->vtable->vaQuerySurfaceStatus = bc250_QuerySurfaceStatus;
    ctx->vtable->vaQueryImageFormats = bc250_QueryImageFormats;
    ctx->vtable->vaQuerySurfaceAttributes = bc250_QuerySurfaceAttributes;
    ctx->vtable->vaCreateImage = bc250_CreateImage;
    ctx->vtable->vaDestroyImage = bc250_DestroyImage;
    ctx->vtable->vaDeriveImage = bc250_DeriveImage;
    ctx->vtable->vaGetImage = bc250_GetImage;
    ctx->vtable->vaPutImage = bc250_PutImage;
    ctx->vtable->vaSetImagePalette = bc250_SetImagePalette;
    ctx->vtable->vaQuerySubpictureFormats = bc250_QuerySubpictureFormats;
    ctx->vtable->vaCreateSubpicture = bc250_CreateSubpicture;
    ctx->vtable->vaDestroySubpicture = bc250_DestroySubpicture;
    ctx->vtable->vaSetSubpictureImage = bc250_SetSubpictureImage;
    ctx->vtable->vaSetSubpictureChromakey = bc250_SetSubpictureChromakey;
    ctx->vtable->vaSetSubpictureGlobalAlpha = bc250_SetSubpictureGlobalAlpha;
    ctx->vtable->vaAssociateSubpicture = bc250_AssociateSubpicture;
    ctx->vtable->vaDeassociateSubpicture = bc250_DeassociateSubpicture;
    ctx->vtable->vaQueryDisplayAttributes = bc250_QueryDisplayAttributes;
    ctx->vtable->vaGetDisplayAttributes = bc250_GetDisplayAttributes;
    ctx->vtable->vaSetDisplayAttributes = bc250_SetDisplayAttributes;

    if (major_version) *major_version = VA_MAJOR_VERSION;
    if (minor_version) *minor_version = VA_MINOR_VERSION;

    return VA_STATUS_SUCCESS;
}

VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    int major = VA_MAJOR_VERSION;
    int minor = VA_MINOR_VERSION;
    return bc250_Initialize(ctx, &major, &minor);
}

VAStatus __vaDriverInit_0_32(VADriverContextP ctx) {
    int major = VA_MAJOR_VERSION;
    int minor = VA_MINOR_VERSION;
    return bc250_Initialize(ctx, &major, &minor);
}
