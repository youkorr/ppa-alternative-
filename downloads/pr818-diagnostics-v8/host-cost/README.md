# V8 cost probes (benchmark-only)

Copy `../fixtures/ab_cost_probe.c` and `.h` into the standalone benchmark main component.
Require `esp_mm`, `esp_driver_ppa`, LVGL (already public via the board/adapter),
and `esp_timer`. Link with:

```
-Wl,--wrap=lv_draw_buf_copy
-Wl,--wrap=esp_cache_msync
-Wl,--wrap=ppa_do_scale_rotate_mirror
-Wl,--wrap=ppa_do_fill
-Wl,--wrap=ppa_do_blend
```

The wrappers call the real API with identical arguments and return its result.
No source image, drawing algorithm, cache policy, PPA transaction mode or callback
is changed. The shared IDF/component cache must never be patched. When measuring
the native backend, use `patch_native_wait_probe.py --source PRIVATE_COMPONENT/
src/display/rotate_in_draw/lv_draw_ortho_ppa.c` on a private copy reconstructed from
the pinned candidate. It preserves the original wait function and surrounds its
call with weak observer hooks. The uninstrumented production component is intact.

`set_active(false)`, `reset()`, `set_stage(...)`, then `set_active(true)` begin a
capture. Stop only after its final drain. Reset requires inactive capture. An
entry token freezes the stage and epoch; an operation finishing after capture
stops or its epoch resets is not charged to the following phase. LVGL nonmanual
REFR_START/READY callbacks should set/clear the independent background override.
The active main-stage value is preserved while that override is set.

Every stage/kind cell records inclusive wall time, calls and available API errors.
**Do not sum category times**: a PPA call can contain cache operations and a copy
handler can include cache/PPA work. The elapsed times include preemption and
blocking, rather than measuring CPU cycles alone. Inactive wrappers forward
without reading the clock. Instrumentation adds small overhead to active calls;
use identical probes on stock and native, and compare timer modes inside the
same binary. This is a diagnostic fixture rather than a new production FPS claim.

Coverage and interpretation:

- `copy` wraps `lv_draw_buf_copy`, including the DIRECT synchronization calls from
  the separate `lv_refr.c` translation unit. It does not measure every `memcpy`
  or CPU rotation loop. `bytes` is requested logical destination payload once,
  not source+destination traffic or cache-line expansion. `pixels` is requested
  rectangle size. Void copy APIs provide no operation-result status.
- `cache` records the requested `esp_cache_msync` span and OR of flags. Direction
  totals separately include C2M and M2C bytes, respecting default C2M when no
  direction flag is set. They are requested synchronization spans, not a count
  of physically dirty bytes or proof of external bus traffic.
- `srm`, `fill`, `blend` record PPA submission-call wall time and requested input/
  fill pixels. A nonblocking API's duration is not hardware completion latency.
  A blocking API's duration includes its wait. Nonblocking completion failures
  cannot be inferred from a successful submission alone.
- `native_wait` surrounds the real native wait function. `pending` counts tickets
  still outstanding at entry; it does not mean the full duration was stalled.
  Already-complete tickets are still calls. `errors` counts false results.

The cache API is cache-safe in IDF, so its wrapper, token and recording helpers
are explicitly IRAM, with counters/mux/state in DRAM and no allocation, logging,
flash strings or libc call in that path. Final firmware linking must verify both
the wrapping and IRAM call closure, not just source attributes. LD wrapping does
not redirect same-translation-unit resolved calls; the targeted DIRECT calls are
in a separate TU, while internal same-TU uses, if added upstream, need auditing.

Validation already run:

1. Real P4 object compilation with current benchmark flags plus required include
   directories, `-Wall -Wextra -Werror`; no diagnostics. Object sections confirm
   `begin_token`, `record`, and `__wrap_esp_cache_msync` in IRAM and every counter
   state in DRAM (`p4-object-symbols.txt`, disassembly retained).
2. Host model compiles the actual wrapper implementation with fake harmless
   underlying APIs. It checks API preservation, exact logical copy payload,
   direction/error accounting, entry-stage preservation, background override,
   epoch rejection, nested inclusive timing and 4000 concurrent counter updates.
3. Native patcher dry run validates pinned anchors and rejects a second patch.

Run the host model:

```
gcc -std=c11 -O2 -Wall -Wextra -Werror -pthread -Itest-stubs -I../fixtures probe_model_test.c -o /tmp/ab-cost-model
/tmp/ab-cost-model
```
