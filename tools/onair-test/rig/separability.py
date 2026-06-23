"""On-air separability evaluator: real-vs-ghost distribution-closeness metrics.

A pure-Python re-implementation of the five statistical primitives in
``test/host/separability/detector.c``, plus a parser for the observer capture
stream and a scorer that compares a real source's radiated signal against the
device-emitted clone (ghost). The Python port is locked to the C reference by a
host parity test (``test_separability_parity.py``) so the two stay identical.

The metrics measure how close two sample streams look along several feature
axes — RSSI distribution (KS), signal-level variance, inter-emission cadence
regularity, and 1-D cluster separation. ``assert_indistinguishable`` mirrors the
four checks in ``test_separability.c``: a passing result means the ghost's
radiated signal is not separable from the real source along these axes.
"""
import math
import os
import re


# Ground-truth line regexes shared by the harnesses, kept together.
# Observer ADV line:
#   ADV <rnd|pub> <AdvA> rssi=<dBm> len=<n> data=<hex> [txp=<dBm>]
ADV_RE = re.compile(
    r"ADV (?:rnd|pub) ([0-9a-f:]+) rssi=(-?\d+) len=\d+ data=\S+(?: txp=(-?\d+))?"
)
# DUT per-frame emit line:
#   ONAIR emit addr=<aa:bb:..> pwr_idx=<n> adlen=<n>
EMIT_RE = re.compile(r"ONAIR emit addr=([0-9a-f:]+) pwr_idx=(-?\d+)")


# --- detector primitives (port of detector.c) -------------------------------

def ks_statistic(a, b):
    """Two-sample Kolmogorov-Smirnov statistic D in [0,1]: the maximum gap
    between the two empirical CDFs. Mirrors det_ks_statistic."""
    na, nb = len(a), len(b)
    if na == 0 or nb == 0:
        return 1.0
    sa = sorted(a)
    sb = sorted(b)
    i = j = 0
    d = 0.0
    while i < na and j < nb:
        x = sa[i] if sa[i] <= sb[j] else sb[j]
        while i < na and sa[i] <= x:
            i += 1
        while j < nb and sb[j] <= x:
            j += 1
        fa = i / na
        fb = j / nb
        gap = abs(fa - fb)
        if gap > d:
            d = gap
    return d


def ks_critical_05(na, nb):
    """Approximate KS critical value at alpha=0.05. Mirrors det_ks_critical_05:
    c(0.05) * sqrt((na+nb)/(na*nb)), c(0.05) ~= 1.358."""
    n = float(na)
    m = float(nb)
    return 1.358 * math.sqrt((n + m) / (n * m))


def _mean_of(x):
    n = len(x)
    return (sum(x) / n) if n else 0.0


def _stddev_of(x):
    n = len(x)
    if n < 2:
        return 0.0
    m = _mean_of(x)
    s = sum((v - m) * (v - m) for v in x)
    return math.sqrt(s / n)


def interval_regularity_cv(gaps):
    """Coefficient of variation (stddev/mean) of inter-emission gaps. Mirrors
    det_interval_regularity_cv."""
    m = _mean_of(gaps)
    if m <= 1e-9:
        return 0.0
    return _stddev_of(gaps) / m


def rssi_stability(rssi):
    """Standard deviation of an RSSI series (dB). Mirrors det_rssi_stability."""
    return _stddev_of(rssi)


def two_means_separation(x):
    """1-D two-means separation = |c0-c1| / spread after Lloyd's iterations with
    centroids initialised at min/max. Mirrors det_two_means_separation."""
    n = len(x)
    if n < 2:
        return 0.0
    lo = min(x)
    hi = max(x)
    c0, c1 = lo, hi
    for _ in range(20):
        s0 = s1 = 0.0
        n0 = n1 = 0
        for v in x:
            if abs(v - c0) <= abs(v - c1):
                s0 += v
                n0 += 1
            else:
                s1 += v
                n1 += 1
        if n0:
            c0 = s0 / n0
        if n1:
            c1 = s1 / n1
    spread = _stddev_of(x)
    if spread <= 1e-9:
        return 0.0
    return abs(c0 - c1) / spread


# --- capture-stream parsing -------------------------------------------------

def parse_capture(lines):
    """Parse observer ADV lines into {AdvA: {"rssi": [...], "txp": [...]}}.

    ``lines`` is any iterable of strings (a captured stream). RSSI is collected
    per source address; txp is collected when the 0x0A field is present.
    """
    by_addr = {}
    for ln in lines:
        m = ADV_RE.search(ln)
        if not m:
            continue
        addr = m.group(1)
        rssi = int(m.group(2))
        rec = by_addr.setdefault(addr, {"rssi": [], "txp": []})
        rec["rssi"].append(rssi)
        if m.group(3) is not None:
            rec["txp"].append(int(m.group(3)))
    return by_addr


# --- scoring + assertion (port of test_separability.c) ----------------------

def _env_float(name, default, env):
    val = env.get(name)
    if val is None or val == "":
        return default
    return float(val)


