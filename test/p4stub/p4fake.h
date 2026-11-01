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
/* Transactions the P4 would never finish: DIG-734 geometries under the
 * ESP-IDF v5.5.6 driver (what ESPHome builds with), and any made to stall. */
extern int g_p4_dig734;
/* Fills the PPA did. */
extern int g_p4_fills;
/* Of g_p4_pixels, those a fill wrote (a fill writes; a turn reads and writes). */
extern unsigned long long g_p4_fill_pixels;
void p4fake_stall_next(void);
/* Drop everything queued, stuck or not, as a reset would. */
void p4fake_reset(void);
extern p4fake_t g_p4;
/* The timeline, under P4SIM only: 'Q' a transaction queued (its pixels, and
 * whether it is a fill), 'R' the CPU needing transaction `id` finished. */
typedef struct {
    char type;
    int id, fill;
    unsigned px;
} p4fake_event_t;
#define P4FAKE_EVENTS 8192
extern p4fake_event_t g_p4_ev[P4FAKE_EVENTS];
extern int g_p4_nev;
