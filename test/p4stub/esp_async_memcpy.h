/* Stand-in for ESP-IDF v5.5.6's esp_async_memcpy.h (esp_hw_support; in 6.x
 * esp_driver_dma): the types and calls lv_draw_ortho_ppa.c uses, copied. The
 * fake in p4fake.c copies at once or late (p4fake_copy_late). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifndef SOC_AXI_GDMA_SUPPORTED
#define SOC_AXI_GDMA_SUPPORTED 1
#endif
typedef struct async_memcpy_context_t * async_memcpy_handle_t;
typedef struct {
    void * data;
} async_memcpy_event_t;
typedef bool (*async_memcpy_isr_cb_t)(async_memcpy_handle_t mcp_hdl, async_memcpy_event_t * event, void * cb_args);
typedef struct {
    uint32_t backlog;
    size_t dma_burst_size;
    uint32_t flags;
} async_memcpy_config_t;
#define ASYNC_MEMCPY_DEFAULT_CONFIG() \
    {                                 \
        .backlog = 8,                 \
        .dma_burst_size = 16,         \
        .flags = 0,                   \
    }
esp_err_t esp_async_memcpy_install_gdma_axi(const async_memcpy_config_t * config, async_memcpy_handle_t * mcp);
esp_err_t esp_async_memcpy_uninstall(async_memcpy_handle_t mcp);
esp_err_t esp_async_memcpy(async_memcpy_handle_t mcp, void * dst, void * src, size_t n, async_memcpy_isr_cb_t cb_isr,
                           void * cb_args);
