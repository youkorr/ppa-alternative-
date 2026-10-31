/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/*
 * A QEMU plugin that counts what an ESP32-P4 would pay for a program, rather
 * than what the machine running QEMU pays.
 *
 * Every instruction is counted, per phase. Every load and store goes through
 * a model of the P4's data caches, with the figures ESP-IDF itself sets up
 * (v5.5, Kconfig.cache, soc_caps.h, rom/cache.h):
 *
 *   L1   64-byte lines, in front of everything: SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
 *        says internal RAM goes through it too. 64 KiB, 4 ways (the ROM's
 *        cache config says "cache ways, always 4").
 *   L2   in front of the PSRAM only, 128 KiB with 64-byte lines (the defaults,
 *        CACHE_L2_CACHE_128KB / CACHE_L2_CACHE_LINE_64B), 8 ways.
 *
 * Both write back and allocate on a write. A line the L2 has to fetch from the
 * PSRAM, or write back to it, is one burst on the PSRAM's bus: those are the
 * figures that matter, because the display's own scan-out and the PPA share
 * that bus.
 *
 * A phase is chosen by the guest with `addi x0, x0, N` (N from 1 to 2047), an
 * instruction that does nothing on any RISC-V and that no compiler emits.
 * Addresses from psram= up to psram+size are the PSRAM; everything else is
 * internal RAM.
 *
 * The PPA is not the CPU: the fake PPA (test/p4stub/p4fake.c, built with
 * P4SIM) brackets its copy loop with markers 2047 and 2046, and what runs
 * between them is counted in phase 2045 and kept out of the caches -- a DMA
 * engine reads and writes memory without them.
 *
 * Built against QEMU 8.2's qemu-plugin.h (plugin API version 1).
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qemu-plugin.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define LINE 64
#define PHASES 2048

typedef struct {
    uint64_t insns, loads, stores;
    uint64_t l1_miss_internal;  /* line filled from internal RAM */
    uint64_t l1_miss_psram;     /* line asked of the L2 */
    uint64_t l2_hit;
    uint64_t psram_fill;        /* line read from the PSRAM */
    uint64_t psram_writeback;   /* line written to the PSRAM */
    uint64_t internal_writeback;
} counts_t;

static counts_t g_counts[PHASES];
static int g_phase;
static uint64_t g_psram_lo = 0x90000000u, g_psram_hi = 0xA0000000u;

typedef struct {
    int sets, ways;
    uint32_t * tag;    /* line address, or ~0 */
    uint8_t * dirty;
    uint32_t * age;
    uint32_t clock;
} cache_t;

static cache_t g_l1, g_l2;

static void cache_init(cache_t * c, int kib, int ways)
{
    c->ways = ways;
    c->sets = kib * 1024 / LINE / ways;
    size_t n = (size_t)c->sets * ways;
    c->tag = malloc(n * sizeof(uint32_t));
    c->dirty = calloc(n, 1);
    c->age = calloc(n, sizeof(uint32_t));
    memset(c->tag, 0xff, n * sizeof(uint32_t));
}

static inline int is_psram(uint32_t line)
{
    uint64_t a = (uint64_t)line * LINE;
    return a >= g_psram_lo && a < g_psram_hi;
}

/* Look a line up; on a miss, the victim's address and dirtiness are given
 * back, and the line takes its place. Returns 1 on a hit. */
static int cache_access(cache_t * c, uint32_t line, int write, uint32_t * victim, int * victim_dirty)
{
    const int set = line % c->sets;
    uint32_t * tag = c->tag + (size_t)set * c->ways;
    uint8_t * dirty = c->dirty + (size_t)set * c->ways;
    uint32_t * age = c->age + (size_t)set * c->ways;
    c->clock++;
    for(int w = 0; w < c->ways; w++) {
        if(tag[w] == line) {
            age[w] = c->clock;
            if(write) dirty[w] = 1;
            return 1;
        }
    }
    int oldest = 0;
    for(int w = 1; w < c->ways; w++) if(age[w] < age[oldest]) oldest = w;
    *victim = tag[oldest];
    *victim_dirty = tag[oldest] != 0xffffffffu && dirty[oldest];
    tag[oldest] = line;
    dirty[oldest] = write;
    age[oldest] = c->clock;
    return 0;
}

/* A line leaving the L1 dirty: internal RAM takes it; a PSRAM line goes
 * into the L2, which may push its own victim out to the PSRAM. */
static void l1_writeback(counts_t * k, uint32_t line)
{
    if(!is_psram(line)) {
        k->internal_writeback++;
        return;
    }
    uint32_t v;
    int vd;
    if(!cache_access(&g_l2, line, 1, &v, &vd)) {
        /* Writing a whole line: no fetch needed first. */
        if(vd) k->psram_writeback++;
    }
}

static void touch(uint32_t addr, int write)
{
    counts_t * k = &g_counts[g_phase];
    const uint32_t line = addr / LINE;
    uint32_t v;
    int vd;
    if(cache_access(&g_l1, line, write, &v, &vd)) return;
    if(vd) l1_writeback(k, v);
    if(!is_psram(line)) {
        k->l1_miss_internal++;
        return;
    }
    k->l1_miss_psram++;
    uint32_t v2;
    int vd2;
    if(cache_access(&g_l2, line, 0, &v2, &vd2)) {
        k->l2_hit++;
        return;
    }
    k->psram_fill++;
    if(vd2) k->psram_writeback++;
}

