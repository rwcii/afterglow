"""On-air separability e2e: is the device-emitted clone separable from the real?

Three boards. The stimulus advertises a real cloneable source ('s'); the
observer captures its radiated RSSI as the REAL baseline. The DUT promotes and
clones that source, and after it departs re-emits it as a ghost; the observer
captures the ghost's radiated RSSI, joined to the DUT's ONAIR emit ground-truth
window (the address + the emit lines that mark when the clone is on air). The
evaluator then scores real-vs-ghost distribution closeness and asserts the two
are not separable along the configured feature axes.

This is the on-air twin of the host separability test: there the streams are
generated in software, here they are the actual radiated signal. It is rig-gated
(green-skips without hardware) and wrapped in a quorum so timing flakiness on a
live runner is tolerated (AG_RIG_QUORUM_K / _N, default 1/1).

Run:

    AG_DUT_PORT=/dev/ttyACM0 AG_STIM_PORT=/dev/ttyACM1 \
        AG_OBS_PORT=/dev/ttyACM2 \
        pytest tools/onair-test/test_separability_onair.py -v
"""
import os
import re
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from rig import Roles, ADV_RE, quorum  # noqa: E402
from rig.separability import (  # noqa: E402
    score_real_vs_ghost,
    assert_indistinguishable,
    Bounds,
)


# Per-phase capture windows. The real baseline is captured while the stimulus is
# on air; the ghost while the DUT replays the departed clone.
REAL_S = float(os.environ.get("AG_SEP_REAL_S", "30"))
GHOST_S = float(os.environ.get("AG_SEP_GHOST_S", "60"))
# Minimum samples per stream before scoring is meaningful.
MIN_SAMPLES = int(os.environ.get("AG_SEP_MIN_SAMPLES", "30"))

QUORUM_K = int(os.environ.get("AG_RIG_QUORUM_K", "1"))
QUORUM_N = int(os.environ.get("AG_RIG_QUORUM_N", "1"))
RETRIES = int(os.environ.get("AG_RIG_RETRIES", "0"))


def _collect_rssi_for(obs, addr, seconds):
    """Capture the observer's RSSI samples for a specific source address."""
    obs.drain()
    rssi = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        for ln in obs.lines():
            m = ADV_RE.search(ln)
            if m and m.group(1) == addr:
                rssi.append(int(m.group(2)))
        time.sleep(0.05)
    return rssi


def _one_run(dut, stim, obs):
    """One real-vs-ghost capture + score. Raises AssertionError on failure.

    Starts each run from a clean DUT: reboot the DUT and stop the stimulus so a
    ghost still being replayed from a previous quorum run cannot contaminate this
    run's real-source baseline (the stimulus and the lingering ghost would share
    the same address). This makes each quorum repeat genuinely independent."""
    stim.send("x")
    stim.wait(r"STIM x stopped", timeout=3)
    dut.reset_pulse()
    stim.drain()

    # (a) Real baseline: advertise a cloneable source and capture its radiated
    #     RSSI from the observer while it is genuinely on air.
    stim.send("s")
    sm = stim.wait(r"STIM s addr=([0-9a-f:]+)", timeout=6)
    assert sm, "stimulus did not start advertising 's'"
    addr = sm.group(1)
    assert obs.wait(r"ADV (?:rnd|pub) ", timeout=15), \
        "observer is not reporting any advertisements"
    real_rssi = _collect_rssi_for(obs, addr, REAL_S)

    # Let the DUT promote and clone the source before it departs.
    assert dut.wait(r"ONAIR census ", timeout=10), "DUT is not reporting census"
    time.sleep(20)

    # (b) Walk the real source out so the DUT replays the departed clone.
    stim.send("x")
    stim.wait(r"STIM x stopped", timeout=3)

    # Join via the DUT's emit ground-truth window: wait until the DUT is emitting
    # THIS cloned address, then capture the ghost's radiated RSSI.
    em = dut.wait(r"ONAIR emit addr=%s" % re.escape(addr), timeout=60)
    assert em, f"DUT never emitted the clone {addr} after departure"
    ghost_rssi = _collect_rssi_for(obs, addr, GHOST_S)

    assert len(real_rssi) >= MIN_SAMPLES, \
        f"too few real samples ({len(real_rssi)} < {MIN_SAMPLES})"
    assert len(ghost_rssi) >= MIN_SAMPLES, \
        f"too few ghost samples ({len(ghost_rssi)} < {MIN_SAMPLES})"

    score = score_real_vs_ghost(real_rssi, ghost_rssi)
    print(
        f"\n[sep] real n={len(real_rssi)} sd={score['real_sd']:.2f}  "
        f"ghost n={len(ghost_rssi)} sd={score['ghost_sd']:.2f}  "
        f"KS={score['ks_d']:.3f}/{score['ks_crit']:.3f}  "
        f"sep={score['pooled_separation']:.2f}",
        flush=True,
    )
    for line in assert_indistinguishable(score, Bounds()):
        print(f"[sep]   {line}", flush=True)


@pytest.mark.rig_roles("dut", "stimulus", "observer")
def test_real_vs_ghost_not_separable(rig):
    dut, stim, obs = rig[Roles.DUT], rig[Roles.STIMULUS], rig[Roles.OBSERVER]

    def attempt():
        try:
            _one_run(dut, stim, obs)
        finally:
            stim.send("x")

    quorum(attempt, QUORUM_K, QUORUM_N, retries=RETRIES,
           on_result=lambda i, ok, exc: print(
               f"[sep] quorum run {i + 1}/{QUORUM_N}: "
               f"{'pass' if ok else 'fail'}"
               + (f" ({exc})" if exc else ""), flush=True))
