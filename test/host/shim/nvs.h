// nvs.h — host shim. NVS calls return "not found" so config_load falls back to
// defaults; the clamp path (the portable logic under test) runs unchanged.
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef int nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h)
{ (void)ns; (void)m; (void)h; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v)
{ (void)h; (void)k; (void)v; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l)
{ (void)h; (void)k; (void)o; (void)l; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v)
{ (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *o, size_t l)
{ (void)h; (void)k; (void)o; (void)l; return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
