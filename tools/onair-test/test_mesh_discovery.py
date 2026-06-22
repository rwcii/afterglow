"""On-air mesh HELLO discovery + contact-table cooldown: two ESP32-S3 boards.

Confirms that two Afterglow nodes running the mesh discover each other over the
air and that the per-peer contact cooldown gates a re-HELLO from an
already-discovered peer.

Both boards run Afterglow built with -DAG_ONAIR_TEST=1, which force-enables the
mesh (it ships disabled in production) and prints ground-truth discovery lines:

    ONAIR mesh hello-rx        self=<id24> peer=<id24>   (a HELLO was received)
    ONAIR mesh peer-discovered self=<id24> peer=<id24>   (first contact -> xfer)
    ONAIR mesh cooldown-gated  self=<id24> peer=<id24>   (re-HELLO inside cooldown)

Each board also logs its NodeID once at init:

    mesh init (enabled=1 node_id=<id32>)

The two boards are mutual peers: each emits the HELLO heartbeat (~3-5 s) and
recognizes the other's HELLO during its passive scan. self/peer in the ONAIR
lines are NodeID low-24, matching the HELLO mfg-data field.

Pass criteria:
  1. Node A discovers Node B's NodeID (peer-discovered self=A peer=B) AND
     Node B discovers Node A's NodeID (peer-discovered self=B peer=A).
  2. After discovery, a subsequent HELLO from the already-known peer is gated by
     the contact cooldown (cooldown-gated self=A peer=B is logged) — i.e. the
     node does not re-transfer on every HELLO.

Boards (only two are needed; the third rig slot is unused here):
  - peer A — /dev/ttyACM0 by default (AG_MESH_A_PORT)
  - peer B — /dev/ttyACM1 by default (AG_MESH_B_PORT)

Run:

    AG_MESH_A_PORT=/dev/ttyACM0 AG_MESH_B_PORT=/dev/ttyACM1 \
        pytest tools/onair-test/test_mesh_discovery.py -v

or standalone (prints the discovery summary and the verdict):

    AG_MESH_A_PORT=... AG_MESH_B_PORT=... python \
        tools/onair-test/test_mesh_discovery.py

Requires: pytest, pyserial. Both firmwares must already be flashed with the
AG_ONAIR_TEST build.
"""
import os
import re
import time

from test_rssi_power import Board


A_PORT = os.environ.get("AG_MESH_A_PORT", "/dev/ttyACM0")
B_PORT = os.environ.get("AG_MESH_B_PORT", "/dev/ttyACM1")

# Discovery window. The HELLO heartbeat is ~3-5 s, so a mutual first contact
# typically lands within ~10 s; allow generous slack for scan/adv interleave.
COLLECT_S = float(os.environ.get("AG_MESH_COLLECT_S", "75"))

INIT_RE = re.compile(r"mesh init \(enabled=\d+ node_id=([0-9a-f]+)\)")
HELLO_RX_RE = re.compile(r"ONAIR mesh hello-rx self=([0-9a-f]+) peer=([0-9a-f]+)")
DISCOVERED_RE = re.compile(r"ONAIR mesh peer-discovered self=([0-9a-f]+) peer=([0-9a-f]+)")
GATED_RE = re.compile(r"ONAIR mesh cooldown-gated self=([0-9a-f]+) peer=([0-9a-f]+)")


def _lo24(id_hex):
    """Low 24 bits of a NodeID hex string, as a zero-padded 6-hex-digit string."""
    return "%06x" % (int(id_hex, 16) & 0xFFFFFF)


def _reset(board):
    """Pulse the ESP32-S3 into a clean reboot via the USB-JTAG reset line, so the
    boot banner and the very first peer-discovered land inside the capture window
    (the first contact otherwise fires once at boot and scrolls past)."""
    board.ser.setDTR(False)
    board.ser.setRTS(True)   # assert reset
    time.sleep(0.2)
    board.ser.setRTS(False)  # release reset
    time.sleep(0.3)
    board.drain()            # discard the pre-reset tail


