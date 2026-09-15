/* SPDX-License-Identifier: MIT */
#include "include/rinimage/decoder.h"

#include <limits.h>
#include <string.h>

#include "../ringif/ringif.h"
#include "../rinjpeg/rinjpeg.h"
#include "../rinpng/rpng.h"
#include "../rinwebp/src/webp/decode.h"

static int rin_image_limits_valid(const RinImageDecodeLimits* limits)
{
    return limits != NULL && limits->max_source_bytes != 0u &&
           limits->max_width != 0u && limits->max_height != 0u &&
           limits->max_pixels != 0u && limits->max_frames != 0u &&
           limits->max_output_bytes != 0u;
}

static uint16_t rin_image_le16(const uint8_t* p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8u);
}

static uint32_t rin_image_le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static int rin_image_ascii_space(uint8_t value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\f' || value == '\v';
}

static int rin_image_ppm_skip(const uint8_t* data, size_t size, size_t* pos)
{
    while (*pos < size) {
        if (rin_image_ascii_space(data[*pos])) {
            ++*pos;
            continue;
        }
        if (data[*pos] == '#') {
            while (*pos < size && data[*pos] != '\n') ++*pos;
            continue;
        }
        break;
    }
    return *pos < size;
}

static int rin_image_ppm_token(const uint8_t* data, size_t size, size_t* pos,
                               char* token, size_t token_capacity)
{
    size_t length = 0u;
    if (token == NULL || token_capacity < 2u ||
        !rin_image_ppm_skip(data, size, pos)) return 0;
    while (*pos < size && !rin_image_ascii_space(data[*pos]) &&
           data[*pos] != '#') {
        if (length + 1u >= token_capacity) return 0;
        token[length++] = (char)data[(*pos)++];
    }
    if (length == 0u) return 0;
    token[length] = '\0';
    return 1;
}

static int rin_image_ppm_number(const char* token, uint32_t* value)
{
    uint64_t result = 0u;
    size_t index = 0u;
    if (token == NULL || value == NULL || token[0] == '\0') return 0;
    while (token[index] != '\0') {
        if (token[index] < '0' || token[index] > '9' || result >
            (UINT32_MAX - (uint32_t)(token[index] - '0')) / 10u)
            return 0;
        result = result * 10u + (uint32_t)(token[index] - '0');
        ++index;
    }
    *value = (uint32_t)result;
    return 1;
}

static RinImageStatus rin_image_ppm_header(const uint8_t* data, size_t size,
                                           RinImageProbe* probe, int* ascii,
                                           size_t* payload_offset)
{
    char token[32];
    size_t pos = 0u;
    uint32_t width;
    uint32_t height;
    uint32_t maximum;
    if (!rin_image_ppm_token(data, size, &pos, token, sizeof(token)))
        return RIN_IMAGE_MALFORMED;
    if (strcmp(token, "P6") == 0) *ascii = 0;
    else if (strcmp(token, "P3") == 0) *ascii = 1;
    else return RIN_IMAGE_UNSUPPORTED;
    if (!rin_image_ppm_token(data, size, &pos, token, sizeof(token)) ||
        !rin_image_ppm_number(token, &width) ||
        !rin_image_ppm_token(data, size, &pos, token, sizeof(token)) ||
        !rin_image_ppm_number(token, &height) ||
        !rin_image_ppm_token(data, size, &pos, token, sizeof(token)) ||
        !rin_image_ppm_number(token, &maximum) || width == 0u || height == 0u ||
        maximum != 255u)
        return RIN_IMAGE_UNSUPPORTED;
    if (*ascii == 0) {
        /* Consume the single separator after maxval.  Binary pixel bytes may
         * themselves be whitespace or '#', so a generic comment skipper here
         * would silently eat valid image data. */
        if (pos >= size || !rin_image_ascii_space(data[pos]))
            return RIN_IMAGE_MALFORMED;
        *payload_offset = pos + 1u;
    } else {
        *payload_offset = pos;
    }
    probe->format = RIN_IMAGE_FORMAT_PPM;
    probe->size.width = width;
    probe->size.height = height;
    probe->frame_count = 1u;
    probe->kind = RIN_IMAGE_FRAME_STATIC;
    return RIN_IMAGE_OK;
}

