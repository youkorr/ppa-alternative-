# Rebuild V9 native eager/deferred comparison

Use ESP-IDF5.5.6, LVGL9.6.0~1, Python3.12+ and the matching P4 toolchain. Prepare
from official esp-iot-solution commit da53cfaa90b63085ef21f721f7593b5e2747d448.
The supplied integration patch reconstructs native candidatef8ce296abfe2f6cc1fa38fc5ef1928361f1dc3a7,
tree88fec6fe91b499dfd9ab62face91a2673ff429a0. It includes the per-display policy,
its adapter activation and real-LVGL host regressions. Fetching the local commit
is not required.

```sh
python portable/prepare_ab.py --repo /path/esp-iot-solution \
  --out /path/new-private-v9-tab5 --board tab5 --variant native64
cd /path/new-private-v9-tab5/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.bench;sdkconfig.ab' -D IDF_TARGET=esp32p4 build
```

Use --board waveshare-7b for the other board. The native policy is enabled by the
adapter on a participating rotated display; the benchmark temporarily selects
its eager/deferred value per phase. Automatic refresh remains active in both.
There is no strict-timer mode in V9. Same-on-both-mode diagnostic wrappers and
private DMA/wait probes remain; their inclusive timing overhead must not be
mistaken for an uninstrumented production benchmark.

Use a fresh private managed LVGL source per build. The compatibility hook replaces
four files atomically, preserves the shared cache and defaults the policy off.
Ordinary/custom-sync displays are not opted in. LVGL9.5 lacks9.6's subtraction of
current invalid areas from pending sync regions: correctness is preserved, but
full-image performance benefits may differ across versions.

Validate the private esp_lcd component selection, linked per-display setter/getter,
IRAM cache-probe call path, native batch64 compile definition and exact generated
source/settings hashes. The descriptor-retargeting panel path still needs physical
validation; stock LVGL three-buffer host tests alone do not certify it.

Merge the actual segments in build/flasher_args.json using DIO/40MHz, flash16MB
for Tab5 or32MB for Waveshare. Flash the merged binary at0x0. Save the full serial
AB_READY throughAB_DONE log at115200baud, including all new cost and policy rows.
Analyse matching protocol9 captures using fixtures/parse_logs.py. V7/V8 data must
not be pooled into this comparison.
