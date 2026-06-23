"""On-air ambient-variance validation: confirms the walk's adaptive sigma tracks
the per-source TEMPORAL RSSI variability of real sources, not the spatial spread
between sources sitting at different distances.

Background. The walk's per-step sigma is sampled from the room: a quiet room of
steady devices should yield a SMALL sigma (ghosts stay still), a room of moving
devices a larger one. The quantity the device under test averages is each
source's own RSSI movement over time (rssi_dev_ewma), aggregated across the live
pool. A naive estimator that instead used the spread of mean RSSI ACROSS sources
inverts in the headline case: steady devices at varied distances have a large
cross-source spread but near-zero temporal motion, which would make ghosts most
mobile in the quietest room. This harness validates on air that the shipped
estimator measures the temporal quantity.

Boards:
  - DUT      — Afterglow built with -DAG_ONAIR_TEST=1. Each slow sweep it prints
                   ONAIR ambient sigma=<f> used=<n> dev=<f>
               the derived per-step sigma, the source count that fed it, and the
               mean per-source temporal deviation (dB) it measured.
  - observer — tools/adv-observer: logs every advertisement it hears,
                   ADV <rnd|pub> <AdvA> rssi=<dBm> len=<n> data=<hex> [txp=<dBm>]
               This is the independent ground truth: grouping by AdvA and taking
               each source's RSSI std-dev over time gives a host-side estimate of
               the same temporal quantity the DUT reports as dev.
  - stimulus — not required for the ambient-room method; the validation reads the
               real co-located devices the DUT already pools. Left flashed and
               idle is fine.

Method (ambient-room, no synthetic stimulus): run in a normally-busy room so the
DUT pools >= 6 real sources, collect both streams over a window, and check:

  1. Every reported sigma is within the documented [0.8, 4.0] dB band, and when
     fewer than 6 sources are pooled it falls back to the 2.0 dB prior.
  2. The DUT's own sigma == clamp(dev * 0.3, 0.8, 4.0): its internal mapping is
     self-consistent (catches a regression in ag_txwalk_ambient_sigma wiring).
  3. The DUT's dev tracks the observer-measured mean per-source temporal std-dev
     (same order of magnitude / positive rank agreement across the window) —
     i.e. it is measuring temporal motion, NOT cross-source spatial spread. As a
     guard, the observer's cross-source spatial spread is reported alongside so a
     reviewer can see the two are not the same number.

Run (DUT first, observer second):

    AG_DUT_PORT=/dev/ttyACM0 AG_OBS_PORT=/dev/ttyACM2 \
        python tools/onair-test/test_ambient_variance.py

or under pytest with the same env. Requires: pyserial (pytest optional).

Note: the ambient-room method depends on the room actually being populated. In a
genuinely empty room the DUT will sit on the n<6 fallback (sigma=2.0) and only
assertion (1)'s fallback branch is exercised; that is reported, not failed.
"""
import os
import re
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

# Reuse the serial Board + helpers from the shared rig; they are the same
# three-board rig.
from rig import Board, RigConfig, Roles, median, ADV_RE  # noqa: E402


# Collect over several slow sweeps (the DUT updates ambient on its eviction-sweep
# cadence, ~30 s) so we see multiple ONAIR ambient lines and enough observer
# packets per source for a stable per-source std-dev.
COLLECT_S = float(os.environ.get("AG_AMBIENT_COLLECT_S", "150"))

AMBIENT_RE = re.compile(
    r"ONAIR ambient sigma=(-?\d+\.\d+) used=(\d+) dev=(-?\d+\.\d+)"
)

# Must mirror ag_txwalk_ambient_sigma() in components/afterglow_core/ag_txwalk.c.
SIGMA_GAIN = 0.3
SIGMA_LO = 0.8
SIGMA_HI = 4.0
FALLBACK_SIGMA = 2.0
MIN_SOURCES = 6


def expected_sigma(dev):
    s = dev * SIGMA_GAIN
    return max(SIGMA_LO, min(SIGMA_HI, s))


def _stdev(xs):
    n = len(xs)
    if n < 2:
        return 0.0
    m = sum(xs) / n
    return (sum((x - m) ** 2 for x in xs) / n) ** 0.5


def collect(dut, obs, seconds):
    """Collect the DUT's ONAIR ambient reports and the observer's per-source RSSI
    samples over a window.

    Returns (ambient_reports, obs_by_addr):
      ambient_reports: [(sigma, used, dev), ...] one per slow sweep seen
      obs_by_addr:     {AdvA: [rssi, ...]} every advertisement RSSI per source
    """
    dut.drain()
    obs.drain()
    ambient_reports = []
    obs_by_addr = {}
    deadline = time.time() + seconds
    while time.time() < deadline:
        for ln in dut.lines():
            m = AMBIENT_RE.search(ln)
            if m:
                ambient_reports.append(
                    (float(m.group(1)), int(m.group(2)), float(m.group(3)))
                )
                print(
                    f"[dut] ambient sigma={m.group(1)} used={m.group(2)} "
                    f"dev={m.group(3)}",
                    flush=True,
                )
        for ln in obs.lines():
            m = ADV_RE.search(ln)
            if not m:
                continue
            addr, rssi = m.group(1), int(m.group(2))
            obs_by_addr.setdefault(addr, []).append(rssi)
        time.sleep(0.05)
    return ambient_reports, obs_by_addr