static RinImageStatus rin_image_probe_tga(const uint8_t* data, size_t size,
                                          RinImageProbe* probe)
{
    uint16_t width;
    uint16_t height;
    uint8_t image_type;
    uint8_t bits_per_pixel;
    uint64_t row_bytes;
    uint64_t image_bytes;
    if (size < 18u || data[1] != 0u || data[0] != 0u ||
        data[2] != 2u) return RIN_IMAGE_UNSUPPORTED;
    image_type = data[2];
    bits_per_pixel = data[16];
    width = rin_image_le16(data + 12u);
    height = rin_image_le16(data + 14u);
    if (image_type != 2u || width == 0u || height == 0u ||
        (bits_per_pixel != 24u && bits_per_pixel != 32u))
        return RIN_IMAGE_UNSUPPORTED;
    row_bytes = (uint64_t)width * ((uint64_t)bits_per_pixel / 8u);
    image_bytes = row_bytes * (uint64_t)height;
    if ((uint64_t)18u + (uint64_t)data[0] > (uint64_t)size ||
        image_bytes > (uint64_t)size - 18u - (uint64_t)data[0])
        return RIN_IMAGE_MALFORMED;
    probe->format = RIN_IMAGE_FORMAT_TGA;
    probe->size.width = width;
    probe->size.height = height;
    probe->frame_count = 1u;
    probe->kind = RIN_IMAGE_FRAME_STATIC;
    return RIN_IMAGE_OK;
}

typedef struct RinImageIconPayload {
    const uint8_t* data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_offset;
    uint32_t row_size;
    uint16_t bits_per_pixel;
    int png;
} RinImageIconPayload;

static RinImageStatus rin_image_icon_payload(const uint8_t* data, size_t size,
                                             RinImageProbe* probe,
                                             RinImageIconPayload* payload,
                                             const RinImageDecodeLimits* limits)
{
    uint16_t type;
    uint16_t count;
    uint32_t image_size;
    uint32_t image_offset;
    const uint8_t* image;
    size_t dib_size;
    uint32_t dib_width;
    uint32_t dib_height;
    uint16_t bits;
    uint32_t compression;
    uint64_t row_size;
    uint64_t pixel_bytes;
    RinImageProbe nested = {};
    RinImageStatus status;
    if (size < 22u || rin_image_le16(data) != 0u ||
        ((type = rin_image_le16(data + 2u)) != 1u && type != 2u) ||
        (count = rin_image_le16(data + 4u)) == 0u || count > 256u ||
        (uint64_t)6u + (uint64_t)count * 16u > (uint64_t)size)
        return RIN_IMAGE_MALFORMED;
    image_size = rin_image_le32(data + 6u + 8u);
    image_offset = rin_image_le32(data + 6u + 12u);
    if (image_size == 0u || (uint64_t)image_offset > (uint64_t)size ||
        (uint64_t)image_size > (uint64_t)size - (uint64_t)image_offset)
        return RIN_IMAGE_MALFORMED;
    image = data + image_offset;
    if (image_size >= 8u && image[0] == 0x89u && image[1] == 'P' &&
        image[2] == 'N' && image[3] == 'G') {
        status = rin_image_probe(image, image_size, limits, &nested);
        if (status != RIN_IMAGE_OK) return status;
        payload->data = image;
        payload->size = image_size;
        payload->width = nested.size.width;
        payload->height = nested.size.height;
        payload->pixel_offset = 0u;
        payload->row_size = 0u;
        payload->bits_per_pixel = 0u;
        payload->png = 1;
    } else {
        if (image_size < 40u) return RIN_IMAGE_UNSUPPORTED;
        dib_size = rin_image_le32(image);
        dib_width = rin_image_le32(image + 4u);
        dib_height = rin_image_le32(image + 8u);
        bits = rin_image_le16(image + 14u);
        compression = rin_image_le32(image + 16u);
        if (dib_size < 40u || dib_size > image_size || dib_width == 0u ||
            dib_height < 2u || (dib_height & 1u) != 0u ||
            (bits != 24u && bits != 32u) || compression != 0u)
            return RIN_IMAGE_UNSUPPORTED;
        row_size = (((uint64_t)dib_width * (uint64_t)bits) + 31u) /
                   32u * 4u;
        if (row_size > UINT64_MAX / (uint64_t)(dib_height / 2u))
            return RIN_IMAGE_OVERFLOW;
        pixel_bytes = row_size * (uint64_t)(dib_height / 2u);
        if (row_size > UINT32_MAX || (uint64_t)dib_size + pixel_bytes > image_size)
            return RIN_IMAGE_MALFORMED;
        payload->data = image;
        payload->size = image_size;
        payload->width = dib_width;
        payload->height = dib_height / 2u;
        payload->pixel_offset = (uint32_t)dib_size;
        payload->row_size = (uint32_t)row_size;
        payload->bits_per_pixel = bits;
        payload->png = 0;
    }
    probe->format = type == 1u ? RIN_IMAGE_FORMAT_ICO : RIN_IMAGE_FORMAT_CUR;
    probe->size.width = payload->width;
    probe->size.height = payload->height;
    probe->frame_count = 1u;
    probe->kind = RIN_IMAGE_FRAME_STATIC;
    return RIN_IMAGE_OK;
}

