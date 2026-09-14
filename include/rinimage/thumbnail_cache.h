/* SPDX-License-Identifier: MIT */
/* Fixed-capacity thumbnail cache and service-owner work queue. */

#ifndef RINIMAGE_THUMBNAIL_CACHE_H
#define RINIMAGE_THUMBNAIL_CACHE_H

#include "image.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RIN_IMAGE_THUMBNAIL_CACHE_VERSION_1 UINT16_C(1)
#define RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES UINT32_C(32)
#define RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS UINT32_C(32)
#define RIN_IMAGE_THUMBNAIL_CACHE_MAX_WIDTH UINT32_C(1024)
#define RIN_IMAGE_THUMBNAIL_CACHE_MAX_HEIGHT UINT32_C(1024)
#define RIN_IMAGE_THUMBNAIL_CACHE_MAX_PIXELS UINT64_C(262144)
#define RIN_IMAGE_THUMBNAIL_CACHE_MAX_BYTES UINT64_C(1048576)

typedef enum RinImageThumbnailCacheStatus {
    RIN_IMAGE_THUMBNAIL_CACHE_OK = 0,
    RIN_IMAGE_THUMBNAIL_CACHE_HIT = 1,
    RIN_IMAGE_THUMBNAIL_CACHE_ALREADY_QUEUED = 2,
    RIN_IMAGE_THUMBNAIL_CACHE_NOT_FOUND = 3,
    RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT = 4,
    RIN_IMAGE_THUMBNAIL_CACHE_INVALID_LAYOUT = 5,
    RIN_IMAGE_THUMBNAIL_CACHE_BUFFER_TOO_SMALL = 6,
    RIN_IMAGE_THUMBNAIL_CACHE_QUEUE_FULL = 7,
    RIN_IMAGE_THUMBNAIL_CACHE_BUSY = 8
} RinImageThumbnailCacheStatus;

typedef struct RinImageThumbnailCacheKeyV1 {
    uint64_t source_id;
    uint64_t source_generation;
    uint32_t width;
    uint32_t height;
    uint32_t reserved[2];
} RinImageThumbnailCacheKeyV1;

typedef struct RinImageThumbnailCacheRequestV1 {
    uint64_t request_id;
    RinImageThumbnailCacheKeyV1 key;
    uint32_t priority;
    uint32_t reserved;
} RinImageThumbnailCacheRequestV1;

typedef struct RinImageThumbnailCacheSlotV1 {
    uint32_t* pixels;
    size_t pixel_capacity;
    uint32_t pixel_count;
    uint32_t priority;
    uint64_t last_used;
    RinImageThumbnailCacheKeyV1 key;
    uint8_t valid;
    uint8_t reserved[7];
} RinImageThumbnailCacheSlotV1;

typedef struct RinImageThumbnailCachePendingV1 {
    RinImageThumbnailCacheRequestV1 request;
    uint64_t sequence;
    uint8_t state;
    uint8_t reserved[7];
} RinImageThumbnailCachePendingV1;

typedef struct RinImageThumbnailCacheV1 {
    RinImageThumbnailCacheSlotV1 slots[RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES];
    RinImageThumbnailCachePendingV1 pending[RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS];
    uint64_t next_touch;
    uint64_t next_sequence;
    uint64_t next_request_id;
} RinImageThumbnailCacheV1;

static inline int rin_image_thumbnail_cache_key_valid(
    const RinImageThumbnailCacheKeyV1* key)
{
    return key != NULL && key->source_id != 0u &&
           key->source_generation != 0u && key->width != 0u &&
           key->height != 0u &&
           key->width <= RIN_IMAGE_THUMBNAIL_CACHE_MAX_WIDTH &&
           key->height <= RIN_IMAGE_THUMBNAIL_CACHE_MAX_HEIGHT &&
           (uint64_t)key->width * (uint64_t)key->height <=
               RIN_IMAGE_THUMBNAIL_CACHE_MAX_PIXELS &&
           key->reserved[0] == 0u && key->reserved[1] == 0u;
}

static inline int rin_image_thumbnail_cache_key_equal(
    const RinImageThumbnailCacheKeyV1* left,
    const RinImageThumbnailCacheKeyV1* right)
{
    return left != NULL && right != NULL &&
           left->source_id == right->source_id &&
           left->source_generation == right->source_generation &&
           left->width == right->width && left->height == right->height;
}

