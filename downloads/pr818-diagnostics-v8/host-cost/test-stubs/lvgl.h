#pragma once
#include <stdint.h>
typedef struct {int32_t x1,y1,x2,y2;} lv_area_t;
typedef struct {struct {uint32_t w,h,cf;} header;} lv_draw_buf_t;
static inline int32_t lv_area_get_width(const lv_area_t *a){return a->x2-a->x1+1;}
static inline int32_t lv_area_get_height(const lv_area_t *a){return a->y2-a->y1+1;}
static inline unsigned lv_color_format_get_bpp(unsigned cf){return cf;}
