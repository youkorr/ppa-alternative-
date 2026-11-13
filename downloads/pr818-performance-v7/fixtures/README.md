# A/B benchmark against official esp_lvgl_adapter 0.7.8

This fixture prepares a hardware comparison, not a claim of a measured speed-up.
No result is available until the generated applications run on the physical P4s.

## Variants and controls

For each original Tab5 / Waveshare 7B, build the same application with:

| Label | Adapter source | Display path | Panel buffers | Batch limit |
|---|---|---|---:|---:|
| `official` | Official 0.7.8 | Stock `TRIPLE_PARTIAL` | 3 | unused |
| `official_ppa1` | Official 0.7.8, stock PPA drawing requested | Stock `TRIPLE_PARTIAL` | 3 | unused |
| `control` | Candidate integrated on 0.7.8, option disabled | Stock `TRIPLE_PARTIAL` | 3 | unused |
| `native64` | Same candidate, option enabled | `DOUBLE_DIRECT` + rotation-in-draw | 3 | 64 |
| `native128` | Same candidate, option enabled | `DOUBLE_DIRECT` + rotation-in-draw | 3 | 128 |

The four public performance variants are `official`, `official_ppa1`, `native64`
and `native128`. The candidate `control` is an additional integration check.
`official_ppa1` is installed with `--variant official --stock-ppa`; it enables the
stock PPA draw path and uses its required single software draw unit. The other
variants use two software draw units. This intentional stock tuning is recorded
in the metadata and `AB_READY` log. Native variants reject `--stock-ppa` because
their PPA use must not be combined with another draw client. Compare against the
better official baseline separately for each scene, rather than picking the
slower official configuration.

The stock baseline must be the existing fast pipelined path. Comparing only
against a weaker stock rotated `DOUBLE_DIRECT` path could flatter the result.
The tear mode is intentionally part of the architecture change; do not describe
all buffer strategies as identical. CPU, PSRAM, L2, resolution, input pixels,
physical panel buffer count, fonts, SDK/LVGL versions and RTOS tick rate must match.
Record the exact source tree as well as its base commit: an uncommitted integration
tree is not identified by the old PR head alone.

## Workload

Three repeats per phase, 16 warm-up frames, then an identical ordered trace of
128 application frames. The default phases are:

- animated dashboard with natural dirty regions;
- four changing RGB565 PSRAM frames, precomputed outside measurement and marked
  modifiable so they cannot enter the immutable-image cache;
- a full-screen immutable flash image, cache disabled, with a changing marker.

Native variants additionally run `fixed_flash_warm_cache`. It is a separate cache
experiment and has no stock same-cache counterpart. Do not use its gain to claim
that all changing video becomes faster. The raw source pixels and deterministic
frame-index progression are the same within a board across all variants.

`--dashboard-full` additionally forces full dashboard invalidation; use that same
setting in every compared build. The default dashboard uses normal dirty-region
invalidation instead of forcing a full redraw. No LVGL animation depends on wall
time. No camera sensor, network, audio or touch source runs during measurement.

The application pauses only the automatic LVGL display refresh timer; other LVGL
timers continue, including the adapter's asynchronous setup. Each refresh is
explicitly owned by the deterministic trace.

The application initializes the display once. Rendering performance is not
confounded by the unrelated stock LVGL teardown behavior. Every phase logs memory
and heap integrity; local heap-minimum monitoring covers only that measured phase
and its final drain, not the entire boot. Largest internal free blocks and native
tile/batch/wait/bytes-turned counters expose memory and batching trade-offs.
The earlier V6 programs separately validate repeated teardown.
One idle tick every sixteen measured frames lets the watchdog's idle tasks run;
the tick rate and yield count are logged. These pauses are included in throughput,
but not in per-refresh median/p95. Expect approximately 1.5–3 minutes per firmware,
depending on the board and observed rate.

## What is actually counted

`LV_EVENT_REFR_READY` is not proof that a unique image reached the panel. Likewise,
the IDF 5.5.6 `on_frame_buf_complete` callback repeats every scanout, including
unchanged frames, and carries no framebuffer identity. A VSYNC count would tend
toward the panel rate even if the UI stopped producing pictures.

The optional private driver probe observes the exact index selected by the DPI
DMA ISR. The linker wrapper tags real framebuffer submissions with the current
application generation. Repeated scanouts and multiple flushes of one generation
are counted once. `scanout_fps` therefore means **distinct application generations
selected for DMA scanout**. It is not an optical measurement of the panel glass.
The probe reports unselected/superseded generations, failed submissions and invalid
buffer associations. A requested but absent/inconsistent probe invalidates a run.

`submitted_fps` measures the complete application trace through the LVGL refresh
calls, including update and lock costs, and the small recorded idle yields.
The per-refresh median/p95 time covers `lv_refr_now`, including any pipeline wait;
it must not be described as CPU-only rendering cost. If the scanout probe is not
enabled, `scanout_fps=-1`/`UNAVAILABLE`: publish only submitted refreshes/s.

## Installation and build

First stage the correct board's panel hardware and adapter source. The installer
does not touch those drivers, fetch dependencies or edit the shared SDK.

```sh
python install_ab.py --app /path/app --board tab5 --variant native --batch-limit 128 \
  --source-sha BASE_COMMIT_40_HEX --source-tree EXACT_INTEGRATED_TREE_40_HEX --probe
python patch_dpi_probe.py --idf "$IDF_PATH" --out /path/app/components/esp_lcd
```

Use the same probe source in **all four** variants, including the official one.
The patcher copies ESP-IDF 5.5.6's `esp_lcd` component to a private application
override and inserts only the weak observer call at DMA selection. It never edits
the SDK/component cache. The original and instrumented driver hashes are recorded.
Check that the final build really selected this private component and the correct
variant; do not rely only on a filename label.

Add `sdkconfig.ab` as the **last** defaults file and regenerate `sdkconfig` when
switching variants. It explicitly disables the automatically configured image
cache; only the named warm-cache phase enables it. Preserve the board's 16/32 MB
flash settings and supported silicon revision from its known-good hardware setup.

The native main CMake attaches `LV_DRAW_ORTHO_MAX_BATCH=64` or `128` privately to
the actual adapter component library, not to global CFLAGS. Verify that definition
on the real `lv_draw_ortho.c` entry in `compile_commands.json`. The standalone
application source is identical across variants; generated config records the
intentional compile/profile differences. The integrated source tree and the
wallpaper/fixture fingerprints are stored in `ab-benchmark.json`.

Capture the complete log from `AB_READY` through `AB_DONE`, then:

```sh
python parse_logs.py official.log official_ppa1.log native64.log native128.log --out comparison.json
```

The parser rejects incomplete runs, duplicate scene rounds, heap/transfer failures,
and mismatched board settings, instrumentation or tick rate. It compares the
median of repeated matching phases against that board's official baseline and
reports min/max spread, p95 refresh time, skipped submissions and memory use.
The warm-cache phase stays separate. A performance percentage is publishable only
after these hardware measurements; do not convert host fake-PPA byte savings into
a physical FPS claim.

## Current validation scope

The installer and private probe generator have been dry-run; the unchanged SDK
driver fingerprint was verified. Both C sources pass syntax checking with the
actual existing ESP32-P4 compiler flags and LVGL 9.6 headers. The probe's host
state-machine test checks repeated IRQ filtering, skipped and reused generations,
the tagging/selection boundary, invalid pointers and failed real driver calls.
This does not
replace a complete image build or either board's physical run.
