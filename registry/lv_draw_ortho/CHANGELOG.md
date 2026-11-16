# Changelog

## 0.1.0

First release on the ESP Component Registry: the engine of
[ppa-alternative](https://github.com/youkorr/ppa-alternative-) (its ESPHome
component `lvgl_ortho`, and esp-iot-solution PR 818), packed on its own.

* Tiles drawn in the panel's orientation and turned by the PPA while the next
  ones are drawn, on one core or two (`lv_draw_ortho_set_cores`).
* Plain rectangles filled by the PPA.
* Big pictures turned straight into the frame buffer; those in PSRAM through
  two bands of internal RAM copied by the AXI GDMA, the bands being the first
  tile, lent while the picture is turned (`lv_draw_ortho_ppa_use_tiles`).
* Pictures that do not change turned once and kept (`lv_draw_ortho_set_image_cache`).
* LVGL's copies between frame buffers in DIRECT mode taken over
  (`lv_draw_ortho_take_sync`).
