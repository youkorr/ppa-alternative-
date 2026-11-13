/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Benchmark-only wrappers. All real arguments/results are preserved.
 */
#include "ab_cost_probe.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include <stddef.h>
#include <string.h>

static DRAM_ATTR portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR ab_cost_stats_t s_stats;
static DRAM_ATTR uint32_t s_epoch = 1;
static DRAM_ATTR ab_cost_stage_t s_stage = AB_COST_STAGE_SETUP;
static DRAM_ATTR bool s_active;
static DRAM_ATTR bool s_background;

/* Zero means inactive. The entry token fixes both epoch and stage, including
 * background override, across a context switch or another CPU changing stage. */
static uint32_t IRAM_ATTR begin_token(void)
{
    uint32_t token = 0;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (s_active) {
        const unsigned stage = s_background ? AB_COST_STAGE_BACKGROUND : s_stage;
        token = (s_epoch << 4) | (stage + 1);
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    return token;
}

static void IRAM_ATTR record(uint32_t token, ab_cost_kind_t kind,
                             uint64_t elapsed, uint64_t bytes, uint64_t pixels,
                             uint32_t errors, uint32_t pending, uint32_t flags,
                             uint64_t c2m, uint64_t m2c)
{
    if (!token) return;
    const unsigned stage = (token & 15u) - 1u;
    if (stage >= AB_COST_STAGE_COUNT || kind >= AB_COST_KIND_COUNT) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    /* Ignore operations ending outside the capture window or after reset. */
    if (s_active && (token >> 4) == s_epoch) {
        ab_cost_cell_t *cell = &s_stats.cell[stage][kind];
        cell->calls++;
        cell->elapsed_us += elapsed;
        cell->bytes += bytes;
        cell->pixels += pixels;
        cell->errors += errors;
        cell->pending += pending;
        cell->flags_or |= flags;
        cell->c2m_bytes += c2m;
        cell->m2c_bytes += m2c;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void ab_cost_probe_reset(void)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    configASSERT(!s_active);
    /* Avoid a libc call while holding the mux. This initialization is outside
     * the timed interval, and the explicit volatile loop cannot become memset. */
    volatile unsigned char *bytes = (volatile unsigned char *)&s_stats;
    for (size_t i = 0; i < sizeof(s_stats); i++) bytes[i] = 0;
    s_epoch = (s_epoch + 1) & 0x0fffffffu;
    if (!s_epoch) s_epoch = 1;
    s_stage = AB_COST_STAGE_SETUP;
    s_background = false;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void ab_cost_probe_set_stage(ab_cost_stage_t stage)
{
    configASSERT(stage < AB_COST_STAGE_COUNT);
    portENTER_CRITICAL_SAFE(&s_mux);
    s_stage = stage;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void ab_cost_probe_set_background(bool background)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    s_background = background;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

void ab_cost_probe_set_active(bool active)
{
    portENTER_CRITICAL_SAFE(&s_mux);
    s_active = active;
    portEXIT_CRITICAL_SAFE(&s_mux);
}

ab_cost_stats_t ab_cost_probe_snapshot(void)
{
    ab_cost_stats_t snapshot;
    portENTER_CRITICAL_SAFE(&s_mux);
    snapshot = s_stats;
    portEXIT_CRITICAL_SAFE(&s_mux);
    return snapshot;
}

const char *ab_cost_probe_stage_name(ab_cost_stage_t stage)
{
    static const char *const names[] = {
        "setup", "lock", "update", "refresh", "unlock", "yield", "drain", "background"
    };
    return stage < AB_COST_STAGE_COUNT ? names[stage] : "invalid";
}

const char *ab_cost_probe_kind_name(ab_cost_kind_t kind)
{
    static const char *const names[] = {"copy", "cache", "srm", "fill", "blend", "native_wait"};
    return kind < AB_COST_KIND_COUNT ? names[kind] : "invalid";
}

uint32_t ab_cost_native_wait_begin(void)
{
    return begin_token();
}

void ab_cost_native_wait_end(uint32_t token, uint32_t elapsed_us,
                             bool pending, bool success)
{
    record(token, AB_COST_NATIVE_WAIT, elapsed_us, 0, 0,
           !success, pending, 0, 0, 0);
}

extern void __real_lv_draw_buf_copy(lv_draw_buf_t *, const lv_area_t *,
                                    const lv_draw_buf_t *, const lv_area_t *);
void __wrap_lv_draw_buf_copy(lv_draw_buf_t *dest, const lv_area_t *dest_area,
                             const lv_draw_buf_t *src, const lv_area_t *src_area)
{
    const uint32_t token = begin_token();
    if (!token) { __real_lv_draw_buf_copy(dest, dest_area, src, src_area); return; }
    uint64_t pixels = 0, bytes = 0;
    if (dest) {
        const lv_area_t *area = dest_area ? dest_area : src_area;
        const uint32_t width = area ? (uint32_t)lv_area_get_width(area) : dest->header.w;
        const uint32_t height = area ? (uint32_t)lv_area_get_height(area) : dest->header.h;
        pixels = (uint64_t)width * height;
        /* Logical destination payload, not read+write or cache-line traffic. */
        bytes = (uint64_t)(((uint64_t)width * lv_color_format_get_bpp(dest->header.cf) + 7) / 8) * height;
    }
    const int64_t started = esp_timer_get_time();
    __real_lv_draw_buf_copy(dest, dest_area, src, src_area);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - started);
    record(token, AB_COST_COPY, elapsed, bytes, pixels, 0, 0, 0, 0, 0);
}

/* The real cache API is cache-safe and is called by IDF as well as LVGL.
 * Keep this wrapper and its entire inactive/active call path in IRAM; all
 * state lives in DRAM and no string, allocation, log or libc helper is used. */
extern esp_err_t __real_esp_cache_msync(void *, size_t, int);
esp_err_t IRAM_ATTR __wrap_esp_cache_msync(void *addr, size_t size, int flags)
{
    const uint32_t token = begin_token();
    if (!token) return __real_esp_cache_msync(addr, size, flags);
    const int64_t started = esp_timer_get_time();
    const esp_err_t result = __real_esp_cache_msync(addr, size, flags);
    const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - started);
    const bool m2c = (flags & ESP_CACHE_MSYNC_FLAG_DIR_M2C) != 0;
    const bool c2m = (flags & ESP_CACHE_MSYNC_FLAG_DIR_C2M) != 0 || !m2c;
    record(token, AB_COST_CACHE, elapsed, size, 0, result != ESP_OK, 0,
           (uint32_t)flags, c2m ? size : 0, m2c ? size : 0);
    return result;
}

extern esp_err_t __real_ppa_do_scale_rotate_mirror(ppa_client_handle_t, const ppa_srm_oper_config_t *);
esp_err_t __wrap_ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *config)
{
    const uint32_t token = begin_token();
    if (!token) return __real_ppa_do_scale_rotate_mirror(client, config);
    const uint64_t pixels = config ? (uint64_t)config->in.block_w * config->in.block_h : 0;
    const int64_t started = esp_timer_get_time();
    const esp_err_t result = __real_ppa_do_scale_rotate_mirror(client, config);
    record(token, AB_COST_SRM, (uint64_t)(esp_timer_get_time() - started), 0, pixels,
           result != ESP_OK, 0, 0, 0, 0);
    return result;
}

extern esp_err_t __real_ppa_do_fill(ppa_client_handle_t, const ppa_fill_oper_config_t *);
esp_err_t __wrap_ppa_do_fill(ppa_client_handle_t client, const ppa_fill_oper_config_t *config)
{
    const uint32_t token = begin_token();
    if (!token) return __real_ppa_do_fill(client, config);
    const uint64_t pixels = config ? (uint64_t)config->fill_block_w * config->fill_block_h : 0;
    const int64_t started = esp_timer_get_time();
    const esp_err_t result = __real_ppa_do_fill(client, config);
    record(token, AB_COST_FILL, (uint64_t)(esp_timer_get_time() - started), 0, pixels,
           result != ESP_OK, 0, 0, 0, 0);
    return result;
}

extern esp_err_t __real_ppa_do_blend(ppa_client_handle_t, const ppa_blend_oper_config_t *);
esp_err_t __wrap_ppa_do_blend(ppa_client_handle_t client, const ppa_blend_oper_config_t *config)
{
    const uint32_t token = begin_token();
    if (!token) return __real_ppa_do_blend(client, config);
    const uint64_t pixels = config ? (uint64_t)config->in_bg.block_w * config->in_bg.block_h : 0;
    const int64_t started = esp_timer_get_time();
    const esp_err_t result = __real_ppa_do_blend(client, config);
    record(token, AB_COST_BLEND, (uint64_t)(esp_timer_get_time() - started), 0, pixels,
           result != ESP_OK, 0, 0, 0, 0);
    return result;
}
