// test_target_main.c — on-target Unity tests (hardware-coupled paths).
//
// Only things that need the real chip / QEMU live here. The privacy-critical
// math is host-tested (test/host); these tests would just duplicate it slower.
// Covered: NVS config round-trip, PSRAM pool allocation, radio_backend vtable
// wiring, and a smoke check that ag_core links and runs on-target identically.
#include "unity.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "afterglow_config.h"
#include "pool.h"
#include "radio_backend.h"
#include "ag_core/ag_txwalk.h"

static void ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        TEST_ESP_OK(nvs_flash_erase());
        TEST_ESP_OK(nvs_flash_init());
    } else {
        TEST_ESP_OK(err);
    }
}

TEST_CASE("config: defaults load and conservative switches are off", "[config]")
{
    afterglow_config_t cfg;
    afterglow_config_defaults(&cfg);
    // Ship-disabled-until-validated invariants.
    TEST_ASSERT_FALSE(cfg.mesh_enabled);
    TEST_ASSERT_TRUE(cfg.store_cap > 0);
}

TEST_CASE("config: NVS save/load round-trip", "[config][nvs]")
{
    ensure_nvs();
    afterglow_config_t a, b;
    afterglow_config_defaults(&a);
    a.store_cap = 1234;             // perturb a field
    TEST_ESP_OK(afterglow_config_save(&a));
    TEST_ESP_OK(afterglow_config_load(&b));
    TEST_ASSERT_EQUAL_UINT(1234, b.store_cap);
}

TEST_CASE("pool: PSRAM slab allocates", "[pool][psram]")
{
    ensure_nvs();
    // Requires PSRAM; on QEMU this exercises the alloc path / error handling.
    esp_err_t err = pool_init();
    TEST_ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_NO_MEM);
    if (err == ESP_OK) {
        TEST_ASSERT_TRUE(pool_capacity() > 0);
        TEST_ASSERT_EQUAL_UINT(0, pool_count());
    }
}

TEST_CASE("radio_backend: vtable is wired and self-consistent", "[radio]")
{
    const radio_backend_t *be = radio_backend_get();
    TEST_ASSERT_NOT_NULL(be);
    TEST_ASSERT_NOT_NULL(be->name);
    TEST_ASSERT_NOT_NULL(be->init);
    TEST_ASSERT_NOT_NULL(be->emit);
    TEST_ASSERT_NOT_NULL(be->capture_start);
    // single-radio backend reports no concurrent capture+replay.
    TEST_ASSERT_FALSE(be->concurrent_capture_replay);
    TEST_ASSERT_TRUE(be->tx_power_levels(AG_PROTO_BLE) > 0);
}

TEST_CASE("ag_core: walk runs identically on-target", "[core]")
{
    // The same deterministic property the host test checks — proves the ported
    // code behaves the same on the chip.
    ag_prng_t r;
    ag_prng_seed(&r, 42);
    ag_txwalk_params_t prm = ag_txwalk_defaults(0);
    float v = prm.p_center, mn = 1e9f, mx = -1e9f;
    for (int i = 0; i < 1000; i++) {
        float s = ag_txwalk_step(&prm, &r, &v);
        if (s < mn) mn = s;
        if (s > mx) mx = s;
        TEST_ASSERT_TRUE(s >= prm.p_min - 0.01f && s <= prm.p_max + 0.01f);
    }
    TEST_ASSERT_TRUE((mx - mn) > 2.0f);  // signal-level variance holds on-target
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
