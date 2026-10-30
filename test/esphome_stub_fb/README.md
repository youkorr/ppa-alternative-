`esphome/components/mipi_dsi/mipi_dsi_fast_present.h` is copied unchanged
from youkorr/lvgl_9.5 (branch `claude/jolly-lamport-ERZOC`,
`components/mipi_dsi/`), the fork whose mipi_dsi publishes the panel's own
frame buffers. On the include path it switches lvgl_ortho to drawing into
them, exactly as it would on a board built with that fork.
