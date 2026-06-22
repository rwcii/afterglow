"""Host-only parity gate: lock the Python detector port to the C reference.

Reproduces the EXACT fixed inputs from
test/host/separability/parity/gen_vectors.c (a tiny self-contained LCG so no
shared library is needed), runs the Python primitives in rig/separability.py,
and asserts each result matches the C-computed value in expected_vectors.json
within a tight tolerance.

This test needs no hardware and runs in host CI. It is the per-PR gate that keeps
the Python evaluator identical to detector.c — if detector.c changes, regenerate
the vectors (build/host/gen_vectors > expected_vectors.json) and this re-locks.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rig import separability as sep  # noqa: E402


VECTORS = (
    Path(__file__).resolve().parent.parent.parent
    / "test" / "host" / "separability" / "parity" / "expected_vectors.json"
)

N = 1500
# Tight tolerance: the C side prints 17 significant digits and the algorithms are
# the same arithmetic, so only IEEE-754 rounding of independent summation orders
# can differ. The dominant operation order matches, so this is conservatively
# loose enough to never flake while still catching any real divergence.
TOL = 1e-9


class _LCG:
    """Mirror of the Numerical Recipes ranqd1 LCG in gen_vectors.c."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def unit(self):
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state / 4294967296.0

    def uniform(self, lo, hi):
        return lo + (hi - lo) * self.unit()


def _build_inputs():
    """Reproduce the fixed inputs from gen_vectors.c exactly."""
    r = _LCG(0xC0FFEE)
    center = -68.0
    a_rssi = []
    b_rssi = []
    for _ in range(N):
        a_rssi.append(center + r.uniform(-6.0, 6.0))
        b_rssi.append(center + r.uniform(-5.0, 5.0))
    a_gaps = []
    b_gaps = []
    for _ in range(N):
        a_gaps.append(100.0 + r.uniform(-5.0, 5.0))
        b_gaps.append(100.0 + r.uniform(-4.0, 4.0))
    pooled = a_rssi + b_rssi
    small_set = [-70, -71, -69, -70, -72, -68, -70, -71]
    bimodal = [-90, -91, -89, -90, -50, -49, -51, -50, -90, -50]
    return {
        "a_rssi": a_rssi,
        "b_rssi": b_rssi,
        "a_gaps": a_gaps,
        "b_gaps": b_gaps,
        "pooled": pooled,
        "small_set": small_set,
        "bimodal": bimodal,
    }


def _expected():
    with open(VECTORS) as f:
        return json.load(f)


def test_vectors_file_present_and_c_computed():
    exp = _expected()
    assert exp["n"] == N
    assert "C-computed" in exp["_note"]


def test_ks_statistic_parity():
    inp = _build_inputs()
    exp = _expected()
    assert abs(sep.ks_statistic(inp["a_rssi"], inp["b_rssi"])
               - exp["ks_statistic"]) < TOL
    assert abs(sep.ks_statistic(inp["small_set"], inp["bimodal"])
               - exp["ks_statistic_small"]) < TOL


def test_ks_critical_parity():
    exp = _expected()
    assert abs(sep.ks_critical_05(N, N) - exp["ks_critical_05"]) < TOL
    assert abs(sep.ks_critical_05(8, 10) - exp["ks_critical_05_small"]) < TOL


def test_interval_regularity_cv_parity():
    inp = _build_inputs()
    exp = _expected()
    assert abs(sep.interval_regularity_cv(inp["a_gaps"])
               - exp["interval_regularity_cv_a"]) < TOL
    assert abs(sep.interval_regularity_cv(inp["b_gaps"])
               - exp["interval_regularity_cv_b"]) < TOL
    assert abs(sep.interval_regularity_cv(inp["small_set"])
               - exp["interval_regularity_cv_small"]) < TOL


def test_rssi_stability_parity():
    inp = _build_inputs()
    exp = _expected()
    assert abs(sep.rssi_stability(inp["a_rssi"]) - exp["rssi_stability_a"]) < TOL
    assert abs(sep.rssi_stability(inp["b_rssi"]) - exp["rssi_stability_b"]) < TOL
    assert abs(sep.rssi_stability(inp["small_set"])
               - exp["rssi_stability_small"]) < TOL


def test_two_means_separation_parity():
    inp = _build_inputs()
    exp = _expected()
    assert abs(sep.two_means_separation(inp["pooled"])
               - exp["two_means_separation_pooled"]) < TOL
    assert abs(sep.two_means_separation(inp["bimodal"])
               - exp["two_means_separation_bimodal"]) < TOL
    assert abs(sep.two_means_separation(inp["small_set"])
               - exp["two_means_separation_small"]) < TOL
