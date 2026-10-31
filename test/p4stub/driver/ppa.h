/* Stand-in for ESP-IDF v5.5.5 components/esp_driver_ppa/include/driver/ppa.h
 * and hal/ppa_types.h: the types and calls this project uses, field for
 * field in the same order. The enumerators' VALUES are not the hardware's:
 * only their names are used. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct ppa_client_t *ppa_client_handle_t;
typedef enum { PPA_OPERATION_SRM, PPA_OPERATION_BLEND, PPA_OPERATION_FILL } ppa_operation_t;
typedef enum { PPA_DATA_BURST_LENGTH_8 = 8, PPA_DATA_BURST_LENGTH_128 = 128 } ppa_data_burst_length_t;
typedef enum {
    PPA_SRM_ROTATION_ANGLE_0,        /*!< Picture does no rotation */
    PPA_SRM_ROTATION_ANGLE_90,       /*!< Picture rotates 90 degrees CCW */
    PPA_SRM_ROTATION_ANGLE_180,      /*!< Picture rotates 180 degrees CCW */
    PPA_SRM_ROTATION_ANGLE_270,      /*!< Picture rotates 270 degrees CCW */
} ppa_srm_rotation_angle_t;
typedef enum { PPA_SRM_COLOR_MODE_ARGB8888, PPA_SRM_COLOR_MODE_RGB888, PPA_SRM_COLOR_MODE_RGB565 } ppa_srm_color_mode_t;
typedef enum { PPA_BLEND_COLOR_MODE_ARGB8888 } ppa_blend_color_mode_t;
typedef enum { PPA_FILL_COLOR_MODE_ARGB8888 } ppa_fill_color_mode_t;
typedef enum { PPA_COLOR_RANGE_LIMIT, PPA_COLOR_RANGE_FULL } ppa_color_range_t;
typedef enum { PPA_COLOR_CONV_STD_RGB_YUV_BT601 } ppa_color_conv_std_rgb_yuv_t;
typedef enum { PPA_ALPHA_NO_CHANGE } ppa_alpha_update_mode_t;

typedef struct {
    ppa_operation_t oper_type;
    uint32_t max_pending_trans_num;
    ppa_data_burst_length_t data_burst_length;
} ppa_client_config_t;

esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *ret_client);
esp_err_t ppa_unregister_client(ppa_client_handle_t ppa_client);

typedef struct {
    const void *buffer;
    uint32_t pic_w;
    uint32_t pic_h;
    uint32_t block_w;
    uint32_t block_h;
    uint32_t block_offset_x;
    uint32_t block_offset_y;
    union {
        ppa_srm_color_mode_t srm_cm;
        ppa_blend_color_mode_t blend_cm;
        ppa_fill_color_mode_t fill_cm;
        uint32_t cm;
    };
    ppa_color_range_t yuv_range;
    ppa_color_conv_std_rgb_yuv_t yuv_std;
} ppa_in_pic_blk_config_t;

typedef struct {
    void *buffer;
    uint32_t buffer_size;
    uint32_t pic_w;
    uint32_t pic_h;
    uint32_t block_offset_x;
    uint32_t block_offset_y;
    union {
        ppa_srm_color_mode_t srm_cm;
        ppa_blend_color_mode_t blend_cm;
        ppa_fill_color_mode_t fill_cm;
        uint32_t cm;
    };
    ppa_color_range_t yuv_range;
    ppa_color_conv_std_rgb_yuv_t yuv_std;
} ppa_out_pic_blk_config_t;

typedef enum { PPA_TRANS_MODE_BLOCKING, PPA_TRANS_MODE_NON_BLOCKING } ppa_trans_mode_t;

typedef struct {
    ppa_in_pic_blk_config_t in;
    ppa_out_pic_blk_config_t out;
    ppa_srm_rotation_angle_t rotation_angle;
    float scale_x;
    float scale_y;
    bool mirror_x;
    bool mirror_y;
    bool rgb_swap;
    bool byte_swap;
    ppa_alpha_update_mode_t alpha_update_mode;
    union {
        uint32_t alpha_fix_val;
        float alpha_scale_ratio;
    };
    ppa_trans_mode_t mode;
    void *user_data;
} ppa_srm_oper_config_t;

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client, const ppa_srm_oper_config_t *config);

typedef struct {
} ppa_event_data_t;
typedef bool (*ppa_event_callback_t)(ppa_client_handle_t ppa_client, ppa_event_data_t *event_data, void *user_data);
typedef struct {
    ppa_event_callback_t on_trans_done;
} ppa_event_callbacks_t;
esp_err_t ppa_client_register_event_callbacks(ppa_client_handle_t ppa_client, const ppa_event_callbacks_t *cbs);
