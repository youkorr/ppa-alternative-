/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/**
 * @file lv_draw_ortho.c
 *
 * See lv_draw_ortho.h for what this is for. The comments here are about how.
 */

#include "lv_draw_ortho.h"

#if LV_USE_DRAW_ORTHO

#include "src/draw/lv_draw_private.h"
#include "src/draw/lv_draw_buf_private.h"
#include "src/draw/lv_draw_image_private.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "src/misc/lv_area_private.h"
#include <math.h>
#include <string.h>

/* Any id no other unit uses. LVGL's own units count up from 1. */
#define DRAW_UNIT_ID_ORTHO 79

/**
 * A quarter-turn matrix reduced to integers, pixel for pixel:
 *     native_x = a * x + b * y + tx
 *     native_y = c * x + d * y + ty
 * The matrix LVGL hands over maps pixel EDGES (a vector renderer's view), so
 * a pixel's centre x + 0.5 lands on native_x + 0.5. Where a row of the matrix
 * sums to -1 that moves the integer result down by one; `ortho_from_matrix`
 * folds that in so nothing downstream has to know.
 */
typedef struct {
    int32_t a, b, c, d;
    int32_t tx, ty;
} ortho_t;

typedef struct {
    lv_draw_unit_t base;
    lv_draw_task_t * task_act;
    uint8_t * tile;          /* LV_DRAW_ORTHO_TILE_BYTES, aligned */
    void * tile_alloc;
} ortho_unit_t;

static ortho_unit_t * g_unit;
static lv_draw_ortho_stats_t g_stats;
static lv_draw_ortho_copy_cb_t g_copy_cb;
static bool g_fast = true;

/* ------------------------------------------------------------------------ */
/* The matrix                                                               */
/* ------------------------------------------------------------------------ */

static bool near_int(float v, int32_t * out)
{
    float r = roundf(v);
    if(fabsf(v - r) > 1e-3f) return false;
    *out = (int32_t)r;
    return true;
}

static bool ortho_from_matrix(const lv_matrix_t * m, ortho_t * o)
{
    int32_t tx, ty;
    if(!near_int(m->m[0][0], &o->a) || !near_int(m->m[0][1], &o->b) ||
       !near_int(m->m[1][0], &o->c) || !near_int(m->m[1][1], &o->d) ||
       !near_int(m->m[0][2], &tx) || !near_int(m->m[1][2], &ty)) return false;

    /* No perspective. */
    if(fabsf(m->m[2][0]) > 1e-3f || fabsf(m->m[2][1]) > 1e-3f || fabsf(m->m[2][2] - 1.0f) > 1e-3f) return false;

    /* A signed permutation: exactly one +-1 per row and per column. */
    if(LV_ABS(o->a) + LV_ABS(o->b) != 1 || LV_ABS(o->c) + LV_ABS(o->d) != 1) return false;
    if(LV_ABS(o->a) + LV_ABS(o->c) != 1 || LV_ABS(o->b) + LV_ABS(o->d) != 1) return false;

    o->tx = tx + ((o->a + o->b) == 1 ? 0 : -1);
    o->ty = ty + ((o->c + o->d) == 1 ? 0 : -1);
    return true;
}

static inline bool ortho_is_identity(const ortho_t * o)
{
    return o->a == 1 && o->b == 0 && o->c == 0 && o->d == 1 && o->tx == 0 && o->ty == 0;
}

static inline void ortho_point(const ortho_t * o, int32_t x, int32_t y, int32_t * nx, int32_t * ny)
{
    *nx = o->a * x + o->b * y + o->tx;
    *ny = o->c * x + o->d * y + o->ty;
}

/* The inverse of a signed permutation is its transpose. */
static inline void ortho_unpoint(const ortho_t * o, int32_t nx, int32_t ny, int32_t * x, int32_t * y)
{
    int32_t u = nx - o->tx, v = ny - o->ty;
    *x = o->a * u + o->c * v;
    *y = o->b * u + o->d * v;
}

