#pragma once
#include <cstdio>
#define ESP_LOGE(tag, ...) (std::printf("  [E][%s] ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
#define ESP_LOGW(tag, ...) (std::printf("  [W][%s] ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
#define ESP_LOGI(tag, ...) (std::printf("  [I][%s] ", tag), std::printf(__VA_ARGS__), std::printf("\n"))
#define ESP_LOGD(tag, ...) ((void) 0)
#define ESP_LOGCONFIG(tag, ...) ((void) 0)
static inline const char *esp_err_to_name(int e) { return e == 0 ? "ESP_OK" : "error"; }