def _parse(out, side, line):
    """Fold one serial line from `side` ('a'/'b') into the event sets, learning
    the board's own NodeID lo24 from any line that carries it."""
    m = INIT_RE.search(line)
    if m:
        out[side + "_self"].add(_lo24(m.group(1)))
    for rx, key in ((DISCOVERED_RE, "_discovered"), (GATED_RE, "_gated"),
                    (HELLO_RX_RE, "_hello_rx")):
        m = rx.search(line)
        if m:
            out[side + "_self"].add(m.group(1))   # self= is lo24
            out[side + key].add(m.group(2))       # peer= is lo24


def collect(a, b, seconds):
    """Reboot both boards and capture the full discovery exchange from boot.

    Returns a dict of sets keyed a_/b_ + {self,discovered,gated,hello_rx}:
      *_self       : the board's own NodeID lo24 (from banner or ONAIR self=)
      *_discovered : peer ids reported via peer-discovered (first contact -> xfer)
      *_gated      : peer ids reported as cooldown-gated (re-HELLO inside cooldown)
      *_hello_rx   : peer ids the board heard a HELLO from
    """
    out = {f"{s}_{k}": set() for s in ("a", "b")
           for k in ("self", "discovered", "gated", "hello_rx")}
    _reset(a)
    _reset(b)
    deadline = time.time() + seconds
    while time.time() < deadline:
        for ln in a.lines():
            _parse(out, "a", ln)
        for ln in b.lines():
            _parse(out, "b", ln)
        time.sleep(0.05)
    return out


def run(collect_s=COLLECT_S):
    a = Board(A_PORT)
    b = Board(B_PORT)
    try:
        events = collect(a, b, collect_s)
    finally:
        a.close()
        b.close()

    assert events["a_self"], "peerA never logged a NodeID (AG_ONAIR_TEST + mesh?)"
    assert events["b_self"], "peerB never logged a NodeID (AG_ONAIR_TEST + mesh?)"
    a_id = sorted(events["a_self"])[0]
    b_id = sorted(events["b_self"])[0]
    assert a_id != b_id, \
        f"both boards report the same NodeID lo24 ({a_id}); NodeID collision"

    print("\n=== mesh discovery ===", flush=True)
    print(f"  peerA={a_id}  peerB={b_id}", flush=True)
    print(f"  A heard HELLO from   : {sorted(events['a_hello_rx'])}", flush=True)
    print(f"  B heard HELLO from   : {sorted(events['b_hello_rx'])}", flush=True)
    print(f"  A discovered (xfer)  : {sorted(events['a_discovered'])}", flush=True)
    print(f"  B discovered (xfer)  : {sorted(events['b_discovered'])}", flush=True)
    print(f"  A cooldown-gated     : {sorted(events['a_gated'])}", flush=True)
    print(f"  B cooldown-gated     : {sorted(events['b_gated'])}", flush=True)
    return a_id, b_id, events


# --- pytest entry -----------------------------------------------------------

def test_mutual_discovery_and_cooldown():
    a_id, b_id, ev = run()
    # (1) mutual discovery: each node discovered the OTHER's NodeID.
    assert b_id in ev["a_discovered"], \
        f"peerA never discovered peerB ({b_id}); discovered={sorted(ev['a_discovered'])}"
    assert a_id in ev["b_discovered"], \
        f"peerB never discovered peerA ({a_id}); discovered={sorted(ev['b_discovered'])}"
    # (2) cooldown gating: after discovery, at least one of the nodes logged a
    #     cooldown-gated re-HELLO from the already-known peer (the heartbeat
    #     repeats every ~3-5 s, well inside the 120 s contact cooldown).
    gated = (b_id in ev["a_gated"]) or (a_id in ev["b_gated"])
    assert gated, (
        "no cooldown-gated re-HELLO observed; the contact cooldown is not "
        f"gating repeat HELLOs (A_gated={sorted(ev['a_gated'])}, "
        f"B_gated={sorted(ev['b_gated'])})"
    )


if __name__ == "__main__":
    run()
