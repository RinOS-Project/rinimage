/* SPDX-License-Identifier: MIT */

#include "include/rinimage/service.h"

#include <limits.h>
#include <string.h>

enum { RIN_IMAGE_SERVICE_READ_CHUNK = 4096 };
enum { RIN_IMAGE_SERVICE_MAX_SOURCE = 64 * 1024 * 1024 };
enum { RIN_IMAGE_SERVICE_MAX_DIMENSION = 4096 };
enum { RIN_IMAGE_SERVICE_MAX_FRAMES = 1024 };
enum { RIN_IMAGE_SERVICE_MAX_OUTPUT = 64 * 1024 * 1024 };

static int rin_image_service_limits_valid(
    const RinImageDecodeLimits* limits)
{
    return limits != NULL && limits->max_source_bytes != 0u &&
           limits->max_source_bytes <= RIN_IMAGE_SERVICE_MAX_SOURCE &&
           limits->max_width != 0u &&
           limits->max_width <= RIN_IMAGE_SERVICE_MAX_DIMENSION &&
           limits->max_height != 0u &&
           limits->max_height <= RIN_IMAGE_SERVICE_MAX_DIMENSION &&
           limits->max_pixels != 0u &&
           limits->max_pixels <= (uint64_t)RIN_IMAGE_SERVICE_MAX_DIMENSION *
                                      RIN_IMAGE_SERVICE_MAX_DIMENSION &&
           limits->max_frames != 0u &&
           limits->max_frames <= RIN_IMAGE_SERVICE_MAX_FRAMES &&
           limits->max_output_bytes != 0u &&
           limits->max_output_bytes <= RIN_IMAGE_SERVICE_MAX_OUTPUT;
}

typedef struct RinImageServicePollContext {
    const RinImageServiceDecodeRequest* request;
    uint64_t start_ticks;
    RinImageStatus status;
} RinImageServicePollContext;

static void rin_image_service_clear_outputs(
    const RinImageServiceDecodeRequest* request, RinImageFrame* frame_out)
{
    size_t pixel_bytes = 0u;
    size_t scratch_bytes = 0u;
    if (frame_out != NULL) memset(frame_out, 0, sizeof(*frame_out));
    if (request == NULL) return;
    /* The decoder can publish at most the service output limit.  Clear that
     * bounded prefix even when a caller supplies a larger backing region;
     * skipping the clear for oversized capacities would leave stale pixels
     * visible after a malformed-source failure. */
    if (request->pixels != NULL) {
        const size_t max_pixels = RIN_IMAGE_SERVICE_MAX_OUTPUT / sizeof(uint32_t);
        const size_t clear_pixels = request->pixel_capacity < max_pixels
            ? request->pixel_capacity : max_pixels;
        if (clear_pixels <= SIZE_MAX / sizeof(uint32_t))
            pixel_bytes = clear_pixels * sizeof(uint32_t);
        if (pixel_bytes != 0u) memset(request->pixels, 0, pixel_bytes);
    }
    if (request->scratch != NULL) {
        scratch_bytes = request->scratch_capacity < RIN_IMAGE_SERVICE_MAX_OUTPUT
            ? request->scratch_capacity : RIN_IMAGE_SERVICE_MAX_OUTPUT;
        if (scratch_bytes != 0u) memset(request->scratch, 0, scratch_bytes);
    }
    if (request->source != NULL && request->source_buffer != NULL &&
        request->source->size <= RIN_IMAGE_SERVICE_MAX_SOURCE)
        memset(request->source_buffer, 0, request->source->size);
}

static RinImageStatus rin_image_service_poll(
    RinImageServicePollContext* poll, int check_authorization)
{
    const RinImageServiceDecodeRequest* request;
    const RinImageServiceSource* source;
    uint64_t now;

    if (poll == NULL || poll->request == NULL) return RIN_IMAGE_INVALID_ARGUMENT;
    request = poll->request;
    source = request->source;
    if (check_authorization && source->authorize != NULL &&
        !source->authorize(source->context, request->capability,
                           request->generation)) {
        poll->status = RIN_IMAGE_AUTHORIZATION;
        return poll->status;
    }
    if (request->cancellation != NULL &&
        request->cancellation(request->cancellation_context)) {
        poll->status = RIN_IMAGE_CANCELLED;
        return poll->status;
    }
    now = request->clock(request->clock_context);
    if (now < poll->start_ticks || now - poll->start_ticks >=
                                      request->timeout_ticks) {
        poll->status = RIN_IMAGE_TIMEOUT;
        return poll->status;
    }
    return RIN_IMAGE_OK;
}

