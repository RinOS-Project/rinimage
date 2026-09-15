/* SPDX-License-Identifier: MIT */
#include "include/rinimage/decoder.h"

#include <string.h>

#include "../rinpng/rpng.h"

static int rin_image_png_limits_valid(const RinImageDecodeLimits* limits)
{
    return limits != NULL && limits->max_source_bytes != 0u &&
           limits->max_width != 0u && limits->max_height != 0u &&
           limits->max_pixels != 0u && limits->max_frames != 0u &&
           limits->max_output_bytes != 0u;
}

static RinImageStatus rin_image_png_codec_status(int result)
{
    return result == RPNG_ERR_UNSUPPORTED ? RIN_IMAGE_UNSUPPORTED
                                          : RIN_IMAGE_MALFORMED;
}

static int rin_image_png_signature(const uint8_t* data, size_t size)
{
    static const uint8_t signature[8] = {
        0x89u, 'P', 'N', 'G', 0x0du, 0x0au, 0x1au, 0x0au
    };
    return data != NULL && size >= sizeof(signature) &&
           memcmp(data, signature, sizeof(signature)) == 0;
}

RinImageStatus rin_image_probe_png(const uint8_t* data, size_t source_bytes,
                                   const RinImageDecodeLimits* limits,
                                   RinImageProbe* probe_out)
{
    int width = 0;
    int height = 0;
    int result;
    RinImageStatus status;

    if (probe_out != NULL) memset(probe_out, 0, sizeof(*probe_out));
    if (!rin_image_png_limits_valid(limits) || data == NULL ||
        source_bytes == 0u || probe_out == NULL)
        return RIN_IMAGE_INVALID_ARGUMENT;
    if (source_bytes > limits->max_source_bytes)
        return RIN_IMAGE_LIMIT;
    if (!rin_image_png_signature(data, source_bytes))
        return RIN_IMAGE_UNSUPPORTED;

    result = rpng_get_info(data, source_bytes, &width, &height);
    if (result != RPNG_OK) return rin_image_png_codec_status(result);
    if (width <= 0 || height <= 0) return RIN_IMAGE_MALFORMED;

    probe_out->format = RIN_IMAGE_FORMAT_PNG;
    probe_out->size.width = (uint32_t)width;
    probe_out->size.height = (uint32_t)height;
    probe_out->frame_count = 1u;
    probe_out->kind = RIN_IMAGE_FRAME_STATIC;
    status = rin_image_validate_decode(
        limits, source_bytes, probe_out->size, probe_out->frame_count,
        RIN_IMAGE_PIXEL_ARGB8888);
    if (status != RIN_IMAGE_OK) memset(probe_out, 0, sizeof(*probe_out));
    return status;
}

RinImageStatus rin_image_decode_png(const uint8_t* data, size_t source_bytes,
                                    const RinImageDecodeLimits* limits,
                                    uint32_t* pixels, size_t pixel_capacity,
                                    RinImageProbe* probe_out)
{
    RinImageProbe probe = {};
    RinImageFrame frame = {};
    RinImageStatus status;
    uint64_t pixel_count;
    size_t pixel_bytes;

    if (probe_out != NULL) memset(probe_out, 0, sizeof(*probe_out));
    if (pixels == NULL) return RIN_IMAGE_INVALID_ARGUMENT;
    status = rin_image_probe_png(data, source_bytes, limits, &probe);
    if (status != RIN_IMAGE_OK) return status;

    pixel_count = (uint64_t)probe.size.width * (uint64_t)probe.size.height;
    if (pixel_count == 0u || pixel_count > SIZE_MAX / sizeof(uint32_t) ||
        pixel_count > (uint64_t)pixel_capacity)
        return RIN_IMAGE_LIMIT;
    pixel_bytes = (size_t)pixel_count * sizeof(uint32_t);
    if (rpng_decode_rgba(data, source_bytes, pixels, (int)probe.size.width,
                         (int)probe.size.height) != RPNG_OK) {
        memset(pixels, 0, pixel_bytes);
        return RIN_IMAGE_MALFORMED;
    }

    frame.size = probe.size;
    frame.stride_bytes = probe.size.width * sizeof(uint32_t);
    frame.pixel_format = RIN_IMAGE_PIXEL_ARGB8888;
    frame.kind = probe.kind;
    frame.pixels = pixels;
    frame.pixel_bytes = pixel_bytes;
    status = rin_image_frame_validate(limits, &frame);
    if (status != RIN_IMAGE_OK) {
        memset(pixels, 0, pixel_bytes);
        return status;
    }
    if (probe_out != NULL) *probe_out = probe;
    return RIN_IMAGE_OK;
}
