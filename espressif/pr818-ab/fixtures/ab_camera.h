/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 *
 * The board's own camera, as a source of pictures for the A/B benchmark:
 * Espressif's esp_video (V4L2 over MIPI-CSI, the ISP turning RAW into
 * RGB565), identical in both variants.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint16_t width, height;     /* the picture the ISP hands over */
    uint32_t stride;            /* bytes per line */
    char sensor[32];            /* what the driver calls the camera */
} ab_camera_info_t;

typedef struct {
    uint32_t received;          /* frames the camera delivered */
    uint32_t shown;             /* of those, handed to the display */
    uint32_t skipped;           /* delivered but replaced by a newer one before shown */
    uint32_t errors;            /* frames flagged as errors by the driver */
    uint64_t wait_us;           /* time spent waiting for a frame */
} ab_camera_counts_t;

/* Once. Fails, with a log line saying where, if the camera does not answer. */
esp_err_t ab_camera_start(ab_camera_info_t *info);

/* The newest frame, waiting for one if none is newer than the last returned.
 * The last two frames returned stay the caller's: a frame is given back to
 * the driver only once two newer ones have been taken. NULL on failure. */
const uint8_t *ab_camera_take(void);

void ab_camera_reset_counts(void);
ab_camera_counts_t ab_camera_counts(void);
