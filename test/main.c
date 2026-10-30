/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/*
 * The proof that lv_draw_ortho draws what LVGL draws.
 *
 * The same UI is built twice. Once on a display that is NOT turned, rendered
 * by LVGL's own software renderer, and then turned with LVGL's own
 * lv_draw_sw_rotate -- exactly what a flush callback does today. Once on a
 * display turned with matrix rotation, rendered by lv_draw_ortho straight into
 * native orientation. The two native frames must be identical, byte for byte,
 * at every angle, in both colour formats, with the fast paths on and off.
 *
 * Then it times a full-screen picture (what a video is) both ways.
 */

#include "lvgl.h"

/* 9.6 renamed lv_draw_sw_rotate and warns on the old name. */
#if LVGL_VERSION_MAJOR > 9 || (LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 6)
#define turn_buffer lv_draw_rotate
#else
#define turn_buffer lv_draw_sw_rotate
#endif
#include "lv_draw_ortho.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures;
static int g_cases;

typedef struct {
    lv_display_t * disp;
    uint8_t * buf;     /* the whole native frame */
    uint32_t stride;
    int32_t w, h;      /* the frame's own size */
    uint8_t * render;  /* partial mode: LVGL's small buffer, else NULL */
} screen_t;

/* Direct mode draws into the frame itself. Partial mode hands over a small
 * buffer holding the NATIVE area (with the patch in ../patches), which is
 * copied in as it stands: no turn, which is the whole point. */
static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px)
{
    screen_t * s = lv_display_get_user_data(disp);
    if(s->render) {
        const int32_t bpp = lv_color_format_get_size(lv_display_get_color_format(disp));
        const int32_t row = lv_area_get_width(area) * bpp;
        const uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area),
                                                                lv_display_get_color_format(disp));
        for(int32_t y = area->y1; y <= area->y2; y++)
            memcpy(s->buf + y * s->stride + area->x1 * bpp, px + (y - area->y1) * src_stride, row);
    }
    lv_display_flush_ready(disp);
}

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Partial mode's band height in rows; 0 for an eighth of the screen. */
static int32_t g_partial_rows;

static screen_t * make_screen(int32_t w, int32_t h, lv_color_format_t cf, lv_display_rotation_t rot, bool matrix,
                              bool partial)
{
    screen_t * ps = calloc(1, sizeof(*ps));
    screen_t s = {0};
    s.w = w;
    s.h = h;
    s.disp = lv_display_create(w, h);
    lv_display_set_color_format(s.disp, cf);
    s.stride = lv_draw_buf_width_to_stride(w, cf);
    size_t size = (size_t)s.stride * h;
    s.buf = aligned_alloc(LV_DRAW_BUF_ALIGN, (size + LV_DRAW_BUF_ALIGN - 1) / LV_DRAW_BUF_ALIGN * LV_DRAW_BUF_ALIGN);
    memset(s.buf, 0x5a, size);
    if(partial) {
        /* An eighth of the screen, odd on purpose so bands split unevenly. */
        size_t rsize = g_partial_rows ? (size_t)s.stride * g_partial_rows : size / 8 + 1234;
        s.render = aligned_alloc(LV_DRAW_BUF_ALIGN, (rsize + LV_DRAW_BUF_ALIGN - 1) / LV_DRAW_BUF_ALIGN * LV_DRAW_BUF_ALIGN);
        lv_display_set_buffers(s.disp, s.render, NULL, rsize, LV_DISPLAY_RENDER_MODE_PARTIAL);
    }
    else {
        lv_display_set_buffers_with_stride(s.disp, s.buf, NULL, size, s.stride, LV_DISPLAY_RENDER_MODE_DIRECT);
    }
    lv_display_set_flush_cb(s.disp, flush_cb);
    lv_display_set_rotation(s.disp, rot);
    if(matrix) lv_display_set_matrix_rotation(s.disp, true);
    *ps = s;
    lv_display_set_user_data(s.disp, ps);
    return ps;
}

static void drop_screen(screen_t * s)
{
    lv_display_delete(s->disp);
    free(s->buf);
    free(s->render);
    free(s);
}

/* ------------------------------------------------------------------------ */
/* Pictures                                                                 */
/* ------------------------------------------------------------------------ */

