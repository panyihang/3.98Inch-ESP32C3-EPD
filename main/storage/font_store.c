#include "font_store.h"

#include <stdint.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_partition.h"
#endif

#define FONT_STORE_HEADER_SIZE 0x10u
#define FONT_STORE_CODEPOINT_SIZE 4u

static const uint32_t *s_table = NULL;
static const uint8_t *s_glyphs = NULL;
static uint32_t s_count = 0;

#ifdef ESP_PLATFORM
static esp_partition_mmap_handle_t s_map_handle = 0;
#endif

static int index_lookup(uint32_t codepoint, const uint32_t *table, uint32_t n)
{
    uint32_t lo = 0u, hi = n;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        if (table[mid] < codepoint) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < n && table[lo] == codepoint) {
        return (int)lo;
    }
    return -1;
}

int font_store_lookup(uint32_t codepoint, const uint8_t **rows)
{
    if (!rows || !s_table || !s_glyphs) return 0;
    const int idx = index_lookup(codepoint, s_table, s_count);
    if (idx < 0) return 0;
    *rows = s_glyphs + (size_t)idx * FONT_STORE_GLYPH_BYTES;
    return 1;
}

#ifdef ESP_PLATFORM
esp_err_t font_store_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x41, "fonts");
    if (!part) {
        ESP_LOGW("font_store", "fonts partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (part->size < FONT_STORE_HEADER_SIZE) {
        ESP_LOGW("font_store", "fonts partition too small");
        return ESP_ERR_INVALID_SIZE;
    }
    const void *map = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA, &map, &s_map_handle);
    if (err != ESP_OK) {
        ESP_LOGW("font_store", "mmap failed: %d", err);
        return err;
    }
    const uint8_t *p = (const uint8_t *)map;
    if (memcmp(p, FONT_STORE_MAGIC, 4) != 0) {
        ESP_LOGW("font_store", "bad font magic");
        esp_partition_munmap(s_map_handle);
        s_map_handle = 0;
        return ESP_ERR_INVALID_ARG;
    }
    s_count = ((const uint32_t *)p)[1];
    if (s_count == 0 || s_count > part->size / (FONT_STORE_CODEPOINT_SIZE + FONT_STORE_GLYPH_BYTES)) {
        ESP_LOGW("font_store", "invalid glyph count %lu", (unsigned long)s_count);
        esp_partition_munmap(s_map_handle);
        s_map_handle = 0;
        return ESP_ERR_INVALID_SIZE;
    }
    s_table = (const uint32_t *)(p + FONT_STORE_HEADER_SIZE);
    s_glyphs = p + FONT_STORE_HEADER_SIZE + FONT_STORE_CODEPOINT_SIZE * s_count;
    ESP_LOGI("font_store", "loaded %lu glyphs", (unsigned long)s_count);
    return ESP_OK;
}
#else
esp_err_t font_store_init(void)
{
    s_table = NULL;
    s_glyphs = NULL;
    s_count = 0;
    return ESP_OK;
}
#endif