static RinImageStatus rin_image_codec_status(int result, int unsupported)
{
    if (result == unsupported) return RIN_IMAGE_UNSUPPORTED;
    return RIN_IMAGE_MALFORMED;
}

static RinImageStatus rin_image_admit(const RinImageDecodeLimits* limits,
                                      size_t source_bytes,
                                      RinImageProbe* probe)
{
    RinImageMetadata metadata;
    RinImageStatus status;
    if (!rin_image_limits_valid(limits) || probe == NULL ||
        probe->format == RIN_IMAGE_FORMAT_UNKNOWN || probe->frame_count == 0u)
        return RIN_IMAGE_INVALID_ARGUMENT;
    /* The audited codec adapters expose signed int dimensions and the
     * canonical frame descriptor exposes a 32-bit stride. */
    if (probe->size.width > (uint32_t)INT_MAX ||
        probe->size.height > (uint32_t)INT_MAX ||
        probe->size.width > UINT32_MAX / 4u)
        return RIN_IMAGE_LIMIT;

    metadata.canvas = probe->size;
    metadata.frame_count = probe->frame_count;
    metadata.kind = probe->kind;
    metadata.has_alpha = 0u;
    metadata.reserved[0] = metadata.reserved[1] = metadata.reserved[2] = 0u;
    status = rin_image_metadata_validate(limits, &metadata);
    if (status != RIN_IMAGE_OK) return status;

    /* This API publishes one frame.  Metadata still admits the complete
     * animation count, while output budgeting covers only the frame emitted. */
    return rin_image_validate_decode(
        limits, source_bytes, probe->size, 1u,
        RIN_IMAGE_PIXEL_ARGB8888);
}

static RinImageStatus rin_image_probe_bmp(const uint8_t* data, size_t size,
                                          RinImageProbe* probe)
{
    uint32_t data_offset;
    uint32_t dib_size;
    uint32_t width_raw;
    uint32_t height_raw;
    uint16_t planes;
    uint16_t bits_per_pixel;
    uint32_t compression;
    uint32_t height;
    uint64_t row_bits;
    uint64_t row_size;
    uint64_t image_bytes;

    if (size < 54u) return RIN_IMAGE_MALFORMED;
    if (data[0] != 'B' || data[1] != 'M') return RIN_IMAGE_MALFORMED;
    data_offset = rin_image_le32(data + 10u);
    dib_size = rin_image_le32(data + 14u);
    width_raw = rin_image_le32(data + 18u);
    height_raw = rin_image_le32(data + 22u);
    planes = rin_image_le16(data + 26u);
    bits_per_pixel = rin_image_le16(data + 28u);
    compression = rin_image_le32(data + 30u);
    if (dib_size < 40u || (uint64_t)dib_size > (uint64_t)size - 14u ||
        (uint64_t)data_offset < 14u + (uint64_t)dib_size ||
        (uint64_t)data_offset > (uint64_t)size || planes != 1u ||
        width_raw == 0u || (width_raw & UINT32_C(0x80000000)) != 0u ||
        height_raw == 0u || height_raw == UINT32_C(0x80000000) ||
        (bits_per_pixel != 24u && bits_per_pixel != 32u) || compression != 0u)
        return RIN_IMAGE_UNSUPPORTED;

    height = (height_raw & UINT32_C(0x80000000)) != 0u
                 ? (~height_raw + 1u) : height_raw;
    row_bits = (uint64_t)width_raw * (uint64_t)bits_per_pixel;
    row_size = ((row_bits + 31u) / 32u) * 4u;
    if (row_size > UINT64_MAX / (uint64_t)height)
        return RIN_IMAGE_OVERFLOW;
    image_bytes = row_size * (uint64_t)height;
    if (image_bytes > (uint64_t)size - (uint64_t)data_offset)
        return RIN_IMAGE_MALFORMED;

    probe->format = RIN_IMAGE_FORMAT_BMP;
    probe->size.width = width_raw;
    probe->size.height = height;
    probe->frame_count = 1u;
    probe->kind = RIN_IMAGE_FRAME_STATIC;
    return RIN_IMAGE_OK;
}

