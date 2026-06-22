# onair-test

An automated on-air test for the BLE replay path. No human in the loop: one
ESP32-S3 advertises a known source on command, another (the device under test)
captures, classifies, and re-emits it, and the host harness asserts the emitted
address against ground truth. A second test (`test_rssi_power.py`) adds a third
board, a passive observer, to confirm that the per-ghost TX-power level the
firmware commands actually changes the radiated signal.

## Boards

- **DUT** — the Afterglow firmware built with the on-air test hook:

  ```sh
  . $IDF_PATH/export.sh
  idf.py -B build/onair -DAG_ONAIR_TEST=1 build
  idf.py -B build/onair -p /dev/ttyACM0 flash
  ```

  The hook (compiled in only with `-DAG_ONAIR_TEST=1`) makes the DUT print an
  eligibility census and a ground-truth line per emitted frame, and shortens the
  departure sweep so the test runs in seconds. A normal build carries none of it.

- **Stimulus** — `tools/adv-stimulus`, flashed to the second board:

  ```sh
  cd tools/adv-stimulus && idf.py -B build set-target esp32s3 build
  idf.py -B build -p /dev/ttyACM1 flash
  ```

  It advertises on single-character serial commands: `s` static-random,
  `n` NRPA, `c` connectable, `r` RPA, `v` reserved, `x` stop.

- **Observer** (RSSI/power test only) — `tools/adv-observer`, flashed to a third
  board. It passively logs `rssi=` and the TX Power (`0x0A`) AD field of every
  advertisement it hears, so the harness can compare what radiated against what
  the DUT commanded. Under `-DAG_ONAIR_TEST=1` the DUT ramps the per-ghost
  power level deterministically across the full ladder so the sweep is
  observable.

## Run

```sh
pip install pytest pyserial

# Replay-path test (two boards):
AG_DUT_PORT=/dev/ttyACM0 AG_STIM_PORT=/dev/ttyACM1 \
    pytest tools/onair-test/test_onair.py -v

# RSSI/power test (three boards):
AG_DUT_PORT=/dev/ttyACM0 AG_STIM_PORT=/dev/ttyACM1 AG_OBS_PORT=/dev/ttyACM2 \
    pytest tools/onair-test/test_rssi_power.py -v
```

## What it checks

- **Positive:** a static-random and an NRPA broadcast-only source, once it has
  been seen enough and then departs, is re-emitted under its cloned address.
- **Negative:** a connectable source, an RPA, and a reserved-subtype source are
  never re-emitted, and the DUT never hands the controller an unsettable address.
- **RSSI/power:** as the DUT ramps a ghost's commanded power level across the
  ladder, the observer's received RSSI rises with it and the `0x0A` TX Power AD
  field matches the emitted power — confirming the per-ghost level actually
  radiates and stays internally consistent.
