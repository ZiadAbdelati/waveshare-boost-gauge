#include "boost_media_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_rom_crc.h"

#define TAG "boost_media_store"
#define MEDIA_LABEL "media"
#define SLOT_COUNT 2u
#define SECTOR_SIZE 0x1000u
#define HEADER_SIZE SECTOR_SIZE
#define MEDIA_SIZE 0x7E0000u
#define SLOT_SIZE (MEDIA_SIZE / SLOT_COUNT)
#define PAYLOAD_OFFSET HEADER_SIZE
#define MAGIC 0x314D4742u /* BGM1 */
#define VERSION 1u

/* Header is written only after payload and CRC are complete. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t generation;
    uint32_t payload_size;
    uint32_t payload_crc;
    uint32_t uploaded_at_ms;
    uint16_t width;
    uint16_t height;
    uint32_t header_crc;
} media_header_t;

_Static_assert(sizeof(media_header_t) <= HEADER_SIZE, "media header must fit one sector");

static const esp_partition_t *s_partition;
static SemaphoreHandle_t s_lock;
static media_header_t s_header;
static int s_active_slot = -1;
static bool s_uploading;
static int s_upload_slot = -1;
static size_t s_expected_size;
static size_t s_written_size;
static uint32_t s_upload_crc;
static uint8_t s_gif_header[10];
static size_t s_gif_header_size;
static esp_partition_mmap_handle_t s_map_handle;
static bool s_mapped;

static uint32_t crc32(const void *data, size_t size)
{
    return esp_rom_crc32_le(0, (const uint8_t *)data, size);
}

static uint32_t header_crc(const media_header_t *header)
{
    media_header_t copy = *header;
    copy.header_crc = 0;
    return crc32(&copy, sizeof(copy));
}

static bool header_valid(const media_header_t *header)
{
    if (header->magic != MAGIC || header->version != VERSION ||
        header->header_size != sizeof(media_header_t) || header->payload_size == 0 ||
        header->payload_size > BOOST_MEDIA_STORE_MAX_BYTES || header->width == 0 ||
        header->height == 0 || header->width > BOOST_MEDIA_STORE_MAX_DIMENSION ||
        header->height > BOOST_MEDIA_STORE_MAX_DIMENSION ||
        header->header_crc != header_crc(header)) {
        return false;
    }
    return true;
}

static esp_err_t payload_crc_valid(int slot, const media_header_t *header)
{
    uint8_t buf[4096];
    uint32_t crc = 0;
    size_t remaining = header->payload_size;
    size_t offset = 0;
    while (remaining != 0) {
        size_t n = remaining > sizeof(buf) ? sizeof(buf) : remaining;
        esp_err_t err = esp_partition_read(s_partition,
                                           (size_t)slot * SLOT_SIZE + PAYLOAD_OFFSET + offset,
                                           buf, n);
        if (err != ESP_OK) {
            return err;
        }
        crc = esp_rom_crc32_le(crc, buf, n);
        offset += n;
        remaining -= n;
    }
    return crc == header->payload_crc ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static void clear_status(void)
{
    memset(&s_header, 0, sizeof(s_header));
    s_active_slot = -1;
}

static esp_err_t lock_store(void)
{
    if (s_lock == NULL || s_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

esp_err_t boost_media_store_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (s_partition == NULL) {
        s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                               ESP_PARTITION_SUBTYPE_ANY, MEDIA_LABEL);
        if (s_partition == NULL || s_partition->size < MEDIA_SIZE) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_FOUND;
        }
    }
    clear_status();
    media_header_t candidate;
    for (int slot = 0; slot < (int)SLOT_COUNT; ++slot) {
        esp_err_t err = esp_partition_read(s_partition, (size_t)slot * SLOT_SIZE,
                                           &candidate, sizeof(candidate));
        if (err != ESP_OK || !header_valid(&candidate) || payload_crc_valid(slot, &candidate) != ESP_OK) {
            continue;
        }
        if (s_active_slot < 0 || (int32_t)(candidate.generation - s_header.generation) > 0) {
            s_header = candidate;
            s_active_slot = slot;
        }
    }
    s_uploading = false;
    s_mapped = false;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t boost_media_store_status(boost_media_store_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_store();
    if (err != ESP_OK) {
        return err;
    }
    memset(out, 0, sizeof(*out));
    if (s_active_slot >= 0) {
        out->present = true;
        out->size = s_header.payload_size;
        out->generation = s_header.generation;
        out->uploaded_at_ms = s_header.uploaded_at_ms;
        out->width = s_header.width;
        out->height = s_header.height;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t boost_media_store_begin(size_t expected_size)
{
    if (expected_size == 0 || expected_size > BOOST_MEDIA_STORE_MAX_BYTES) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    if (s_uploading || s_mapped) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    int slot = s_active_slot < 0 ? 0 : (s_active_slot ^ 1);
    const size_t erase_size = ((HEADER_SIZE + expected_size + SECTOR_SIZE - 1) / SECTOR_SIZE) * SECTOR_SIZE;
    if (erase_size > SLOT_SIZE) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_SIZE; }
    err = esp_partition_erase_range(s_partition, (size_t)slot * SLOT_SIZE, erase_size);
    if (err == ESP_OK) {
        s_uploading = true; s_upload_slot = slot; s_expected_size = expected_size;
        s_written_size = 0; s_upload_crc = 0; s_gif_header_size = 0;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t boost_media_store_write(const void *data, size_t size)
{
    if (data == NULL || size == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    if (!s_uploading || size > s_expected_size - s_written_size) {
        xSemaphoreGive(s_lock); return ESP_ERR_INVALID_SIZE;
    }
    if (s_gif_header_size < sizeof(s_gif_header)) {
        size_t n = sizeof(s_gif_header) - s_gif_header_size;
        if (n > size) n = size;
        memcpy(s_gif_header + s_gif_header_size, data, n);
        s_gif_header_size += n;
    }
    const size_t offset = (size_t)s_upload_slot * SLOT_SIZE + PAYLOAD_OFFSET + s_written_size;
    err = esp_partition_write(s_partition, offset, data, size);
    if (err == ESP_OK) {
        s_upload_crc = esp_rom_crc32_le(s_upload_crc, (const uint8_t *)data, size);
        s_written_size += size;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t boost_media_store_commit(void)
{
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    if (!s_uploading || s_written_size != s_expected_size || s_gif_header_size < sizeof(s_gif_header)) {
        xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE;
    }
    const bool gif_magic = memcmp(s_gif_header, "GIF87a", 6) == 0 || memcmp(s_gif_header, "GIF89a", 6) == 0;
    const uint16_t width = (uint16_t)s_gif_header[6] | ((uint16_t)s_gif_header[7] << 8);
    const uint16_t height = (uint16_t)s_gif_header[8] | ((uint16_t)s_gif_header[9] << 8);
    if (!gif_magic || width == 0 || height == 0 || width > BOOST_MEDIA_STORE_MAX_DIMENSION || height > BOOST_MEDIA_STORE_MAX_DIMENSION) {
        s_uploading = false; s_upload_slot = -1; xSemaphoreGive(s_lock); return ESP_ERR_INVALID_ARG;
    }
    media_header_t header = {
        .magic = MAGIC, .version = VERSION, .header_size = sizeof(media_header_t),
        .generation = s_active_slot < 0 ? 1 : s_header.generation + 1,
        .payload_size = s_written_size, .payload_crc = s_upload_crc,
        .uploaded_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .width = width, .height = height,
    };
    header.header_crc = header_crc(&header);
    err = esp_partition_write(s_partition, (size_t)s_upload_slot * SLOT_SIZE, &header, sizeof(header));
    if (err == ESP_OK) {
        s_header = header; s_active_slot = s_upload_slot; s_uploading = false; s_upload_slot = -1;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void boost_media_store_abort(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    s_uploading = false;
    s_upload_slot = -1;
    s_expected_size = 0;
    s_written_size = 0;
    s_gif_header_size = 0;
    xSemaphoreGive(s_lock);
}

esp_err_t boost_media_store_delete(void)
{
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    if (s_uploading) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mapped) {
        esp_partition_munmap(s_map_handle);
        s_mapped = false;
    }
    if (s_active_slot >= 0) {
        err = esp_partition_erase_range(s_partition, (size_t)s_active_slot * SLOT_SIZE, HEADER_SIZE);
        if (err == ESP_OK) clear_status();
    }
    if (s_active_slot < 0) err = ESP_OK;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t boost_media_store_map(const uint8_t **data, size_t *size,
                                uint16_t *width, uint16_t *height)
{
    if (data == NULL || size == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = lock_store();
    if (err != ESP_OK) return err;
    if (s_active_slot < 0 || s_uploading || s_mapped) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    const void *mapped = NULL;
    err = esp_partition_mmap(s_partition, (size_t)s_active_slot * SLOT_SIZE + PAYLOAD_OFFSET,
                             s_header.payload_size, ESP_PARTITION_MMAP_DATA, &mapped, &s_map_handle);
    if (err == ESP_OK) {
        *data = (const uint8_t *)mapped;
        *size = s_header.payload_size;
        if (width) *width = s_header.width;
        if (height) *height = s_header.height;
        s_mapped = true;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void boost_media_store_unmap(void)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    if (s_mapped) {
        esp_partition_munmap(s_map_handle);
        s_mapped = false;
    }
    xSemaphoreGive(s_lock);
}