static uint8_t * pattern(int32_t w, int32_t h, lv_color_format_t cf, uint32_t seed, uint32_t * stride_out)
{
    const int32_t px = lv_color_format_get_size(cf);
    const uint32_t stride = w * px;
    uint8_t * p = malloc((size_t)stride * h);
    for(int32_t y = 0; y < h; y++) {
        for(int32_t x = 0; x < w; x++) {
            uint32_t r = (x * 7 + seed) & 0xff, g = (y * 5 + seed * 3) & 0xff, b = ((x ^ y) + seed) & 0xff;
            uint8_t * q = p + y * stride + x * px;
            if(cf == LV_COLOR_FORMAT_RGB565) {
                uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                memcpy(q, &v, 2);
            }
            else if(cf == LV_COLOR_FORMAT_XRGB8888 || cf == LV_COLOR_FORMAT_ARGB8888) {
                q[0] = b;
                q[1] = g;
                q[2] = r;
                q[3] = cf == LV_COLOR_FORMAT_ARGB8888 ? (uint8_t)((x * 255) / (w - 1)) : 0xff;
            }
        }
    }
    *stride_out = stride;
    return p;
}

static lv_image_dsc_t * image_of(int32_t w, int32_t h, lv_color_format_t cf, uint32_t seed)
{
    lv_image_dsc_t * d = calloc(1, sizeof(*d));
    uint32_t stride;
    uint8_t * data = pattern(w, h, cf, seed, &stride);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = cf;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = stride;
    d->data = data;
    d->data_size = stride * h;
    return d;
}

/* The same picture stored the way the panel will show it. */
static lv_image_dsc_t * native_image_of(const lv_image_dsc_t * up, lv_display_rotation_t rot)
{
    const int32_t px = lv_color_format_get_size(up->header.cf);
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    const int32_t nw = swap ? up->header.h : up->header.w;
    const int32_t nh = swap ? up->header.w : up->header.h;
    lv_image_dsc_t * d = calloc(1, sizeof(*d));
    *d = *up;
    uint8_t * data = malloc((size_t)nw * nh * px);
    turn_buffer(up->data, data, up->header.w, up->header.h, up->header.stride, nw * px, rot, up->header.cf);
    d->data = data;
    d->header.stride = nw * px;
    d->header.flags |= LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE;
    return d;
}

/* ------------------------------------------------------------------------ */
/* Scenes                                                                   */
/* ------------------------------------------------------------------------ */

static lv_image_dsc_t * g_photo;
static lv_image_dsc_t * g_alpha;