RinImageStatus rin_image_probe(const uint8_t* data, size_t source_bytes,
                               const RinImageDecodeLimits* limits,
                               RinImageProbe* probe)
{
    int width = 0;
    int height = 0;
    int frames = 1;
    RinImageStatus status;
    if (!rin_image_limits_valid(limits) || data == NULL || source_bytes == 0u ||
        probe == NULL)
        return RIN_IMAGE_INVALID_ARGUMENT;
    memset(probe, 0, sizeof(*probe));
    if (source_bytes > limits->max_source_bytes) return RIN_IMAGE_LIMIT;

    if (source_bytes >= 6u && data[0] == 0u && data[1] == 0u &&
        (data[2] == 1u || data[2] == 2u) && data[3] == 0u &&
        rin_image_le16(data + 4u) != 0u &&
        (uint64_t)6u + (uint64_t)rin_image_le16(data + 4u) * 16u <=
            (uint64_t)source_bytes) {
        RinImageIconPayload payload;
        status = rin_image_icon_payload(data, source_bytes, probe, &payload,
                                         limits);
    } else if (source_bytes >= 2u && data[0] == 'P' &&
               (data[1] == '3' || data[1] == '6')) {
        int ascii = 0;
        size_t payload_offset = 0u;
        status = rin_image_ppm_header(data, source_bytes, probe, &ascii,
                                      &payload_offset);
        (void)ascii;
        (void)payload_offset;
    } else if (source_bytes >= 18u && data[0] == 0u && data[1] == 0u &&
               data[2] == 2u) {
        status = rin_image_probe_tga(data, source_bytes, probe);
    } else if (source_bytes >= 2u && data[0] == 'B' && data[1] == 'M') {
        status = rin_image_probe_bmp(data, source_bytes, probe);
    } else if (source_bytes >= 2u && data[0] == 0xffu && data[1] == 0xd8u) {
        const int result = rjpeg_get_info(data, source_bytes, &width, &height);
        if (result != RJPEG_OK)
            return rin_image_codec_status(result, RJPEG_UNSUPPORTED);
        probe->format = RIN_IMAGE_FORMAT_JPEG;
        probe->size.width = (uint32_t)width;
        probe->size.height = (uint32_t)height;
        probe->frame_count = 1u;
        probe->kind = RIN_IMAGE_FRAME_STATIC;
        status = RIN_IMAGE_OK;
    } else if (source_bytes >= 3u && data[0] == 'G' && data[1] == 'I' &&
               data[2] == 'F') {
        const int result = rgif_get_info(data, source_bytes, &width, &height,
                                         &frames);
        if (result != RGIF_OK)
            return rin_image_codec_status(result, RGIF_UNSUPPORTED);
        probe->format = RIN_IMAGE_FORMAT_GIF;
        probe->size.width = (uint32_t)width;
        probe->size.height = (uint32_t)height;
        probe->frame_count = (uint32_t)frames;
        probe->kind = frames > 1 ? RIN_IMAGE_FRAME_ANIMATED
                                 : RIN_IMAGE_FRAME_STATIC;
        status = RIN_IMAGE_OK;
    } else if (source_bytes >= 4u && data[0] == 0x89u && data[1] == 'P' &&
               data[2] == 'N' && data[3] == 'G') {
        status = rin_image_probe_png(data, source_bytes, limits, probe);
    } else if (source_bytes >= 12u && data[0] == 'R' && data[1] == 'I' &&
               data[2] == 'F' && data[3] == 'F' && data[8] == 'W' &&
               data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        if (!WebPGetInfo(data, source_bytes, &width, &height))
            return RIN_IMAGE_MALFORMED;
        probe->format = RIN_IMAGE_FORMAT_WEBP;
        probe->size.width = (uint32_t)width;
        probe->size.height = (uint32_t)height;
        probe->frame_count = 1u;
        probe->kind = RIN_IMAGE_FRAME_STATIC;
        status = RIN_IMAGE_OK;
    } else {
        return RIN_IMAGE_UNSUPPORTED;
    }
    if (status != RIN_IMAGE_OK) return status;
    return rin_image_admit(limits, source_bytes, probe);
}

