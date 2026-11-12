/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * V6 hardware exercise: golden software reference beside live LVGL objects.
 * The visual comparison is intentionally not reported as an automatic PASS.
 */
#include "src/misc/cache/instance/lv_image_cache.h"
#include "v6_reference_config.h"
#include <assert.h>
#include <stdio.h>

extern const uint8_t v6_reference_start[] asm("_binary_v6_reference_rgb565_start");
extern const uint8_t v6_reference_end[] asm("_binary_v6_reference_rgb565_end");

#define V6_TILE_W 240
#define V6_TILE_H 200
#define V6_CASES 16
#define V6_CYCLES 2
#define V6_CASE_MS 1000

static const char *const v6_names[V6_CASES] = {
    "Control", "Image rotation 30", "Image rotation 90", "Image scale",
    "Image rotation + scale", "Object rotation 30", "Object rotation 90", "Object scale",
    "Object rotation + scale", "Parent rotation 30", "Parent scale", "Nested rotations",
    "Nested scales", "Layered object rotation", "Layered parent + child", "Clipped rotation"
};

static lv_image_dsc_t s_v6_photo;
static uint8_t *s_v6_photo_pixels;
static lv_image_dsc_t s_v6_reference;

static lv_obj_t *v6_box(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

static void v6_transform(lv_obj_t *object, int angle, int scale_x, int scale_y)
{
    lv_obj_set_style_transform_pivot_x(object, 40, 0);
    lv_obj_set_style_transform_pivot_y(object, 25, 0);
    lv_obj_set_style_transform_rotation(object, angle, 0);
    lv_obj_set_style_transform_scale_x(object, scale_x, 0);
    lv_obj_set_style_transform_scale_y(object, scale_y, 0);
}

static void v6_make_photo(void)
{
    const int width = 65, height = 43;
    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    s_v6_photo_pixels = calloc(height, stride);
    assert(s_v6_photo_pixels);
    for(int y = 0; y < height; y++) for(int x = 0; x < width; x++) {
        const unsigned r = (x * 5 + y * 3) & 255;
        const unsigned g = (y * 7 + 17) & 255;
        const unsigned b = (x * 3 + 90) & 255;
        const uint16_t pixel = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        memcpy(s_v6_photo_pixels + y * stride + x * 2, &pixel, 2);
    }
    s_v6_photo = (lv_image_dsc_t) {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = width, .h = height, .stride = stride},
        .data_size = stride * height, .data = s_v6_photo_pixels
    };
}

/* Same geometry as the independent host oracle, translated by (-40,-160)
 * into a 240x200 tile. All non-clipped test shapes fit fully inside it. */
static void v6_live_scene(lv_obj_t *tile, unsigned test)
{
    if(test >= 1 && test <= 4) {
        lv_obj_t *image = lv_image_create(tile);
        lv_image_set_src(image, &s_v6_photo);
        lv_obj_set_pos(image, 88, 59);
        lv_image_set_pivot(image, 21, 13);
        if(test == 1 || test == 4) lv_image_set_rotation(image, 300);
        if(test == 2) lv_image_set_rotation(image, 900);
        if(test == 3 || test == 4) {
            lv_image_set_scale_x(image, 384);
            lv_image_set_scale_y(image, 192);
        }
        return;
    }
    lv_obj_t *parent = tile;
    int x = 60, y = 40;
    if(test == 15) {
        parent = v6_box(tile, x, y, 90, 65, 0x386078);
        x = 36; y = 20;
    }
    lv_obj_t *object = v6_box(parent, x, y, 120, 80, 0xd52e45);
    v6_box(object, 7, 9, 22, 13, 0x30e09a);
    v6_box(object, 73, 42, 35, 28, 0x3a88ff);
    lv_obj_t *label = lv_label_create(object);
    lv_label_set_text(label, "Ab7");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(label, 35, 8);
    switch(test) {
    case 5: case 9: case 13: case 14: case 15: case 11:
        v6_transform(object, 300, 256, 256); break;
    case 6: v6_transform(object, 900, 256, 256); break;
    case 7: case 10: v6_transform(object, 0, 384, 192); break;
    case 8: v6_transform(object, 300, 384, 192); break;
    case 12: v6_transform(object, 0, 384, 256); break;
    default: break;
    }
    if((test >= 9 && test <= 12) || test == 14) {
        lv_obj_t *child = v6_box(object, 28, 27, 58, 35, 0xf4d85a);
        v6_box(child, 3, 3, 14, 9, 0x9338ca);
        if(test == 11 || test == 14) v6_transform(child, 900, 320, 192);
        if(test == 12) v6_transform(child, 0, 192, 320);
    }
    if(test == 13 || test == 14) lv_obj_set_style_opa_layered(object, 160, 0);
}

