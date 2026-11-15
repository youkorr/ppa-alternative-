/* Stand-in for ESP-IDF's esp_memory_utils.h on a PC: which buffers stand for
 * PSRAM is the test's to say (p4fake_external). */
#pragma once
#include <stdbool.h>
bool esp_ptr_external_ram(const void * p);
