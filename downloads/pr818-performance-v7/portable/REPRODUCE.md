# Rebuild the four hardware A/B variants

Requires ESP-IDF **v5.5.6**, Python 3.12+, Git and `patch`. Use the IDF export
script for your installed toolchain. The script exports pinned Git objects and
never modifies your source checkout or the shared IDF SDK.

The official source is `espressif/esp-iot-solution` commit
`da53cfaa90b63085ef21f721f7593b5e2747d448` (adapter 0.7.8). Native variants apply
the supplied `integration-on-0.7.8.patch` to that exact commit. This reconstructs
the integrated candidate component identified by local commit
`79b87e9e8703e85e66812850414428c4cd978865`, repository tree
`765e2a516a3a42c91d0145150e35e9c5dc41293c`. Fetching that local commit is not required.

With `fixtures/`, `boards/`, `integration-on-0.7.8.patch`, and `portable/` as kit
siblings, prepare a **new empty output for each variant**:

```sh
python portable/prepare_ab.py --repo /path/esp-iot-solution \
  --out /path/ab-tab5-native128 --board tab5 --variant native128
cd /path/ab-tab5-native128/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.bench;sdkconfig.ab' -D IDF_TARGET=esp32p4 build
```

Repeat with `official`, `official_ppa1`, `native64`, `native128`; use
`--board waveshare-7b` for the Waveshare. Explicit `--fixtures`, `--boards`,
`--patch` and `--idf` paths are accepted if using another kit layout. The output
records source/fixture hashes and adds the **same private DMA observer** to all
variants. CPU, PSRAM, L2 cache, input trace and three physical framebuffers match.
The official PPA variant requests the stock one-worker PPA path; other variants
use two software draw workers. The different stock/native buffering strategies
are intentional and documented in the fixture README.

Dependencies resolve LVGL exactly `9.6.0~1`. Begin with a fresh private
`managed_components` for every variant: native CMake installs its required LVGL
core hooks into that private copy. Never share a patched LVGL source directory or
hard-linked component cache with a stock build. When deliberately reusing a
private staging directory, restore the six `lvgl-pristine/` files and verify them
against the vendor `CHECKSUMS.json` **before** configuring every variant. Otherwise
the alleged official baseline could still contain candidate code.

The merged download uses bootloader at `0x2000`, partition table at `0x10000`, and
application at `0x20000`. Inspect the generated `build/flasher_args.json`; merge
those **actual** segments with `esptool --chip esp32p4 merge_bin`, DIO/40 MHz, flash
16 MB for Tab5 or 32 MB for Waveshare. Flash the resulting merged image at `0x0`.
`idf.py flash` can instead flash the generated separate segments directly.

Before accepting a build, verify `build/project_description.json` selects
`APP/components/esp_lcd`, the ELF/link contains
`__wrap_esp_lcd_panel_draw_bitmap`, and native `compile_commands.json` has the
correct `LV_DRAW_ORTHO_MAX_BATCH=64` or `128` on `lv_draw_ortho.c`. Official variants
must not compile `lv_draw_ortho.c`. Save the final sdkconfig/dependencies lock and
firmware SHA256 alongside the log. Source fingerprints provide traceability;
bit-identical output additionally requires identical resolved dependency versions,
toolchain, timestamps/build environment and paths.

Hardware results remain pending until these binaries run on the physical boards.
Use `fixtures/parse_logs.py` on complete serial logs; compare matching scenes to
the better of the two official profiles. The changing PSRAM banks are a synthetic
rendering load, not camera sensor/ISP FPS. Warm immutable-image caching is reported
separately from normal video work.