static RinImageStatus rin_image_decode_bmp(const uint8_t* data, size_t size,
                                           const RinImageProbe* probe,
                                           uint32_t* pixels,
                                           RinImageCancellationFunction cancellation,
                                           void* cancellation_context)
{
    const uint32_t data_offset = rin_image_le32(data + 10u);
    const uint16_t bits_per_pixel = rin_image_le16(data + 28u);
    const uint32_t height_raw = rin_image_le32(data + 22u);
    const size_t width = (size_t)probe->size.width;
    const size_t height = (size_t)probe->size.height;
    const size_t bytes_per_pixel = (size_t)bits_per_pixel / 8u;
    const size_t row_size = (size_t)((((uint64_t)width *
                                       (uint64_t)bits_per_pixel) + 31u) /
                                      32u * 4u);
    const int top_down = (height_raw & UINT32_C(0x80000000)) != 0u;
    size_t y;
    if (row_size == 0u || height > (size - (size_t)data_offset) / row_size)
        return RIN_IMAGE_MALFORMED;
    for (y = 0u; y < height; ++y) {
        if (cancellation != NULL && cancellation(cancellation_context))
            return RIN_IMAGE_CANCELLED;
        const size_t source_y = top_down ? y : height - 1u - y;
        const uint8_t* row = data + (size_t)data_offset + source_y * row_size;
        size_t x;
        for (x = 0u; x < width; ++x) {
            const uint8_t* pixel = row + x * bytes_per_pixel;
            const uint32_t alpha = bits_per_pixel == 32u ? pixel[3] : 0xffu;
            pixels[y * width + x] = (alpha << 24u) |
                                     ((uint32_t)pixel[2] << 16u) |
                                     ((uint32_t)pixel[1] << 8u) | pixel[0];
        }
    }
    return RIN_IMAGE_OK;
}

static RinImageStatus rin_image_decode_ppm(const uint8_t* data, size_t size,
                                           const RinImageProbe* probe,
                                           uint32_t* pixels,
                                           RinImageCancellationFunction cancellation,
                                           void* cancellation_context)
{
    char token[32];
    size_t pos = 0u;
    size_t payload_offset = 0u;
    int ascii = 0;
    RinImageProbe parsed_probe;
    uint64_t pixel_count = (uint64_t)probe->size.width *
                           (uint64_t)probe->size.height;
    if (rin_image_ppm_header(data, size, &parsed_probe, &ascii,
                             &payload_offset) != RIN_IMAGE_OK ||
        pixel_count > SIZE_MAX / 3u)
        return RIN_IMAGE_MALFORMED;
    pos = payload_offset;
    if (!ascii) {
        const size_t bytes = (size_t)pixel_count * 3u;
        size_t index;
        if (bytes > size - pos) return RIN_IMAGE_MALFORMED;
        for (index = 0u; index < (size_t)pixel_count; ++index) {
            if (cancellation != NULL && (index & 4095u) == 0u &&
                cancellation(cancellation_context))
                return RIN_IMAGE_CANCELLED;
            const uint8_t* rgb = data + pos + index * 3u;
            pixels[index] = UINT32_C(0xff000000) |
                            ((uint32_t)rgb[0] << 16u) |
                            ((uint32_t)rgb[1] << 8u) | rgb[2];
        }
        return RIN_IMAGE_OK;
    }
    {
        size_t index;
        for (index = 0u; index < (size_t)pixel_count; ++index) {
            uint32_t red;
            uint32_t green;
            uint32_t blue;
            if (cancellation != NULL && (index & 4095u) == 0u &&
                cancellation(cancellation_context))
                return RIN_IMAGE_CANCELLED;
            if (!rin_image_ppm_token(data, size, &pos, token, sizeof(token)) ||
                !rin_image_ppm_number(token, &red) || red > 255u ||
                !rin_image_ppm_token(data, size, &pos, token, sizeof(token)) ||
                !rin_image_ppm_number(token, &green) || green > 255u ||
                !rin_image_ppm_token(data, size, &pos, token, sizeof(token)) ||
                !rin_image_ppm_number(token, &blue) || blue > 255u)
                return RIN_IMAGE_MALFORMED;
            pixels[index] = UINT32_C(0xff000000) | (red << 16u) |
                            (green << 8u) | blue;
        }
    }
    return RIN_IMAGE_OK;
}

