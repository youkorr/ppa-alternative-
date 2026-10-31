# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
#
# Host tests against a real LVGL:
#   git clone --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl /tmp/lvgl
#   make LVGL=/tmp/lvgl run
LVGL  ?= $(error give LVGL=/path/to/lvgl, see the top of this file)
BUILD ?= build
CC    ?= cc
CFLAGS ?= -O2 -g
FLAGS = $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -I. -I../components/lvgl_ortho -I$(SRC) -Wall -Wextra -Wno-unused-parameter

# LVGL is copied and the patches in ../patches applied to the copy, so the
# tree given is never modified.
SRC := $(BUILD)/lvgl-src
# The patches for exactly the LVGL given: lvgl-<major>.<minor>.<patch>-*.patch
VERSION_H := $(firstword $(wildcard $(LVGL)/include/lvgl/lv_version.h) $(LVGL)/lv_version.h)
LV_VERSION := $(shell awk '/define LVGL_VERSION_(MAJOR|MINOR|PATCH) /{v=v (v?".":"") $$3} END{print v}' $(VERSION_H))
PATCHES := $(wildcard ../patches/lvgl-$(LV_VERSION)-*.patch)
LV_SRC := $(patsubst $(LVGL)/%,$(SRC)/%,$(shell find $(LVGL)/src -name '*.c'))
LV_OBJ := $(patsubst $(SRC)/%.c,$(BUILD)/lvgl/%.o,$(LV_SRC))

all: $(BUILD)/ortho_test $(BUILD)/esphome_test $(BUILD)/esphome_test_fb $(BUILD)/bench_test

$(SRC)/.patched: $(PATCHES)
	@rm -rf $(SRC) && mkdir -p $(SRC)
	@cp -r $(LVGL)/src $(LVGL)/lvgl.h $(SRC)/
	@for f in lv_version.h lvgl_private.h include; do if [ -e $(LVGL)/$$f ]; then cp -r $(LVGL)/$$f $(SRC)/; fi; done
	@echo "LVGL $(LV_VERSION), patches: $(notdir $(PATCHES))"
	@for p in $(PATCHES); do patch -s -p1 -d $(SRC) < $$p || exit 1; done
	@touch $@

$(LV_SRC): $(SRC)/.patched

$(BUILD)/lvgl/%.o: $(SRC)/%.c lv_conf.h $(SRC)/.patched
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -I. -I$(SRC) -w -c $< -o $@

$(BUILD)/lvgl.a: $(LV_OBJ)
	@rm -f $@ && ar rcs $@ $^

# p4stub/ stands in for ESP-IDF's PPA and cache, so the ESP32-P4 video
# producer is built and run here against a PPA that behaves like the board's.
VIDEO := ../components/lvgl_ortho/lv_draw_ortho_video.c ../components/lvgl_ortho/lv_draw_ortho_ppa.c p4stub/p4fake.c

$(BUILD)/ortho_test: $(SRC)/.patched main.c ../components/lvgl_ortho/lv_draw_ortho.c ../components/lvgl_ortho/lv_draw_ortho.h ../components/lvgl_ortho/lv_draw_ortho_video.h ../components/lvgl_ortho/lv_draw_ortho_ppa.h $(VIDEO) lv_conf.h $(BUILD)/lvgl.a
	$(CC) $(FLAGS) -Ip4stub -Werror main.c ../components/lvgl_ortho/lv_draw_ortho.c $(VIDEO) $(BUILD)/lvgl.a -lm -o $@

run: $(BUILD)/ortho_test $(BUILD)/esphome_test $(BUILD)/esphome_test_fb $(BUILD)/bench_test
	cd $(BUILD) && ./ortho_test
	cd $(BUILD) && ./bench_test
	cd $(BUILD) && for r in 90 180 270 0; do ./esphome_test $$r || exit 1; done
	cd $(BUILD) && ./esphome_test 270 720 1280 && ./esphome_test 90 800 1280
	cd $(BUILD) && for r in 90 180 270 0; do ./esphome_test_fb $$r || exit 1; done
	cd $(BUILD) && ./esphome_test_fb 270 720 1280 3 && ./esphome_test_fb 90 800 1280 2