#define DMA_PHASE 2045
static int g_saved_phase;

static void on_mem(unsigned int vcpu, qemu_plugin_meminfo_t info, uint64_t vaddr, void * udata)
{
    if(g_phase == DMA_PHASE) return;
    const int store = qemu_plugin_mem_is_store(info);
    const unsigned size = 1u << qemu_plugin_mem_size_shift(info);
    counts_t * k = &g_counts[g_phase];
    if(store) k->stores++;
    else k->loads++;
    const uint32_t a = (uint32_t)vaddr;
    touch(a, store);
    if((a % LINE) + size > LINE) touch(a + size - 1, store);
}

/* profile=N: instructions per function in phase N, by the symbol QEMU finds
 * for the first instruction of each translated block. */
static int g_profile;
#define PROF 4096
static struct { const char * name; uint64_t insns; } g_prof[PROF];

static void on_tb_profile(unsigned int vcpu, void * udata)
{
    if(g_phase != g_profile) return;
    const uintptr_t v = (uintptr_t)udata;
    g_prof[v >> 16].insns += v & 0xffff;
}

static int prof_slot(const char * name)
{
    if(!name) name = "?";
    for(int i = 0; i < PROF; i++) {
        if(!g_prof[i].name) { g_prof[i].name = name; return i; }
        if(g_prof[i].name == name || !strcmp(g_prof[i].name, name)) return i;
    }
    return PROF - 1;
}

static void on_tb(unsigned int vcpu, void * udata)
{
    g_counts[g_phase].insns += (uintptr_t)udata;
}

static void on_marker(unsigned int vcpu, void * udata)
{
    const int n = (int)(uintptr_t)udata;
    if(n == 2047) {
        g_saved_phase = g_phase;
        g_phase = DMA_PHASE;
    }
    else if(n == 2046) g_phase = g_saved_phase;
    else g_phase = n;
}

static void on_translate(qemu_plugin_id_t id, struct qemu_plugin_tb * tb)
{
    const size_t n = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb, QEMU_PLUGIN_CB_NO_REGS, (void *)(uintptr_t)n);
    if(g_profile) {
        const int slot = prof_slot(qemu_plugin_insn_symbol(qemu_plugin_tb_get_insn(tb, 0)));
        qemu_plugin_register_vcpu_tb_exec_cb(tb, on_tb_profile, QEMU_PLUGIN_CB_NO_REGS,
                                             (void *)(uintptr_t)(((uintptr_t)slot << 16) | n));
    }
    for(size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn * insn = qemu_plugin_tb_get_insn(tb, i);
        if(qemu_plugin_insn_size(insn) == 4) {
            uint32_t op;
            memcpy(&op, qemu_plugin_insn_data(insn), 4);
            /* addi x0, x0, N */
            if((op & 0xfffff) == 0x00013 && (op >> 20) != 0 && (op >> 20) < PHASES) {
                qemu_plugin_register_vcpu_insn_exec_cb(insn, on_marker, QEMU_PLUGIN_CB_NO_REGS,
                                                       (void *)(uintptr_t)(op >> 20));
                continue;
            }
        }
        qemu_plugin_register_vcpu_mem_cb(insn, on_mem, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW, NULL);
    }
}

static const char * g_out = "p4cache.txt";


static void at_exit(qemu_plugin_id_t id, void * p)
{
    FILE * f = fopen(g_out, "w");
    if(!f) return;
    fprintf(f, "phase insns loads stores l1_miss_internal l1_miss_psram l2_hit psram_fill psram_writeback "
            "internal_writeback\n");
    for(int i = 1; i < PHASES; i++) {
        const counts_t * k = &g_counts[i];
        if(!k->insns) continue;
        fprintf(f, "%d %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                " %" PRIu64 "\n", i, k->insns, k->loads, k->stores, k->l1_miss_internal, k->l1_miss_psram,
                k->l2_hit, k->psram_fill, k->psram_writeback, k->internal_writeback);
    }
    if(g_profile) {
        fprintf(f, "profile of phase %d\n", g_profile);
        for(int i = 0; i < PROF && g_prof[i].name; i++)
            if(g_prof[i].insns) fprintf(f, "%" PRIu64 " %s\n", g_prof[i].insns, g_prof[i].name);
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t * info, int argc, char ** argv)
{
    int l1_kib = 64, l1_ways = 4, l2_kib = 128, l2_ways = 8;
    for(int i = 0; i < argc; i++) {
        if(!strncmp(argv[i], "out=", 4)) g_out = argv[i] + 4;
        else if(!strncmp(argv[i], "l1=", 3)) l1_kib = atoi(argv[i] + 3);
        else if(!strncmp(argv[i], "l1ways=", 7)) l1_ways = atoi(argv[i] + 7);
        else if(!strncmp(argv[i], "l2=", 3)) l2_kib = atoi(argv[i] + 3);
        else if(!strncmp(argv[i], "l2ways=", 7)) l2_ways = atoi(argv[i] + 7);
        else if(!strncmp(argv[i], "profile=", 8)) g_profile = atoi(argv[i] + 8);
    }
    cache_init(&g_l1, l1_kib, l1_ways);
    cache_init(&g_l2, l2_kib, l2_ways);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_translate);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
