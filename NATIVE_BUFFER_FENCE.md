# Experimental native DSI per-buffer fence

This branch is independent of ESPHome PR #23332. It uses the corresponding
`youkorr/esphome@test-native-dsi-buffer-fence` driver instead of the PR source.
Keep the camera and LVGL sources, RGB565, three panel buffers and `native_dsi: true`.
No panel timing changes. No changes to the official PR.

The driver retires a replaced selection for two DMA-complete generations, but
only waits when LVGL actually needs to write to that retired buffer. The ISR
counter is atomic and lives in internal RAM; a semaphore is just a wake hint.
The lease covers rendering and the deferred sync copies to the active buffer.
Unexpected sync-hook changes, unsafe writes, missing leases and failed
submissions stop execution rather than permit a potentially unsafe write.
Accepted PPA/GDMA operations must complete before fallback/reuse in native
mode. The out-of-refresh show_video() path is deliberately disabled in this
experiment; use lvgl_camera_display with a canvas.

On the board compare the new `native DSI acquire` and `native DSI present`
statistics with 24.1 screens/s and 6.55 ms/frame on PR #23332. Check zero
panel rows copied, tearing, full-screen camera and partial UI updates.
No FPS or hardware correctness claim until the board has been tested.
