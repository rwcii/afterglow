// detector.c — two-sample statistical metrics.
#include "detector.h"
#include <math.h>
#include <stdlib.h>

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

double det_ks_statistic(const double *a, size_t na, const double *b, size_t nb)
{
    if (na == 0 || nb == 0) return 1.0;
    double *sa = malloc(na * sizeof(double));
    double *sb = malloc(nb * sizeof(double));
    for (size_t i = 0; i < na; i++) sa[i] = a[i];
    for (size_t i = 0; i < nb; i++) sb[i] = b[i];
    qsort(sa, na, sizeof(double), cmp_double);
    qsort(sb, nb, sizeof(double), cmp_double);

    // Walk the merged order, tracking the two empirical CDFs.
    size_t i = 0, j = 0;
    double d = 0.0;
    while (i < na && j < nb) {
        double x;
        if (sa[i] <= sb[j]) x = sa[i]; else x = sb[j];
        while (i < na && sa[i] <= x) i++;
        while (j < nb && sb[j] <= x) j++;
        double fa = (double)i / (double)na;
        double fb = (double)j / (double)nb;
        double gap = fabs(fa - fb);
        if (gap > d) d = gap;
    }
    free(sa);
    free(sb);
    return d;
}

double det_ks_critical_05(size_t na, size_t nb)
{
    // c(0.05) * sqrt((na+nb)/(na*nb)), c(0.05) ≈ 1.358.
    double n = (double)na, m = (double)nb;
    return 1.358 * sqrt((n + m) / (n * m));
}

static double mean_of(const double *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += x[i];
    return n ? s / n : 0.0;
}

static double stddev_of(const double *x, size_t n)
{
    if (n < 2) return 0.0;
    double m = mean_of(x, n), s = 0;
    for (size_t i = 0; i < n; i++) s += (x[i] - m) * (x[i] - m);
    return sqrt(s / n);
}

double det_interval_regularity_cv(const double *gaps, size_t n)
{
    double m = mean_of(gaps, n);
    if (m <= 1e-9) return 0.0;
    return stddev_of(gaps, n) / m;
}

double det_rssi_stability(const double *rssi, size_t n)
{
    return stddev_of(rssi, n);
}

double det_two_means_separation(const double *x, size_t n)
{
    if (n < 2) return 0.0;
    // init centroids at min/max
    double lo = x[0], hi = x[0];
    for (size_t i = 1; i < n; i++) {
        if (x[i] < lo) lo = x[i];
        if (x[i] > hi) hi = x[i];
    }
    double c0 = lo, c1 = hi;
    for (int iter = 0; iter < 20; iter++) {
        double s0 = 0, s1 = 0;
        size_t n0 = 0, n1 = 0;
        for (size_t i = 0; i < n; i++) {
            if (fabs(x[i] - c0) <= fabs(x[i] - c1)) { s0 += x[i]; n0++; }
            else { s1 += x[i]; n1++; }
        }
        if (n0) c0 = s0 / n0;
        if (n1) c1 = s1 / n1;
    }
    double spread = stddev_of(x, n);
    if (spread <= 1e-9) return 0.0;
    return fabs(c0 - c1) / spread;
}
