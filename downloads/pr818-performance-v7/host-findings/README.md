# Host evidence for the batch128 experiment

This folder records a rendering-work experiment, not a hardware speed result. Production defaults remain64. The V7 hardware firmware alone overrides the real draw-unit macro to128 for the D variant.

| Stress scene | Batches64→128 | Tiles64→128 | Rotation bytes64→128 | Pixel differences |
|---|---:|---:|---:|---:|
|48-card dashboard|3→2|30→22|4394792→3088336 (-29.727%)|0|
|Same dashboard plus opaque overlay|4→2|42→28|7465772→5060144 (-32.222%)|0|

Native720×1280, logical1280×720, RGB565, rotation90°, fake PPA. Software-painted bytes are unchanged. Lower bytes in these counters do not establish the reduction of total physical memory traffic or an ESP32-P4 FPS gain.

Batch128 passes the historical LVGL9.6 suite (342/342 CPU and353/353 pthread),768 style-transform comparisons within their documented bounds (384 DIRECT comparisons RGB-exact),32 custom-matrix renders, API isolation/lifecycle and negative controls. Logs andtransform-summary.json retain the output. These host checks use PR head4dfac1c; the draw-unit source is unchanged in the integrated0.7.8 candidate.

A larger jobs array consumes approximately2304 additional bytes on P4 and may change available internal tiles. The hardware kit records tile placement, batch/work/wait counters, largest internal block and local minimum memory to reveal that trade-off. Its dashboard geometry, real PPA and scanout differ from this stress probe. Physical performance remains pending.

The original host runners relied on local prebuilt LVGL archives; this folder preserves evidence and probe source rather than a portable host build. portable/prepare_ab.py in the parent kit rebuilds the actual hardware comparison from pinned public sources plus the supplied integration patch.