static void ortho_area(const ortho_t * o, const lv_area_t * in, lv_area_t * out)
{
    int32_t x1, y1, x2, y2;
    ortho_point(o, in->x1, in->y1, &x1, &y1);
    ortho_point(o, in->x2, in->y2, &x2, &y2);
    out->x1 = LV_MIN(x1, x2);
    out->x2 = LV_MAX(x1, x2);
    out->y1 = LV_MIN(y1, y2);
    out->y2 = LV_MAX(y1, y2);
}

static void ortho_unarea(const ortho_t * o, const lv_area_t * in, lv_area_t * out)
{
    int32_t x1, y1, x2, y2;
    ortho_unpoint(o, in->x1, in->y1, &x1, &y1);
    ortho_unpoint(o, in->x2, in->y2, &x2, &y2);
    out->x1 = LV_MIN(x1, x2);
    out->x2 = LV_MAX(x1, x2);
    out->y1 = LV_MIN(y1, y2);
    out->y2 = LV_MAX(y1, y2);
}

/* ------------------------------------------------------------------------ */
/* Moving pixels                                                            */
/* ------------------------------------------------------------------------ */

static void copy_straight(uint8_t * dest, int32_t dest_stride, const uint8_t * src, int32_t src_stride,
                          int32_t row_bytes, int32_t h)
{
    g_stats.bytes_straight += (uint64_t)row_bytes * (uint64_t)h;
    if(g_copy_cb && g_copy_cb(dest, dest_stride, src, src_stride, row_bytes, h)) return;
    for(int32_t r = 0; r < h; r++) {
        lv_memcpy(dest, src, row_bytes);
        dest += dest_stride;
        src += src_stride;
    }
}

/* Square blocks a transpose works in. A native row walks a column of the
 * upright side, touching one cache line per pixel; done a block at a time,
 * the BLOCK lines a block touches are still cached when the next native row
 * of the same block wants the pixel beside the one it just read. */
#ifndef LV_DRAW_ORTHO_BLOCK
#define LV_DRAW_ORTHO_BLOCK 32
#endif

/**
 * Move the pixels of a NATIVE rectangle between the native buffer and an
 * upright one, in either direction. `up` addresses upright pixel (x, y) as
 * up + (y - up_area->y1) * up_stride + (x - up_area->x1) * px.
 */
