/* SPDX-License-Identifier: MIT */
#include "include/rinimage/image.h"

#include <string.h>
#include <stdint.h>

static int rin_image_limits_valid(const RinImageDecodeLimits* limits)
{
    return limits != NULL && limits->max_source_bytes != 0u &&
           limits->max_width != 0u && limits->max_height != 0u &&
           limits->max_pixels != 0u && limits->max_frames != 0u &&
           limits->max_output_bytes != 0u;
}

void rin_image_decode_limits_default(RinImageDecodeLimits* limits)
{
    if (limits == NULL) return;
    limits->max_source_bytes = 64u * 1024u * 1024u;
    limits->max_width = 4096u;
    limits->max_height = 4096u;
    limits->max_pixels = UINT64_C(4096) * UINT64_C(4096);
    limits->max_frames = 1024u;
    limits->max_output_bytes = 64u * 1024u * 1024u;
}

void rin_image_scale_limits_default(RinImageScaleLimits* limits)
{
    if (limits == NULL) return;
    limits->max_width = 4096u;
    limits->max_height = 4096u;
    limits->max_pixels = UINT64_C(4096) * UINT64_C(4096);
    limits->max_output_bytes = 64u * 1024u * 1024u;
}

static int rin_image_scale_limits_valid(const RinImageScaleLimits* limits)
{
    return limits != NULL && limits->max_width != 0u &&
           limits->max_height != 0u && limits->max_pixels != 0u &&
           limits->max_output_bytes >= sizeof(uint32_t);
}

static int rin_image_scale_ranges_overlap(const RinImageFrame* source,
                                          const uint32_t* output_pixels,
                                          size_t output_bytes)
{
    const uintptr_t source_start = (uintptr_t)(const void*)source->pixels;
    const uintptr_t output_start = (uintptr_t)(const void*)output_pixels;
    const uintptr_t source_size = (uintptr_t)source->pixel_bytes;
    const uintptr_t output_size = (uintptr_t)output_bytes;
    const uintptr_t source_end = source_start + source_size;
    const uintptr_t output_end = output_start + output_size;
    if (source_end < source_start || output_end < output_start)
        return 1;
    return source_start < output_end && output_start < source_end;
}

RinImageStatus rin_image_scale_nearest(const RinImageFrame* source,
                                       RinImageRect source_region,
                                       RinImageSize output_size,
                                       const RinImageScaleLimits* limits,
                                       uint32_t* output_pixels,
                                       size_t output_capacity,
                                       RinImageFrame* output_frame)
{
    RinImageDecodeLimits source_limits;
    uint64_t output_pixels_count;
    uint64_t output_bytes;
    uint64_t source_right;
    uint64_t source_bottom;
    size_t required_output_bytes;
    RinImageStatus source_status;
    uint32_t y;

    if (output_frame != NULL) memset(output_frame, 0, sizeof(*output_frame));
    if (source == NULL || output_pixels == NULL || output_frame == NULL ||
        !rin_image_scale_limits_valid(limits))
        return RIN_IMAGE_INVALID_ARGUMENT;

    rin_image_decode_limits_default(&source_limits);
    source_status = rin_image_frame_validate(&source_limits, source);
    if (source_status != RIN_IMAGE_OK) return source_status;
    if (output_size.width == 0u || output_size.height == 0u ||
        output_size.width > limits->max_width ||
        output_size.height > limits->max_height)
        return RIN_IMAGE_LIMIT;
    output_pixels_count = (uint64_t)output_size.width *
                          (uint64_t)output_size.height;
    if (output_pixels_count > limits->max_pixels ||
        output_pixels_count > (uint64_t)SIZE_MAX / sizeof(uint32_t) ||
        output_size.width > UINT32_MAX / sizeof(uint32_t))
        return RIN_IMAGE_LIMIT;
    output_bytes = output_pixels_count * sizeof(uint32_t);
    if (output_bytes > (uint64_t)limits->max_output_bytes ||
        output_pixels_count > (uint64_t)output_capacity)
        return RIN_IMAGE_LIMIT;
    required_output_bytes = (size_t)output_bytes;
    if (rin_image_scale_ranges_overlap(source, output_pixels,
                                       required_output_bytes))
        return RIN_IMAGE_INVALID_ARGUMENT;

    if (source_region.width == 0u || source_region.height == 0u)
        return RIN_IMAGE_INVALID_ARGUMENT;
    source_right = (uint64_t)source_region.x + source_region.width;
    source_bottom = (uint64_t)source_region.y + source_region.height;
    if (source_right > source->size.width ||
        source_bottom > source->size.height ||
        source->stride_bytes % sizeof(uint32_t) != 0u)
        return RIN_IMAGE_INVALID_ARGUMENT;

    for (y = 0u; y < output_size.height; ++y) {
        const uint64_t source_y = (uint64_t)source_region.y +
            (uint64_t)y * source_region.height / output_size.height;
        uint32_t x;
        for (x = 0u; x < output_size.width; ++x) {
            const uint64_t source_x = (uint64_t)source_region.x +
                (uint64_t)x * source_region.width / output_size.width;
            const size_t source_index = (size_t)source_y *
                (source->stride_bytes / sizeof(uint32_t)) +
                (size_t)source_x;
            output_pixels[(size_t)y * output_size.width + x] =
                source->pixels[source_index];
        }
    }

    output_frame->size = output_size;
    output_frame->stride_bytes = output_size.width * sizeof(uint32_t);
    output_frame->pixel_format = RIN_IMAGE_PIXEL_ARGB8888;
    output_frame->kind = source->kind;
    output_frame->duration_ms = source->duration_ms;
    output_frame->pixels = output_pixels;
    output_frame->pixel_bytes = required_output_bytes;
    return RIN_IMAGE_OK;
}

