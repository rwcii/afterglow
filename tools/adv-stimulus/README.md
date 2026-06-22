# adv-stimulus

A controllable BLE advertiser for the on-air test rig. It reads single-character
commands on the console serial port and advertises a known source accordingly,
printing one ground-truth line per command so a host harness can correlate what
the device under test should observe.

The cloneable cases (`s`, `n`) carry a recognized broadcast-beacon payload
(iBeacon-shaped manufacturer-specific data), which the classifier requires
before it will promote a random-address source.

## Commands

| Key | Source advertised | Expected handling by the DUT |
|---|---|---|
| `s` | static-random (`0b11`) broadcast-only beacon | cloneable, replayed after departure |
| `n` | NRPA (`0b00`) broadcast-only beacon | cloneable, replayed after departure |
| `c` | static-random but **connectable** | never replayed |
| `r` | RPA (`0b01`) broadcast-only | gated (uncloneable) |
| `v` | reserved (`0b10`) broadcast-only | gated (unsettable address) |
| `x` | stop advertising (the "walk out") | DUT sees the source depart |

On each advertise command it prints:

```
STIM <cmd> addr=<aa:bb:..> type=<conn|nonconn> set=<0|1>
```

While advertising is active, the instance is cycled on/off in short bursts
(~2.5 s on / ~1.2 s off). Two co-located radios contending on the same channels
otherwise starve the DUT's time-shared scan; the off gaps let it reliably land
on the source.

## Build and flash

```sh
. $IDF_PATH/export.sh
idf.py -B build set-target esp32s3      # first build only
idf.py -B build build
idf.py -B build -p /dev/ttyACM1 flash    # use the stimulus board's port
```

It only advertises; it never scans or relays. See `tools/onair-test/README.md`
for the full rig topology and how the harness drives it.
