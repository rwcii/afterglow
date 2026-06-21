// classifier.c — class inference + replay-eligibility gate (hardware wrapper).
// The inference + gate math is the portable, host-tested classifier_logic; this
// wrapper supplies config (min_sightings, eligibility policy).
#include "classifier.h"
#include "classifier_logic.h"
#include "afterglow_config.h"
#include "ag_core/ag_eligible.h" // portable eligibility predicate (host-tested)
#include "esp_log.h"

static const char *TAG = "classifier";

static uint8_t s_min_sightings = 3;
static bool s_require_beacon_payload = true;
static ag_elig_policy_t s_pol;

esp_err_t classifier_init(void)
{
    afterglow_config_t cfg;
    afterglow_config_load(&cfg);
    s_min_sightings = cfg.replay_min_sightings;
    s_require_beacon_payload = cfg.require_beacon_payload;
    // Build the eligibility policy from loaded (clamped) config rather than the
    // hardcoded defaults, so the broadcast-only / Wi-Fi gates are configurable.
    // ag_elig_defaults() seeds the conservative posture; config overrides it.
    s_pol = ag_elig_defaults();
    s_pol.require_broadcast_only = cfg.require_broadcast_only;
    s_pol.wifi_beacons_enabled = cfg.wifi_beacons_enabled;
    ESP_LOGI(TAG, "classifier init (min_sightings=%u, broadcast_only=%d, "
             "beacon_payload=%d, wifi_beacons=%d)",
             s_min_sightings, s_pol.require_broadcast_only,
             s_require_beacon_payload, s_pol.wifi_beacons_enabled);
    return ESP_OK;
}

void classifier_observe(ag_beacon_record_t *rec)
{
    ag_classify_observe(rec, s_min_sightings, s_require_beacon_payload);
}

ag_beacon_class_t classifier_class_of(const ag_beacon_record_t *rec)
{
    return rec ? (ag_beacon_class_t)rec->cls : AG_CLASS_TENTATIVE;
}

bool classifier_replay_eligible(const ag_beacon_record_t *rec)
{
    if (!rec) return false;
    // The record carries its inferred class + adv_kind + the eligibility flag
    // (set by ag_classify_observe once stable). Compose with the policy gate.
    bool sightings_ok = (rec->flags & AG_FLAG_REPLAY_ELIGIBLE) != 0;
    bool is_wifi = (rec->cls == AG_CLASS_WIFI);
    // The source is considered present until lifecycle marks it departed (silent
    // past its cadence-scaled gap); the departed flag is cleared the instant the
    // source is observed again, so a record is re-emitted only while its source
    // is absent.
    bool source_present = (rec->flags & AG_FLAG_DEPARTING) == 0;
    return ag_replay_eligible(&s_pol, ag_classify_elig_class(rec->cls),
                              (ag_adv_kind_t)rec->adv_kind, is_wifi,
                              sightings_ok, /*is_own_device=*/false,
                              source_present);
}
