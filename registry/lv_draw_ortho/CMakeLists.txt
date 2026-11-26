# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-

# The engine's sources: next to this file in the packed component (src/), and
# in the ppa-alternative repository the ESPHome component's own -- one copy,
# which tools/pack_lv_draw_ortho.py puts in src/ when it packs.
set(src_dir "${CMAKE_CURRENT_LIST_DIR}/src")
if(NOT EXISTS "${src_dir}/lv_draw_ortho.c")
    set(src_dir "${CMAKE_CURRENT_LIST_DIR}/../../components/lvgl_ortho")
endif()

idf_build_get_property(build_components BUILD_COMPONENTS)
# LVGL from the registry is lvgl__lvgl; a project may carry its own as lvgl.
if("lvgl" IN_LIST build_components)
    set(lvgl_name lvgl)
else()
    set(lvgl_name lvgl__lvgl)
endif()
set(priv_requires esp_timer esp_mm esp_hw_support esp_rom heap)
# esp_async_memcpy (the GDMA copy of the camera bands) moved out of
# esp_hw_support in ESP-IDF 6.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    list(APPEND priv_requires esp_driver_dma)
endif()

idf_component_register(
    SRCS "${src_dir}/lv_draw_ortho.c" "${src_dir}/lv_draw_ortho_ppa.c" "${src_dir}/lv_draw_ortho_video.c"
    INCLUDE_DIRS "${src_dir}"
    REQUIRES ${lvgl_name} esp_driver_ppa
    PRIV_REQUIRES ${priv_requires}
)
