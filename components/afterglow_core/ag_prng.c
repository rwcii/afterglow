// ag_prng.c — xoshiro256** + splitmix64 seeding + Box-Muller normals.
#include "ag_core/ag_prng.h"
#include <math.h>

static inline uint64_t rotl(uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void ag_prng_seed(ag_prng_t *p, uint64_t seed)
{
    uint64_t sm = seed;
    for (int i = 0; i < 4; i++) {
        p->s[i] = splitmix64(&sm);
    }
    // Guard against the (probability-zero) all-zero state.
    if ((p->s[0] | p->s[1] | p->s[2] | p->s[3]) == 0) {
        p->s[0] = 0x9E3779B97F4A7C15ULL;
    }
    p->gauss_cache = 0.0;
    p->gauss_has_cache = 0;
}

uint64_t ag_prng_u64(ag_prng_t *p)
{
    const uint64_t result = rotl(p->s[1] * 5, 7) * 9;
    const uint64_t t = p->s[1] << 17;
    p->s[2] ^= p->s[0];
    p->s[3] ^= p->s[1];
    p->s[1] ^= p->s[2];
    p->s[0] ^= p->s[3];
    p->s[2] ^= t;
    p->s[3] = rotl(p->s[3], 45);
    return result;
}

uint32_t ag_prng_u32(ag_prng_t *p)
{
    return (uint32_t)(ag_prng_u64(p) >> 32);
}

double ag_prng_unit(ag_prng_t *p)
{
    // Top 53 bits → [0,1) with full double precision.
    return (double)(ag_prng_u64(p) >> 11) * (1.0 / 9007199254740992.0);
}

float ag_prng_uniform(ag_prng_t *p, float lo, float hi)
{
    return lo + (float)ag_prng_unit(p) * (hi - lo);
}

float ag_prng_gauss(ag_prng_t *p, float mu, float sigma)
{
    if (p->gauss_has_cache) {
        p->gauss_has_cache = 0;
        return mu + sigma * (float)p->gauss_cache;
    }
    // Box-Muller; reject the degenerate u1==0.
    double u1, u2;
    do {
        u1 = ag_prng_unit(p);
    } while (u1 <= 1e-12);
    u2 = ag_prng_unit(p);
    double r = sqrt(-2.0 * log(u1));
    double z0 = r * cos(2.0 * M_PI * u2);
    double z1 = r * sin(2.0 * M_PI * u2);
    p->gauss_cache = z1;
    p->gauss_has_cache = 1;
    return mu + sigma * (float)z0;
}