/* The polyline of scene_ui alone, for --lvgl-quirk. */
static void scene_line(lv_obj_t * scr)
{
    static lv_point_precise_t pts[] = {{0, 0}, {60, 30}, {120, 5}, {170, 50}};
    lv_obj_t * line = lv_line_create(scr);
    lv_line_set_points(line, pts, 4);
    lv_obj_set_style_line_width(line, 5, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_align(line, LV_ALIGN_BOTTOM_RIGHT, -10, -20);
}

static void scene_ui(lv_obj_t * scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x203040), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x80a0f0), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t * panel = lv_obj_create(scr);
    lv_obj_set_size(panel, lv_pct(80), lv_pct(45));
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_border_width(panel, 3, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0xffcc00), 0);
    lv_obj_set_style_shadow_width(panel, 20, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_offset_y(panel, 6, 0);
    lv_obj_set_style_bg_grad_color(panel, lv_color_hex(0xff4060), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW_WRAP);

    for(int i = 0; i < 5; i++) {
        lv_obj_t * b = lv_button_create(panel);
        lv_obj_t * l = lv_label_create(b);
        lv_label_set_text_fmt(l, "Lien %d", i + 1);
        if(i == 2) lv_obj_set_style_bg_opa(b, LV_OPA_50, 0);
        if(i == 4) lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    }

    lv_obj_t * big = lv_label_create(scr);
    lv_obj_set_style_text_font(big, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(big, lv_color_hex(0xffffff), 0);
    lv_label_set_text(big, "12:34 Bonjour, ecran tourne!");
    lv_obj_align(big, LV_ALIGN_CENTER, 0, 10);

    lv_obj_t * arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 90, 90);
    lv_arc_set_value(arc, 70);
    lv_obj_align(arc, LV_ALIGN_BOTTOM_LEFT, 10, -10);

    static lv_point_precise_t pts[] = {{0, 0}, {60, 30}, {120, 5}, {170, 50}};
    lv_obj_t * line = lv_line_create(scr);
    lv_line_set_points(line, pts, 4);
    lv_obj_set_style_line_width(line, 5, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_align(line, LV_ALIGN_BOTTOM_RIGHT, -10, -20);

    lv_obj_t * img = lv_image_create(scr);
    lv_image_set_src(img, g_photo);
    lv_obj_align(img, LV_ALIGN_LEFT_MID, 6, 40);

    lv_obj_t * img2 = lv_image_create(scr);
    lv_image_set_src(img2, g_alpha);
    lv_obj_align(img2, LV_ALIGN_RIGHT_MID, -6, 40);

    /* Partly off-screen, so clipping at the buffer's edge is exercised. */
    lv_obj_t * img3 = lv_image_create(scr);
    lv_image_set_src(img3, g_photo);
    lv_obj_set_pos(img3, -20, -15);

    lv_obj_t * slider = lv_slider_create(scr);
    lv_obj_set_width(slider, lv_pct(60));
    lv_slider_set_value(slider, 40, LV_ANIM_OFF);
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -110);

    /* A translucent group: LVGL draws it on its own layer. */
    lv_obj_t * glass = lv_obj_create(scr);
    lv_obj_set_size(glass, 120, 70);
    lv_obj_align(glass, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_opa(glass, LV_OPA_60, 0);
    lv_obj_t * gl = lv_label_create(glass);
    lv_label_set_text(gl, "verre");
}

/* ------------------------------------------------------------------------ */
/* Comparing                                                                */
/* ------------------------------------------------------------------------ */

static void save_ppm(const char * name, const uint8_t * buf, int32_t w, int32_t h, uint32_t stride,
                     lv_color_format_t cf)
{
    FILE * f = fopen(name, "wb");
    if(!f) return;
    fprintf(f, "P6\n%d %d\n255\n", (int)w, (int)h);
    for(int32_t y = 0; y < h; y++) {
        for(int32_t x = 0; x < w; x++) {
            const uint8_t * q = buf + y * stride + x * lv_color_format_get_size(cf);
            uint8_t rgb[3];
            if(cf == LV_COLOR_FORMAT_RGB565) {
                uint16_t v;
                memcpy(&v, q, 2);
                rgb[0] = (v >> 8) & 0xf8;
                rgb[1] = (v >> 3) & 0xfc;
                rgb[2] = (v << 3) & 0xf8;
            }
            else {
                rgb[0] = q[2];
                rgb[1] = q[1];
                rgb[2] = q[0];
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static const char * rot_name(lv_display_rotation_t r)
{
    return r == LV_DISPLAY_ROTATION_0 ? "0" : r == LV_DISPLAY_ROTATION_90 ? "90" :
           r == LV_DISPLAY_ROTATION_180 ? "180" : "270";
}

static const char * cf_name(lv_color_format_t cf)
{
    return cf == LV_COLOR_FORMAT_RGB565 ? "RGB565" : "XRGB8888";
}

#define MAX_EXCUSED 32
typedef struct {
    int32_t n;
    int32_t x[MAX_EXCUSED], y[MAX_EXCUSED];
} diffs_t;

static bool pixel_same(const uint8_t * a, const uint8_t * b, uint32_t stride, int32_t x, int32_t y,
                       lv_color_format_t cf)
{
    const int32_t px = lv_color_format_get_size(cf);
    /* The X byte of XRGB8888 carries nothing. */
    const int n = cf == LV_COLOR_FORMAT_XRGB8888 ? 3 : px;
    return memcmp(a + y * stride + x * px, b + y * stride + x * px, n) == 0;
}

static long count_diffs(const screen_t * nat, const uint8_t * want, lv_color_format_t cf, diffs_t * d)
{
    long diff = 0;
    d->n = 0;
    for(int32_t y = 0; y < nat->h; y++) {
        for(int32_t x = 0; x < nat->w; x++) {
            if(pixel_same(nat->buf, want, nat->stride, x, y, cf)) continue;
            if(d->n < MAX_EXCUSED) {
                d->x[d->n] = x;
                d->y[d->n] = y;
                d->n++;
            }
            diff++;
        }
    }
    return diff;
}

static void expect_same(const char * what, const screen_t * nat, const uint8_t * want, lv_color_format_t cf)
{
    g_cases++;
    const int32_t px = lv_color_format_get_size(cf);
    long diff = 0;
    int32_t fx = -1, fy = -1;
    for(int32_t y = 0; y < nat->h; y++) {
        for(int32_t x = 0; x < nat->w; x++) {
            const uint8_t * a = nat->buf + y * nat->stride + x * px;
            const uint8_t * b = want + y * nat->stride + x * px;
            /* The X byte of XRGB8888 carries nothing. */
            int n = cf == LV_COLOR_FORMAT_XRGB8888 ? 3 : px;
            if(memcmp(a, b, n) != 0) {
                if(diff == 0) {
                    fx = x;
                    fy = y;
                }
                diff++;
            }
        }
    }
    if(diff == 0) {
        printf("  ok    %s\n", what);
        return;
    }
    g_failures++;
    printf("  FAIL  %s: %ld pixels differ, first at %d,%d\n", what, diff, (int)fx, (int)fy);
    char name[256];
    snprintf(name, sizeof(name), "fail-%d-got.ppm", g_failures);
    save_ppm(name, nat->buf, nat->w, nat->h, nat->stride, cf);
    snprintf(name, sizeof(name), "fail-%d-want.ppm", g_failures);
    save_ppm(name, want, nat->w, nat->h, nat->stride, cf);
}

/* Render `build` upright and turn it the way a flush callback would. With
 * `rows`, LVGL renders in partial bands of that many rows. */
static uint8_t * reference_cut(int32_t nw, int32_t nh, lv_color_format_t cf, lv_display_rotation_t rot,
                               void (*build)(lv_obj_t *), int32_t rows)
{
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    g_partial_rows = rows;
    screen_t * up = make_screen(swap ? nh : nw, swap ? nw : nh, cf, LV_DISPLAY_ROTATION_0, false, rows > 0);
    g_partial_rows = 0;
    lv_display_set_default(up->disp);
    build(lv_display_get_screen_active(up->disp));
    lv_refr_now(up->disp);
    const uint32_t nstride = lv_draw_buf_width_to_stride(nw, cf);
    uint8_t * want = malloc((size_t)nstride * nh);
    if(rot == LV_DISPLAY_ROTATION_0) memcpy(want, up->buf, (size_t)nstride * nh);
    else turn_buffer(up->buf, want, up->w, up->h, up->stride, nstride, rot, cf);
    drop_screen(up);
    return want;
}

static uint8_t * reference(int32_t nw, int32_t nh, lv_color_format_t cf, lv_display_rotation_t rot,
                           void (*build)(lv_obj_t *))
{
    return reference_cut(nw, nh, cf, rot, build, 0);
}

/* LVGL's own renderer is not quite independent of where an area is cut: a
 * polyline's joint can come out one anti-aliased step different when a band
 * ends on it (reproduced with LVGL alone, see the README). lv_draw_ortho cuts
 * into tiles, so it can meet the same thing. A differing pixel is excused only
 * if LVGL ITSELF, made to cut its bands at that pixel's row, draws exactly
 * what lv_draw_ortho drew. Everything else is a failure. */
static int32_t excuse(const screen_t * nat, const diffs_t * d, int32_t nw, int32_t nh, lv_color_format_t cf,
                      lv_display_rotation_t rot, void (*build)(lv_obj_t *))
{
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    const int32_t lh = swap ? nw : nh;
    int32_t excused = 0;
    for(int32_t i = 0; i < d->n; i++) {
        /* The row of the upright picture this native pixel came from. */
        int32_t ly = rot == LV_DISPLAY_ROTATION_90 ? d->x[i] : rot == LV_DISPLAY_ROTATION_180 ? nh - 1 - d->y[i] :
                     rot == LV_DISPLAY_ROTATION_270 ? lh - 1 - d->x[i] : d->y[i];
        bool ok = false;
        for(int32_t k = 0; k < 2 && !ok; k++) {
            int32_t rows = ly + k;  /* a band ending just above the row, then on it */
            if(rows < 1) continue;
            uint8_t * alt = reference_cut(nw, nh, cf, rot, build, rows);
            ok = pixel_same(nat->buf, alt, nat->stride, d->x[i], d->y[i], cf);
            free(alt);
        }
        if(!ok) return -1;
        excused++;
    }
    return excused;
}

static void check_scene(const char * scene, int32_t nw, int32_t nh, lv_color_format_t cf,
                        lv_display_rotation_t rot, void (*build)(lv_obj_t *))
{
    uint8_t * want = reference(nw, nh, cf, rot, build);
    for(int run = 0; run < 4; run++) {
        const bool fast = run != 1, partial = run == 2, batch = run != 3;
        lv_draw_ortho_set_fast_paths(fast);
        lv_draw_ortho_set_batching(batch);
        screen_t * nat = make_screen(nw, nh, cf, rot, true, partial);
        lv_display_set_default(nat->disp);
        build(lv_display_get_screen_active(nat->disp));
        lv_draw_ortho_reset_stats();
        lv_refr_now(nat->disp);
        const lv_draw_ortho_stats_t * st = lv_draw_ortho_get_stats();
        char what[200];
        snprintf(what, sizeof(what), "%s %dx%d %s at %s, %s, fast %s, %s (%u fills, %u images, %u tiles, %u refused)",
                 scene, (int)nw, (int)nh, cf_name(cf), rot_name(rot), partial ? "partial" : "direct ",
                 fast ? "on " : "off", batch ? "batched" : "one by one",
                 st->fills_direct, st->images_turned + st->images_native, st->tiles, st->tasks_refused);
        diffs_t d;
        long nd = count_diffs(nat, want, cf, &d);
        int32_t excused = nd > 0 && nd <= MAX_EXCUSED ? excuse(nat, &d, nw, nh, cf, rot, build) : -1;
        if(excused > 0) {
            g_cases++;
            printf("  ok    %s -- %d pixel(s) exactly as LVGL itself draws them when cut there\n", what, (int)excused);
        }
        else expect_same(what, nat, want, cf);
        if(rot != LV_DISPLAY_ROTATION_0 && st->tasks_general + st->fills_direct + st->images_turned == 0) {
            g_failures++;
            printf("  FAIL  ...and lv_draw_ortho drew nothing, so the case proved nothing\n");
        }
        drop_screen(nat);
    }
    lv_draw_ortho_set_fast_paths(true);
    lv_draw_ortho_set_batching(true);
    free(want);
}

/* A full-screen picture, which is what one frame of a video is. */
static lv_image_dsc_t * g_frame;
static void scene_frame(lv_obj_t * scr)
{
    lv_obj_t * img = lv_image_create(scr);
    lv_image_set_src(img, g_frame);
    lv_obj_set_pos(img, 0, 0);
}

static lv_image_dsc_t * g_native_frame;
static void scene_native_frame(lv_obj_t * scr)
{
    lv_obj_t * img = lv_image_create(scr);
    lv_image_set_src(img, g_native_frame);
    lv_obj_set_pos(img, 0, 0);
}

static void check_native(int32_t nw, int32_t nh, lv_color_format_t cf, lv_display_rotation_t rot)
{
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    g_frame = image_of(swap ? nh : nw, swap ? nw : nh, cf, 11);
    g_native_frame = native_image_of(g_frame, rot);
    uint8_t * want = reference(nw, nh, cf, rot, scene_frame);

    screen_t * nat = make_screen(nw, nh, cf, rot, true, false);
    lv_display_set_default(nat->disp);
    scene_native_frame(lv_display_get_screen_active(nat->disp));
    lv_draw_ortho_reset_stats();
    lv_refr_now(nat->disp);
    char what[160];
    snprintf(what, sizeof(what), "pre-turned frame %dx%d %s at %s (%u straight copies)", (int)nw, (int)nh,
             cf_name(cf), rot_name(rot), lv_draw_ortho_get_stats()->images_native);
    expect_same(what, nat, want, cf);
    if(lv_draw_ortho_get_stats()->images_native == 0) {
        g_failures++;
        printf("  FAIL  ...and it was not copied straight\n");
    }
    drop_screen(nat);
    free(want);
}

/* ------------------------------------------------------------------------ */
/* Timing                                                                   */
/* ------------------------------------------------------------------------ */

static double time_frames(lv_display_t * disp, lv_obj_t * img, int n, void (*after)(void *), void * arg)
{
    double best = 1e9;
    for(int i = 0; i < n; i++) {
        lv_obj_invalidate(img);
        double t0 = now_ms();
        lv_refr_now(disp);
        if(after) after(arg);
        double dt = now_ms() - t0;
        if(dt < best) best = dt;
    }
    return best;
}

typedef struct {
    screen_t * up;
    uint8_t * fb;
    uint32_t fb_stride;
    lv_display_rotation_t rot;
} turn_arg_t;

static void flush_turn(void * p)
{
    turn_arg_t * a = p;
    turn_buffer(a->up->buf, a->fb, a->up->w, a->up->h, a->up->stride, a->fb_stride, a->rot,
                      lv_display_get_color_format(a->up->disp));
}

static void bench(int32_t nw, int32_t nh, lv_display_rotation_t rot)
{
    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    g_frame = image_of(swap ? nh : nw, swap ? nw : nh, cf, 3);
    g_native_frame = native_image_of(g_frame, rot);
    const int n = 20;

    screen_t * up = make_screen(swap ? nh : nw, swap ? nw : nh, cf, LV_DISPLAY_ROTATION_0, false, false);
    lv_display_set_default(up->disp);
    lv_obj_t * i1 = lv_image_create(lv_display_get_screen_active(up->disp));
    lv_image_set_src(i1, g_frame);
    lv_refr_now(up->disp);
    turn_arg_t a = {up, malloc(lv_draw_buf_width_to_stride(nw, cf) * nh), lv_draw_buf_width_to_stride(nw, cf), rot};
    double classic = time_frames(up->disp, i1, n, flush_turn, &a);
    double upright = time_frames(up->disp, i1, n, NULL, NULL);
    free(a.fb);
    drop_screen(up);

    screen_t * nat = make_screen(nw, nh, cf, rot, true, false);
    lv_display_set_default(nat->disp);
    lv_obj_t * i2 = lv_image_create(lv_display_get_screen_active(nat->disp));
    lv_image_set_src(i2, g_frame);
    lv_refr_now(nat->disp);
    double turned = time_frames(nat->disp, i2, n, NULL, NULL);
    lv_image_set_src(i2, g_native_frame);
    lv_refr_now(nat->disp);
    double straight = time_frames(nat->disp, i2, n, NULL, NULL);
    drop_screen(nat);

    printf("  %dx%d at %s, one full-screen picture, best of %d (this machine, not a P4):\n",
           (int)nw, (int)nh, rot_name(rot), n);
    printf("    LVGL today, draw upright then turn in flush : %7.2f ms\n", classic);
    printf("      of which the upright draw alone           : %7.2f ms\n", upright);
    printf("    lv_draw_ortho, turned in one pass           : %7.2f ms\n", turned);
    printf("    lv_draw_ortho, picture already native       : %7.2f ms\n", straight);
}

/* A whole UI redrawn: text, shadows, borders, gradients -- the tasks that go
 * through tiles rather than a fast path. */
static void bench_ui(int32_t nw, int32_t nh, lv_display_rotation_t rot)
{
    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    const int n = 20;

    screen_t * up = make_screen(swap ? nh : nw, swap ? nw : nh, cf, LV_DISPLAY_ROTATION_0, false, false);
    lv_display_set_default(up->disp);
    lv_obj_t * s1 = lv_display_get_screen_active(up->disp);
    scene_ui(s1);
    lv_refr_now(up->disp);
    turn_arg_t a = {up, malloc(lv_draw_buf_width_to_stride(nw, cf) * nh), lv_draw_buf_width_to_stride(nw, cf), rot};
    double classic = time_frames(up->disp, s1, n, flush_turn, &a);
    free(a.fb);
    drop_screen(up);

    screen_t * nat = make_screen(nw, nh, cf, rot, true, false);
    lv_display_set_default(nat->disp);
    lv_obj_t * s2 = lv_display_get_screen_active(nat->disp);
    scene_ui(s2);
    lv_refr_now(nat->disp);
    double ortho = time_frames(nat->disp, s2, n, NULL, NULL);
    lv_draw_ortho_reset_stats();
    lv_obj_invalidate(s2);
    lv_refr_now(nat->disp);
    const lv_draw_ortho_stats_t * st = lv_draw_ortho_get_stats();
    drop_screen(nat);

    printf("  %dx%d at %s, the whole test UI redrawn, best of %d:\n", (int)nw, (int)nh, rot_name(rot), n);
    printf("    LVGL today, draw upright then turn in flush : %7.2f ms\n", classic);
    printf("    lv_draw_ortho                               : %7.2f ms\n", ortho);
    printf("      per frame: %u tasks in %u batches, %u tiles (%u not read back), %u fills and %u images direct,\n"
           "      %.0f KiB turned, %.0f KiB copied straight\n",
           st->tasks_general, st->batches, st->tiles, st->tiles_unread, st->fills_direct,
           st->images_turned + st->images_native, st->bytes_turned / 1024.0, st->bytes_straight / 1024.0);
}

/* LVGL alone, no lv_draw_ortho: the same polyline drawn whole, and drawn in
 * partial bands of 51 rows (so a band ends on row 407, on the line's joint).
 * The two should be identical and are not -- which is LVGL's, and is why the
 * test excuses a pixel only when LVGL itself draws it the same way. */
static int lvgl_quirk(void)
{
    uint8_t * whole = reference_cut(320, 480, LV_COLOR_FORMAT_RGB565, LV_DISPLAY_ROTATION_0, scene_line, 0);
    uint8_t * banded = reference_cut(320, 480, LV_COLOR_FORMAT_RGB565, LV_DISPLAY_ROTATION_0, scene_line, 51);
    const uint32_t stride = lv_draw_buf_width_to_stride(320, LV_COLOR_FORMAT_RGB565);
    int n = 0;
    for(int32_t y = 0; y < 480; y++) {
        for(int32_t x = 0; x < 320; x++) {
            if(!pixel_same(whole, banded, stride, x, y, LV_COLOR_FORMAT_RGB565)) {
                uint16_t a, b;
                memcpy(&a, whole + y * stride + x * 2, 2);
                memcpy(&b, banded + y * stride + x * 2, 2);
                printf("  LVGL alone: pixel %d,%d is %04x drawn whole and %04x drawn in bands of 51 rows\n",
                       (int)x, (int)y, a, b);
                n++;
            }
        }
    }
    if(n == 0) printf("  LVGL alone draws the polyline the same whole and in bands\n");
    free(whole);
    free(banded);
    return 0;
}

int main(int argc, char ** argv)
{
    bool run_bench = argc > 1 && strcmp(argv[1], "--bench") == 0;
    bool quirk = argc > 1 && strcmp(argv[1], "--lvgl-quirk") == 0;
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_draw_ortho_init();
    if(quirk) return lvgl_quirk();

    const lv_display_rotation_t rots[] = {LV_DISPLAY_ROTATION_90, LV_DISPLAY_ROTATION_180,
                                          LV_DISPLAY_ROTATION_270, LV_DISPLAY_ROTATION_0
                                         };
    const lv_color_format_t cfs[] = {LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_XRGB8888};

    for(unsigned c = 0; c < 2; c++) {
        g_photo = image_of(97, 61, cfs[c], 1);
        g_alpha = image_of(80, 50, LV_COLOR_FORMAT_ARGB8888, 2);
        for(unsigned r = 0; r < 4; r++) {
            /* Portrait panels as the P4 boards have them, and an odd size. */
            check_scene("ui", 320, 480, cfs[c], rots[r], scene_ui);
            check_scene("ui", 333, 517, cfs[c], rots[r], scene_ui);
            if(rots[r] != LV_DISPLAY_ROTATION_0) check_native(320, 480, cfs[c], rots[r]);
        }
    }

    printf("%d of %d cases passed\n", g_cases - g_failures, g_cases);

    if(run_bench) {
        bench(720, 1280, LV_DISPLAY_ROTATION_270);
        bench(800, 1280, LV_DISPLAY_ROTATION_90);
        g_photo = image_of(97, 61, LV_COLOR_FORMAT_RGB565, 1);
        bench_ui(720, 1280, LV_DISPLAY_ROTATION_270);
    }
    return g_failures ? 1 : 0;
}