static int rin_image_service_decoder_cancel(void* context)
{
    RinImageServicePollContext* poll = (RinImageServicePollContext*)context;
    return rin_image_service_poll(poll, 1) != RIN_IMAGE_OK;
}

static RinImageStatus rin_image_service_copy_source(
    const RinImageServiceDecodeRequest* request,
    RinImageServicePollContext* poll)
{
    const RinImageServiceSource* source = request->source;
    size_t offset = 0u;

    while (offset < source->size) {
        const size_t remaining = source->size - offset;
        const size_t capacity = remaining < RIN_IMAGE_SERVICE_READ_CHUNK
                                    ? remaining
                                    : RIN_IMAGE_SERVICE_READ_CHUNK;
        size_t bytes_read = 0u;
        RinImageStatus status = rin_image_service_poll(poll, 1);
        if (status != RIN_IMAGE_OK) return status;
        if (source->read(source->context, offset,
                         request->source_buffer + offset, capacity,
                         &bytes_read) != 0)
            return RIN_IMAGE_SERVICE_UNAVAILABLE;
        if (bytes_read == 0u || bytes_read > capacity)
            return RIN_IMAGE_MALFORMED;
        if (bytes_read > source->size - offset) return RIN_IMAGE_MALFORMED;
        offset += bytes_read;
    }
    return rin_image_service_poll(poll, 1);
}

RinImageStatus rin_image_service_decode(
    const RinImageServiceDecodeRequest* request, RinImageFrame* frame_out)
{
    RinImageServicePollContext poll;
    RinImageProbe probe;
    RinImageFrame frame;
    RinImageStatus status;
    uint64_t start_ticks;
    uint64_t pixel_bytes;

    rin_image_service_clear_outputs(request, frame_out);
    if (request == NULL || frame_out == NULL || request->source == NULL ||
        request->source->read == NULL || request->source->authorize == NULL ||
        request->source->capability == 0u ||
        request->source->generation == 0u || request->source->size == 0u ||
        request->capability == 0u || request->generation == 0u ||
        request->source_buffer == NULL ||
        request->source_capacity < request->source->size ||
        request->timeout_ticks == 0u || request->clock == NULL)
        return RIN_IMAGE_INVALID_ARGUMENT;
    if (request->capability != request->source->capability ||
        request->generation != request->source->generation)
        return RIN_IMAGE_AUTHORIZATION;
    if (!rin_image_service_limits_valid(&request->limits))
        return RIN_IMAGE_INVALID_ARGUMENT;
    if (request->source->size > request->limits.max_source_bytes)
        return RIN_IMAGE_LIMIT;

    start_ticks = request->clock(request->clock_context);
    poll.request = request;
    poll.start_ticks = start_ticks;
    poll.status = RIN_IMAGE_OK;

    status = rin_image_service_poll(&poll, 1);
    if (status == RIN_IMAGE_OK)
        status = rin_image_service_copy_source(request, &poll);
    if (status != RIN_IMAGE_OK) return status;

    memset(&probe, 0, sizeof(probe));
    status = rin_image_decode_cancellable(
        request->source_buffer, request->source->size, &request->limits,
        request->pixels, request->pixel_capacity, request->scratch,
        request->scratch_capacity, rin_image_service_decoder_cancel, &poll,
        &probe);
    if (status != RIN_IMAGE_OK) {
        if (poll.status != RIN_IMAGE_OK) status = poll.status;
        return status;
    }

    pixel_bytes = (uint64_t)probe.size.width * (uint64_t)probe.size.height * 4u;
    if ((uint64_t)probe.size.width * 4u > UINT32_MAX ||
        pixel_bytes > SIZE_MAX) {
        return RIN_IMAGE_OVERFLOW;
    }
    memset(&frame, 0, sizeof(frame));
    frame.size = probe.size;
    frame.stride_bytes = probe.size.width * 4u;
    frame.pixel_format = RIN_IMAGE_PIXEL_ARGB8888;
    frame.kind = probe.kind;
    frame.pixels = request->pixels;
    frame.pixel_bytes = (size_t)pixel_bytes;
    status = rin_image_frame_validate(&request->limits, &frame);
    if (status != RIN_IMAGE_OK) return status;
    status = rin_image_service_poll(&poll, 1);
    if (status != RIN_IMAGE_OK) return status;
    *frame_out = frame;
    return RIN_IMAGE_OK;
}
