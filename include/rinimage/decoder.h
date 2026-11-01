/* SPDX-License-Identifier: MIT */
#ifndef RINIMAGE_DECODER_H
#define RINIMAGE_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "image.h"
#include <rinresource/loader.h>

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
    uint32_t loop_count;
    uint32_t flags;
} RinImageProbe;

enum {
    /* loop_count is the total number of plays; zero means infinite. */
    RIN_IMAGE_PROBE_LOOP_COUNT_KNOWN = 1u << 0
};

typedef int (*RinImageCancellationFunction)(void* context);

/* Probe a bounded PNG without allocating.  This small backend entry is also
 * usable by compatibility adapters that must not link the complete
 * multi-format decoder. */
RinImageStatus rin_image_probe_png(const uint8_t* data, size_t source_bytes,
                                   const RinImageDecodeLimits* limits,
                                   RinImageProbe* probe_out);
RinImageStatus rin_image_probe_png_cancellable(
    const uint8_t* data, size_t source_bytes,
    const RinImageDecodeLimits* limits, RinImageProbe* probe_out,
    RinImageCancellationFunction cancellation, void* cancellation_context);

/* Decode a bounded PNG into canonical ARGB words. */
RinImageStatus rin_image_decode_png(const uint8_t* data, size_t source_bytes,
                                    const RinImageDecodeLimits* limits,
                                    uint32_t* pixels, size_t pixel_capacity,
                                    RinImageProbe* probe_out);

/* Deadline/cancellation-aware bounded PNG decode.  Cancellation is polled by
 * the PNG parser, inflate, unfilter, and pixel conversion loops. */
RinImageStatus rin_image_decode_png_cancellable(
    const uint8_t* data, size_t source_bytes,
    const RinImageDecodeLimits* limits, uint32_t* pixels,
    size_t pixel_capacity, RinImageCancellationFunction cancellation,
    void* cancellation_context, RinImageProbe* probe_out);

/* Probe and validate an untrusted encoded image without allocating. */
RinImageStatus rin_image_probe(const uint8_t* data, size_t source_bytes,
                               const RinImageDecodeLimits* limits,
                               RinImageProbe* probe);

/*
 * Decode frame zero into canonical 0xAARRGGBB words. The output is published
 * only after a successful return. GIF uses scratch for LZW indices and, for
 * later frames, an aligned canvas restore snapshot. WebP uses scratch as a
 * BGRA staging buffer (four bytes per canvas pixel). Other formats do not
 * require scratch.
 */
RinImageStatus rin_image_decode(const uint8_t* data, size_t source_bytes,
                                const RinImageDecodeLimits* limits,
                                uint32_t* pixels, size_t pixel_capacity,
                                uint8_t* scratch, size_t scratch_capacity,
                                RinImageProbe* probe_out);

/* Resolve one public TYPE_IMAGE resource into caller-owned source storage and
 * decode its first frame. The catalog loader performs no filesystem access;
 * source_size_out and probe_out are cleared before a failure is returned. */
RinImageStatus rin_image_decode_resource(
    const RinResourceCatalogV1* catalog, uint32_t resource_id,
    RinResourceCatalogReadPathFunction read_path, void* context,
    uint8_t* source, size_t source_capacity, size_t* source_size_out,
    const RinImageDecodeLimits* limits, uint32_t* pixels, size_t pixel_capacity,
    uint8_t* scratch, size_t scratch_capacity, RinImageProbe* probe_out);

/* Cancellation-aware form used by a service owner. The callback is polled
 * during supported codec parsing, decode, and bounded row/plane processing;
 * it never owns decoder state and cancellation publishes no frame. */
RinImageStatus rin_image_decode_cancellable(
    const uint8_t* data, size_t source_bytes,
    const RinImageDecodeLimits* limits, uint32_t* pixels,
    size_t pixel_capacity, uint8_t* scratch, size_t scratch_capacity,
    RinImageCancellationFunction cancellation, void* cancellation_context,
    RinImageProbe* probe_out);

/* Compatibility service entry for the original first-frame contract. It
 * keeps the single-frame output budget while also returning frame timing. */
RinImageStatus rin_image_decode_first_frame_cancellable(
    const uint8_t* data, size_t source_bytes,
    const RinImageDecodeLimits* limits, uint32_t* pixels,
    size_t pixel_capacity, uint8_t* scratch, size_t scratch_capacity,
    RinImageCancellationFunction cancellation, void* cancellation_context,
    RinImageProbe* probe_out, uint32_t* duration_ms_out);

/* Decode one composited canvas frame by its zero-based source index. Static
 * formats accept only index zero. Frame pixels are published only after the
 * complete frame succeeds; probe_out reports source sequence and loop data.
 * A known loop_count is total plays (zero means infinite), and duration_ms_out
 * receives the selected frame delay (zero for stills). */
RinImageStatus rin_image_decode_frame_cancellable(
    const uint8_t* data, size_t source_bytes,
    const RinImageDecodeLimits* limits, uint32_t frame_index,
    uint32_t* pixels, size_t pixel_capacity, uint8_t* scratch,
    size_t scratch_capacity, RinImageCancellationFunction cancellation,
    void* cancellation_context, RinImageProbe* probe_out,
    uint32_t* duration_ms_out);

#ifdef __cplusplus
}
#endif

#endif /* RINIMAGE_DECODER_H */
