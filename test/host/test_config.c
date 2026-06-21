// test_config.c — config defaults + clamp-on-load properties.
//
// Exercises the portable config logic host-side (NVS is shimmed to "not found"
// so load() falls back to defaults, then clamps). Asserts defaults are sane and
// that out-of-range overrides are pulled back into their documented bands.
#include "afterglow_config.h"
#include "ag_core/ag_eligible.h"
#include "classifier_logic.h"
#include "pool.h"
#include "test_util.h"
#include <string.h>

// Mirror of classifier_init()'s config→policy mapping (classifier.c is not
// host-buildable — it pulls ESP deps — so the wiring is reproduced here and
// asserted against). Keep in lockstep with classifier_init().
static ag_elig_policy_t policy_from_cfg(const afterglow_config_t *cfg)
{
    ag_elig_policy_t pol = ag_elig_defaults();
    pol.require_broadcast_only = cfg->require_broadcast_only;
    pol.wifi_beacons_enabled = cfg->wifi_beacons_enabled;
    return pol;
}

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

    // (6) the conservative eligibility keys default to the safe posture and
    //     survive a clamp unchanged (they are bools — always in range).
    afterglow_config_t e;
    afterglow_config_defaults(&e);
    CHECK_MSG(e.require_broadcast_only == true, "require_broadcast_only default");
    CHECK_MSG(e.require_beacon_payload == true, "require_beacon_payload default");
    afterglow_config_clamp(&e);
    CHECK(e.require_broadcast_only == true);
    CHECK(e.require_beacon_payload == true);

    // (7) the policy built from config honors a non-default eligibility posture:
    //     with the default config the broadcast-only gate refuses a connectable
    //     source; flipping require_broadcast_only off in config widens it. This
    //     mirrors classifier_init()'s config→ag_elig_policy_t composition.
    afterglow_config_t def;
    afterglow_config_defaults(&def);
    ag_elig_policy_t pol_def = policy_from_cfg(&def);
    CHECK_MSG(ag_replay_eligible(&pol_def, AG_ELIG_STATIC_RANDOM,
              AG_ADV_CONNECTABLE, /*is_wifi*/false, /*sightings_ok*/true,
              /*own*/false) == false,
              "default config admitted a connectable source");

    afterglow_config_t wide = def;
    wide.require_broadcast_only = false;
    ag_elig_policy_t pol_wide = policy_from_cfg(&wide);
    CHECK_MSG(ag_replay_eligible(&pol_wide, AG_ELIG_STATIC_RANDOM,
              AG_ADV_CONNECTABLE, false, true, false) == true,
              "non-default config did not widen the broadcast-only gate");

    // (8) require_beacon_payload gates the payload-class requirement in
    //     ag_classify_observe: a persistent static-random source WITHOUT a
    //     recognized beacon payload stays TENTATIVE under the default (true),
    //     but promotes (and becomes eligible) once the requirement is relaxed.
    {
        // AdvA with static-random subtype (top two bits of byte 5 = 0b11),
        // followed by a non-beacon AD field (0x09 = complete local name) so the
        // beacon-payload heuristic does NOT recognize it.
        uint8_t payload[12] = {0};
        payload[5] = 0xC0;                 // static-random subtype bits
        payload[6] = 0x03; payload[7] = 0x09; payload[8] = 'a'; payload[9] = 'b';
        ag_beacon_record_t rec = {0};
        rec.proto = AG_PROTO_BLE;
        rec.addr_type = 1;                 // random
        memcpy(rec.orig_addr, payload, 6);
        memcpy(rec.payload, payload, sizeof payload);
        rec.payload_len = 10;
        rec.adv_kind = AG_ADV_NONCONN_NONSCAN;
        rec.obs_count = 5;                 // well past min_sightings

        ag_beacon_record_t strict = rec;
        ag_classify_observe(&strict, 3, /*require_beacon_payload=*/true);
        CHECK_MSG(strict.cls == AG_CLASS_TENTATIVE,
                  "non-beacon source promoted under strict payload policy (cls=%u)",
                  strict.cls);
        CHECK_MSG((strict.flags & AG_FLAG_REPLAY_ELIGIBLE) == 0,
                  "non-beacon source eligible under strict payload policy");

        ag_beacon_record_t relaxed = rec;
        ag_classify_observe(&relaxed, 3, /*require_beacon_payload=*/false);
        CHECK_MSG(relaxed.cls == AG_CLASS_STATIC_RANDOM_BLE,
                  "relaxed payload policy did not promote (cls=%u)", relaxed.cls);
        CHECK_MSG((relaxed.flags & AG_FLAG_REPLAY_ELIGIBLE) != 0,
                  "relaxed payload policy did not mark eligible");
    }

    TEST_SUMMARY();
}
