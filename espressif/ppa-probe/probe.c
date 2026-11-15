/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 *
 * Where a camera picture's turn onto the panel spends its time, on the board.
 *
 * The camera's picture and the panel's frame buffer are both in PSRAM, which
 * the panel's scan-out and the camera already share, and the PPA works on
 * them in 16x16 macroblocks: short accesses, which PSRAM serves badly. The
 * question this firmware answers is whether turning the picture in bands
 * into internal SRAM, then copying each band to the frame buffer in long
 * runs, beats turning it straight into the frame buffer -- and by how much,
 * since the same bytes cross PSRAM either way.
 *
 * Every figure is a whole picture, the median of REPEAT runs:
 *
 *   ppa_whole           the PPA turns the picture straight into the frame buffer
 *                       (what lv_draw_ortho does today); its result is the
 *                       reference every banded result is compared with
 *   bands_direct        the same, cut in bands: what cutting alone costs
 *   fetch_to_sram       DMA2D copying each band's SOURCE from PSRAM into SRAM
 *   turn_sram_to_dst    the PPA turning a band from SRAM into the frame buffer
 *   in_seq              fetch a band's source into SRAM, then turn it into the
 *                       frame buffer, band after band
 *   in_pipe             the same with two SRAM bands: DMA2D fetches the next
 *                       band while the PPA turns the last one
 *   out_pipe            the first version's idea, for comparison: the PPA turns
 *                       into SRAM and DMA2D copies the result out
 *   gdma_rows           the AXI GDMA copying each band's source ROWS, whole,
 *                       into SRAM (where a band's source is whole rows)
 *   in_pipe_gdma        in_pipe with that copy: the PPA reads the band's
 *                       columns out of whole rows in SRAM
 *
 * Why the GDMA: esp_async_fbcpy, the DMA2D copy, is private to ESP-IDF 5.5's
 * esp_lcd and gone in 6.0; esp_async_memcpy is public in both. It only copies
 * runs, so a band has to be whole source rows, which is the layout that won
 * anyway: output rows at 180, output columns at 90 and 270.
 *
 * The first run of this firmware (both boards, 2026-11-15) answered half of
 * it: the PPA WRITING to PSRAM costs nothing over writing to SRAM, its
 * READING from PSRAM in 16x16 macroblocks is what doubles its time, and
 * DMA2D reads and writes PSRAM about three times faster than the PPA does.
 * So the band goes through SRAM on the way IN, not on the way out.
 *
 * "rows" bands are output rows, "cols" output columns (only for a 90/270
 * turn: their source is whole source rows, one run each). Three conditions:
 * before the panel exists (the destination is a PSRAM buffer), with the
 * panel scanning out its frame buffer, and with the camera streaming into
 * PSRAM as well.
 *
 * Lines start with PROBE, SUMMARY, PROBE_START and PROBE_DONE.
 */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/ppa.h"
#include "esp_async_fbcpy.h"
#include "esp_async_memcpy.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/color_types.h"
#include "hw_init.h"
#include "sdkconfig.h"
#include "probe_config.h"
#if PROBE_HAS_CAMERA
#include "ab_camera.h"
#endif

#define REPEAT      7

/* What the build was configured with, said in the log rather than assumed. */
#ifdef CONFIG_SPIRAM_SPEED
#define PSRAM_MHZ CONFIG_SPIRAM_SPEED
#else
#define PSRAM_MHZ 0
#endif
#ifdef CONFIG_CACHE_L2_CACHE_SIZE
#define L2_KB (CONFIG_CACHE_L2_CACHE_SIZE / 1024)
#else
#define L2_KB 0
#endif
#define ALIGN       128u
#define WAIT_TICKS  pdMS_TO_TICKS(2000)
#define PIX         2u          /* RGB565 */

typedef enum { LAYOUT_ROWS, LAYOUT_COLS } layout_t;
typedef enum { COPY_FBCPY, COPY_GDMA, COPY_CPU } copier_t;
static const char *const LAYOUT_NAME[] = {"rows", "cols"};
static const char *const COPY_NAME[] = {"fbcpy", "gdma", "cpu"};

typedef struct {
    uint32_t x, y, w, h;
} rect_t;

typedef struct {
    layout_t layout;
    uint32_t n;                     /* band size, in output rows or columns */
    copier_t copier;
    ppa_client_handle_t client;
    uint8_t *buf[2];                /* SRAM bands */
    size_t cap;                     /* bytes in each */
} job_t;

typedef bool (*op_t)(const job_t *);

static const rect_t BLOCK = {PROBE_BLOCK_X, PROBE_BLOCK_Y, PROBE_BLOCK_W, PROBE_BLOCK_H};
static uint16_t *s_src;             /* the camera-like picture, in PSRAM */
static size_t s_src_bytes;
static uint8_t *s_dst;              /* the panel's frame buffer, or PSRAM before there is a panel */
static const size_t s_dst_bytes = (size_t)PROBE_OUT_W * PROBE_OUT_H * PIX;
static uint32_t s_ref_crc;
static bool s_ref_known;
static bool s_last_ok;
static const char *s_cond = "?";

static ppa_client_handle_t s_ppa, s_ppa64, s_ppa_async;
static SemaphoreHandle_t s_ppa_done, s_copy_done;
static esp_async_fbcpy_handle_t s_fbcpy;
static async_memcpy_handle_t s_gdma;

static bool IRAM_ATTR ppa_done_cb(ppa_client_handle_t c, ppa_event_data_t *e, void *u)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_ppa_done, &woken);
    return woken == pdTRUE;
}

