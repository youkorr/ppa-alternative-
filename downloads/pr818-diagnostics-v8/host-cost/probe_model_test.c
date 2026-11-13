/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Compile the ACTUAL wrapper implementation with harmless fake APIs. */
#include "ab_cost_probe.c"
#include <stdatomic.h>
#include <stdio.h>

static atomic_llong clock_us;
static atomic_uint forwarded;
static bool move_stage;
static bool fail_api;
static void *expected_client;
static const void *expected_config;

int64_t esp_timer_get_time(void) { return atomic_fetch_add(&clock_us,1); }
void __real_lv_draw_buf_copy(lv_draw_buf_t *d,const lv_area_t *da,const lv_draw_buf_t *s,const lv_area_t *sa)
{
    assert(d && s && da==sa);atomic_fetch_add(&forwarded,1);atomic_fetch_add(&clock_us,40);
    if(move_stage)ab_cost_probe_set_stage(AB_COST_STAGE_UPDATE);
}
esp_err_t __real_esp_cache_msync(void *a,size_t n,int f)
{
    assert(a==(void *)0x1234);(void)n;(void)f;atomic_fetch_add(&forwarded,1);atomic_fetch_add(&clock_us,10);
    return fail_api?ESP_FAIL:ESP_OK;
}
esp_err_t __real_ppa_do_scale_rotate_mirror(ppa_client_handle_t c,const ppa_srm_oper_config_t *p)
{
    assert(c==expected_client && p==expected_config);atomic_fetch_add(&forwarded,1);
    __wrap_esp_cache_msync((void *)0x1234,128,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    atomic_fetch_add(&clock_us,7);return fail_api?ESP_FAIL:ESP_OK;
}
esp_err_t __real_ppa_do_fill(ppa_client_handle_t c,const ppa_fill_oper_config_t *p)
{assert(c==expected_client && p==expected_config);atomic_fetch_add(&forwarded,1);return fail_api?ESP_FAIL:ESP_OK;}
esp_err_t __real_ppa_do_blend(ppa_client_handle_t c,const ppa_blend_oper_config_t *p)
{assert(c==expected_client && p==expected_config);atomic_fetch_add(&forwarded,1);return fail_api?ESP_FAIL:ESP_OK;}
static void start(ab_cost_stage_t stage)
{ab_cost_probe_set_active(false);ab_cost_probe_reset();ab_cost_probe_set_stage(stage);ab_cost_probe_set_active(true);}
static void *worker(void *n)
{for(unsigned i=0;i<(uintptr_t)n;i++)assert(__wrap_esp_cache_msync((void *)0x1234,128,0)==ESP_OK);return NULL;}
int main(void)
{
    lv_draw_buf_t buffer={.header={.w=10,.h=8,.cf=16}};lv_area_t area={0,0,3,2};
    __wrap_lv_draw_buf_copy(&buffer,&area,&buffer,&area);
    ab_cost_stats_t x=ab_cost_probe_snapshot();assert(x.cell[0][AB_COST_COPY].calls==0 && atomic_load(&forwarded)==1);
    start(AB_COST_STAGE_REFRESH);move_stage=true;__wrap_lv_draw_buf_copy(&buffer,&area,&buffer,&area);move_stage=false;
    x=ab_cost_probe_snapshot();assert(x.cell[AB_COST_STAGE_REFRESH][AB_COST_COPY].calls==1);
    assert(x.cell[AB_COST_STAGE_REFRESH][AB_COST_COPY].bytes==24 && x.cell[AB_COST_STAGE_REFRESH][AB_COST_COPY].pixels==12);
    assert(x.cell[AB_COST_STAGE_REFRESH][AB_COST_COPY].elapsed_us==41 && x.cell[AB_COST_STAGE_UPDATE][AB_COST_COPY].calls==0);
    start(AB_COST_STAGE_UPDATE);
    assert(__wrap_esp_cache_msync((void *)0x1234,128,0)==ESP_OK);
    assert(__wrap_esp_cache_msync((void *)0x1234,256,ESP_CACHE_MSYNC_FLAG_DIR_M2C)==ESP_OK);
    fail_api=true;assert(__wrap_esp_cache_msync((void *)0x1234,512,ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_DIR_M2C)==ESP_FAIL);fail_api=false;
    x=ab_cost_probe_snapshot();ab_cost_cell_t cache=x.cell[AB_COST_STAGE_UPDATE][AB_COST_CACHE];
    assert(cache.calls==3 && cache.bytes==896 && cache.c2m_bytes==640 && cache.m2c_bytes==768 && cache.errors==1);
    start(AB_COST_STAGE_LOCK);ab_cost_probe_set_background(true);uint32_t token=ab_cost_native_wait_begin();
    ab_cost_probe_set_background(false);ab_cost_native_wait_end(token,50,true,true);
    x=ab_cost_probe_snapshot();assert(x.cell[AB_COST_STAGE_BACKGROUND][AB_COST_NATIVE_WAIT].calls==1 && x.cell[AB_COST_STAGE_BACKGROUND][AB_COST_NATIVE_WAIT].pending==1 && x.cell[AB_COST_STAGE_BACKGROUND][AB_COST_NATIVE_WAIT].elapsed_us==50);
    assert(x.cell[AB_COST_STAGE_LOCK][AB_COST_NATIVE_WAIT].calls==0);
    start(AB_COST_STAGE_REFRESH);token=ab_cost_native_wait_begin();start(AB_COST_STAGE_REFRESH);ab_cost_native_wait_end(token,100,true,false);
    x=ab_cost_probe_snapshot();assert(x.cell[AB_COST_STAGE_REFRESH][AB_COST_NATIVE_WAIT].calls==0);
    expected_client=(void *)0x88;ppa_srm_oper_config_t srm={.in={20,30}};expected_config=&srm;
    assert(__wrap_ppa_do_scale_rotate_mirror(expected_client,&srm)==ESP_OK);
    x=ab_cost_probe_snapshot();assert(x.cell[AB_COST_STAGE_REFRESH][AB_COST_SRM].calls==1 && x.cell[AB_COST_STAGE_REFRESH][AB_COST_SRM].pixels==600);
    assert(x.cell[AB_COST_STAGE_REFRESH][AB_COST_CACHE].calls==1 && x.cell[AB_COST_STAGE_REFRESH][AB_COST_SRM].elapsed_us>x.cell[AB_COST_STAGE_REFRESH][AB_COST_CACHE].elapsed_us);
    ppa_fill_oper_config_t fill={40,50};expected_config=&fill;assert(__wrap_ppa_do_fill(expected_client,&fill)==ESP_OK);
    ppa_blend_oper_config_t blend={.in_bg={60,70}};expected_config=&blend;fail_api=true;assert(__wrap_ppa_do_blend(expected_client,&blend)==ESP_FAIL);fail_api=false;
    x=ab_cost_probe_snapshot();assert(x.cell[AB_COST_STAGE_REFRESH][AB_COST_FILL].pixels==2000 && x.cell[AB_COST_STAGE_REFRESH][AB_COST_BLEND].pixels==4200 && x.cell[AB_COST_STAGE_REFRESH][AB_COST_BLEND].errors==1);
    start(AB_COST_STAGE_UPDATE);pthread_t threads[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,worker,(void *)1000));
    for(unsigned i=0;i<4;i++) { assert(!pthread_join(threads[i],NULL)); }
    x=ab_cost_probe_snapshot();
    assert(x.cell[AB_COST_STAGE_UPDATE][AB_COST_CACHE].calls==4000 && x.cell[AB_COST_STAGE_UPDATE][AB_COST_CACHE].bytes==512000 && x.cell[AB_COST_STAGE_UPDATE][AB_COST_CACHE].c2m_bytes==512000);
    ab_cost_probe_set_active(false);assert(ab_cost_native_wait_begin()==0);
    puts("PASS: actual wrappers preserve APIs; count payload/cache flags/errors; attribute entry stages/background; reject stale epochs; nested timing stays inclusive; 4000 concurrent calls have no lost counters");
}
