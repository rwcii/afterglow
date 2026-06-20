// ag_eviction.c — capacity sigmoid + log-normal TTL + Weibull dropout.
#include "ag_core/ag_eviction.h"
#include <math.h>

ag_evict_params_t ag_evict_defaults(void)
{
    ag_evict_params_t p;
    p.soft_fill_frac   = 0.75f;
    p.ttl_min_s        = 180.0f;
    p.ttl_max_s        = 1500.0f;
    p.ttl_median_s     = 540.0f;
    p.weibull_k        = 1.5f;
    p.base_rate        = 0.015f;
    p.mod_weak_old     = 3.0f;
    p.mod_strong_fresh = 0.4f;
    p.jitter_frac      = 0.20f;
    return p;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float ag_evict_draw_base_ttl(const ag_evict_params_t *prm, ag_prng_t *rng)
{
    // Log-normal located at ttl_median_s. sigma chosen so the bulk spans the
    // [min,max] band; the draw is then clamped into the band. Uniform sampling
    // is not used — it produces sharp edges in the lifetime distribution.
    const float mu = logf(prm->ttl_median_s);
    // ~one sigma reaching toward the band edge from the median.
    const float sigma = 0.55f;
    float z = ag_prng_gauss(rng, 0.0f, 1.0f);
    float ttl = expf(mu + sigma * z);
    return clampf(ttl, prm->ttl_min_s, prm->ttl_max_s);
}

static float sigmoidf(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

float ag_evict_capacity_prob(const ag_evict_params_t *prm,
                             float fill_frac, float score_percentile)
{
    if (fill_frac <= prm->soft_fill_frac) {
        return 0.0f;
    }
    // excess in [0,1] past the knee, normalized by the headroom above soft-fill.
    float headroom = 1.0f - prm->soft_fill_frac;
    float excess = (headroom > 1e-6f) ? (fill_frac - prm->soft_fill_frac) / headroom : 1.0f;
    excess = clampf(excess, 0.0f, 1.0f);
    // sigmoid(4*excess) centered so the knee is gentle; weighted by how poor
    // the record's score is (best records ~spared).
    float s = sigmoidf(4.0f * excess) - 0.5f;   // 0 at knee → ~0.48 at full
    s *= 2.0f;                                   // rescale to ~[0,0.96]
    float p = s * (1.0f - clampf(score_percentile, 0.0f, 1.0f));
    return clampf(p, 0.0f, 1.0f);
}

float ag_evict_dropout_hazard(const ag_evict_params_t *prm, ag_prng_t *rng,
                              float norm_age, float score_percentile)
{
    if (norm_age < 0.0f) norm_age = 0.0f;
    // Weibull hazard h(t) = (k/lambda)*(t/lambda)^(k-1); with lambda≈1 in
    // normalized-age units this is k * norm_age^(k-1) — rising with age.
    float hazard_shape = prm->weibull_k * powf(norm_age + 1e-3f, prm->weibull_k - 1.0f);
    float base = prm->base_rate * hazard_shape;

    // Score/age modulation: weak (low percentile) and old records up to
    // mod_weak_old; strong/fresh floored toward mod_strong_fresh.
    float sp = clampf(score_percentile, 0.0f, 1.0f);
    float mod = prm->mod_strong_fresh
              + (prm->mod_weak_old - prm->mod_strong_fresh) * (1.0f - sp);
    base *= mod;

    // ±jitter_frac multiplicative jitter so the per-sweep rate is never a
    // clean constant.
    float j = 1.0f + ag_prng_uniform(rng, -prm->jitter_frac, prm->jitter_frac);
    base *= j;

    return clampf(base, 0.0f, 1.0f);
}

bool ag_evict_decide(const ag_evict_params_t *prm, ag_prng_t *rng,
                     float age_s, float base_ttl_s,
                     float fill_frac, float score_percentile)
{
    // 1. Capacity (soft, score-ranked).
    float pcap = ag_evict_capacity_prob(prm, fill_frac, score_percentile);
    if (pcap > 0.0f && ag_prng_unit(rng) < pcap) {
        return true;
    }
    // 2. Age: per-record log-normal TTL. Soft, not a hard cliff — once past the
    //    drawn TTL the dropout hazard (below) climbs steeply via norm_age.
    float norm_age = (base_ttl_s > 1e-3f) ? (age_s / base_ttl_s) : 999.0f;

    // 3. Stochastic dropout (Weibull-shaped age hazard).
    float pdrop = ag_evict_dropout_hazard(prm, rng, norm_age, score_percentile);
    if (ag_prng_unit(rng) < pdrop) {
        return true;
    }
    return false;
}
