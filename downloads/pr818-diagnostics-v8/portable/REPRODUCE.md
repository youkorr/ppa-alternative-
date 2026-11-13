# Rebuild the V8 paired diagnostics

Requires ESP-IDF **v5.5.6**, its matching RISC-V toolchain, Python 3.12+, Git and
`patch`. Activate the IDF environment for your installation first. The script
exports exact Git objects into a new private output; the input checkout and
shared IDF SDK are not edited.

The four downloads are the original Tab5 and Waveshare 7B, each with `official`
and `native64`. Each binary runs **both** timer policies itself. It measures
64 frames after 16 warm-up frames, twice per policy/scene, alternating policy
order by round. No separate firmware flash is needed to switch timer policies.

## Pinned rendering sources

- Official adapter 0.7.8: `espressif/esp-iot-solution` commit
  `da53cfaa90b63085ef21f721f7593b5e2747d448`.
- Native candidate: that official source plus `integration-on-0.7.8.patch`,
  reconstructing local commit `79b87e9e8703e85e66812850414428c4cd978865`, tree
  `765e2a516a3a42c91d0145150e35e9c5dc41293c`. Fetching the local commit is not
  required; the official commit and supplied patch suffice.
- LVGL dependency: exactly `9.6.0~1`. The candidate draw unit uses batch capacity 64.
- Both profiles use two software draw units, CPU at 360 MHz, PSRAM at 200 MHz, L2
  cache of 128 KiB and three physical panel framebuffers. The official profile sets
  `enable_ppa_accel=false`; this does **not** imply that all existing rotation
  paths avoid the PPA. Stock `TRIPLE_PARTIAL` and native `DOUBLE_DIRECT` are
  deliberately different rendering architectures.

## Prepare and compile

The kit layout has sibling `fixtures/`, `boards/`,
`integration-on-0.7.8.patch` and `portable/` directories. Use a **new empty output
for each board/profile**:

```sh
python portable/prepare_ab.py --repo /path/esp-iot-solution \
  --out /path/v8-tab5-native64 --board tab5 --variant native64
cd /path/v8-tab5-native64/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.bench;sdkconfig.ab' -D IDF_TARGET=esp32p4 build
```

Repeat with `--variant official`; use `--board waveshare-7b` for the Waveshare.
Explicit `--fixtures`, `--boards`, `--patch` and `--idf` paths are supported.
`portable-preparation.json` records the pinned component's file hashes before
fixture overlays, all fixture/BSP hashes, and the native observer's before/after
source hashes. The application records protocol 8, both policies and fixed frame
counts in `ab-benchmark.json`.

Keep `sdkconfig.ab` last. Delete an existing sdkconfig when deliberately changing
profiles: appending defaults does not overwrite an old configuration. Fresh
outputs also avoid native LVGL core hooks contaminating an official baseline.
Do not share patched source files or hard-linked managed components between
builds. If deliberately reusing a private stage, restore the six supplied
`fixtures/lvgl-pristine/` files against vendor `CHECKSUMS.json` before configuring
any profile, as the supplied build driver does.

## Test-only observers

Every profile receives the same private ESP-IDF `esp_lcd` DMA-generation observer
and the same application wrappers for draw-buffer copy, cache synchronization,
PPA submission and fill/blend calls. The native profile additionally observes
entry/exit of its private static PPA wait helper through weak hooks; its original
waiting algorithm and return values are retained. Neither patch edits the
shared SDK or production source checkout.

Inspect the actual final build, not only the firmware name:

- `project_description.json` must select `APP/components/esp_lcd`.
- The ELF must retain `__wrap_esp_lcd_panel_draw_bitmap`,
  `ab_scanout_latched_from_isr`, `__wrap_lv_draw_buf_copy`,
  `__wrap_esp_cache_msync`, and the three PPA wrapper symbols.
- The cache wrapper and its helper call path must be in IRAM, with probe state
  in DRAM; the real cache API can run with flash cache disabled.
- Native `compile_commands.json` must set `LV_DRAW_ORTHO_MAX_BATCH=64` on
  `lv_draw_ortho.c`; the official build must not compile that file.
- Preserve the generated sdkconfig, dependencies lock, probe manifests and
  firmware SHA256 next to the capture.

The probes collect **inclusive wall times**. Nested categories can overlap and
must not be added to estimate a total. A PPA submission's duration is not the
complete asynchronous operation duration; native wait-pending counts distinguish
an outstanding ticket at entry from calls that find work already complete.
Reported logical copy payload/cache spans are not measured physical bus traffic.
The instrumentation itself may perturb timings. Compare paired timer policies
and matching V8 profiles; do not pool V7 and V8 FPS or call V8 a production speed-up.

## Export and flash

Inspect `build/flasher_args.json` and merge the **actual** listed segments with
`esptool --chip esp32p4 merge_bin`, using DIO/40 MHz and flash of 16 MB for Tab5 or
32 MB for Waveshare. These projects normally use bootloader `0x2000`, partition
table `0x10000` and app `0x20000`. Save the merged output as
`{board}-diagnostics-v8-{variant}.bin` and flash that merged file at **0x0**.
Alternatively, `idf.py flash` flashes the generated separate segments directly.

Keep your normal application firmware to restore afterward. The diagnostic is a
standalone temporary application; capture serial output at 115200 baud, reset,
and retain the full `AB_READY` through `AB_DONE` capture, including all new
`AB_TIMING`, `AB_REFRESH` and `AB_COST` records. The synthetic image banks isolate
rendering, not camera sensor/ISP performance. The native warm-cache scene remains
a separate immutable-image experiment.

Hardware results are pending until the four V8 programs run on the physical
boards. Correctness of the strict policy and diagnostic counters is checked
separately by the bundled host tests; those tests do not establish an ESP32-P4
FPS improvement.
