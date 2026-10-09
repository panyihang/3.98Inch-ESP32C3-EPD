#ifndef FONT_STORE_H
#define FONT_STORE_H

#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_STORE_MAGIC "EPF1"
#define FONT_STORE_GLYPH_BYTES 128u
#define FONT_STORE_GLYPH_WIDTH 32u
#define FONT_STORE_GLYPH_HEIGHT 32u

/* Look up a codepoint in the flash-resident font library. On hit sets *rows to
 * the 128-byte 32x32 1bpp bitmap and returns 1; otherwise returns 0. */
int font_store_lookup(uint32_t codepoint, const uint8_t **rows);

#ifdef ESP_PLATFORM
esp_err_t font_store_init(void);
#else
esp_err_t font_store_init(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FONT_STORE_H */