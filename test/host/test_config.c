// test_config.c — config defaults + clamp-on-load properties.
//
// Exercises the portable config logic host-side (NVS is shimmed to "not found"
// so load() falls back to defaults, then clamps). Asserts defaults are sane and
// that out-of-range overrides are pulled back into their documented bands.
#include "afterglow_config.h"
#include "test_util.h"

int main(void)
{
    TEST_BEGIN("config");

    // (1) defaults pass through clamp unchanged (defaults are in-range).
    afterglow_config_t d;
    afterglow_config_defaults(&d);
    afterglow_config_t c = d;
    afterglow_config_clamp(&c);
    CHECK(c.store_cap == d.store_cap);
    CHECK(c.max_concurrent_ghosts == d.max_concurrent_ghosts);
    CHECK_NEAR(c.w_cls, d.w_cls, 1e-6);
    CHECK(c.interval_jitter_pct == d.interval_jitter_pct);

    // (2) load() returns defaults+clamp when NVS is absent (shim).
    afterglow_config_t l;
    CHECK(afterglow_config_load(&l) == ESP_OK);
    CHECK(l.store_cap == d.store_cap);

    // (3) over-range values are clamped down.
    afterglow_config_t hi;
    afterglow_config_defaults(&hi);
    hi.store_cap = 9000;
    hi.max_concurrent_ghosts = 200;
    hi.interval_jitter_pct = 90;
    hi.dropout_base_rate = 5.0f;
    hi.w_cls = 2.0f;
    afterglow_config_clamp(&hi);
    CHECK_MSG(hi.store_cap == 4096, "store_cap not clamped: %u", hi.store_cap);
    CHECK_MSG(hi.max_concurrent_ghosts == 16, "ghosts not clamped: %u",
              hi.max_concurrent_ghosts);
    CHECK(hi.interval_jitter_pct == 25);
    CHECK(hi.dropout_base_rate <= 0.10f + 1e-6f);
    CHECK(hi.w_cls <= 0.45f + 1e-6f);

    // (4) under-range values are clamped up. Critically, interval_jitter_pct
    //     must NEVER be 0 (that would pin adv_int_min==adv_int_max).
    afterglow_config_t lo;
    afterglow_config_defaults(&lo);
    lo.store_cap = 10;
    lo.max_concurrent_ghosts = 0;
    lo.interval_jitter_pct = 0;
    lo.mesh_ttl_init = 0;
    afterglow_config_clamp(&lo);
    CHECK_MSG(lo.store_cap == 512, "store_cap floor: %u", lo.store_cap);
    CHECK(lo.max_concurrent_ghosts == 4);
    CHECK_MSG(lo.interval_jitter_pct >= 1, "jitter pinned to 0 — forbidden");
    CHECK(lo.mesh_ttl_init >= 2);

    // (5) interdependent bounds stay coherent: lo<=hi for paired fields.
    afterglow_config_t p;
    afterglow_config_defaults(&p);
    p.ttl_lognorm_lo_min = 40.0f;   // above hi
    p.ttl_lognorm_hi_min = 5.0f;    // below lo
    afterglow_config_clamp(&p);
    CHECK_MSG(p.ttl_lognorm_lo_min <= p.ttl_lognorm_hi_min,
              "ttl band inverted: lo=%.1f hi=%.1f",
              p.ttl_lognorm_lo_min, p.ttl_lognorm_hi_min);
    CHECK(p.ble_min_lvl <= p.ble_max_lvl);
    CHECK(p.wifi_min_dbm <= p.wifi_max_dbm);

    TEST_SUMMARY();
}
