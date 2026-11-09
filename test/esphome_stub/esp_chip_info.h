/* What lvgl_ortho.cpp uses of ESP-IDF's esp_chip_info.h. */
#pragma once
#include <stdint.h>
typedef struct {
    int model;
    uint32_t features;
    uint16_t revision;
    uint8_t cores;
} esp_chip_info_t;
static inline void esp_chip_info(esp_chip_info_t *info)
{
    info->model = 18;
    info->features = 0;
    info->revision = 100;
    info->cores = 2;
}