static inline uint32_t rin_image_thumbnail_cache_pixel_count(
    const RinImageThumbnailCacheKeyV1* key)
{
    return (uint32_t)((uint64_t)key->width * (uint64_t)key->height);
}

static inline int rin_image_thumbnail_cache_ranges_overlap(
    const void* left, size_t left_size, const void* right, size_t right_size)
{
    uintptr_t left_start;
    uintptr_t right_start;
    uintptr_t left_end;
    uintptr_t right_end;
    if (left == NULL || right == NULL || left_size == 0u || right_size == 0u)
        return 0;
    if (left_size > (size_t)UINTPTR_MAX || right_size > (size_t)UINTPTR_MAX)
        return 1;
    left_start = (uintptr_t)left;
    right_start = (uintptr_t)right;
    if (left_start > UINTPTR_MAX - (uintptr_t)left_size ||
        right_start > UINTPTR_MAX - (uintptr_t)right_size)
        return 1;
    left_end = left_start + (uintptr_t)left_size;
    right_end = right_start + (uintptr_t)right_size;
    return left_start < right_end && right_start < left_end;
}

static inline void rin_image_thumbnail_cache_slot_clear(
    RinImageThumbnailCacheSlotV1* slot)
{
    uint32_t* pixels = slot->pixels;
    size_t pixel_capacity = slot->pixel_capacity;
    memset(slot, 0, sizeof(*slot));
    slot->pixels = pixels;
    slot->pixel_capacity = pixel_capacity;
}

static inline uint64_t rin_image_thumbnail_cache_touch(
    RinImageThumbnailCacheV1* cache)
{
    uint32_t index;
    if (cache->next_touch == UINT64_MAX) {
        uint64_t rank = 1u;
        for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES;
             ++index) {
            if (cache->slots[index].valid)
                cache->slots[index].last_used = rank++;
        }
        cache->next_touch = rank;
    }
    return ++cache->next_touch;
}

static inline uint64_t rin_image_thumbnail_cache_sequence(
    RinImageThumbnailCacheV1* cache)
{
    if (cache->next_sequence == UINT64_MAX) cache->next_sequence = 0u;
    return ++cache->next_sequence;
}

static inline int rin_image_thumbnail_cache_slot_find(
    const RinImageThumbnailCacheV1* cache,
    const RinImageThumbnailCacheKeyV1* key)
{
    uint32_t index;
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES; ++index) {
        if (cache->slots[index].valid &&
            rin_image_thumbnail_cache_key_equal(&cache->slots[index].key,
                                                key))
            return (int)index;
    }
    return -1;
}

static inline int rin_image_thumbnail_cache_pending_find(
    const RinImageThumbnailCacheV1* cache, uint64_t request_id)
{
    uint32_t index;
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS; ++index) {
        if (cache->pending[index].state != 0u &&
            cache->pending[index].request.request_id == request_id)
            return (int)index;
    }
    return -1;
}

static inline int rin_image_thumbnail_cache_pending_key_find(
    const RinImageThumbnailCacheV1* cache,
    const RinImageThumbnailCacheKeyV1* key)
{
    uint32_t index;
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS; ++index) {
        if (cache->pending[index].state != 0u &&
            rin_image_thumbnail_cache_key_equal(&cache->pending[index].request.key,
                                                key))
            return (int)index;
    }
    return -1;
}

static inline void rin_image_thumbnail_cache_pending_clear(
    RinImageThumbnailCachePendingV1* pending)
{
    memset(pending, 0, sizeof(*pending));
}

static inline uint64_t rin_image_thumbnail_cache_request_id(
    RinImageThumbnailCacheV1* cache)
{
    uint64_t candidate = cache->next_request_id;
    uint32_t attempt;
    if (candidate == 0u) candidate = 1u;
    for (attempt = 0u; attempt < RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS;
         ++attempt) {
        if (rin_image_thumbnail_cache_pending_find(cache, candidate) < 0) {
            cache->next_request_id = candidate == UINT64_MAX
                ? 1u : candidate + 1u;
            return candidate;
        }
        candidate = candidate == UINT64_MAX ? 1u : candidate + 1u;
    }
    return 0u;
}

