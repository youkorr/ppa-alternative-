/* Stand-in for ESP-IDF v5.5.5 components/esp_rom/include/esp_rom_sys.h: the
 * one call this project uses. In the tests it is where time passes, so the
 * fake PPA finishes a queued transaction there (p4fake.c). */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void esp_rom_delay_us(uint32_t us);
#ifdef __cplusplus
}
#endif
