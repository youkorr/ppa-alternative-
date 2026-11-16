idf_component_register(
    SRCS "main.c" "example_lcd.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_timer
)
