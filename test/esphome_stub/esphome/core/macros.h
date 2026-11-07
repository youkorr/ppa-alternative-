// Stand-in for ESPHome's esphome/core/macros.h: the version macro.
#pragma once
#define VERSION_CODE(major, minor, patch) ((major) << 16 | (minor) << 8 | (patch))