static void move_turned(const ortho_t * o, int32_t px,
                        uint8_t * nat, int32_t nat_stride, const lv_area_t * nat_buf_area,
                        uint8_t * up, int32_t up_stride, const lv_area_t * up_area,
                        const lv_area_t * nat_rect, bool to_native)
{
    /* One step right in native space is (a, b) in upright space. */
    const intptr_t up_step = (intptr_t)o->a * px + (intptr_t)o->b * up_stride;
    g_stats.bytes_turned += (uint64_t)lv_area_get_width(nat_rect) *
                            (uint64_t)lv_area_get_height(nat_rect) * (uint64_t)px;

    for(int32_t by = nat_rect->y1; by <= nat_rect->y2; by += LV_DRAW_ORTHO_BLOCK) {
        const int32_t ey = LV_MIN(by + LV_DRAW_ORTHO_BLOCK - 1, nat_rect->y2);
        for(int32_t bx = nat_rect->x1; bx <= nat_rect->x2; bx += LV_DRAW_ORTHO_BLOCK) {
            const int32_t w = LV_MIN(bx + LV_DRAW_ORTHO_BLOCK - 1, nat_rect->x2) - bx + 1;
            for(int32_t ny = by; ny <= ey; ny++) {
                int32_t x, y;
                ortho_unpoint(o, bx, ny, &x, &y);
                uint8_t * u = up + (intptr_t)(y - up_area->y1) * up_stride + (intptr_t)(x - up_area->x1) * px;
                uint8_t * n = nat + (intptr_t)(ny - nat_buf_area->y1) * nat_stride +
                              (intptr_t)(bx - nat_buf_area->x1) * px;
                if(px == 2) {
                    uint16_t * n16 = (uint16_t *)n;
                    if(to_native) for(int32_t i = 0; i < w; i++, u += up_step) n16[i] = *(const uint16_t *)u;
                    else for(int32_t i = 0; i < w; i++, u += up_step) *(uint16_t *)u = n16[i];
                }
                else if(px == 4) {
                    uint32_t * n32 = (uint32_t *)n;
                    if(to_native) for(int32_t i = 0; i < w; i++, u += up_step) n32[i] = *(const uint32_t *)u;
                    else for(int32_t i = 0; i < w; i++, u += up_step) *(uint32_t *)u = n32[i];
                }
                else {
                    for(int32_t i = 0; i < w; i++, u += up_step, n += px) {
                        if(to_native) lv_memcpy(n, u, px);
                        else lv_memcpy(u, n, px);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* The software renderer, run on a copy of the task                         */
/* ------------------------------------------------------------------------ */

static void sw_execute(lv_draw_task_t * t)
{
    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL:
            lv_draw_sw_fill(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_BORDER:
            lv_draw_sw_border(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_BOX_SHADOW:
            lv_draw_sw_box_shadow(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LETTER:
            lv_draw_sw_letter(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LABEL:
            lv_draw_sw_label(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_IMAGE:
            lv_draw_sw_image(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_ARC:
            lv_draw_sw_arc(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LINE:
            lv_draw_line_iterate(t, t->draw_dsc, lv_draw_sw_line);
            break;
        case LV_DRAW_TASK_TYPE_BLUR:
            lv_draw_sw_blur(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_TRIANGLE:
            lv_draw_sw_triangle(t, t->draw_dsc);
            break;
        case LV_DRAW_TASK_TYPE_LAYER:
            lv_draw_sw_layer(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE:
            lv_draw_sw_mask_rect(t, t->draw_dsc);
            break;
#if LV_USE_VECTOR_GRAPHIC && LV_USE_THORVG
        case LV_DRAW_TASK_TYPE_VECTOR:
            lv_draw_sw_vector(t, t->draw_dsc);
            break;
#endif
        default:
            break;
    }
}

/* A copy of `t` drawn with no matrix onto `layer`, clipped to `clip`. */
static void sw_execute_on(const lv_draw_task_t * t, lv_layer_t * layer, const lv_area_t * area,
                          const lv_area_t * clip)
{
    lv_draw_task_t copy = *t;
    copy.next = NULL;
    copy.target_layer = layer;
    copy.area = *area;
    copy.clip_area = *clip;
    lv_matrix_identity(&copy.matrix);
    sw_execute(&copy);
}

static void tile_layer(lv_layer_t * tl, lv_draw_buf_t * tb, const lv_area_t * area,
                       const lv_layer_t * target)
{
    lv_memzero(tl, sizeof(*tl));
    tl->draw_buf = tb;
    tl->buf_area = *area;
    tl->phy_clip_area = *area;
    tl->_clip_area = *area;
    tl->color_format = target->color_format;
    tl->opa = LV_OPA_COVER;
    tl->recolor = target->recolor;
    lv_matrix_identity(&tl->matrix);
}

/* ------------------------------------------------------------------------ */
/* The general path: draw upright into a tile, put it back turned           */
/* ------------------------------------------------------------------------ */

static void draw_one_tile(lv_draw_task_t * t, const ortho_t * o, const lv_area_t * tile,
                          uint8_t * mem, size_t mem_size)
{
    lv_layer_t * target = t->target_layer;
    lv_draw_buf_t * nb = target->draw_buf;
    const lv_color_format_t cf = nb->header.cf;
    const int32_t px = lv_color_format_get_size(cf);
    const int32_t w = lv_area_get_width(tile);
    const int32_t h = lv_area_get_height(tile);
    const uint32_t stride = lv_draw_buf_width_to_stride(w, cf);

    lv_draw_buf_t tb;
    if(lv_draw_buf_init(&tb, w, h, cf, stride, mem, mem_size) != LV_RESULT_OK) {
        LV_LOG_WARN("ortho: tile %dx%d does not fit", (int)w, (int)h);
        return;
    }

    lv_area_t nat;
    ortho_area(o, tile, &nat);

    /* The tile starts as what is already under it, so blending is right. */
    move_turned(o, px, nb->data, nb->header.stride, &target->buf_area,
                tb.data, stride, tile, &nat, false);

    lv_layer_t tl;
    tile_layer(&tl, &tb, tile, target);
    lv_area_t clip;
    if(lv_area_intersect(&clip, &t->clip_area, tile)) sw_execute_on(t, &tl, &t->area, &clip);

    move_turned(o, px, nb->data, nb->header.stride, &target->buf_area,
                tb.data, stride, tile, &nat, true);
    g_stats.tiles++;
}

static void draw_general(ortho_unit_t * u, lv_draw_task_t * t, const ortho_t * o, const lv_area_t * todo)
{
    lv_draw_buf_t * nb = t->target_layer->draw_buf;
    const lv_color_format_t cf = nb->header.cf;
    const int32_t w = lv_area_get_width(todo);
    const int32_t h = lv_area_get_height(todo);
    g_stats.tasks_general++;

    /* A blur reads its neighbours, so it is never cut into tiles. */
    if(t->type == LV_DRAW_TASK_TYPE_BLUR) {
        const uint32_t stride = lv_draw_buf_width_to_stride(w, cf);
        size_t size = (size_t)stride * h + LV_DRAW_BUF_ALIGN;
        void * raw = lv_malloc(size);
        if(raw == NULL) {
            LV_LOG_WARN("ortho: no memory for a %dx%d blur", (int)w, (int)h);
            return;
        }
        draw_one_tile(t, o, todo, lv_draw_buf_align(raw, cf), size - LV_DRAW_BUF_ALIGN);
        lv_free(raw);
        return;
    }

    /* As wide as the area allows, then as many rows as fit. */
    int32_t tw = w;
    while(tw > 1 && lv_draw_buf_width_to_stride(tw, cf) > LV_DRAW_ORTHO_TILE_BYTES) tw = (tw + 1) / 2;
    const uint32_t tstride = lv_draw_buf_width_to_stride(tw, cf);
    int32_t th = (int32_t)(LV_DRAW_ORTHO_TILE_BYTES / tstride);
    if(th < 1) th = 1;

    for(int32_t y = todo->y1; y <= todo->y2; y += th) {
        for(int32_t x = todo->x1; x <= todo->x2; x += tw) {
            lv_area_t tile = {x, y, LV_MIN(x + tw - 1, todo->x2), LV_MIN(y + th - 1, todo->y2)};
            draw_one_tile(t, o, &tile, u->tile, LV_DRAW_ORTHO_TILE_BYTES);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Fast paths                                                               */
/* ------------------------------------------------------------------------ */

/* A solid fill turned is the fill of the turned rectangle: a corner radius is
 * the same on every side, so nothing about it changes. */
static bool try_fill(lv_draw_task_t * t, const ortho_t * o, const lv_area_t * todo)
{
    if(t->type != LV_DRAW_TASK_TYPE_FILL) return false;
    lv_draw_fill_dsc_t * dsc = t->draw_dsc;
    if(dsc->grad.dir != LV_GRAD_DIR_NONE) return false;

    lv_area_t area, clip;
    ortho_area(o, &t->area, &area);
    ortho_area(o, todo, &clip);
    sw_execute_on(t, t->target_layer, &area, &clip);
    g_stats.fills_direct++;
    return true;
}

static bool plain_image(const lv_draw_task_t * t, const lv_image_dsc_t ** src_out)
{
    if(t->type != LV_DRAW_TASK_TYPE_IMAGE) return false;
    const lv_draw_image_dsc_t * d = t->draw_dsc;
    if(lv_image_src_get_type(d->src) != LV_IMAGE_SRC_VARIABLE) return false;
    if(d->rotation != 0 || d->scale_x != LV_SCALE_NONE || d->scale_y != LV_SCALE_NONE) return false;
    if(d->skew_x != 0 || d->skew_y != 0 || d->tile || d->clip_radius != 0) return false;
    if(d->recolor_opa > LV_OPA_MIN || d->opa < LV_OPA_MAX || t->opa < LV_OPA_MAX) return false;
    if(d->colorkey || d->bitmap_mask_src || d->blend_mode != LV_BLEND_MODE_NORMAL) return false;

    const lv_image_dsc_t * img = d->src;
    const lv_color_format_t layer_cf = t->target_layer->draw_buf->header.cf;
    if(img->header.cf != layer_cf) return false;
    if(layer_cf != LV_COLOR_FORMAT_RGB565 && layer_cf != LV_COLOR_FORMAT_RGB888 &&
       layer_cf != LV_COLOR_FORMAT_XRGB8888) return false;
    if(lv_area_get_width(&t->area) != (int32_t)img->header.w ||
       lv_area_get_height(&t->area) != (int32_t)img->header.h) return false;
    if(img->data == NULL) return false;
    *src_out = img;
    return true;
}

static bool try_image(lv_draw_task_t * t, const ortho_t * o, const lv_area_t * todo)
{
    const lv_image_dsc_t * img;
    if(!plain_image(t, &img)) return false;

    lv_layer_t * target = t->target_layer;
    lv_draw_buf_t * nb = target->draw_buf;
    const int32_t px = lv_color_format_get_size(nb->header.cf);
    lv_area_t nat;
    ortho_area(o, todo, &nat);

    if(img->header.flags & LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE) {
        /* Already in the panel's orientation: its rows ARE native rows. */
        lv_area_t whole;
        ortho_area(o, &t->area, &whole);
        const int32_t sstride = img->header.stride ? (int32_t)img->header.stride
                                : lv_area_get_width(&whole) * px;
        const uint8_t * s = img->data + (intptr_t)(nat.y1 - whole.y1) * sstride +
                            (intptr_t)(nat.x1 - whole.x1) * px;
        uint8_t * dst = nb->data + (intptr_t)(nat.y1 - target->buf_area.y1) * nb->header.stride +
                        (intptr_t)(nat.x1 - target->buf_area.x1) * px;
        copy_straight(dst, nb->header.stride, s, sstride, lv_area_get_width(&nat) * px,
                      lv_area_get_height(&nat));
        g_stats.images_native++;
        return true;
    }

    const int32_t sstride = img->header.stride ? (int32_t)img->header.stride
                            : (int32_t)img->header.w * px;
    move_turned(o, px, nb->data, nb->header.stride, &target->buf_area,
                (uint8_t *)img->data, sstride, &t->area, &nat, true);
    g_stats.images_turned++;
    return true;
}

/* ------------------------------------------------------------------------ */
/* The draw unit                                                            */
/* ------------------------------------------------------------------------ */

/* A label's area is the size of its text, and a glyph may still reach past
 * it -- a bearing, an accent, a letter's anti-aliased edge. The software
 * renderer is held back only by the clip area, and drawing tiles over the
 * whole clip area would turn every pixel of a screen for one word. A glyph
 * cannot reach further than a line height from the text it belongs to, so
 * that is the margin. */
static void widen_for_glyphs(const lv_draw_task_t * t, lv_area_t * area)
{
    const lv_font_t * font = NULL;
    if(t->type == LV_DRAW_TASK_TYPE_LABEL) font = ((const lv_draw_label_dsc_t *)t->draw_dsc)->font;
    else if(t->type == LV_DRAW_TASK_TYPE_LETTER) font = ((const lv_draw_letter_dsc_t *)t->draw_dsc)->font;
    else return;
    int32_t m = font ? lv_font_get_line_height(font) : 32;
    lv_area_increase(area, m, m);
}

static void execute(ortho_unit_t * u, lv_draw_task_t * t)
{
    ortho_t o;
    if(!ortho_from_matrix(&t->matrix, &o)) {
        g_stats.tasks_refused++;
        return;
    }

    /* Only what is both drawn and allowed, and inside the buffer. */
    lv_area_t todo, buf_up, real = t->_real_area;
    widen_for_glyphs(t, &real);
    if(!lv_area_intersect(&todo, &real, &t->clip_area)) return;
    ortho_unarea(&o, &t->target_layer->buf_area, &buf_up);
    if(!lv_area_intersect(&todo, &todo, &buf_up)) return;

    if(g_fast) {
        if(try_fill(t, &o, &todo)) return;
        if(try_image(t, &o, &todo)) return;
    }
    draw_general(u, t, &o, &todo);
}

static int32_t evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    LV_UNUSED(draw_unit);
    ortho_t o;
    if(!ortho_from_matrix(&task->matrix, &o)) return 0;
    /* An identity matrix is the software renderer's own case. */
    if(ortho_is_identity(&o)) return 0;
    switch(task->type) {
        case LV_DRAW_TASK_TYPE_FILL:
        case LV_DRAW_TASK_TYPE_BORDER:
        case LV_DRAW_TASK_TYPE_BOX_SHADOW:
        case LV_DRAW_TASK_TYPE_LETTER:
        case LV_DRAW_TASK_TYPE_LABEL:
        case LV_DRAW_TASK_TYPE_IMAGE:
        case LV_DRAW_TASK_TYPE_ARC:
        case LV_DRAW_TASK_TYPE_LINE:
        case LV_DRAW_TASK_TYPE_BLUR:
        case LV_DRAW_TASK_TYPE_TRIANGLE:
        case LV_DRAW_TASK_TYPE_LAYER:
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE:
#if LV_USE_VECTOR_GRAPHIC && LV_USE_THORVG
        case LV_DRAW_TASK_TYPE_VECTOR:
#endif
            break;
        default:
            return 0;
    }
    /* Nothing else can draw a turned task, so take it whatever was offered. */
    task->preference_score = 0;
    task->preferred_draw_unit_id = DRAW_UNIT_ID_ORTHO;
    return 1;
}

static int32_t dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    ortho_unit_t * u = (ortho_unit_t *)draw_unit;
    if(u->task_act) return 0;

    lv_draw_task_t * t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_ORTHO);
    if(t == NULL) return LV_DRAW_UNIT_IDLE;
    if(lv_draw_layer_alloc_buf(layer) == NULL) return LV_DRAW_UNIT_IDLE;

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    t->draw_unit = draw_unit;
    u->task_act = t;
    execute(u, t);
    t->state = LV_DRAW_TASK_STATE_FINISHED;
    u->task_act = NULL;

    lv_draw_dispatch_request();
    return 1;
}

static int32_t delete_cb(lv_draw_unit_t * draw_unit)
{
    ortho_unit_t * u = (ortho_unit_t *)draw_unit;
    lv_free(u->tile_alloc);
    u->tile_alloc = NULL;
    u->tile = NULL;
    g_unit = NULL;
    return 0;
}

void lv_draw_ortho_init(void)
{
    if(g_unit) return;
    ortho_unit_t * u = lv_draw_create_unit(sizeof(ortho_unit_t));
    u->base.name = "ORTHO";
    u->base.evaluate_cb = evaluate;
    u->base.dispatch_cb = dispatch;
    u->base.delete_cb = delete_cb;
    u->tile_alloc = lv_malloc(LV_DRAW_ORTHO_TILE_BYTES + LV_DRAW_BUF_ALIGN);
    LV_ASSERT_MALLOC(u->tile_alloc);
    u->tile = lv_draw_buf_align(u->tile_alloc, LV_COLOR_FORMAT_RGB565);
    g_unit = u;
}

void lv_draw_ortho_set_copy_cb(lv_draw_ortho_copy_cb_t cb)
{
    g_copy_cb = cb;
}

void lv_draw_ortho_set_fast_paths(bool enable)
{
    g_fast = enable;
}

const lv_draw_ortho_stats_t * lv_draw_ortho_get_stats(void)
{
    return &g_stats;
}

void lv_draw_ortho_reset_stats(void)
{
    lv_memzero(&g_stats, sizeof(g_stats));
}

#endif /* LV_USE_DRAW_ORTHO */
