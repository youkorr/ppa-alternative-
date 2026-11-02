/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */
/* ESP-IDF's esp_memory_utils.h, the one call lv_draw_ortho makes: whether a
 * picture lies in the flash. The simulated P4 has none; p4bench says which
 * pictures may be kept with lv_draw_ortho_set_image_constant_cb instead. */
#pragma once
#include <stdbool.h>
static inline bool esp_ptr_in_drom(const void * p)
{
    (void)p;
    return false;
}
