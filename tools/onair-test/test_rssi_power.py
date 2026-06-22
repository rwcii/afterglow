"""On-air RSSI vs commanded-power validation: three ESP32-S3 boards.

Confirms that a commanded per-ghost power index actually changes the radiated
signal on this ESP32-S3 + Bluedroid build, and that the TX Power Level (0x0A)
AD field stays consistent with the commanded power.

Boards:
  - DUT      — Afterglow built with -DAG_ONAIR_TEST=1. Under the test hook the
               replay path ramps the per-ghost power index deterministically
               across the full BLE ladder and prints one ground-truth line per
               emitted frame:
                   ONAIR emit addr=<aa:bb:..> pwr_idx=<n> adlen=<n>
  - stimulus — tools/adv-stimulus: advertises a cloneable source on command so
               the DUT has a ghost to replay (and ramp power on).
  - observer — tools/adv-observer: passively logs every advertisement it hears:
                   ADV <rnd|pub> <AdvA> rssi=<dBm> len=<n> data=<hex> [txp=<dBm>]

The DUT's pwr_idx is the commanded index; the observer's rssi is what actually
radiated, and its txp is the 0x0A field the DUT wrote. Joining the two streams
on the ghost address gives the full chain:

    commanded pwr_idx  ->  radiated RSSI  (does esp_ble_tx_power_set radiate?)
    commanded pwr_idx  ->  0x0A txp       (is the AD field consistent?)

Pass criteria:
  1. Median observed RSSI rises with commanded pwr_idx (monotone trend; a flat
     trend means the power register write does not radiate).
  2. Observed txp == -24 + 3*pwr_idx for every joined frame (0x0A consistency).

Run (DUT first, then stimulus, then observer):

    AG_DUT_PORT=/dev/ttyACM0 AG_STIM_PORT=/dev/ttyACM1 \
        AG_OBS_PORT=/dev/ttyACM2 pytest tools/onair-test/test_rssi_power.py -v

or standalone (prints a per-level RSSI table and the verdict):

    AG_DUT_PORT=... AG_STIM_PORT=... AG_OBS_PORT=... python \
        tools/onair-test/test_rssi_power.py

Requires: pytest, pyserial. The three firmwares must already be flashed.
"""
import os
import re
import time

import serial


DUT_PORT = os.environ.get("AG_DUT_PORT", "/dev/ttyACM0")
STIM_PORT = os.environ.get("AG_STIM_PORT", "/dev/ttyACM1")
OBS_PORT = os.environ.get("AG_OBS_PORT", "/dev/ttyACM2")
BAUD = 115200

# Window over which to collect the swept ghost on air. The ramp advances one
# ladder step per BLE replay slot, so this needs to cover several full sweeps
# of the 16-level ladder for a stable per-level median.
COLLECT_S = float(os.environ.get("AG_RSSI_COLLECT_S", "90"))

EMIT_RE = re.compile(r"ONAIR emit addr=([0-9a-f:]+) pwr_idx=(-?\d+)")
ADV_RE = re.compile(
    r"ADV (?:rnd|pub) ([0-9a-f:]+) rssi=(-?\d+) len=\d+ data=\S+(?: txp=(-?\d+))?"
)


def dbm_for_idx(idx):
    """The dBm the DUT writes into the 0x0A field for a commanded ladder index."""
    return -24 + 3 * idx


class Board:
    def __init__(self, port, settle=2.0):
        s = serial.Serial()
        s.port = port
        s.baudrate = BAUD
        s.timeout = 0.2
        s.write_timeout = 3
        s.dtr = True
        s.rts = False
        s.open()
        time.sleep(settle)  # let the board settle after the open-induced reset
        self.ser = s
        self.buf = ""

    def send(self, ch):
        self.ser.write(ch.encode())
        self.ser.flush()

    def pump(self):
        data = self.ser.read(8192)
        if data:
            self.buf += data.decode(errors="replace")

    def wait(self, pattern, timeout):
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < deadline:
            self.pump()
            for line in self.buf.splitlines():
                m = rx.search(line)
                if m:
                    return m
            time.sleep(0.1)
        return None

    def lines(self):
        """Return complete lines seen so far, keeping any partial tail buffered."""
        self.pump()
        parts = self.buf.split("\n")
        self.buf = parts[-1]
        return parts[:-1]

    def drain(self):
        self.pump()
        self.buf = ""

    def close(self):
        self.ser.close()


def _start_cloneable(dut, stim, cmd="s"):
    """Seed a cloneable ghost: advertise a source, let the DUT promote it, then
    walk it out so it becomes a replayable (departed) ghost. Return its address."""
    dut.drain()
    stim.drain()
    stim.send(cmd)
    sm = stim.wait(r"STIM %c addr=([0-9a-f:]+)" % cmd, timeout=6)
    assert sm, f"stimulus did not start advertising for '{cmd}'"
    addr = sm.group(1)
    print(f"[rig] seeding ghost via '{cmd}' addr={addr}", flush=True)

    assert dut.wait(r"ONAIR census ", timeout=10), "DUT is not reporting census"
    # Hold on air so the DUT sees it enough times to promote + clone it.
    time.sleep(30)
    stim.send("x")
    stim.wait(r"STIM x stopped", timeout=3)
    # Confirm the ghost is now being emitted before we start measuring.
    em = dut.wait(r"ONAIR emit addr=%s" % re.escape(addr), timeout=60)
    assert em, f"DUT never emitted the seeded ghost {addr}"
    print(f"[rig] ghost {addr} is replaying; starting power sweep", flush=True)
    return addr


