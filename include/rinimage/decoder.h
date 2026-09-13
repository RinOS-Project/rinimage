/* SPDX-License-Identifier: MIT */
#ifndef RINIMAGE_DECODER_H
#define RINIMAGE_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RinImageFormat {
    RIN_IMAGE_FORMAT_UNKNOWN = 0,
    RIN_IMAGE_FORMAT_PNG = 1,
    RIN_IMAGE_FORMAT_JPEG = 2,
    RIN_IMAGE_FORMAT_GIF = 3,
    RIN_IMAGE_FORMAT_WEBP = 4,
    RIN_IMAGE_FORMAT_BMP = 5,
    RIN_IMAGE_FORMAT_ICO = 6,
    RIN_IMAGE_FORMAT_CUR = 7,
    RIN_IMAGE_FORMAT_TGA = 8,
    RIN_IMAGE_FORMAT_PPM = 9
} RinImageFormat;

typedef struct RinImageProbe {
    RinImageFormat format;
    RinImageSize size;
    uint32_t frame_count;
    RinImageFrameKind kind;
} RinImageProbe;

typedef int (*RinImageCancellationFunction)(void* context);

/* Probe and validate an untrusted encoded image without allocating. */
RinImageStatus rin_image_probe(const uint8_t* data, size_t source_bytes,
                               const RinImageDecodeLimits* limits,
                               RinImageProbe* probe);

/*
 * Decode the first frame into canonical 0xAARRGGBB words.  The output is
 * published only after a successful return.  GIF uses scratch as an index
 * buffer (one byte per canvas pixel); WebP uses it as a BGRA staging buffer
 * (four bytes per pixel).  Other formats do not require scratch.
 */
RinImageStatus rin_image_decode(const uint8_t* data, size_t source_bytes,
                                const RinImageDecodeLimits* limits,
                                uint32_t* pixels, size_t pixel_capacity,
                                uint8_t* scratch, size_t scratch_capacity,
                                RinImageProbe* probe_out);

/* Cancellation-aware form used by a service owner.  The callback is polled
 * at admission, codec boundaries, and bounded row/plane loops; it never owns
 * decoder state and a cancellation result publishes no frame. */
RinImageStatus rin_image_decode_cancellable(
    const uint8_t* data, size_t source_bytes,
    const RinImageDecodeLimits* limits, uint32_t* pixels,
    size_t pixel_capacity, uint8_t* scratch, size_t scratch_capacity,
    RinImageCancellationFunction cancellation, void* cancellation_context,
    RinImageProbe* probe_out);

#ifdef __cplusplus
}
#endif

#endif /* RINIMAGE_DECODER_H */
