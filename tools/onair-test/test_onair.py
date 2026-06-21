"""On-air replay test: two ESP32-S3 boards, no human in the loop.

One board runs the Afterglow firmware built with -DAG_ONAIR_TEST=1 (the device
under test, DUT); the other runs tools/adv-stimulus (the stimulus). The stimulus
advertises a known source on command; the DUT captures, classifies, and — once
the source departs — re-emits it. The DUT prints ground-truth lines under the
test hook:

    ONAIR census pool=<n> elig=<n> dep=<n> replayable=<n>
    ONAIR emit addr=<aa:bb:..> pwr_idx=<n> adlen=<n>

and the stimulus prints:

    STIM <cmd> addr=<aa:bb:..> type=<conn|nonconn> set=<0|1>

Run (ports are the two boards; DUT first):

    AG_DUT_PORT=/dev/ttyACM0 AG_STIM_PORT=/dev/ttyACM1 pytest tools/onair-test -v

Requires: pytest, pyserial. The two firmwares must already be flashed.
"""
import os
import re
import time

import pytest
import serial


DUT_PORT = os.environ.get("AG_DUT_PORT", "/dev/ttyACM0")
STIM_PORT = os.environ.get("AG_STIM_PORT", "/dev/ttyACM1")
BAUD = 115200


class Board:
    def __init__(self, port):
        # ESP32-S3 USB-Serial-JTAG: assert DTR so the device accepts host writes,
        # and bound the write so a wedged port surfaces as an error, not a hang.
        s = serial.Serial()
        s.port = port
        s.baudrate = BAUD
        s.timeout = 0.2
        s.write_timeout = 3
        s.dtr = True
        s.rts = False
        s.open()
        time.sleep(2)  # let the board settle after the open-induced reset
        self.ser = s
        self.buf = ""

    def send(self, ch):
        self.ser.write(ch.encode())
        self.ser.flush()

    def pump(self):
        data = self.ser.read(4096)
        if data:
            self.buf += data.decode(errors="replace")

    def wait(self, pattern, timeout):
        """Wait until a regex matches a complete line; return the match or None."""
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

    def drain(self):
        self.pump()
        self.buf = ""

    def close(self):
        self.ser.close()


@pytest.fixture(scope="module")
def boards():
    dut = Board(DUT_PORT)
    stim = Board(STIM_PORT)
    yield dut, stim
    stim.send("x")
    dut.close()
    stim.close()


def _census(line):
    m = re.search(r"elig=(\d+) dep=(\d+) replayable=(\d+)", line)
    return tuple(int(x) for x in m.groups()) if m else None


def _drive_cloneable(boards, cmd):
    """Advertise a cloneable source, wait for promotion, stop it, return the
    stimulus address and whether the DUT emitted it under a cloned address."""
    dut, stim = boards
    dut.drain()
    stim.drain()

    stim.send(cmd)
    sm = stim.wait(r"STIM %c addr=([0-9a-f:]+)" % cmd, timeout=6)
    assert sm, f"stimulus did not start advertising for '{cmd}'"
    addr = sm.group(1)
    # The stimulus 'set' flag is advisory; the real ground truth is whether the
    # DUT goes on to capture and replay this address below.

    print(f"\n[rig] {cmd} advertising as {addr}", flush=True)

    # Hold the source on air long enough for the DUT to see it repeatedly and
    # promote it (the scan window time-shares with Wi-Fi, so allow margin).
    assert dut.wait(r"ONAIR census ", timeout=10), "DUT is not reporting census"
    hold_end = time.time() + 30
    while time.time() < hold_end:
        m = dut.wait(r"ONAIR census .*", timeout=6)
        if m:
            print(f"[rig] hold: {m.group(0).strip()[-60:]}", flush=True)

    # Walk the source out.
    stim.send("x")
    stim.wait(r"STIM x stopped", timeout=3)
    print("[rig] walked out", flush=True)

    # The DUT should now emit THIS cloned address (departed + eligible). Match the
    # specific address so a ghost left replayable by an earlier test can't be
    # mistaken for this one.
    em = dut.wait(r"ONAIR emit addr=%s" % re.escape(addr), timeout=60)
    if not em:
        print(f"[rig] no emit for {addr}; last census:", flush=True)
        m = dut.wait(r"ONAIR census .*", timeout=6)
        if m:
            print(f"[rig]   {m.group(0).strip()[-60:]}", flush=True)
    return addr, (addr if em else None)


def test_static_random_is_cloned_and_replayed(boards):
    addr, emitted = _drive_cloneable(boards, "s")
    assert emitted is not None, "DUT did not emit any ghost after the source departed"
    assert emitted == addr, f"DUT emitted {emitted}, expected the cloned {addr}"


def test_nrpa_is_cloned_and_replayed(boards):
    addr, emitted = _drive_cloneable(boards, "n")
    assert emitted is not None, "DUT did not emit an NRPA ghost after departure"
    assert emitted == addr, f"DUT emitted {emitted}, expected the cloned {addr}"


def _drive_negative(boards, cmd, hold_s=25):
    """Advertise a non-cloneable source, stop it, and assert the DUT never emits
    that address and logs no controller address-set rejection."""
    dut, stim = boards
    dut.drain()
    stim.drain()

    stim.send(cmd)
    sm = stim.wait(r"STIM %c addr=([0-9a-f:]+)" % cmd, timeout=5)
    assert sm, f"stimulus did not advertise for '{cmd}'"
    addr = sm.group(1)

    time.sleep(hold_s)
    stim.send("x")
    stim.wait(r"STIM x stopped", timeout=3)

    # Give the DUT time to (wrongly) emit if the gate failed.
    em = dut.wait(r"ONAIR emit addr=%s" % re.escape(addr), timeout=20)
    assert em is None, f"DUT replayed a non-cloneable source ({cmd}) addr={addr}"
    # The DUT must never hand the controller an unsettable address.
    assert "Invalid random address type" not in dut.buf, \
        f"DUT attempted to set an invalid address for '{cmd}'"


def test_connectable_source_is_not_replayed(boards):
    _drive_negative(boards, "c")


def test_rpa_source_is_not_replayed(boards):
    _drive_negative(boards, "r")


def test_reserved_subtype_is_not_replayed(boards):
    _drive_negative(boards, "v")