static RinImageStatus rin_image_decode_tga(const uint8_t* data, size_t size,
                                           const RinImageProbe* probe,
                                           uint32_t* pixels,
                                           RinImageCancellationFunction cancellation,
                                           void* cancellation_context)
{
    const size_t bytes_per_pixel = (size_t)data[16] / 8u;
    const size_t row_bytes = (size_t)probe->size.width * bytes_per_pixel;
    const size_t pixel_offset = 18u + (size_t)data[0];
    const int top_down = (data[17] & 0x20u) != 0u;
    size_t y;
    if (pixel_offset > size || probe->size.height >
        (size - pixel_offset) / row_bytes)
        return RIN_IMAGE_MALFORMED;
    for (y = 0u; y < (size_t)probe->size.height; ++y) {
        if (cancellation != NULL && cancellation(cancellation_context))
            return RIN_IMAGE_CANCELLED;
        const size_t source_y = top_down ? y :
            (size_t)probe->size.height - 1u - y;
        const uint8_t* row = data + pixel_offset + source_y * row_bytes;
        size_t x;
        for (x = 0u; x < (size_t)probe->size.width; ++x) {
            const uint8_t* pixel = row + x * bytes_per_pixel;
            const uint32_t alpha = bytes_per_pixel == 4u ? pixel[3] : 255u;
            pixels[y * (size_t)probe->size.width + x] = (alpha << 24u) |
                ((uint32_t)pixel[2] << 16u) | ((uint32_t)pixel[1] << 8u) |
                pixel[0];
        }
    }
    return RIN_IMAGE_OK;
}

static RinImageStatus rin_image_decode_icon(
    const uint8_t* data, size_t size, const RinImageDecodeLimits* limits,
    const RinImageProbe* probe, uint32_t* pixels, size_t pixel_capacity,
    uint8_t* scratch, size_t scratch_capacity,
    RinImageCancellationFunction cancellation, void* cancellation_context)
{
    RinImageIconPayload payload;
    RinImageProbe checked = {};
    RinImageStatus status = rin_image_icon_payload(data, size, &checked,
                                                   &payload, limits);
    size_t y;
    if (status != RIN_IMAGE_OK) return status;
    if (payload.width != probe->size.width || payload.height != probe->size.height)
        return RIN_IMAGE_MALFORMED;
    if (payload.png)
        return rin_image_decode_cancellable(
            payload.data, payload.size, limits, pixels, pixel_capacity, scratch,
            scratch_capacity, cancellation, cancellation_context, NULL);
    for (y = 0u; y < (size_t)probe->size.height; ++y) {
        if (cancellation != NULL && cancellation(cancellation_context))
            return RIN_IMAGE_CANCELLED;
        const size_t source_y = (size_t)probe->size.height - 1u - y;
        const uint8_t* row = payload.data + payload.pixel_offset +
                             source_y * payload.row_size;
        size_t x;
        for (x = 0u; x < (size_t)probe->size.width; ++x) {
            const uint8_t* pixel = row + x * ((size_t)payload.bits_per_pixel / 8u);
            const uint32_t alpha = payload.bits_per_pixel == 32u ? pixel[3] : 255u;
            pixels[y * (size_t)probe->size.width + x] = (alpha << 24u) |
                ((uint32_t)pixel[2] << 16u) | ((uint32_t)pixel[1] << 8u) |
                pixel[0];
        }
    }
    return RIN_IMAGE_OK;
}

