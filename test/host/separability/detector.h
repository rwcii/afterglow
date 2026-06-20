// detector.h — two-sample statistical metrics (portable).
//
// Standard distribution-comparison tools (KS, clustering, interval regularity,
// signal-level stability). Each function returns a scalar metric; the test
// suite uses them to compare two sample streams and assert the metric stays
// within a configured bound.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Two-sample Kolmogorov-Smirnov statistic D in [0,1]: the maximum gap between
// the two empirical CDFs. D near 0 means the samples look like the same
// distribution; D near 1 means they differ. Inputs need not be sorted.
double det_ks_statistic(const double *a, size_t na, const double *b, size_t nb);

// Approximate KS critical value at alpha=0.05 for sample sizes na, nb. An
// observed D above this rejects the equal-distribution hypothesis at 95%.
double det_ks_critical_05(size_t na, size_t nb);

// Coefficient of variation (stddev/mean) of a sequence of inter-emission gaps.
// A perfectly regular sequence has CV→0; jittered intervals raise it.
double det_interval_regularity_cv(const double *gaps, size_t n);

// Standard deviation of an RSSI series (dB). A constant-power series has
// stddev→0; a varying one is larger.
double det_rssi_stability(const double *rssi, size_t n);

// 1-D two-means separation score: cluster `x` into 2 groups (Lloyd's, fixed
// iters) and return the silhouette-like separation = |c0-c1| / spread. A high
// value means a single feature splits the data into two well-separated groups;
// a low value means it does not.
double det_two_means_separation(const double *x, size_t n);

#ifdef __cplusplus
}
#endif
