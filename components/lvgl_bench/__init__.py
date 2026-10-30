# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""How long LVGL takes to redraw the screen, measured on the board.

    lvgl_bench:
      ms_per_frame:
        name: "LVGL ms per frame"
      max_fps:
        name: "LVGL max fps"

    button:
      - platform: template
        name: "LVGL bench"
        on_press:
          - lvgl_bench.run: 10s

Works with ESPHome's own lvgl (turned in the flush, as esp_lvgl_port does)
and with lvgl_ortho alike, so the same lines in two firmwares compare them.
A frame is timed from LVGL's RENDER_START to its REFR_READY: the drawing,
the turn and the flush to the panel. `lvgl_bench.run` redraws the whole
active screen on every refresh for that long; without it, whatever the UI
redraws by itself is measured.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_DURATION, CONF_ID, UNIT_MILLISECOND

DEPENDENCIES = ["lvgl"]
AUTO_LOAD = ["sensor"]
CODEOWNERS = ["@youkorr"]

CONF_MS_PER_FRAME = "ms_per_frame"
CONF_MAX_FPS = "max_fps"
CONF_REPORT_INTERVAL = "report_interval"

lvgl_bench_ns = cg.esphome_ns.namespace("lvgl_bench")
LvglBench = lvgl_bench_ns.class_("LvglBench", cg.Component)
RunAction = lvgl_bench_ns.class_("RunAction", automation.Action)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LvglBench),
        cv.Optional(CONF_REPORT_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MS_PER_FRAME): sensor.sensor_schema(
            unit_of_measurement=UNIT_MILLISECOND, accuracy_decimals=1, icon="mdi:timer-outline"
        ),
        cv.Optional(CONF_MAX_FPS): sensor.sensor_schema(
            unit_of_measurement="fps", accuracy_decimals=1, icon="mdi:speedometer"
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_report_interval(config[CONF_REPORT_INTERVAL]))
    if CONF_MS_PER_FRAME in config:
        cg.add(var.set_ms_sensor(await sensor.new_sensor(config[CONF_MS_PER_FRAME])))
    if CONF_MAX_FPS in config:
        cg.add(var.set_fps_sensor(await sensor.new_sensor(config[CONF_MAX_FPS])))


@automation.register_action(
    "lvgl_bench.run",
    RunAction,
    cv.maybe_simple_value(
        {
            cv.GenerateID(): cv.use_id(LvglBench),
            cv.Optional(CONF_DURATION, default="10s"): cv.templatable(
                cv.positive_time_period_milliseconds
            ),
        },
        key=CONF_DURATION,
    ),
    synchronous=True,
)
async def run_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_duration(await cg.templatable(config[CONF_DURATION], args, cg.uint32)))
    return var
