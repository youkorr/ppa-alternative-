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
    /* Transactions run, and the pixels they wrote: the simulated P4 times
     * the PPA from these (test/p4sim). Not cleared with the rest above. */
} p4fake_t;
extern unsigned long long g_p4_pixels;
extern int g_p4_runs;
/* A non-blocking transaction runs only when time is let pass
 * (esp_rom_delay_us) or a blocking one follows it, so a caller that does not
 * wait sees the frame buffer as it was. */
void p4fake_run_all(void);
/* Transactions queued and not yet run. */
int p4fake_pending(void);
/* Count the transactions reading `buf` from now on: a camera frame, say,
 * among the tiles lv_draw_ortho hands the PPA at the same time. */
void p4fake_watch(const void * buf);
int p4fake_watched(void);
extern p4fake_t g_p4;
