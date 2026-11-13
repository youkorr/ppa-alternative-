#!/usr/bin/env python3
from pathlib import Path
import argparse,hashlib,json,subprocess
ROOT=Path(__file__).resolve().parent
p=argparse.ArgumentParser(description='Run the extracted V7 scene flow on a matched real LVGL9.6 host archive')
p.add_argument('--lvgl',type=Path,default=Path('/workspace/work/pr818-upstream-tests96/test/build-plain/lvgl-src'))
p.add_argument('--archive',type=Path,default=Path('/workspace/work/pr818-upstream-tests96/test/transform-build/oracle.a'))
p.add_argument('--fixture',type=Path,default=Path('/workspace/work/ab-performance-kit/fixtures/ab_main.c'))
a=p.parse_args();LVGL=a.lvgl;ARCHIVE=a.archive;SOURCE=a.fixture
source=SOURCE.read_text()
functions=source[source.index('static void disable_scroll'):source.index('static void prepare_bank')]
# Only extract scene functions, not ESP timing/cache/probe functions.
functions=functions[:functions.index('static uint32_t percentile')]+functions[functions.index('static lv_obj_t *card'):]
prefix=(ROOT/'timer_policy.c').read_text().split('static void run_case')[0].replace('static lv_obj_t *screen, *marker;','static lv_obj_t *screen;')
headers='''\n#include "src/misc/cache/instance/lv_image_cache.h"\n#include <inttypes.h>\n#define AB_WIDTH 1280\n#define AB_HEIGHT 720\n#define AB_DASHBOARD_FULL 0\n#define s_display display\nstatic lv_obj_t *s_arc, *s_bar, *s_slider, *s_counter, *s_marker;\nstatic lv_image_dsc_t s_image;\nstatic uint8_t *s_bank[4];\nstatic const uint8_t *ab_wallpaper_start;\n'''
main=r'''
static void scene_trace(bool controlled, unsigned kind)
{
    strict = controlled; track = false; inject_once = false;
    lv_init();
    display = lv_display_create(AB_WIDTH, AB_HEIGHT); assert(display);
    const size_t bytes = AB_WIDTH * AB_HEIGHT * 2;
    void *buf[3] = {calloc(1,bytes),calloc(1,bytes),calloc(1,bytes)};
    assert(buf[0] && buf[1] && buf[2]);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf[0], buf[1], bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_draw_buf_t third;
    assert(lv_draw_buf_init(&third, AB_WIDTH, AB_HEIGHT, LV_COLOR_FORMAT_RGB565, AB_WIDTH * 2, buf[2], bytes) == LV_RESULT_OK);
    lv_display_set_3rd_draw_buffer(display, &third);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, observe, LV_EVENT_ALL, NULL);
    screen = lv_display_get_screen_active(display);
    for(unsigned b = 0; b < 4; b++) {
        s_bank[b] = malloc(bytes); assert(s_bank[b]);
        /* Precompute V7 input banks; values cannot affect invalidation logic. */
        for(unsigned y = 0; y < AB_HEIGHT; y++) for(unsigned x = 0; x < AB_WIDTH; x++) {
            const unsigned r = (x * 31 / AB_WIDTH + b * 7) & 31;
            const unsigned g = (y * 63 / AB_HEIGHT + b * 11) & 63;
            const unsigned blue = ((x / 48) ^ (y / 48) ^ (b * 9)) & 31;
            const uint16_t pixel = (r << 11) | (g << 5) | blue;
            memcpy(s_bank[b] + ((size_t)y * AB_WIDTH + x) * 2, &pixel, 2);
        }
    }
    ab_wallpaper_start = s_bank[0];
    lv_timer_t *other = lv_timer_create(beat, 5, NULL);
    assert(other);
    lv_timer_pause(lv_display_get_refr_timer(display));
    build_scene(kind);
    for(unsigned i = 0; i < 16; i++) {
        update_scene(kind,i); refresh();
        lv_tick_inc(40); lv_timer_handler();
    }
    reset_counters(); track = true;
    unsigned resumed_after_manual = 0;
    for(unsigned i = 0; i < 64; i++) {
        update_scene(kind,16+i); refresh();
        if(!lv_timer_get_paused(lv_display_get_refr_timer(display))) resumed_after_manual++;
        /* Controlled opportunity for the background timer after unlocking.
         * This is NOT wall-clock or RTOS scheduling emulation. */
        lv_tick_inc(40); lv_timer_handler();
    }
    assert(manual_starts == 64 && manual_ready == 64);
    assert(heartbeat == 64);
    if(controlled) assert(auto_starts == 0 && auto_copies == 0 && auto_flushes == 0);
    printf("{\"case\":\"v7_scene_trace\",\"mode\":\"%s\",\"scene\":%u,\"frames\":64,\"resumed_after_manual\":%u,\"automatic_starts\":%u,\"automatic_flushes\":%u,\"automatic_copy_calls\":%u,\"automatic_copy_pixel_bytes\":%zu,\"manual_copy_calls\":%u,\"manual_copy_pixel_bytes\":%zu,\"other_timer_callbacks\":%u}\n",
           controlled ? "strict" : "legacy", kind, resumed_after_manual,auto_starts,auto_flushes,
           auto_copies,auto_copy_bytes,manual_copies,manual_copy_bytes,heartbeat);
    track=false;
    lv_timer_delete(other);lv_obj_clean(screen);lv_image_cache_drop(&s_image);
    lv_display_delete(display);lv_deinit();
    for(unsigned b = 0; b < 4; b++) free(s_bank[b]);
    for(unsigned b = 0; b < 3; b++) free(buf[b]);
}
int main(void)
{
    for(unsigned kind=0;kind<3;kind++) {scene_trace(false,kind);scene_trace(true,kind);}
    return 0;
}
'''
(ROOT/'scene_trace.c').write_text(prefix+headers+functions+main)
flags=['cc','-O2','-Wall','-Wextra','-Werror','-DLV_CONF_INCLUDE_SIMPLE','-DLV_FONT_MONTSERRAT_20=1','-DLV_DRAW_TRANSFORM_USE_MATRIX=0','-DLV_USE_OS=0','-DESP_LVGL_ADAPTER_OS_MUTEX_CLEANUP=1','-I'+str(ROOT),'-I'+str(LVGL)]
cmd=flags+[str(ROOT/'scene_trace.c'),str(LVGL/'src/font/lv_font_montserrat_20.c'),str(ARCHIVE),'-Wl,--wrap=lv_draw_buf_copy','-lm','-o',str(ROOT/'scene_trace')]
subprocess.run(cmd,check=True)
r=subprocess.run([str(ROOT/'scene_trace')],capture_output=True,text=True)
(ROOT/'scene-trace.log').write_text(r.stdout+r.stderr)
if r.returncode:raise RuntimeError(r.stdout+r.stderr)
rows=[json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]
(ROOT/'scene-trace-results.json').write_text(json.dumps({'source_fixture_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),'lvgl_archive_sha256':hashlib.sha256(ARCHIVE.read_bytes()).hexdigest(),'host':True,'source_frames':64,'font20_compiled_separately':True,'software_renderer':'LVGL stock/matrix disabled; no PPA/adapter/RTOS','scheduling':'explicit40ms tick + actual timerhandler after each manual refresh; not RTOS emulation','results':rows},indent=2)+'\n')
print(r.stdout)