class Bounds:
    """Tunable bounds for ``assert_indistinguishable``; defaults match the
    constants in test_separability.c. Each is overridable from the environment."""

    def __init__(self, env=None):
        env = env if env is not None else os.environ
        # (1) KS: D < Dcrit * KS_CRIT_MULT
        self.ks_crit_mult = _env_float("AG_SEP_KS_CRIT_MULT", 2.0, env)
        # (2) signal-level variance band
        self.ghost_sd_floor = _env_float("AG_SEP_GHOST_SD_FLOOR", 2.0, env)
        self.sd_ratio_lo = _env_float("AG_SEP_SD_RATIO_LO", 0.4, env)
        self.sd_ratio_hi = _env_float("AG_SEP_SD_RATIO_HI", 2.5, env)
        # (3) interval-regularity CV
        self.cv_floor = _env_float("AG_SEP_CV_FLOOR", 0.005, env)
        self.cv_diff_bound = _env_float("AG_SEP_CV_DIFF_BOUND", 0.02, env)
        # (4) pooled cluster separation
        self.sep_bound = _env_float("AG_SEP_POOLED_BOUND", 2.5, env)


def score_real_vs_ghost(real_rssi, ghost_rssi, real_gaps=None, ghost_gaps=None):
    """Compute the separability metrics comparing a real source's radiated signal
    against the device-emitted clone (ghost).

    ``*_rssi`` are RSSI series; ``*_gaps`` are inter-emission gap series (ms). If
    gaps are omitted, the CV fields are None and the CV checks are skipped by
    ``assert_indistinguishable``. Returns a dict of metric scalars.
    """
    n = min(len(real_rssi), len(ghost_rssi))
    ks_d = ks_statistic(real_rssi, ghost_rssi)
    ks_crit = ks_critical_05(len(real_rssi), len(ghost_rssi))
    real_sd = rssi_stability(real_rssi)
    ghost_sd = rssi_stability(ghost_rssi)
    pooled = list(real_rssi) + list(ghost_rssi)
    sep = two_means_separation(pooled)
    cv_real = interval_regularity_cv(real_gaps) if real_gaps else None
    cv_ghost = interval_regularity_cv(ghost_gaps) if ghost_gaps else None
    return {
        "n": n,
        "ks_d": ks_d,
        "ks_crit": ks_crit,
        "real_sd": real_sd,
        "ghost_sd": ghost_sd,
        "pooled_separation": sep,
        "cv_real": cv_real,
        "cv_ghost": cv_ghost,
    }


def assert_indistinguishable(score, bounds=None):
    """Assert the four separability checks from test_separability.c against a
    score dict. Raises AssertionError naming the first failing axis; returns the
    list of human-readable check results on success.

    The CV check (axis 3) is applied only when both cv_real and cv_ghost are
    present in the score; otherwise it is reported as skipped.
    """
    b = bounds if bounds is not None else Bounds()
    results = []

    # (1) RSSI distribution: KS D below the (scaled) 95% critical value.
    ks_ok = score["ks_d"] < score["ks_crit"] * b.ks_crit_mult
    results.append(
        f"KS D={score['ks_d']:.3f} < {score['ks_crit'] * b.ks_crit_mult:.3f}: {ks_ok}"
    )
    if not ks_ok:
        raise AssertionError(
            f"RSSI distribution diverges: KS D={score['ks_d']:.3f} vs "
            f"~{score['ks_crit']:.3f} crit (x{b.ks_crit_mult})"
        )

    # (2) signal-level variance: ghost varies, and stays in band vs real.
    ghost_sd = score["ghost_sd"]
    real_sd = score["real_sd"]
    sd_floor_ok = ghost_sd > b.ghost_sd_floor
    sd_band_ok = (ghost_sd > real_sd * b.sd_ratio_lo
                  and ghost_sd < real_sd * b.sd_ratio_hi)
    results.append(f"ghost_sd={ghost_sd:.2f} > {b.ghost_sd_floor}: {sd_floor_ok}")
    results.append(
        f"{b.sd_ratio_lo}*real_sd < ghost_sd < {b.sd_ratio_hi}*real_sd "
        f"({real_sd:.2f}): {sd_band_ok}"
    )
    if not sd_floor_ok:
        raise AssertionError(
            f"ghost RSSI stddev {ghost_sd:.2f} dB is near-constant"
        )
    if not sd_band_ok:
        raise AssertionError(
            f"ghost RSSI spread {ghost_sd:.2f} far from real {real_sd:.2f}"
        )

    # (3) interval regularity: ghost cadence carries jitter close to real.
    if score.get("cv_ghost") is not None and score.get("cv_real") is not None:
        cv_ghost = score["cv_ghost"]
        cv_real = score["cv_real"]
        cv_floor_ok = cv_ghost > b.cv_floor
        cv_diff_ok = abs(cv_ghost - cv_real) < b.cv_diff_bound
        results.append(f"cv_ghost={cv_ghost:.4f} > {b.cv_floor}: {cv_floor_ok}")
        results.append(
            f"|cv_ghost-cv_real|={abs(cv_ghost - cv_real):.4f} < "
            f"{b.cv_diff_bound}: {cv_diff_ok}"
        )
        if not cv_floor_ok:
            raise AssertionError(
                f"ghost interval CV {cv_ghost:.4f} ~ 0 (cadence must carry jitter)"
            )
        if not cv_diff_ok:
            raise AssertionError(
                f"ghost CV {cv_ghost:.4f} vs real {cv_real:.4f} diverge"
            )
    else:
        results.append("CV check: skipped (no gap series)")

    # (4) pooled clustering: no clean two-way split.
    sep = score["pooled_separation"]
    sep_ok = sep < b.sep_bound
    results.append(f"pooled_separation={sep:.2f} < {b.sep_bound}: {sep_ok}")
    if not sep_ok:
        raise AssertionError(
            f"pooled RSSI splits into two clean clusters (separation={sep:.2f})"
        )

    return results
