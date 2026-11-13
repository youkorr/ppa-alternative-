# Rebuild the Waveshare official cleanup diagnostic V8.1

This delivery targets **Waveshare 7B, official profile only**. It retains the V8
protocol, paired legacy/strict timer runs, 64 measured frames and two rounds.
Added markers surround cleanup operations outside the timed trace. The progress
observer is created only after the final measured phase has printed its costs.
It neither aborts nor recovers a stalled operation. The observer's raw console
output bypasses ESP_LOG's mutex and covers UART and secondary USB Serial/JTAG.

Use ESP-IDF **5.5.6**, its matching ESP32-P4 RISC-V toolchain and Python 3.12+.
The official adapter source is `espressif/esp-iot-solution` commit
`da53cfaa90b63085ef21f721f7593b5e2747d448` (0.7.8), with LVGL exactly `9.6.0~1`.
CPU360MHz, PSRAM200MHz, L2 cache128KiB, two software drawing units and three panel
buffers match the previous official test. Stock TRIPLE_PARTIAL remains in use.

Activate the IDF environment, then prepare a new empty private output:

```sh
python portable/prepare_ab.py --repo /path/esp-iot-solution \
  --out /path/wave-cleanup-v8-1 --board waveshare-7b --variant official
cd /path/wave-cleanup-v8-1/components/display/tools/esp_lvgl_adapter/test_apps/lvgl9
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.bench;sdkconfig.ab' -D IDF_TARGET=esp32p4 build
```

The kit contains the already instrumented main source. Its diagnostic patcher,
original V8 source and before/after SHA256 manifest allow review/reconstruction
of the cleanup-only changes. Fresh managed-component sources prevent native
LVGL hooks from contaminating this official reference. Neither the input source
checkout nor shared SDK is edited. Both benchmark-only DMA and cost observers
remain the same as V8; the rendering algorithm is unchanged.

Inspect `build/flasher_args.json`, merge its actual segments using esptool,
DIO/40MHz and 32MB flash, and flash the merged `.bin` at **0x0**. Normal offsets
are bootloader0x2000, partition table0x10000 and app0x20000. Keep the normal
application firmware to restore after this standalone diagnostic.

Verify the real build selects the private `APP/components/esp_lcd`, preserves
all six linker wrappers and their IRAM cache-call path, and does not compile the
native ortho draw unit. Source/probe/configuration hashes and the merged binary
SHA256 are supplied next to the download. The pinned profile prints panic text;
V8.1 deliberately checks this because it reuses only IDF's bounded raw console
output helper, without invoking a panic handler.

Record serial output at115200baud. At an apparent stop, wait15seconds and keep
all `AB_CLEANUP` and `AB_CLEANUP_WAIT` rows. `AB_DONE` and
`AB_CLEANUP_COMPLETE` certify that the normal cleanup returned. A frozen stage
can locate a failing call; no specific Espressif/LVGL fault is established until
that hardware trace is available. New performance claims should not be based on
this cleanup diagnostic.