static bool IRAM_ATTR fbcpy_done_cb(esp_async_fbcpy_handle_t h, esp_async_fbcpy_event_data_t *e, void *u)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_copy_done, &woken);
    return woken == pdTRUE;
}

static bool IRAM_ATTR gdma_done_cb(async_memcpy_handle_t h, async_memcpy_event_t *e, void *u)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_copy_done, &woken);
    return woken == pdTRUE;
}

static size_t up(size_t n)
{
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

static void *in_psram(size_t n)
{
    return heap_caps_aligned_calloc(ALIGN, 1, up(n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *in_sram(size_t n)
{
    return heap_caps_aligned_calloc(ALIGN, 1, up(n), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

/* The part of the block whose turn is the output rectangle o. The PPA turns
 * counter-clockwise: at 90 a block pixel (x, y) lands at (y, bw-1-x), at 270
 * at (bh-1-y, x), at 180 at (bw-1-x, bh-1-y). A turn is pixel for pixel, so
 * a sub-block turned alone gives exactly that part of the whole turn -- which
 * the CRC of every banded result checks against ppa_whole's. */
static rect_t source_of(rect_t o)
{
    const uint32_t bw = BLOCK.w, bh = BLOCK.h;
    rect_t s;
    switch (PROBE_ANGLE) {
    case PPA_SRM_ROTATION_ANGLE_90:
        s = (rect_t){bw - o.y - o.h, o.x, o.h, o.w};
        break;
    case PPA_SRM_ROTATION_ANGLE_180:
        s = (rect_t){bw - o.x - o.w, bh - o.y - o.h, o.w, o.h};
        break;
    case PPA_SRM_ROTATION_ANGLE_270:
        s = (rect_t){o.y, bh - o.x - o.w, o.h, o.w};
        break;
    default:
        s = o;
        break;
    }
    s.x += BLOCK.x;
    s.y += BLOCK.y;
    return s;
}

static uint32_t span(layout_t l)
{
    return l == LAYOUT_ROWS ? PROBE_OUT_H : PROBE_OUT_W;
}

static rect_t band(layout_t l, uint32_t k0, uint32_t n)
{
    if (k0 + n > span(l)) n = span(l) - k0;
    return l == LAYOUT_ROWS ? (rect_t){0, k0, PROBE_OUT_W, n} : (rect_t){k0, 0, n, PROBE_OUT_H};
}

static esp_err_t turn(ppa_client_handle_t c, const void *in, uint32_t in_w, uint32_t in_h, rect_t s,
                      void *out, size_t out_size, uint32_t out_w, uint32_t out_h, uint32_t ox, uint32_t oy,
                      ppa_srm_rotation_angle_t angle, ppa_trans_mode_t mode)
{
    const ppa_srm_oper_config_t op = {
        .in = {
            .buffer = in, .pic_w = in_w, .pic_h = in_h, .block_w = s.w, .block_h = s.h,
            .block_offset_x = s.x, .block_offset_y = s.y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = out, .buffer_size = out_size, .pic_w = out_w, .pic_h = out_h,
            .block_offset_x = ox, .block_offset_y = oy, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = mode,
    };
    return ppa_do_scale_rotate_mirror(c, &op);
}

/* A band of the output, compact in SRAM (o.w x o.h), to its place in the
 * destination. Finished when s_copy_done is given, whatever the copier. */
static esp_err_t copy_start(copier_t c, const uint8_t *from, rect_t o)
{
    switch (c) {
    case COPY_FBCPY: {
        esp_async_fbcpy_trans_desc_t t = {
            .src_buffer = from,
            .dst_buffer = s_dst,
            .src_buffer_size_x = o.w,
            .src_buffer_size_y = o.h,
            .dst_buffer_size_x = PROBE_OUT_W,
            .dst_buffer_size_y = PROBE_OUT_H,
            .dst_offset_x = o.x,
            .dst_offset_y = o.y,
            .copy_size_x = o.w,
            .copy_size_y = o.h,
            .pixel_format_unique_id = {.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565)},
        };
        return esp_async_fbcpy(s_fbcpy, &t, fbcpy_done_cb, NULL);
    }
    case COPY_GDMA:
        /* Rows only: a full-width band is one run in the frame buffer. */
        return esp_async_memcpy(s_gdma, s_dst + (size_t)o.y * PROBE_OUT_W * PIX, (void *)from,
                                (size_t)o.w * o.h * PIX, gdma_done_cb, NULL);
    case COPY_CPU:
    default: {
        uint8_t *to = s_dst + ((size_t)o.y * PROBE_OUT_W + o.x) * PIX;
        for (uint32_t y = 0; y < o.h; y++) {
            memcpy(to + (size_t)y * PROBE_OUT_W * PIX, from + (size_t)y * o.w * PIX, (size_t)o.w * PIX);
        }
        esp_cache_msync(s_dst + (size_t)o.y * PROBE_OUT_W * PIX, (size_t)o.h * PROBE_OUT_W * PIX,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        xSemaphoreGive(s_copy_done);
        return ESP_OK;
    }
    }
}

static bool copy_wait(void)
{
    return xSemaphoreTake(s_copy_done, WAIT_TICKS) == pdTRUE;
}

static bool ppa_wait(void)
{
    return xSemaphoreTake(s_ppa_done, WAIT_TICKS) == pdTRUE;
}

/* ---- whole pictures ---- */

static bool op_whole(const job_t *j)
{
    const rect_t o = {0, 0, PROBE_OUT_W, PROBE_OUT_H};
    return turn(j->client, s_src, PROBE_SRC_W, PROBE_SRC_H, source_of(o), s_dst, s_dst_bytes,
                PROBE_OUT_W, PROBE_OUT_H, 0, 0, PROBE_ANGLE, PPA_TRANS_MODE_BLOCKING) == ESP_OK;
}

/* The PPA as a plain copy: as many pixels, no turn. */
static bool op_copy0(const job_t *j)
{
    const rect_t s = {0, 0, PROBE_OUT_W, PROBE_OUT_H};
    return turn(j->client, s_src, PROBE_OUT_W, PROBE_OUT_H, s, s_dst, s_dst_bytes,
                PROBE_OUT_W, PROBE_OUT_H, 0, 0, PPA_SRM_ROTATION_ANGLE_0, PPA_TRANS_MODE_BLOCKING) == ESP_OK;
}

/* What the PPA driver does to its input before every operation: a cache
 * write-back of the whole input window, clean or not. */
static bool op_c2m(const job_t *j)
{
    return esp_cache_msync(s_src, s_src_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED) == ESP_OK;
}

static bool op_raw_fbcpy(const job_t *j)
{
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = s_src, .dst_buffer = s_dst,
        .src_buffer_size_x = PROBE_OUT_W, .src_buffer_size_y = PROBE_OUT_H,
        .dst_buffer_size_x = PROBE_OUT_W, .dst_buffer_size_y = PROBE_OUT_H,
        .copy_size_x = PROBE_OUT_W, .copy_size_y = PROBE_OUT_H,
        .pixel_format_unique_id = {.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565)},
    };
    return esp_async_fbcpy(s_fbcpy, &t, fbcpy_done_cb, NULL) == ESP_OK && copy_wait();
}

static bool op_raw_gdma(const job_t *j)
{
    return esp_async_memcpy(s_gdma, s_dst, s_src, s_dst_bytes, gdma_done_cb, NULL) == ESP_OK && copy_wait();
}

static bool op_raw_cpu(const job_t *j)
{
    memcpy(s_dst, s_src, s_dst_bytes);
    return esp_cache_msync(s_dst, s_dst_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M) == ESP_OK;
}

/* ---- bands ---- */

static bool op_bands_direct(const job_t *j)
{
    for (uint32_t k = 0; k < span(j->layout); k += j->n) {
        const rect_t o = band(j->layout, k, j->n);
        if (turn(j->client, s_src, PROBE_SRC_W, PROBE_SRC_H, source_of(o), s_dst, s_dst_bytes,
                 PROBE_OUT_W, PROBE_OUT_H, o.x, o.y, PROBE_ANGLE, PPA_TRANS_MODE_BLOCKING) != ESP_OK) {
            return false;
        }
    }
    return true;
}

/* A band's source, out of the camera picture in PSRAM, into a compact SRAM
 * buffer: DMA2D reads the picture in long runs, where the PPA reads it in
 * 16x16 macroblocks. Finished when s_copy_done is given. */
static esp_err_t fetch_start(rect_t s, uint8_t *to)
{
    esp_async_fbcpy_trans_desc_t t = {
        .src_buffer = s_src,
        .dst_buffer = to,
        .src_buffer_size_x = PROBE_SRC_W,
        .src_buffer_size_y = PROBE_SRC_H,
        .dst_buffer_size_x = s.w,
        .dst_buffer_size_y = s.h,
        .src_offset_x = s.x,
        .src_offset_y = s.y,
        .copy_size_x = s.w,
        .copy_size_y = s.h,
        .pixel_format_unique_id = {.color_type_id = COLOR_TYPE_ID(COLOR_SPACE_RGB, COLOR_PIXEL_RGB565)},
    };
    return esp_async_fbcpy(s_fbcpy, &t, fbcpy_done_cb, NULL);
}

/* The source rows of band o, whole, into SRAM by the AXI GDMA: one run. */
static esp_err_t fetch_rows_start(rect_t s, uint8_t *to)
{
    return esp_async_memcpy(s_gdma, to, (uint8_t *)s_src + (size_t)s.y * PROBE_SRC_W * PIX,
                            (size_t)s.h * PROBE_SRC_W * PIX, gdma_done_cb, NULL);
}

/* The band o turned out of its source rows, whole, in SRAM. */
static bool turn_from_rows(ppa_client_handle_t c, const uint8_t *rows, rect_t o, ppa_trans_mode_t mode)
{
    const rect_t s = source_of(o);
    const rect_t in = {s.x, 0, s.w, s.h};
    return turn(c, rows, PROBE_SRC_W, s.h, in, s_dst, s_dst_bytes, PROBE_OUT_W, PROBE_OUT_H, o.x, o.y,
                PROBE_ANGLE, mode) == ESP_OK;
}

static bool op_fetch_rows(const job_t *j)
{
    for (uint32_t k = 0; k < span(j->layout); k += j->n) {
        if (fetch_rows_start(source_of(band(j->layout, k, j->n)), j->buf[0]) != ESP_OK || !copy_wait()) return false;
    }
    return true;
}

static bool op_in_pipe_gdma(const job_t *j)
{
    const uint32_t total = span(j->layout), n = j->n;
    const uint32_t count = (total + n - 1) / n;
    if (fetch_rows_start(source_of(band(j->layout, 0, n)), j->buf[0]) != ESP_OK || !copy_wait()) return false;
    for (uint32_t i = 0; i < count; i++) {
        const bool more = i + 1 < count;
        if (!turn_from_rows(s_ppa_async, j->buf[i & 1], band(j->layout, i * n, n), PPA_TRANS_MODE_NON_BLOCKING)) {
            return false;
        }
        if (more && fetch_rows_start(source_of(band(j->layout, (i + 1) * n, n)), j->buf[(i + 1) & 1]) != ESP_OK) {
            return false;
        }
        if (!ppa_wait()) return false;
        if (more && !copy_wait()) return false;
    }
    return true;
}

/* The band o's source, already compact in SRAM, turned into its place. */
static bool turn_from(ppa_client_handle_t c, const uint8_t *from, rect_t o, ppa_trans_mode_t mode)
{
    const rect_t s = source_of(o);
    const rect_t whole = {0, 0, s.w, s.h};
    return turn(c, from, s.w, s.h, whole, s_dst, s_dst_bytes, PROBE_OUT_W, PROBE_OUT_H, o.x, o.y,
                PROBE_ANGLE, mode) == ESP_OK;
}

static bool op_fetch(const job_t *j)
{
    for (uint32_t k = 0; k < span(j->layout); k += j->n) {
        if (fetch_start(source_of(band(j->layout, k, j->n)), j->buf[0]) != ESP_OK || !copy_wait()) return false;
    }
    return true;
}

static bool op_turn_from_sram(const job_t *j)
{
    for (uint32_t k = 0; k < span(j->layout); k += j->n) {
        if (!turn_from(j->client, j->buf[0], band(j->layout, k, j->n), PPA_TRANS_MODE_BLOCKING)) return false;
    }
    return true;
}

static bool op_in_seq(const job_t *j)
{
    for (uint32_t k = 0; k < span(j->layout); k += j->n) {
        const rect_t o = band(j->layout, k, j->n);
        if (fetch_start(source_of(o), j->buf[0]) != ESP_OK || !copy_wait()) return false;
        if (!turn_from(j->client, j->buf[0], o, PPA_TRANS_MODE_BLOCKING)) return false;
    }
    return true;
}

/* Band i is turned out of buf[i & 1] while band i+1 is fetched into the other.
 * Both are waited for before the next turn, so a fetch never lands on the
 * band the PPA is still reading. */
static bool op_in_pipe(const job_t *j)
{
    const uint32_t total = span(j->layout), n = j->n;
    const uint32_t count = (total + n - 1) / n;
    if (fetch_start(source_of(band(j->layout, 0, n)), j->buf[0]) != ESP_OK || !copy_wait()) return false;
    for (uint32_t i = 0; i < count; i++) {
        const bool more = i + 1 < count;
        if (!turn_from(s_ppa_async, j->buf[i & 1], band(j->layout, i * n, n), PPA_TRANS_MODE_NON_BLOCKING)) {
            return false;
        }
        if (more && fetch_start(source_of(band(j->layout, (i + 1) * n, n)), j->buf[(i + 1) & 1]) != ESP_OK) {
            return false;
        }
        if (!ppa_wait()) return false;
        if (more && !copy_wait()) return false;
    }
    return true;
}

static esp_err_t turn_async(const job_t *j, uint32_t k, uint8_t *to)
{
    const rect_t o = band(j->layout, k, j->n);
    return turn(s_ppa_async, s_src, PROBE_SRC_W, PROBE_SRC_H, source_of(o), to, j->cap,
                o.w, o.h, 0, 0, PROBE_ANGLE, PPA_TRANS_MODE_NON_BLOCKING);
}

/* Band i is turned into buf[i & 1] while band i-1 is copied out of the other.
 * The copy of band i-1 must finish before band i+1 is turned over it. */
static bool op_out_pipe(const job_t *j)
{
    const uint32_t total = span(j->layout), n = j->n;
    const uint32_t count = (total + n - 1) / n;
    if (turn_async(j, 0, j->buf[0]) != ESP_OK) return false;
    for (uint32_t i = 0; i < count; i++) {
        if (!ppa_wait()) return false;
        if (i >= 1 && !copy_wait()) return false;
        if (i + 1 < count && turn_async(j, (i + 1) * n, j->buf[(i + 1) & 1]) != ESP_OK) return false;
        if (copy_start(j->copier, j->buf[i & 1], band(j->layout, i * n, n)) != ESP_OK) return false;
    }
    return copy_wait();
}

/* ---- measuring ---- */

static void clear_dst(void)
{
    memset(s_dst, 0, s_dst_bytes);
    esp_cache_msync(s_dst, s_dst_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static uint32_t dst_crc(void)
{
    esp_cache_msync(s_dst, s_dst_bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return esp_rom_crc32_le(0, s_dst, s_dst_bytes);
}

static int by_value(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* check: the destination must end up as ppa_whole left it. */
static double measure(const char *test, op_t op, const job_t *j, bool check)
{
    double t[REPEAT];
    s_last_ok = false;
    if (check) clear_dst();
    for (int r = 0; r < REPEAT; r++) {
        const int64_t t0 = esp_timer_get_time();
        if (!op(j)) {
            printf("PROBE cond=%s test=%s layout=%s band=%" PRIu32 " copy=%s error\n", s_cond, test,
                   LAYOUT_NAME[j->layout], j->n, COPY_NAME[j->copier]);
            /* An operation that timed out may still finish: let it, and empty the semaphores. */
            vTaskDelay(pdMS_TO_TICKS(100));
            while (xSemaphoreTake(s_ppa_done, 0) == pdTRUE) {}
            while (xSemaphoreTake(s_copy_done, 0) == pdTRUE) {}
            return NAN;
        }
        t[r] = (double)(esp_timer_get_time() - t0) / 1000.0;
    }
    qsort(t, REPEAT, sizeof(t[0]), by_value);
    const char *crc = "-";
    if (check) {
        const uint32_t c = dst_crc();
        if (!s_ref_known) {
            s_ref_crc = c;
            s_ref_known = true;
            crc = "ref";
            s_last_ok = true;
        } else {
            s_last_ok = c == s_ref_crc;
            crc = s_last_ok ? "ok" : "DIFF";
        }
    }
    const double med = t[REPEAT / 2];
    printf("PROBE cond=%s test=%s layout=%s band=%" PRIu32 " copy=%s ms=%.2f min=%.2f max=%.2f fps=%.1f crc=%s\n",
           s_cond, test, LAYOUT_NAME[j->layout], j->n, COPY_NAME[j->copier], med, t[0], t[REPEAT - 1],
           1000.0 / med, crc);
    return med;
}

static void run_condition(const char *cond)
{
    s_cond = cond;
    printf("PROBE_COND %s: internal DMA RAM free %u (largest %u), PSRAM free %u\n", cond,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    job_t j = {.client = s_ppa};
    const double whole = measure("ppa_whole", op_whole, &j, true);
    j.client = s_ppa64;
    measure("ppa_whole_burst64", op_whole, &j, true);
    j.client = s_ppa;
    measure("ppa_copy0", op_copy0, &j, false);
    measure("cache_c2m_src", op_c2m, &j, false);
    measure("raw_fbcpy", op_raw_fbcpy, &j, false);
    if (s_gdma) measure("raw_gdma", op_raw_gdma, &j, false);
    measure("raw_cpu", op_raw_cpu, &j, false);

    double best_in = INFINITY, best_out = INFINITY, best_gdma = INFINITY;
    char name_in[48] = "none", name_out[48] = "none", name_gdma[48] = "none";
    static const uint32_t SIZES[] = {8, 16, 32, 64};
    const bool sideways = PROBE_ANGLE == PPA_SRM_ROTATION_ANGLE_90 || PROBE_ANGLE == PPA_SRM_ROTATION_ANGLE_270;
    for (int l = LAYOUT_ROWS; l <= LAYOUT_COLS; l++) {
        if (l == LAYOUT_COLS && !sideways) continue;
        for (size_t s = 0; s < sizeof(SIZES) / sizeof(SIZES[0]); s++) {
            job_t b = {.layout = (layout_t)l, .n = SIZES[s], .client = s_ppa, .copier = COPY_FBCPY};
            const rect_t o = band(b.layout, 0, b.n);
            /* A band whose source is whole rows (output rows at 0/180,
             * output columns at 90/270) can also be fetched as those rows. */
            bool rows_src = (l == LAYOUT_ROWS) != sideways && s_gdma;
            const size_t compact = up((size_t)o.w * o.h * PIX);
            /* Whole source rows can be wider than the band's columns (the
             * Waveshare's block is 1024 of 1280): room for them if there is
             * any, else the band compact and no GDMA tests for it. */
            b.cap = rows_src ? up((size_t)PROBE_SRC_W * source_of(o).h * PIX) : compact;
            b.buf[0] = in_sram(b.cap);
            b.buf[1] = in_sram(b.cap);
            if (b.cap != compact && (!b.buf[0] || !b.buf[1])) {
                heap_caps_free(b.buf[0]);
                heap_caps_free(b.buf[1]);
                rows_src = false;
                b.cap = compact;
                b.buf[0] = in_sram(b.cap);
                b.buf[1] = in_sram(b.cap);
            }
            if (!b.buf[0] || !b.buf[1]) {
                printf("PROBE cond=%s layout=%s band=%" PRIu32 " skipped: no room for 2 x %u bytes of SRAM\n",
                       cond, LAYOUT_NAME[l], b.n, (unsigned)b.cap);
            } else {
                /* calloc zeroed them through the cache: without this, the PPA
                 * driver's write-back of its input could put those zeros back
                 * over a band DMA2D has just written. */
                for (int i = 0; i < 2; i++) {
                    esp_cache_msync(b.buf[i], b.cap, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                }
                printf("PROBE cond=%s layout=%s band=%" PRIu32 " uses 2 x %u bytes of SRAM\n", cond,
                       LAYOUT_NAME[l], b.n, (unsigned)b.cap);
                measure("bands_direct", op_bands_direct, &b, true);
                measure("fetch_to_sram", op_fetch, &b, false);
                measure("turn_sram_to_dst", op_turn_from_sram, &b, false);
                double ms = measure("in_seq", op_in_seq, &b, true);
                ms = fmin(ms, measure("in_pipe", op_in_pipe, &b, true));
                if (s_last_ok && ms < best_in) {
                    best_in = ms;
                    snprintf(name_in, sizeof(name_in), "%s %" PRIu32, LAYOUT_NAME[l], b.n);
                }
                ms = measure("out_pipe", op_out_pipe, &b, true);
                if (s_last_ok && ms < best_out) {
                    best_out = ms;
                    snprintf(name_out, sizeof(name_out), "%s %" PRIu32, LAYOUT_NAME[l], b.n);
                }
                if (rows_src) {
                    b.copier = COPY_GDMA;
                    measure("gdma_rows", op_fetch_rows, &b, false);
                    ms = measure("in_pipe_gdma", op_in_pipe_gdma, &b, true);
                    if (s_last_ok && ms < best_gdma) {
                        best_gdma = ms;
                        snprintf(name_gdma, sizeof(name_gdma), "%s %" PRIu32, LAYOUT_NAME[l], b.n);
                    }
                    b.copier = COPY_FBCPY;
                }
            }
            heap_caps_free(b.buf[0]);
            heap_caps_free(b.buf[1]);
        }
    }
    printf("SUMMARY cond=%s ppa_whole=%.2f ms (%.1f fps) in_sram=%.2f ms (%.1f fps, %s) change=%+.0f%% "
           "in_gdma=%.2f ms (%.1f fps, %s) change=%+.0f%% out_sram=%.2f ms (%s) change=%+.0f%%\n",
           cond, whole, 1000.0 / whole, best_in, 1000.0 / best_in, name_in, (whole - best_in) / whole * 100.0,
           best_gdma, 1000.0 / best_gdma, name_gdma, (whole - best_gdma) / whole * 100.0,
           best_out, name_out, (whole - best_out) / whole * 100.0);
}

#if PROBE_HAS_CAMERA
static volatile bool s_cam_run;
static void camera_task(void *arg)
{
    while (s_cam_run) {
        (void)ab_camera_take();
    }
    vTaskDelete(NULL);
}
#endif

static ppa_client_handle_t client(ppa_data_burst_length_t burst, uint32_t pending)
{
    const ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = pending,
        .data_burst_length = burst,
    };
    ppa_client_handle_t c = NULL;
    ESP_ERROR_CHECK(ppa_register_client(&cfg, &c));
    return c;
}

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("PROBE_START board=%s src=%ux%u block=%u,%u,%ux%u out=%ux%u angle=%d rev=v%d.%d cpu=%dMHz "
           "psram=%dMHz l2=%dKB repeat=%d iots=%s\n",
           PROBE_BOARD, PROBE_SRC_W, PROBE_SRC_H, PROBE_BLOCK_X, PROBE_BLOCK_Y, PROBE_BLOCK_W, PROBE_BLOCK_H,
           PROBE_OUT_W, PROBE_OUT_H, (int)PROBE_ANGLE * 90, chip.revision / 100, chip.revision % 100,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, PSRAM_MHZ, L2_KB, REPEAT,
           PROBE_SOURCE_COMMIT);

    s_src_bytes = (size_t)PROBE_SRC_W * PROBE_SRC_H * PIX;
    s_src = in_psram(s_src_bytes);
    assert(s_src && s_dst_bytes % ALIGN == 0);
    /* A gradient and a checkerboard: a pixel out of place changes the CRC. */
    for (uint32_t y = 0; y < PROBE_SRC_H; y++) {
        for (uint32_t x = 0; x < PROBE_SRC_W; x++) {
            const uint32_t r = x * 31 / PROBE_SRC_W, g = y * 63 / PROBE_SRC_H, b = ((x / 48) ^ (y / 48)) & 31;
            s_src[y * PROBE_SRC_W + x] = (uint16_t)(r << 11 | g << 5 | b);
        }
    }
    esp_cache_msync(s_src, up(s_src_bytes), ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    s_ppa_done = xSemaphoreCreateBinary();
    s_copy_done = xSemaphoreCreateBinary();
    s_ppa = client(PPA_DATA_BURST_LENGTH_128, 1);
    s_ppa64 = client(PPA_DATA_BURST_LENGTH_64, 1);
    s_ppa_async = client(PPA_DATA_BURST_LENGTH_128, 2);
    const ppa_event_callbacks_t cbs = {.on_trans_done = ppa_done_cb};
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_ppa_async, &cbs));
    const esp_async_fbcpy_config_t fbcpy = {};
    ESP_ERROR_CHECK(esp_async_fbcpy_install(&fbcpy, &s_fbcpy));
    for (size_t burst = 64; burst >= 16 && !s_gdma; burst /= 2) {
        async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
        cfg.backlog = 4;
        cfg.dma_burst_size = burst;
        if (esp_async_memcpy_install_gdma_axi(&cfg, &s_gdma) == ESP_OK) {
            printf("PROBE_GDMA burst=%u bytes\n", (unsigned)burst);
        } else {
            s_gdma = NULL;
        }
    }

    /* 1. No panel yet: nothing else reads PSRAM. */
    s_dst = in_psram(s_dst_bytes);
    assert(s_dst);
    run_condition("psram");
    heap_caps_free(s_dst);

    /* 2. The panel scanning out the frame buffer the picture is turned into. */
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(hw_lcd_init(&panel, NULL, ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT, ESP_LV_ADAPTER_ROTATE_0));
    void *fb = NULL;
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb));
    s_dst = fb;
    vTaskDelay(pdMS_TO_TICKS(300));
    run_condition("panel");

#if PROBE_HAS_CAMERA
    /* 3. And the camera streaming into PSRAM, as in a camera YAML. */
    ab_camera_info_t info;
    if (ab_camera_start(&info) == ESP_OK) {
        printf("PROBE_CAMERA %s %ux%u\n", info.sensor, info.width, info.height);
        s_cam_run = true;
        xTaskCreatePinnedToCore(camera_task, "probe_cam", 4096, NULL, 5, NULL, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        ab_camera_reset_counts();
        const int64_t t0 = esp_timer_get_time();
        run_condition("panel+camera");
        const double secs = (double)(esp_timer_get_time() - t0) / 1e6;
        const ab_camera_counts_t n = ab_camera_counts();
        printf("PROBE_CAMERA received %" PRIu32 " frames in %.1f s (%.1f fps), %" PRIu32 " errors\n",
               n.received, secs, n.received / secs, n.errors);
        s_cam_run = false;
    } else {
        printf("PROBE_CAMERA did not start: the third condition is skipped\n");
    }
#endif
    printf("PROBE_DONE\n");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
