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
FLAGS = $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -I. -I../src -I$(SRC) -Wall -Wextra -Wno-unused-parameter

# LVGL is copied and the patches in ../patches applied to the copy, so the
# tree given is never modified.
SRC := $(BUILD)/lvgl-src
PATCHES := $(wildcard ../patches/*.patch)
LV_SRC := $(patsubst $(LVGL)/%,$(SRC)/%,$(shell find $(LVGL)/src -name '*.c'))
LV_OBJ := $(patsubst $(SRC)/%.c,$(BUILD)/lvgl/%.o,$(LV_SRC))

all: $(BUILD)/ortho_test

$(SRC)/.patched: $(PATCHES)
	@rm -rf $(SRC) && mkdir -p $(SRC)
	@cp -r $(LVGL)/src $(LVGL)/lvgl.h $(LVGL)/lv_version.h $(LVGL)/lvgl_private.h $(SRC)/
	@for p in $(PATCHES); do patch -s -p1 -d $(SRC) < $$p || exit 1; done
	@touch $@

$(LV_SRC): $(SRC)/.patched

$(BUILD)/lvgl/%.o: $(SRC)/%.c lv_conf.h $(SRC)/.patched
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -I. -I$(SRC) -w -c $< -o $@

$(BUILD)/lvgl.a: $(LV_OBJ)
	@rm -f $@ && ar rcs $@ $^

$(BUILD)/ortho_test: $(SRC)/.patched main.c ../src/lv_draw_ortho.c ../src/lv_draw_ortho.h lv_conf.h $(BUILD)/lvgl.a
	$(CC) $(FLAGS) -Werror main.c ../src/lv_draw_ortho.c $(BUILD)/lvgl.a -lm -o $@

run: $(BUILD)/ortho_test
	cd $(BUILD) && ./ortho_test

bench: $(BUILD)/ortho_test
	cd $(BUILD) && ./ortho_test --bench

clean:
	rm -rf $(BUILD)

.PHONY: all run bench clean
