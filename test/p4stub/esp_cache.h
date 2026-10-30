/* Stand-in for ESP-IDF v5.5.5 components/esp_mm/include/esp_cache.h: the
 * flags and the call, copied. */
#pragma once
#include <stddef.h>
#include "esp_err.h"
#define BIT(n) (1UL << (n))
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE    BIT(0)
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED     BIT(1)
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M       BIT(2)
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C       BIT(3)
#define ESP_CACHE_MSYNC_FLAG_TYPE_DATA     BIT(4)
#define ESP_CACHE_MSYNC_FLAG_TYPE_INST     BIT(5)
esp_err_t esp_cache_msync(void *addr, size_t size, int flags);