def collect(dut, obs, addr, seconds):
    """Collect commanded (pwr_idx) and observed (rssi, txp) samples for a ghost
    address over a window. Pairs each observer ADV with the most recent commanded
    pwr_idx for the same address. Returns (by_idx, txp_mismatches).

    by_idx: {pwr_idx: [rssi, ...]}   txp_mismatches: [(idx, observed_txp), ...]
    """
    dut.drain()
    obs.drain()
    last_idx = {}        # addr -> most recent commanded pwr_idx
    by_idx = {}          # pwr_idx -> [observed rssi]
    txp_mismatches = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        for ln in dut.lines():
            m = EMIT_RE.search(ln)
            if m:
                last_idx[m.group(1)] = int(m.group(2))
        for ln in obs.lines():
            m = ADV_RE.search(ln)
            if not m:
                continue
            a, rssi, txp = m.group(1), int(m.group(2)), m.group(3)
            if a != addr or a not in last_idx:
                continue
            idx = last_idx[a]
            by_idx.setdefault(idx, []).append(rssi)
            if txp is not None and int(txp) != dbm_for_idx(idx):
                txp_mismatches.append((idx, int(txp)))
        time.sleep(0.05)
    return by_idx, txp_mismatches


def _median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def analyze(by_idx):
    """Return (levels_with_data, rise_dbm, spearman_like) where rise_dbm is the
    median-RSSI span from the lowest to the highest populated ladder level and
    spearman_like is the rank correlation between commanded index and median RSSI
    over populated levels (1.0 = perfectly monotone increasing)."""
    pts = sorted((idx, _median(rs)) for idx, rs in by_idx.items() if rs)
    if len(pts) < 2:
        return pts, 0.0, 0.0
    rise = pts[-1][1] - pts[0][1]
    # Rank correlation of (index, median rssi): count concordant vs discordant
    # pairs (Kendall-style), normalized to [-1, 1].
    conc = disc = 0
    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            d = (pts[j][1] - pts[i][1])  # index already increasing
            if d > 0:
                conc += 1
            elif d < 0:
                disc += 1
    total = conc + disc
    tau = (conc - disc) / total if total else 0.0
    return pts, rise, tau


def run(collect_s=COLLECT_S):
    dut = Board(DUT_PORT)
    stim = Board(STIM_PORT)
    obs = Board(OBS_PORT)
    try:
        # The observer logs its boot banner once at startup, which may have
        # scrolled past before we attached; confirm it is alive by the ADV lines
        # it emits for ambient traffic instead.
        assert obs.wait(r"ADV (?:rnd|pub) ", timeout=15), \
            "observer is not reporting any advertisements"
        addr = _start_cloneable(dut, stim)
        by_idx, txp_mismatches = collect(dut, obs, addr, collect_s)
    finally:
        stim.send("x")
        dut.close()
        stim.close()
        obs.close()

    pts, rise, tau = analyze(by_idx)
    print("\n=== RSSI vs commanded power index ===", flush=True)
    print("  idx   dBm   n   median_rssi", flush=True)
    for idx, med in pts:
        n = len(by_idx[idx])
        print(f"  {idx:>3}  {dbm_for_idx(idx):>4}  {n:>3}   {med:>6.1f}", flush=True)
    print(f"\n  populated levels : {len(pts)} / 16", flush=True)
    print(f"  RSSI rise        : {rise:+.1f} dB (low->high level)", flush=True)
    print(f"  rank correlation : {tau:+.2f} (1.0 = perfectly monotone)", flush=True)
    print(f"  0x0A mismatches  : {len(txp_mismatches)}", flush=True)
    if txp_mismatches:
        print(f"    e.g. {txp_mismatches[:5]}", flush=True)
    return pts, rise, tau, txp_mismatches


# --- pytest entry -----------------------------------------------------------

def test_rssi_tracks_commanded_power():
    pts, rise, tau, txp_mismatches = run()
    assert len(pts) >= 4, \
        f"too few ladder levels observed ({len(pts)}); sweep or scan too short"
    # The 0x0A AD field must always equal the commanded power's dBm.
    assert not txp_mismatches, \
        f"0x0A TX-power field inconsistent with commanded power: {txp_mismatches[:5]}"
    # Radiated RSSI must rise with commanded power. A flat (or inverted) trend
    # means esp_ble_tx_power_set is not radiating the change on this build.
    assert tau >= 0.6, \
        f"RSSI does not track commanded power (rank corr {tau:+.2f}); " \
        f"power register write may be a no-op"
    assert rise >= 6.0, \
        f"RSSI span across the ladder only {rise:.1f} dB; expected the power " \
        f"change to radiate as a clear RSSI gradient"


if __name__ == "__main__":
    run()
