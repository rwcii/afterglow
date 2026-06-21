# onair-test

An automated two-board on-air test for the BLE replay path. No human in the
loop: one ESP32-S3 advertises a known source on command, the other (the device
under test) captures, classifies, and re-emits it, and the host harness asserts
the emitted address against ground truth.

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

## Run

```sh
pip install pytest pyserial
AG_DUT_PORT=/dev/ttyACM0 AG_STIM_PORT=/dev/ttyACM1 pytest tools/onair-test -v
```

## What it checks

- **Positive:** a static-random and an NRPA broadcast-only source, once it has
  been seen enough and then departs, is re-emitted under its cloned address.
- **Negative:** a connectable source, an RPA, and a reserved-subtype source are
  never re-emitted, and the DUT never hands the controller an unsettable address.