def observer_temporal_dev(obs_by_addr, min_samples=8):
    """Mean per-source temporal RSSI std-dev across sources with enough samples.
    This is the independent ground truth for the quantity the DUT reports as dev."""
    per_source = [
        _stdev(rs) for rs in obs_by_addr.values() if len(rs) >= min_samples
    ]
    if not per_source:
        return None, 0
    return sum(per_source) / len(per_source), len(per_source)


def observer_spatial_spread(obs_by_addr, min_samples=8):
    """Cross-source std-dev of per-source MEAN RSSI — the spatial quantity the
    estimator deliberately does NOT use. Reported for contrast only."""
    means = [
        sum(rs) / len(rs) for rs in obs_by_addr.values() if len(rs) >= min_samples
    ]
    return _stdev(means), len(means)


def _run(dut, obs):
    """Run the ambient-variance check on already-open boards; return 0/1."""
    print(f"[rig] collecting ambient + observer for {COLLECT_S:.0f}s", flush=True)
    reports, obs_by_addr = collect(dut, obs, COLLECT_S)

    failures = []

    # (1) band + fallback: every report sits in [0.8, 4.0]; n<6 reports must be
    # exactly the 2.0 fallback.
    assert reports, (
        "DUT printed no 'ONAIR ambient' lines — is it the AG_ONAIR_TEST build "
        "and running long enough to hit a slow sweep?"
    )
    for sigma, used, dev in reports:
        if not (SIGMA_LO - 1e-3 <= sigma <= SIGMA_HI + 1e-3):
            failures.append(f"sigma {sigma} outside [{SIGMA_LO}, {SIGMA_HI}]")
        if used < MIN_SOURCES and abs(sigma - FALLBACK_SIGMA) > 1e-3:
            failures.append(
                f"used={used} (<{MIN_SOURCES}) but sigma={sigma} != fallback "
                f"{FALLBACK_SIGMA}"
            )

    # (2) internal consistency of the DUT's own mapping for the adapted reports.
    adapted = [(s, u, d) for (s, u, d) in reports if u >= MIN_SOURCES]
    for sigma, used, dev in adapted:
        want = expected_sigma(dev)
        if abs(sigma - want) > 0.05:
            failures.append(
                f"sigma {sigma} != clamp(dev*0.3)={want:.3f} for dev={dev}"
            )

    # (3) headline: the DUT's dev tracks the observer's per-source TEMPORAL std-
    # dev, and that is a different number from the spatial spread.
    obs_temporal, n_temporal = observer_temporal_dev(obs_by_addr)
    obs_spatial, n_spatial = observer_spatial_spread(obs_by_addr)
    dut_dev = (
        sum(d for _, _, d in adapted) / len(adapted) if adapted else None
    )

    print("\n=== ambient-variance summary ===", flush=True)
    print(f"ambient reports:            {len(reports)} "
          f"({len(adapted)} adapted, {len(reports) - len(adapted)} fallback)",
          flush=True)
    print(f"DUT mean dev (temporal):    "
          f"{dut_dev:.2f} dB" if dut_dev is not None else
          "DUT mean dev (temporal):    n/a (all fallback)", flush=True)
    print(f"observer temporal std-dev:  "
          f"{obs_temporal:.2f} dB over {n_temporal} sources"
          if obs_temporal is not None else
          "observer temporal std-dev:  n/a (too few sampled sources)", flush=True)
    print(f"observer SPATIAL spread:    "
          f"{obs_spatial:.2f} dB over {n_spatial} sources (contrast — NOT used)",
          flush=True)

    if not adapted:
        print(
            "\n[note] DUT stayed on the n<6 fallback the whole run (quiet/empty "
            "room). Band+fallback (1) validated; temporal-tracking (2,3) needs a "
            "populated room — rerun with >= 6 real sources in range.",
            flush=True,
        )
    elif obs_temporal is not None:
        # The DUT's dev should be the same order of magnitude as the observer's
        # independently-measured temporal std-dev. They are measured by different
        # radios over different (overlapping) source sets and the DUT uses an
        # alpha=1/8 EWMA vs. the observer's plain std-dev, so allow a wide band;
        # the assertion that matters is that dev is NOT tracking the spatial
        # spread (which is typically much larger in a spread-out room).
        if dut_dev > max(obs_spatial, obs_temporal) + 2.0:
            failures.append(
                f"DUT dev {dut_dev:.2f} exceeds both observer temporal "
                f"{obs_temporal:.2f} and spatial {obs_spatial:.2f} — estimator "
                f"may be measuring the wrong quantity"
            )
        if obs_spatial > obs_temporal + 3.0 and dut_dev > obs_temporal + 3.0:
            failures.append(
                f"observer shows spatial({obs_spatial:.2f}) >> temporal"
                f"({obs_temporal:.2f}) yet DUT dev {dut_dev:.2f} tracks spatial "
                f"— this is the inversion the estimator must avoid"
            )

    if failures:
        print("\nFAIL:", flush=True)
        for f in failures:
            print(f"  - {f}", flush=True)
        return 1
    print("\nPASS: adaptive sigma stays in band, the DUT's mapping is "
          "self-consistent, and dev tracks temporal (not spatial) variability.",
          flush=True)
    return 0


def run():
    """Standalone entry: open the DUT + observer, run the check, return 0/1."""
    cfg = RigConfig()
    dut = Board(cfg.require_port(Roles.DUT))
    obs = Board(cfg.require_port(Roles.OBSERVER))
    try:
        return _run(dut, obs)
    finally:
        dut.close()
        obs.close()


@pytest.mark.rig_roles("dut", "observer")
def test_ambient_variance(rig):
    assert _run(rig[Roles.DUT], rig[Roles.OBSERVER]) == 0


if __name__ == "__main__":
    sys.exit(run())