RinImageStatus rin_image_decode_cancellable(
    const uint8_t* data, size_t source_bytes,
    const RinImageDecodeLimits* limits, uint32_t* pixels,
    size_t pixel_capacity, uint8_t* scratch, size_t scratch_capacity,
    RinImageCancellationFunction cancellation, void* cancellation_context,
    RinImageProbe* probe_out)
{
    RinImageProbe probe;
    RinImageStatus status;
    size_t pixel_count;
    size_t output_bytes;
    if (probe_out != NULL) memset(probe_out, 0, sizeof(*probe_out));
    if (pixels == NULL) return RIN_IMAGE_INVALID_ARGUMENT;
    status = rin_image_probe(data, source_bytes, limits, &probe);
    if (status != RIN_IMAGE_OK) return status;
    pixel_count = (size_t)probe.size.width * (size_t)probe.size.height;
    output_bytes = pixel_count * sizeof(uint32_t);
    if (pixel_count == 0u || pixel_count > pixel_capacity ||
        output_bytes > limits->max_output_bytes) return RIN_IMAGE_LIMIT;
    if (cancellation != NULL && cancellation(cancellation_context)) {
        memset(pixels, 0, output_bytes);
        return RIN_IMAGE_CANCELLED;
    }

    switch (probe.format) {
    case RIN_IMAGE_FORMAT_JPEG:
        if (rjpeg_decode(data, source_bytes, pixels, pixel_capacity,
                         (int)probe.size.width, (int)probe.size.height) != RJPEG_OK)
            status = RIN_IMAGE_MALFORMED;
        else
            status = RIN_IMAGE_OK;
        break;
    case RIN_IMAGE_FORMAT_GIF:
        if (scratch == NULL || scratch_capacity < pixel_count)
            return RIN_IMAGE_LIMIT;
        status = rgif_decode(data, source_bytes, pixels,
                             (int)probe.size.width, (int)probe.size.height,
                             scratch, scratch_capacity) == RGIF_OK
                     ? RIN_IMAGE_OK : RIN_IMAGE_MALFORMED;
        break;
    case RIN_IMAGE_FORMAT_PNG:
        status = rin_image_decode_png(data, source_bytes, limits, pixels,
                                      pixel_capacity, NULL);
        break;
    case RIN_IMAGE_FORMAT_WEBP: {
        size_t index;
        if (scratch == NULL || scratch_capacity < output_bytes)
            return RIN_IMAGE_LIMIT;
        if (WebPDecodeBGRAInto(data, source_bytes, scratch, output_bytes,
                               (int)probe.size.width * 4) == NULL) {
            status = RIN_IMAGE_MALFORMED;
            break;
        }
        for (index = 0u; index < pixel_count; ++index) {
            const uint8_t* bgra = scratch + index * 4u;
            pixels[index] = ((uint32_t)bgra[3] << 24u) |
                            ((uint32_t)bgra[2] << 16u) |
                            ((uint32_t)bgra[1] << 8u) | bgra[0];
        }
        status = RIN_IMAGE_OK;
        break;
    }
    case RIN_IMAGE_FORMAT_BMP:
        status = rin_image_decode_bmp(data, source_bytes, &probe, pixels,
                                      cancellation, cancellation_context);
        break;
    case RIN_IMAGE_FORMAT_PPM:
        status = rin_image_decode_ppm(data, source_bytes, &probe, pixels,
                                      cancellation, cancellation_context);
        break;
    case RIN_IMAGE_FORMAT_TGA:
        status = rin_image_decode_tga(data, source_bytes, &probe, pixels,
                                      cancellation, cancellation_context);
        break;
    case RIN_IMAGE_FORMAT_ICO:
    case RIN_IMAGE_FORMAT_CUR:
        status = rin_image_decode_icon(data, source_bytes, limits, &probe,
                                       pixels, pixel_capacity, scratch,
                                       scratch_capacity, cancellation,
                                       cancellation_context);
        break;
    default:
        status = RIN_IMAGE_UNSUPPORTED;
        break;
    }
    if (status == RIN_IMAGE_OK && cancellation != NULL &&
        cancellation(cancellation_context))
        status = RIN_IMAGE_CANCELLED;
    if (status != RIN_IMAGE_OK) {
        memset(pixels, 0, output_bytes);
        return status;
    }

    {
        RinImageFrame frame = {};
        frame.size = probe.size;
        frame.stride_bytes = probe.size.width * 4u;
        frame.pixel_format = RIN_IMAGE_PIXEL_ARGB8888;
        frame.kind = probe.kind;
        frame.pixels = pixels;
        frame.pixel_bytes = output_bytes;
        status = rin_image_frame_validate(limits, &frame);
    }
    if (status != RIN_IMAGE_OK) {
        memset(pixels, 0, output_bytes);
        return status;
    }
    if (probe_out != NULL) *probe_out = probe;
    return RIN_IMAGE_OK;
}

