/* SPDX-License-Identifier: MIT */
#ifndef RINIMAGE_IMAGE_H
#define RINIMAGE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The common image contract uses a numeric 0xAARRGGBB pixel word.  This is
 * deliberately independent of the host byte order; codec adapters must
 * convert their native byte layout before publishing a frame. */
typedef enum RinImagePixelFormat {
    RIN_IMAGE_PIXEL_UNKNOWN = 0,
    RIN_IMAGE_PIXEL_ARGB8888 = 1
} RinImagePixelFormat;

typedef enum RinImageStatus {
    RIN_IMAGE_OK = 0,
    RIN_IMAGE_INVALID_ARGUMENT = -1,
    RIN_IMAGE_UNSUPPORTED = -2,
    RIN_IMAGE_LIMIT = -3,
    RIN_IMAGE_OVERFLOW = -4,
    RIN_IMAGE_MALFORMED = -5,
    RIN_IMAGE_CANCELLED = -6,
    RIN_IMAGE_TIMEOUT = -7,
    RIN_IMAGE_AUTHORIZATION = -8,
    /* The source owner/transport failed before a frame could be published.
     * Callers may retry only after re-establishing a fresh authorized source. */
    RIN_IMAGE_SERVICE_UNAVAILABLE = -9
} RinImageStatus;

typedef struct RinImageSize {
    uint32_t width;
    uint32_t height;
} RinImageSize;

typedef struct RinImageDecodeLimits {
    size_t max_source_bytes;
    uint32_t max_width;
    uint32_t max_height;
    uint64_t max_pixels;
    uint32_t max_frames;
    size_t max_output_bytes;
} RinImageDecodeLimits;

typedef enum RinImageFrameKind {
    RIN_IMAGE_FRAME_STATIC = 0,
    RIN_IMAGE_FRAME_ANIMATED = 1
} RinImageFrameKind;

/* A frame is a non-owning output descriptor.  The decoder or consumer that
 * owns pixels remains responsible for their lifetime; this model only checks
 * the published bounds and format. */
typedef struct RinImageFrame {
    RinImageSize size;
    uint32_t stride_bytes;
    RinImagePixelFormat pixel_format;
    RinImageFrameKind kind;
    uint32_t duration_ms;
    const uint32_t* pixels;
    size_t pixel_bytes;
} RinImageFrame;

/* A bounded source region in a canonical ARGB frame. */
typedef struct RinImageRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} RinImageRect;

/* Output limits for allocation-free nearest-neighbor scaling.  The caller
 * owns both the source and destination buffers. */
typedef struct RinImageScaleLimits {
    uint32_t max_width;
    uint32_t max_height;
    uint64_t max_pixels;
    size_t max_output_bytes;
} RinImageScaleLimits;

typedef struct RinImageMetadata {
    RinImageSize canvas;
    uint32_t frame_count;
    RinImageFrameKind kind;
    uint8_t has_alpha;
    uint8_t reserved[3];
} RinImageMetadata;

/* Stable defaults shared by untrusted image consumers.  They match the
 * current bounded codec policy, while callers may lower them for thumbnails
 * or UI resources. */
void rin_image_decode_limits_default(RinImageDecodeLimits* limits);

/* Stable bounds for thumbnail and UI render targets. */
void rin_image_scale_limits_default(RinImageScaleLimits* limits);

/* Scale a validated ARGB frame region without allocating or publishing a
 * partial frame.  The source region is mapped with nearest-neighbor sampling;
 * output_frame is cleared on failure and points at output_pixels on success. */
RinImageStatus rin_image_scale_nearest(const RinImageFrame* source,
                                       RinImageRect source_region,
                                       RinImageSize output_size,
                                       const RinImageScaleLimits* limits,
                                       uint32_t* output_pixels,
                                       size_t output_capacity,
                                       RinImageFrame* output_frame);

/* Generate one aspect-preserving thumbnail from a frame.  The source frame
 * is treated as one static image; limits are also used for the output pixel
 * and byte budgets. */
RinImageStatus rin_image_thumbnail(const RinImageFrame* source,
                                   const RinImageScaleLimits* limits,
                                   uint32_t* output_pixels,
                                   size_t output_capacity,
                                   RinImageFrame* output_frame);

/* Check dimensions, frame count, source size and the canonical ARGB output
 * budget before invoking a codec. */
RinImageStatus rin_image_validate_decode(const RinImageDecodeLimits* limits,
                                         size_t source_bytes,
                                         RinImageSize size,
                                         uint32_t frame_count,
                                         RinImagePixelFormat pixel_format);

/* Validate a frame descriptor received from a decoder/service. */
RinImageStatus rin_image_frame_validate(const RinImageDecodeLimits* limits,
                                        const RinImageFrame* frame);

/* Validate metadata independently of a pixel buffer. */
RinImageStatus rin_image_metadata_validate(const RinImageDecodeLimits* limits,
                                           const RinImageMetadata* metadata);

#ifdef __cplusplus
}
#endif

#endif /* RINIMAGE_IMAGE_H */
