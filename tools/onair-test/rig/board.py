"""Serial board control for the on-air rig.

A single canonical ``Board`` class drives one ESP32-S3 over its USB-Serial-JTAG
port. It is the superset of the per-harness ``Board`` classes that previously
lived in the individual test files: a settle window after open, bounded writes,
a buffered pump, a regex ``wait``, a line-oriented ``lines`` reader, and a
``reset_pulse`` that reboots the board over the USB-JTAG control lines so boot
banners land inside the capture window.
"""
import re
import time

import serial


BAUD = 115200


class Board:
    """One ESP32-S3 dev board on a serial port.

    ESP32-S3 USB-Serial-JTAG: assert DTR so the device accepts host writes, and
    bound the write so a wedged port surfaces as an error, not a hang. The open
    induces a reset, so settle before reading.
    """

    def __init__(self, port, settle=2.0, baud=BAUD):
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
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

    def lines(self):
        """Return complete lines seen so far, keeping any partial tail buffered."""
        self.pump()
        parts = self.buf.split("\n")
        self.buf = parts[-1]
        return parts[:-1]

    def drain(self):
        self.pump()
        self.buf = ""

    def reset_pulse(self):
        """Pulse the board into a clean reboot via the USB-JTAG reset line, so the
        boot banner and the first ground-truth lines land inside the capture
        window (they otherwise fire once at boot and scroll past)."""
        self.ser.setDTR(False)
        self.ser.setRTS(True)   # assert reset
        time.sleep(0.2)
        self.ser.setRTS(False)  # release reset
        time.sleep(0.3)
        self.drain()            # discard the pre-reset tail

    def close(self):
        self.ser.close()