# The ESPHome component itself (../components/lvgl_ortho/lvgl_ortho.cpp, the
# shipped file) against stand-ins for ESPHome's Component and Display.
CXX ?= c++
ORTHO := ../components/lvgl_ortho
$(BUILD)/esphome_test: $(SRC)/.patched esphome_test.cpp $(ORTHO)/lvgl_ortho.cpp $(ORTHO)/lvgl_ortho.h $(ORTHO)/lv_draw_ortho.c $(VIDEO) $(wildcard esphome_stub/*.h esphome_stub/*/*/*.h esphome_stub/*/*/*/*.h) lv_conf.h $(BUILD)/lvgl.a
	@mkdir -p $(BUILD)/esphome
	$(CC) $(FLAGS) -Ip4stub -Werror -c $(ORTHO)/lv_draw_ortho.c -o $(BUILD)/esphome/lv_draw_ortho.o
	$(CC) $(FLAGS) -Ip4stub -Werror -c $(ORTHO)/lv_draw_ortho_video.c -o $(BUILD)/esphome/lv_draw_ortho_video.o
	$(CC) $(FLAGS) -Ip4stub -Werror -c $(ORTHO)/lv_draw_ortho_ppa.c -o $(BUILD)/esphome/lv_draw_ortho_ppa.o
	$(CC) $(FLAGS) -Ip4stub -Werror -c p4stub/p4fake.c -o $(BUILD)/esphome/p4fake.o
	$(CXX) $(CFLAGS) -std=c++17 -DLV_CONF_INCLUDE_SIMPLE -I. -I$(ORTHO) -I$(SRC) -Iesphome_stub -Ip4stub -Wall -Wextra -Werror \
		esphome_test.cpp $(ORTHO)/lvgl_ortho.cpp $(BUILD)/esphome/*.o $(BUILD)/lvgl.a -lm -o $@

# The same test with youkorr/lvgl_9.5's mipi_dsi header on the include path:
# LVGL draws into the panel's own frame buffers and nothing is copied.
$(BUILD)/esphome_test_fb: $(BUILD)/esphome_test esphome_stub_fb/esphome/components/mipi_dsi/mipi_dsi_fast_present.h
	$(CXX) $(CFLAGS) -std=c++17 -DLV_CONF_INCLUDE_SIMPLE -I. -I$(ORTHO) -I$(SRC) -Iesphome_stub -Iesphome_stub_fb -Ip4stub -Wall -Wextra -Werror \
		esphome_test.cpp $(ORTHO)/lvgl_ortho.cpp $(BUILD)/esphome/*.o $(BUILD)/lvgl.a -lm -o $@

# lvgl_bench, the board-side measure, against stand-ins for ESPHome.
BENCH := ../components/lvgl_bench
$(BUILD)/bench_test: $(SRC)/.patched bench_test.cpp $(BENCH)/lvgl_bench.cpp $(BENCH)/lvgl_bench.h $(wildcard bench_stub/*/*/*.h bench_stub/*/*/*/*.h) lv_conf.h $(BUILD)/lvgl.a
	$(CXX) $(CFLAGS) -std=c++17 -DLV_CONF_INCLUDE_SIMPLE -I. -I$(BENCH) -I$(SRC) -Iesphome_stub -Ibench_stub -Wall -Wextra -Werror \
		bench_test.cpp $(BENCH)/lvgl_bench.cpp $(BUILD)/lvgl.a -lm -lpthread -o $@

bench: $(BUILD)/ortho_test
	cd $(BUILD) && ./ortho_test --bench

clean:
	rm -rf $(BUILD)

.PHONY: all run bench clean
