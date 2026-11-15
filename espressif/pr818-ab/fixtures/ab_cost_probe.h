/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Test-only inclusive wall-time counters; never sum nested category times.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AB_COST_STAGE_SETUP, AB_COST_STAGE_LOCK, AB_COST_STAGE_UPDATE,
    AB_COST_STAGE_REFRESH, AB_COST_STAGE_UNLOCK, AB_COST_STAGE_YIELD,
    AB_COST_STAGE_DRAIN, AB_COST_STAGE_BACKGROUND, AB_COST_STAGE_COUNT
} ab_cost_stage_t;

typedef enum {
    AB_COST_COPY, AB_COST_CACHE, AB_COST_SRM, AB_COST_FILL,
    AB_COST_BLEND, AB_COST_NATIVE_WAIT, AB_COST_KIND_COUNT
} ab_cost_kind_t;

typedef struct {
    uint32_t calls;
    uint32_t errors;       /* Error-return API / native wait failures; copy is void. */
    uint32_t pending;      /* Native waits whose ticket was pending at entry. */
    uint32_t flags_or;     /* OR of cache msync flags; zero for other kinds. */
    uint64_t elapsed_us;   /* Inclusive wall time; can contain other categories. */
    uint64_t bytes;        /* Copy: requested logical payload ONCE. Cache: requested span. */
    uint64_t pixels;       /* Copy/PPA requested source/fill pixels; not bus traffic. */
    uint64_t c2m_bytes;    /* Cache requests carrying DIR_C2M, incl both-direction flags. */
    uint64_t m2c_bytes;    /* Cache requests carrying DIR_M2C. */
} ab_cost_cell_t;

typedef struct {
    ab_cost_cell_t cell[AB_COST_STAGE_COUNT][AB_COST_KIND_COUNT];
} ab_cost_stats_t;

/* Reset while inactive. Snapshot is atomic relative to all CPU counter updates. */
void ab_cost_probe_reset(void);
void ab_cost_probe_set_stage(ab_cost_stage_t stage);
void ab_cost_probe_set_background(bool background);
void ab_cost_probe_set_active(bool active);
ab_cost_stats_t ab_cost_probe_snapshot(void);
const char *ab_cost_probe_stage_name(ab_cost_stage_t stage);
const char *ab_cost_probe_kind_name(ab_cost_kind_t kind);

/* Weak-hook targets inserted only into the benchmark's private native backend.
 * The token fixes entry stage/epoch, not whichever stage happens to run at exit. */
uint32_t ab_cost_native_wait_begin(void);
void ab_cost_native_wait_end(uint32_t token, uint32_t elapsed_us,
                             bool pending, bool success);
