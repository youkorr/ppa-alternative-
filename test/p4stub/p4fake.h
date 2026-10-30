/* What the fake PPA and cache saw last, for the tests. */
#pragma once
#include "driver/ppa.h"
#include "esp_cache.h"
#include <stddef.h>
typedef struct {
    int srm_count, msync_count, msync_flags;
    uintptr_t msync_addr;
    size_t msync_size;
    ppa_srm_oper_config_t last;
} p4fake_t;
extern p4fake_t g_p4;