static lv_obj_t *v6_text(lv_obj_t *parent, const char *text, int x, int y, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void v6_show_case(unsigned test, unsigned cycle, unsigned rotation)
{
    lv_obj_t *screen = lv_display_get_screen_active(s_disp);
    lv_obj_clean(screen);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x07111f), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    const int width = lv_display_get_horizontal_resolution(s_disp);
    const int height = lv_display_get_vertical_resolution(s_disp);
    char title[100];
    snprintf(title, sizeof(title), "V6 Waveshare - screen %u - cycle %u/%u", rotation, cycle, V6_CYCLES);
    v6_text(screen, title, 18, 14, &lv_font_montserrat_20);
    snprintf(title, sizeof(title), "%02u/%u  %s", test + 1, V6_CASES, v6_names[test]);
    v6_text(screen, title, 18, 46, &lv_font_montserrat_20);
    const int left = (width - 2 * V6_TILE_W - 24) / 2;
    const int right = left + V6_TILE_W + 24;
    const int top = (height - V6_TILE_H) / 2;
    v6_text(screen, "SOFTWARE REFERENCE", left, top - 30, &lv_font_montserrat_14);
    v6_text(screen, "LIVE LVGL", right, top - 30, &lv_font_montserrat_14);
    s_v6_reference = (lv_image_dsc_t) {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = V6_TILE_W, .h = V6_TILE_H, .stride = V6_TILE_W * 2},
        .data_size = V6_TILE_W * V6_TILE_H * 2,
        .data = v6_reference_start + test * V6_TILE_W * V6_TILE_H * 2
    };
    /* The descriptor address is reused while the selected asset changes. */
    lv_image_cache_drop(&s_v6_reference);
    lv_obj_t *reference = lv_image_create(screen);
    lv_image_set_src(reference, &s_v6_reference);
    lv_obj_set_pos(reference, left, top);
    lv_obj_t *tile = v6_box(screen, right, top, V6_TILE_W, V6_TILE_H, 0x14243a);
    v6_live_scene(tile, test);
    v6_text(screen, "Compare both images. Save every PR818_V6 log line.",
            18, height - 44, &lv_font_montserrat_14);
    lv_obj_invalidate(screen);
    lv_refr_now(s_disp);
}

void app_main(void)
{
    const esp_lv_adapter_rotation_t rotations[] = {
        ESP_LV_ADAPTER_ROTATE_180, ESP_LV_ADAPTER_ROTATE_90,
        ESP_LV_ADAPTER_ROTATE_270, ESP_LV_ADAPTER_ROTATE_0
    };
    assert(v6_reference_end - v6_reference_start == V6_CASES * V6_TILE_W * V6_TILE_H * 2);
    ESP_LOGI(TAG, "PR818_V6_READY board=waveshare-7b cycles=%u cases=%u delay_ms=%u visual_reference_lvgl=%s",
             V6_CYCLES, V6_CASES, V6_CASE_MS, V6_REFERENCE_LVGL);
    vTaskDelay(pdMS_TO_TICKS(3000));
    unsigned completed = 0;
    bool heap_ok = true;
    for(unsigned cycle = 1; cycle <= V6_CYCLES; cycle++) {
        for(unsigned r = 0; r < sizeof(rotations) / sizeof(rotations[0]); r++) {
            s_v6_rotation = rotations[r];
            test_init_display();
            ESP_ERROR_CHECK(esp_lv_adapter_lock(1000));
            lv_display_set_default(s_disp);
            lv_display_set_software_object_transforms(s_disp, true);
            assert(lv_display_get_software_object_transforms(s_disp));
            v6_make_photo();
            esp_lv_adapter_unlock();
            for(unsigned scene = 0; scene < V6_CASES; scene++) {
                const int64_t begin = esp_timer_get_time();
                ESP_ERROR_CHECK(esp_lv_adapter_lock(1000));
                v6_show_case(scene, cycle, (unsigned)rotations[r]);
                esp_lv_adapter_unlock();
                const uint32_t render_us = (uint32_t)(esp_timer_get_time() - begin);
                vTaskDelay(pdMS_TO_TICKS(V6_CASE_MS));
                const bool intact = heap_caps_check_integrity_all(true);
                heap_ok &= intact;
                completed++;
                ESP_LOGI(TAG, "PR818_V6_SCENE cycle=%u rotation=%u case=%u name=\"%s\" render_us=%u free=%u internal=%u largest=%u heap_ok=%u",
                         cycle, (unsigned)rotations[r], scene + 1, v6_names[scene], render_us,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)intact);
                assert(intact);
            }
            ESP_ERROR_CHECK(esp_lv_adapter_lock(1000));
            lv_obj_clean(lv_display_get_screen_active(s_disp));
            lv_image_cache_drop(&s_v6_photo);
            lv_image_cache_drop(&s_v6_reference);
            free(s_v6_photo_pixels);
            s_v6_photo_pixels = NULL;
            esp_lv_adapter_unlock();
            test_deinit_display();
            vTaskDelay(pdMS_TO_TICKS(200));
            ESP_LOGI(TAG, "PR818_V6_ROTATION_DONE cycle=%u rotation=%u free=%u internal=%u tasks=%u heap_ok=%u",
                     cycle, (unsigned)rotations[r], (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)uxTaskGetNumberOfTasks(), (unsigned)heap_caps_check_integrity_all(true));
        }
    }
    /* Leave the difficult nested case visible instead of finishing on a black panel. */
    s_v6_rotation = ESP_LV_ADAPTER_ROTATE_180;
    test_init_display();
    ESP_ERROR_CHECK(esp_lv_adapter_lock(1000));
    lv_display_set_default(s_disp);
    lv_display_set_software_object_transforms(s_disp, true);
    v6_make_photo();
    v6_show_case(11, V6_CYCLES, 180);
    esp_lv_adapter_unlock();
    ESP_LOGI(TAG, "PR818_V6_DONE completed=%u expected=%u heap_ok=%u pixel_equality=VISUAL_CHECK_REQUIRED",
             completed, V6_CYCLES * 4 * V6_CASES, (unsigned)heap_ok);
    while(true) vTaskDelay(pdMS_TO_TICKS(1000));
}