static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_frame_valid(
    const RinImageThumbnailCacheKeyV1* key, const RinImageFrame* frame,
    uint32_t* pixel_count_out)
{
    RinImageDecodeLimits limits;
    RinImageStatus image_status;
    uint32_t pixel_count;
    if (!rin_image_thumbnail_cache_key_valid(key) || frame == NULL ||
        pixel_count_out == NULL || frame->size.width != key->width ||
        frame->size.height != key->height ||
        frame->kind != RIN_IMAGE_FRAME_STATIC || frame->duration_ms != 0u)
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    rin_image_decode_limits_default(&limits);
    limits.max_width = RIN_IMAGE_THUMBNAIL_CACHE_MAX_WIDTH;
    limits.max_height = RIN_IMAGE_THUMBNAIL_CACHE_MAX_HEIGHT;
    limits.max_pixels = RIN_IMAGE_THUMBNAIL_CACHE_MAX_PIXELS;
    limits.max_frames = 1u;
    limits.max_output_bytes = RIN_IMAGE_THUMBNAIL_CACHE_MAX_BYTES;
    image_status = rin_image_frame_validate(&limits, frame);
    if (image_status != RIN_IMAGE_OK)
        return image_status == RIN_IMAGE_LIMIT
            ? RIN_IMAGE_THUMBNAIL_CACHE_BUFFER_TOO_SMALL
            : RIN_IMAGE_THUMBNAIL_CACHE_INVALID_LAYOUT;
    pixel_count = rin_image_thumbnail_cache_pixel_count(key);
    if ((uint64_t)pixel_count * sizeof(uint32_t) >
        RIN_IMAGE_THUMBNAIL_CACHE_MAX_BYTES)
        return RIN_IMAGE_THUMBNAIL_CACHE_BUFFER_TOO_SMALL;
    *pixel_count_out = pixel_count;
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

/* No path, descriptor, decoder, or service handle is stored here.  A
 * service owner binds caller-owned pixel slots and performs the actual decode
 * separately, which keeps this cache usable across sandbox boundaries. */
static inline void rin_image_thumbnail_cache_init(
    RinImageThumbnailCacheV1* cache)
{
    if (cache != NULL) memset(cache, 0, sizeof(*cache));
}

/* Binding a slot invalidates its old contents but preserves no pixel data.
 * The pixel memory remains owned by the caller for the cache lifetime. */
static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_bind_slot(RinImageThumbnailCacheV1* cache,
                                     uint32_t slot_index, uint32_t* pixels,
                                     size_t pixel_capacity)
{
    RinImageThumbnailCacheSlotV1* slot;
    if (cache == NULL || slot_index >= RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES ||
        pixels == NULL || pixel_capacity == 0u ||
        pixel_capacity > (size_t)-1 / sizeof(uint32_t))
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    slot = &cache->slots[slot_index];
    rin_image_thumbnail_cache_slot_clear(slot);
    slot->pixels = pixels;
    slot->pixel_capacity = pixel_capacity;
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

/* Clear cached frames and queued work while retaining the caller's slot
 * bindings. */
static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_clear(RinImageThumbnailCacheV1* cache)
{
    uint32_t index;
    if (cache == NULL) return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES; ++index)
        rin_image_thumbnail_cache_slot_clear(&cache->slots[index]);
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS; ++index)
        rin_image_thumbnail_cache_pending_clear(&cache->pending[index]);
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_get(RinImageThumbnailCacheV1* cache,
                               const RinImageThumbnailCacheKeyV1* key,
                               RinImageFrame* frame_out)
{
    int slot_index;
    uint32_t expected_pixels;
    RinImageThumbnailCacheSlotV1* slot;
    if (frame_out != NULL) memset(frame_out, 0, sizeof(*frame_out));
    if (cache == NULL || frame_out == NULL ||
        !rin_image_thumbnail_cache_key_valid(key))
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    slot_index = rin_image_thumbnail_cache_slot_find(cache, key);
    if (slot_index < 0) return RIN_IMAGE_THUMBNAIL_CACHE_NOT_FOUND;
    slot = &cache->slots[slot_index];
    expected_pixels = rin_image_thumbnail_cache_pixel_count(key);
    if (slot->pixels == NULL || slot->pixel_capacity < expected_pixels ||
        slot->pixel_count != expected_pixels) {
        rin_image_thumbnail_cache_slot_clear(slot);
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_LAYOUT;
    }
    slot->last_used = rin_image_thumbnail_cache_touch(cache);
    frame_out->size.width = key->width;
    frame_out->size.height = key->height;
    frame_out->stride_bytes = key->width * sizeof(uint32_t);
    frame_out->pixel_format = RIN_IMAGE_PIXEL_ARGB8888;
    frame_out->kind = RIN_IMAGE_FRAME_STATIC;
    frame_out->pixels = slot->pixels;
    frame_out->pixel_bytes = (size_t)expected_pixels * sizeof(uint32_t);
    return RIN_IMAGE_THUMBNAIL_CACHE_HIT;
}

static inline int rin_image_thumbnail_cache_better_slot(
    const RinImageThumbnailCacheSlotV1* candidate,
    const RinImageThumbnailCacheSlotV1* current)
{
    if (!candidate->valid) return current->valid;
    if (!current->valid) return 0;
    return candidate->priority < current->priority ||
           (candidate->priority == current->priority &&
            candidate->last_used < current->last_used);
}

static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_put(RinImageThumbnailCacheV1* cache,
                               const RinImageThumbnailCacheKeyV1* key,
                               const RinImageFrame* frame, uint32_t priority)
{
    int matching;
    int selected = -1;
    uint32_t expected_pixels;
    uint32_t index;
    RinImageThumbnailCacheSlotV1* slot;
    RinImageThumbnailCacheSlotV1* best = NULL;
    RinImageThumbnailCacheStatus status;
    size_t source_row_bytes;
    size_t output_bytes;
    if (cache == NULL || frame == NULL)
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    status = rin_image_thumbnail_cache_frame_valid(key, frame,
                                                   &expected_pixels);
    if (status != RIN_IMAGE_THUMBNAIL_CACHE_OK) return status;
    output_bytes = (size_t)expected_pixels * sizeof(uint32_t);
    source_row_bytes = (size_t)key->width * sizeof(uint32_t);
    matching = rin_image_thumbnail_cache_slot_find(cache, key);
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES; ++index) {
        slot = &cache->slots[index];
        if (slot->pixels == NULL || slot->pixel_capacity < expected_pixels ||
            (index != (uint32_t)matching &&
             rin_image_thumbnail_cache_ranges_overlap(
                 frame->pixels, frame->pixel_bytes, slot->pixels,
                 output_bytes)))
            continue;
        if ((int)index == matching) {
            selected = (int)index;
            break;
        }
        if (selected < 0 || rin_image_thumbnail_cache_better_slot(
                                  slot, best)) {
            selected = (int)index;
            best = slot;
        }
        if (!slot->valid) break;
    }
    if (selected < 0) return RIN_IMAGE_THUMBNAIL_CACHE_BUFFER_TOO_SMALL;
    slot = &cache->slots[selected];
    for (index = 0u; index < key->height; ++index) {
        memmove(slot->pixels + (size_t)index * key->width,
                frame->pixels + (size_t)index *
                    (frame->stride_bytes / sizeof(uint32_t)),
                source_row_bytes);
    }
    if (matching >= 0 && matching != selected)
        rin_image_thumbnail_cache_slot_clear(&cache->slots[matching]);
    slot->key = *key;
    slot->pixel_count = expected_pixels;
    slot->priority = priority;
    slot->last_used = rin_image_thumbnail_cache_touch(cache);
    slot->valid = 1u;
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

/* Drop every cached result and queued request from an older source
 * generation.  The source owner calls this before publishing its new
 * generation, so in-flight completions cannot repopulate stale content. */
static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_invalidate_source(
    RinImageThumbnailCacheV1* cache, uint64_t source_id,
    uint64_t source_generation)
{
    uint32_t index;
    if (cache == NULL || source_id == 0u || source_generation == 0u)
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_ENTRIES; ++index) {
        RinImageThumbnailCacheSlotV1* slot = &cache->slots[index];
        if (slot->valid && slot->key.source_id == source_id &&
            slot->key.source_generation != source_generation)
            rin_image_thumbnail_cache_slot_clear(slot);
    }
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS; ++index) {
        RinImageThumbnailCachePendingV1* pending = &cache->pending[index];
        if (pending->state != 0u &&
            pending->request.key.source_id == source_id &&
            pending->request.key.source_generation != source_generation)
            rin_image_thumbnail_cache_pending_clear(pending);
    }
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

