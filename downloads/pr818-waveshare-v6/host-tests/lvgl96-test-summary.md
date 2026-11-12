# LVGL 9.6 — genuine per-display software object transforms

All LVGL source files were rebuilt in three configurations. The new display
field therefore has a consistent layout in list allocation, initialization,
and render code. This is the per-display setter/getter fix, not the earlier
global refresh-branch bypass. The reference uses matrix=0; subjects use matrix=1
and explicitly select software object layers on their display.

| Subject | Exact / 256 | Within one native channel level / 256 | Render failures | DIRECT exact / 128 |
|---|---:|---:|---:|---:|
| CPU | 218 | 254 | 0 | 128 |
| Fake PPA | 218 | 254 | 0 | 128 |
| pthread second render core | 218 | 254 | 0 | 128 |

The two frames beyond one native level are `nested-rotations`, XRGB8888,
screen 90/270, PARTIAL. Each differs at 114 pixels, and exactly one pixel
has a maximum channel difference of 2/255. All other remaining differences
are at most one native channel level. No claim of exact PARTIAL equality.

The fake PPA ran 742 transactions including 434 fills and processed
15,085,968 pixels in these cases, with zero queued transactions at captures.

Separate API assertions passed: no-display NULL access, default false on
new displays, two independent displays, NULL/default-display selection,
and disabling the policy on one display without changing the other.

The earlier comprehensive regression suite also passed using the new policy:

- CPU: 342/342 cases.
- pthread: 353/353 cases, including balanced OS mutex lifecycle (7 created,
  7 deleted) and cache teardown/progress checks.

The pthread legacy run uses `pthread-cleanup.a`, which replaces the rebuilt
`lv_init.o` with the adapter's `ESP_LVGL_ADAPTER_OS_MUTEX_CLEANUP=1` setting.
All other structure-dependent objects had already been rebuilt. Original
rendering archives remain available with their measured fingerprints.

Results: `results.json`, `metadata.json`, `policy_check.log` and
`legacy/results.json`. Reproduction scripts: `run96.py`, `run_legacy96.py`.
These are functional host tests with actual LVGL and a fake PPA, not hardware
DMA/cache, performance, physical screen, or endurance validation.
