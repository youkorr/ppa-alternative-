#pragma once
#include <stdint.h>
#include "esp_cache.h"
typedef void *ppa_client_handle_t;
typedef struct {uint32_t block_w,block_h;} mock_pic_t;
typedef struct {mock_pic_t in;} ppa_srm_oper_config_t;
typedef struct {uint32_t fill_block_w,fill_block_h;} ppa_fill_oper_config_t;
typedef struct {mock_pic_t in_bg;} ppa_blend_oper_config_t;