/* Enqueue work for a decoder/service owner.  Higher priority values are
 * dispatched first; equal priorities retain FIFO order. */
static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_submit(RinImageThumbnailCacheV1* cache,
                                  const RinImageThumbnailCacheKeyV1* key,
                                  uint32_t priority, uint64_t* request_id_out)
{
    uint32_t index;
    int existing;
    uint64_t request_id;
    if (request_id_out != NULL) *request_id_out = 0u;
    if (cache == NULL || request_id_out == NULL ||
        !rin_image_thumbnail_cache_key_valid(key))
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    existing = rin_image_thumbnail_cache_slot_find(cache, key);
    if (existing >= 0) return RIN_IMAGE_THUMBNAIL_CACHE_HIT;
    existing = rin_image_thumbnail_cache_pending_key_find(cache, key);
    if (existing >= 0) {
        RinImageThumbnailCachePendingV1* pending = &cache->pending[existing];
        if (pending->state == 1u && priority > pending->request.priority)
            pending->request.priority = priority;
        *request_id_out = pending->request.request_id;
        return RIN_IMAGE_THUMBNAIL_CACHE_ALREADY_QUEUED;
    }
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS; ++index)
        if (cache->pending[index].state == 0u) break;
    if (index == RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS)
        return RIN_IMAGE_THUMBNAIL_CACHE_QUEUE_FULL;
    request_id = rin_image_thumbnail_cache_request_id(cache);
    if (request_id == 0u) return RIN_IMAGE_THUMBNAIL_CACHE_QUEUE_FULL;
    cache->pending[index].request.request_id = request_id;
    cache->pending[index].request.key = *key;
    cache->pending[index].request.priority = priority;
    cache->pending[index].sequence = rin_image_thumbnail_cache_sequence(cache);
    cache->pending[index].state = 1u;
    *request_id_out = request_id;
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_next(RinImageThumbnailCacheV1* cache,
                               RinImageThumbnailCacheRequestV1* request_out)
{
    int selected = -1;
    uint32_t index;
    if (request_out != NULL) memset(request_out, 0, sizeof(*request_out));
    if (cache == NULL || request_out == NULL)
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    for (index = 0u; index < RIN_IMAGE_THUMBNAIL_CACHE_MAX_REQUESTS; ++index) {
        RinImageThumbnailCachePendingV1* pending = &cache->pending[index];
        RinImageThumbnailCachePendingV1* current;
        if (pending->state != 1u) continue;
        current = selected < 0 ? NULL : &cache->pending[selected];
        if (current == NULL || pending->request.priority > current->request.priority ||
            (pending->request.priority == current->request.priority &&
             pending->sequence < current->sequence))
            selected = (int)index;
    }
    if (selected < 0) return RIN_IMAGE_THUMBNAIL_CACHE_NOT_FOUND;
    cache->pending[selected].state = 2u;
    *request_out = cache->pending[selected].request;
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_cancel(RinImageThumbnailCacheV1* cache,
                                  uint64_t request_id)
{
    int index;
    if (cache == NULL || request_id == 0u)
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    index = rin_image_thumbnail_cache_pending_find(cache, request_id);
    if (index < 0) return RIN_IMAGE_THUMBNAIL_CACHE_NOT_FOUND;
    rin_image_thumbnail_cache_pending_clear(&cache->pending[index]);
    return RIN_IMAGE_THUMBNAIL_CACHE_OK;
}

/* Complete only a request claimed by rin_image_thumbnail_cache_next().  A
 * source-generation invalidation removes the request first, making a late
 * decoder result harmless. */
static inline RinImageThumbnailCacheStatus
rin_image_thumbnail_cache_complete(RinImageThumbnailCacheV1* cache,
                                    uint64_t request_id,
                                    const RinImageFrame* frame)
{
    int index;
    RinImageThumbnailCacheStatus status;
    if (cache == NULL || request_id == 0u || frame == NULL)
        return RIN_IMAGE_THUMBNAIL_CACHE_INVALID_ARGUMENT;
    index = rin_image_thumbnail_cache_pending_find(cache, request_id);
    if (index < 0) return RIN_IMAGE_THUMBNAIL_CACHE_NOT_FOUND;
    if (cache->pending[index].state != 2u)
        return RIN_IMAGE_THUMBNAIL_CACHE_BUSY;
    status = rin_image_thumbnail_cache_put(
        cache, &cache->pending[index].request.key, frame,
        cache->pending[index].request.priority);
    rin_image_thumbnail_cache_pending_clear(&cache->pending[index]);
    return status;
}

#ifdef __cplusplus
}
#endif

#endif /* RINIMAGE_THUMBNAIL_CACHE_H */
