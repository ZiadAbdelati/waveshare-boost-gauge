#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum accepted GIF payload and GIF canvas dimensions. 3.5 MB stays under
 * the ~3.9 MB per-slot payload ceiling (8.25 MB partition / 2 slots - header),
 * leaving headroom for the erase-range alignment in boost_media_store_begin(). */
#define BOOST_MEDIA_STORE_MAX_BYTES (3500u * 1024u)
#define BOOST_MEDIA_STORE_MAX_DIMENSION 466u

/** Persisted state of the currently committed media slot. */
typedef struct {
    bool present;
    size_t size;
    uint32_t generation;
    uint32_t uploaded_at_ms;
    uint16_t width;
    uint16_t height;
} boost_media_store_status_t;

/** Initialize the raw media partition and scan/load its newest valid slot. */
esp_err_t boost_media_store_init(void);

/** Return a snapshot of the currently committed media slot. */
esp_err_t boost_media_store_status(boost_media_store_status_t *out);

/** Begin one serialized upload into the inactive slot. */
esp_err_t boost_media_store_begin(size_t expected_size);

/** Append one contiguous upload chunk to the inactive slot. */
esp_err_t boost_media_store_write(const void *data, size_t size);

/** Validate and atomically publish the inactive slot as the active slot. */
esp_err_t boost_media_store_commit(void);

/** Abort an upload, preserving the previously committed slot. */
void boost_media_store_abort(void);

/** Delete the committed media slot. */
esp_err_t boost_media_store_delete(void);

/**
 * Map the committed GIF payload for display playback. The returned pointer
 * remains valid until boost_media_store_unmap() and must not be freed.
 */
esp_err_t boost_media_store_map(const uint8_t **data, size_t *size,
                                 uint16_t *width, uint16_t *height);

/** Release a mapping returned by boost_media_store_map(). */
void boost_media_store_unmap(void);

#ifdef __cplusplus
}
#endif