RinImageStatus rin_image_thumbnail(const RinImageFrame* source,
                                   const RinImageScaleLimits* limits,
                                   uint32_t* output_pixels,
                                   size_t output_capacity,
                                   RinImageFrame* output_frame)
{
    RinImageSize target;
    uint32_t max_width;
    uint32_t max_height;
    uint64_t left;
    uint64_t right;
    RinImageStatus status;

    if (output_frame != NULL) memset(output_frame, 0, sizeof(*output_frame));
    if (source == NULL || !rin_image_scale_limits_valid(limits))
        return RIN_IMAGE_INVALID_ARGUMENT;
    if (source->size.width == 0u || source->size.height == 0u)
        return RIN_IMAGE_INVALID_ARGUMENT;

    max_width = source->size.width < limits->max_width
        ? source->size.width : limits->max_width;
    max_height = source->size.height < limits->max_height
        ? source->size.height : limits->max_height;
    if (max_width == 0u || max_height == 0u)
        return RIN_IMAGE_LIMIT;

    left = (uint64_t)source->size.width * max_height;
    right = (uint64_t)source->size.height * max_width;
    if (left > right) {
        target.width = max_width;
        target.height = (uint32_t)(((uint64_t)source->size.height *
                                    max_width + source->size.width - 1u) /
                                   source->size.width);
    } else {
        target.height = max_height;
        target.width = (uint32_t)(((uint64_t)source->size.width *
                                   max_height + source->size.height - 1u) /
                                  source->size.height);
    }
    if (target.width == 0u || target.height == 0u ||
        target.width > max_width || target.height > max_height)
        return RIN_IMAGE_LIMIT;

    status = rin_image_scale_nearest(
        source, (RinImageRect){ 0u, 0u, source->size.width,
                                source->size.height }, target, limits,
        output_pixels, output_capacity, output_frame);
    if (status != RIN_IMAGE_OK) return status;
    output_frame->kind = RIN_IMAGE_FRAME_STATIC;
    output_frame->duration_ms = 0u;
    return RIN_IMAGE_OK;
}

