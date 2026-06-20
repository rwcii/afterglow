// esp_err.h — host shim for portable component logic under test.
// Provides just enough of the ESP-IDF error type for the parts of the
// firmware components that are pure logic (config clamp, pool, classifier,
// lifecycle). Hardware-touching code is not compiled host-side.
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK            0
#define ESP_FAIL         -1
#define ESP_ERR_NO_MEM    0x101
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_ARG 0x102
