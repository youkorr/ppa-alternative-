// Stand-in for the esphome/core/version.h ESPHome writes into a build. The
// tests build lvgl_ortho twice: as for ESPHome 2026.10 (the default) and,
// with -DSTUB_ESPHOME_2026_11, as for 2026.11 and later, whose display API
// changed (see display.h).
#pragma once
#include "esphome/core/macros.h"
#ifdef STUB_ESPHOME_2026_11
#define ESPHOME_VERSION_CODE VERSION_CODE(2026, 11, 0)
#else
#define ESPHOME_VERSION_CODE VERSION_CODE(2026, 10, 0)
#endif