static RinImageStatus rin_image_validate_size(
    const RinImageDecodeLimits* limits, RinImageSize size,
    size_t* pixel_count_out, size_t* output_bytes_out)
{
    uint64_t pixels;
    uint64_t output_bytes;
    if (!rin_image_limits_valid(limits) || size.width == 0u ||
        size.height == 0u) return RIN_IMAGE_INVALID_ARGUMENT;
    if (size.width > limits->max_width || size.height > limits->max_height)
        return RIN_IMAGE_LIMIT;
    pixels = (uint64_t)size.width * (uint64_t)size.height;
    if (pixels > limits->max_pixels || pixels > (uint64_t)SIZE_MAX / 4u)
        return RIN_IMAGE_LIMIT;
    output_bytes = pixels * 4u;
    if (output_bytes > (uint64_t)limits->max_output_bytes)
        return RIN_IMAGE_LIMIT;
    if (pixel_count_out != NULL) *pixel_count_out = (size_t)pixels;
    if (output_bytes_out != NULL) *output_bytes_out = (size_t)output_bytes;
    return RIN_IMAGE_OK;
}

RinImageStatus rin_image_validate_decode(const RinImageDecodeLimits* limits,
                                         size_t source_bytes,
                                         RinImageSize size,
                                         uint32_t frame_count,
                                         RinImagePixelFormat pixel_format)
{
    RinImageStatus status;
    if (!rin_image_limits_valid(limits) || source_bytes == 0u ||
        source_bytes > limits->max_source_bytes || frame_count == 0u ||
        frame_count > limits->max_frames)
        return RIN_IMAGE_LIMIT;
    if (pixel_format != RIN_IMAGE_PIXEL_ARGB8888)
        return RIN_IMAGE_UNSUPPORTED;
    status = rin_image_validate_size(limits, size, NULL, NULL);
    if (status != RIN_IMAGE_OK) return status;
    if ((uint64_t)frame_count * (uint64_t)size.width *
            (uint64_t)size.height >
        (uint64_t)limits->max_output_bytes / 4u)
        return RIN_IMAGE_LIMIT;
    return RIN_IMAGE_OK;
}

RinImageStatus rin_image_frame_validate(const RinImageDecodeLimits* limits,
                                        const RinImageFrame* frame)
{
    size_t pixel_bytes;
    size_t required_bytes;
    RinImageStatus status;
    if (frame == NULL || frame->pixels == NULL || frame->stride_bytes == 0u)
        return RIN_IMAGE_INVALID_ARGUMENT;
    if (frame->pixel_format != RIN_IMAGE_PIXEL_ARGB8888)
        return RIN_IMAGE_UNSUPPORTED;
    status = rin_image_validate_size(limits, frame->size, NULL, &pixel_bytes);
    if (status != RIN_IMAGE_OK) return status;
    if ((uint64_t)frame->stride_bytes <
        (uint64_t)frame->size.width * 4u)
        return RIN_IMAGE_MALFORMED;
    if ((uint64_t)frame->stride_bytes * (uint64_t)frame->size.height >
        (uint64_t)SIZE_MAX)
        return RIN_IMAGE_OVERFLOW;
    required_bytes = (size_t)frame->stride_bytes * (size_t)frame->size.height;
    if (required_bytes > frame->pixel_bytes ||
        required_bytes > limits->max_output_bytes ||
        pixel_bytes > frame->pixel_bytes)
        return RIN_IMAGE_LIMIT;
    if (frame->kind != RIN_IMAGE_FRAME_STATIC &&
        frame->kind != RIN_IMAGE_FRAME_ANIMATED)
        return RIN_IMAGE_INVALID_ARGUMENT;
    return RIN_IMAGE_OK;
}

RinImageStatus rin_image_metadata_validate(const RinImageDecodeLimits* limits,
                                           const RinImageMetadata* metadata)
{
    if (metadata == NULL || metadata->frame_count == 0u ||
        metadata->kind < RIN_IMAGE_FRAME_STATIC ||
        metadata->kind > RIN_IMAGE_FRAME_ANIMATED)
        return RIN_IMAGE_INVALID_ARGUMENT;
    if (!rin_image_limits_valid(limits) ||
        metadata->frame_count > limits->max_frames)
        return RIN_IMAGE_LIMIT;
    return rin_image_validate_size(limits, metadata->canvas, NULL, NULL);
}