RinImageStatus rin_image_decode(const uint8_t* data, size_t source_bytes,
                                const RinImageDecodeLimits* limits,
                                uint32_t* pixels, size_t pixel_capacity,
                                uint8_t* scratch, size_t scratch_capacity,
                                RinImageProbe* probe_out)
{
    return rin_image_decode_cancellable(
        data, source_bytes, limits, pixels, pixel_capacity, scratch,
        scratch_capacity, NULL, NULL, probe_out);
}

static RinImageStatus rin_image_resource_status(
    RinResourceCatalogStatus status)
{
    switch (status) {
        case RIN_RESOURCE_CATALOG_BUFFER_TOO_SMALL:
            return RIN_IMAGE_LIMIT;
        case RIN_RESOURCE_CATALOG_INVALID_LAYOUT:
            return RIN_IMAGE_MALFORMED;
        case RIN_RESOURCE_CATALOG_UNSUPPORTED_VERSION:
            return RIN_IMAGE_UNSUPPORTED;
        case RIN_RESOURCE_CATALOG_IO_ERROR:
            return RIN_IMAGE_SERVICE_UNAVAILABLE;
        case RIN_RESOURCE_CATALOG_OK:
            return RIN_IMAGE_OK;
        default:
            return RIN_IMAGE_INVALID_ARGUMENT;
    }
}

RinImageStatus rin_image_decode_resource(
    const RinResourceCatalogV1* catalog, uint32_t resource_id,
    RinResourceCatalogReadPathFunction read_path, void* context,
    uint8_t* source, size_t source_capacity, size_t* source_size_out,
    const RinImageDecodeLimits* limits, uint32_t* pixels, size_t pixel_capacity,
    uint8_t* scratch, size_t scratch_capacity, RinImageProbe* probe_out)
{
    RinResourceCatalogStatus resource_status;
    RinImageStatus status;
    uint64_t loaded_size = 0u;

    if (source_size_out == NULL || probe_out == NULL) {
        if (source_size_out != NULL) *source_size_out = 0u;
        if (probe_out != NULL) memset(probe_out, 0, sizeof(*probe_out));
        return RIN_IMAGE_INVALID_ARGUMENT;
    }
    *source_size_out = 0u;
    memset(probe_out, 0, sizeof(*probe_out));
    if (source_capacity != 0u && source == NULL)
        return RIN_IMAGE_INVALID_ARGUMENT;

    resource_status = rin_resource_catalog_load(
        catalog, RIN_RESOURCE_CATALOG_TYPE_IMAGE, resource_id, read_path,
        context, source, (uint64_t)source_capacity, &loaded_size);
    status = rin_image_resource_status(resource_status);
    if (status != RIN_IMAGE_OK || loaded_size > SIZE_MAX || loaded_size == 0u)
        return status == RIN_IMAGE_OK ? RIN_IMAGE_MALFORMED : status;

    status = rin_image_decode(source, (size_t)loaded_size, limits, pixels,
                              pixel_capacity, scratch, scratch_capacity,
                              probe_out);
    if (status != RIN_IMAGE_OK) {
        *source_size_out = 0u;
        memset(probe_out, 0, sizeof(*probe_out));
        return status;
    }
    *source_size_out = (size_t)loaded_size;
    return RIN_IMAGE_OK;
}
