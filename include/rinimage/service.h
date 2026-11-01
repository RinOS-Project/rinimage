/* SPDX-License-Identifier: MIT */
#ifndef RINIMAGE_SERVICE_H
#define RINIMAGE_SERVICE_H

#include <stddef.h>
#include <stdint.h>

#include "decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The service never accepts a pathname or a caller-selected shared-memory
 * name.  The owner presents an opaque capability and a bounded source view;
 * the service asks the owner to copy bytes into caller-owned storage. */
typedef int (*RinImageServiceSourceReadFunction)(
    void* context, size_t offset, uint8_t* output, size_t output_capacity,
    size_t* bytes_read);

/* A non-zero read result means that the source owner/transport is no longer
 * available.  The service returns RIN_IMAGE_SERVICE_UNAVAILABLE and never
 * publishes a partial frame. */

/* Return non-zero only while the capability and generation are live.  The
 * service calls this before each bounded source read and while decoding. */
typedef int (*RinImageServiceSourceAuthorizeFunction)(
    void* context, uint64_t capability, uint64_t generation);

typedef uint64_t (*RinImageServiceClockFunction)(void* context);

enum {
    /* Apply the aggregate canvas-sequence output budget. Without this flag,
     * service requests retain the original frame-zero-only behavior. */
    RIN_IMAGE_SERVICE_DECODE_FLAG_ENFORCE_AGGREGATE_OUTPUT = 1u << 0
};

typedef struct RinImageServiceSource {
    uint64_t capability;
    uint64_t generation;
    size_t size;
    RinImageServiceSourceReadFunction read;
    RinImageServiceSourceAuthorizeFunction authorize;
    void* context;
} RinImageServiceSource;

typedef struct RinImageServiceDecodeRequest {
    const RinImageServiceSource* source;
    uint64_t capability;
    uint64_t generation;
    uint32_t frame_index; /* zero-based selected source frame */
    uint32_t flags;
    uint8_t* source_buffer;
    size_t source_capacity;
    RinImageDecodeLimits limits;
    uint32_t* pixels;
    size_t pixel_capacity;
    uint8_t* scratch;
    size_t scratch_capacity;
    RinImageCancellationFunction cancellation;
    void* cancellation_context;
    RinImageServiceClockFunction clock;
    void* clock_context;
    uint64_t timeout_ticks;
} RinImageServiceDecodeRequest;

/* Decode one bounded selected frame through the service boundary. frame_out is
 * a non-owning descriptor into pixels and is cleared on every failure.  A
 * successful call leaves source_buffer and pixels owned by the caller. */
RinImageStatus rin_image_service_decode(
    const RinImageServiceDecodeRequest* request, RinImageFrame* frame_out);

/* Probe-bearing form for owners that publish decoded frame metadata.
 * probe_out is zeroed on failure and receives the source's total frame count,
 * loop policy, kind, format, and canvas on success. */
RinImageStatus rin_image_service_decode_with_probe(
    const RinImageServiceDecodeRequest* request, RinImageFrame* frame_out,
    RinImageProbe* probe_out);

#ifdef __cplusplus
}
#endif

#endif /* RINIMAGE_SERVICE_H */
