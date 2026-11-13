# V8 manual-refresh policy: real LVGL host regression

These checks exercise actual LVGL9.6 timers, events, rendering and DIRECT buffer synchronization. No timer/display API is mocked. They validate the benchmark timer policy; they do **not** measure ESP32-P4 speed or prove the frequency/cost of background refreshes in the captured V7 hardware run.

## Policy regression

`timer_policy.c` invalidates an additional screen area during a manual refresh. This resembles legitimate layout/event work after LVGL has paused its timer at refresh start. The default `LV_EVENT_REFR_REQUEST` handler resumes that timer before the test listener runs.

| Mode | Manual refreshes | Later automatic refreshes | Automatic flushes | Automatic copy payload | Other timer callbacks |
|---|---:|---:|---:|---:|---:|
| Legacy one-time pause | 1 | 1 | 0 | 153600 bytes | 16 |
| Strict request-handler pause + after-manual pause | 1 | 0 | 0 | 0 bytes | 16 |

The legacy automatic refresh is empty: it produces no new flush but still executes a DIRECT synchronization copy. Strict mode prevents the extra display refresh while the independent5ms LVGL timer continues to run.

## V7 scene-flow regression

`build_scene_trace.py` extracts the V7 fixture's unchanged scene construction and update functions. It adds the actual Montserrat20 font as one separately compiled LVGL source, reuses the matched host archive, and runs the three scenes at logical1280×720 with three RGB565 DIRECT buffers. Four changing input banks are precomputed using V7's pixel formula. A fixed image uses one of those banks; memory is ordinary host RAM rather than flash/PSRAM.

After16 warm-ups,64 manual frames per scene run with an explicit40ms tick increment and actual `lv_timer_handler()` after each frame. This offers a deterministic background-timer opportunity; it is **not** RTOS or elapsed-render-time emulation. There is no injected invalidation in this scene trace.

| Scene/mode | Timer rearmed after manual | Empty automatic refreshes | Automatic copy payload | Legitimate manual copy payload |
|---|---:|---:|---:|---:|
| Dashboard legacy | 50/64 | 50 | 11021284 bytes | 132840 bytes |
| Dashboard strict | 0/64 | 0 | 0 bytes | 4169732 bytes |
| Changing images legacy | 50/64 | 50 | 184320000 bytes | 0 bytes |
| Changing images strict | 0/64 | 0 | 0 bytes | 0 bytes |
| Fixed image legacy | 50/64 | 50 | 184320000 bytes | 0 bytes |
| Fixed image strict | 0/64 | 0 | 0 bytes | 0 bytes |

Each background image refresh makes two copies because the test uses triple DIRECT buffers. Payload counts are rectangle pixels multiplied by2; they are **not** total physical bus traffic. Dashboard strict mode still needs valid dirty-area synchronization before the next manual frame, so not all manual copies should disappear. The independent timer executes64 callbacks in every scene/mode.

## Reproduction

Requirements: C compiler, Python3, a matching LVGL9.6 source tree and a host archive built with the supplied `lv_conf.h`, `LV_USE_OS=0`, `LV_DRAW_TRANSFORM_USE_MATRIX=0` and the same private display-structure layout. The archive used here is the existing PR818 transform-test oracle build; archive/config/source hashes are recorded in the result JSON. The adapter/PPA backend is not linked. This source tree includes the PR's display-policy field, disabled by default; the exercised timer logic is LVGL's original9.6 logic.

```sh
python run_policy.py --lvgl /path/lvgl --archive /path/oracle.a
python build_scene_trace.py --lvgl /path/lvgl --archive /path/oracle.a \
  --fixture /path/v7/fixtures/ab_main.c
```

The V7 fixture is published at `youkorr/ppa-alternative-`, branch `test/pr818-card-firmwares-20261112`, path `downloads/pr818-performance-v7/fixtures/ab_main.c`, commit `81b582a1aec4d1b057d0f1402a8ae78aa7afbb46`. Defaults in the Python runners select the existing workspace paths; all required external paths can be supplied explicitly.

`policy-results.json`, `scene-trace-results.json`, `policy.log`, `scene-trace.log` and `source-hashes.json` retain the results. Generated executables are not needed to retain the evidence. Actual V8 firmware should count background refreshes and copy costs to determine whether this mechanism explains the hardware timing gap.
