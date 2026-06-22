# adv-observer

A standalone ESP32-S3 BLE advertisement observer. It passively scans and prints
one line per legacy advertisement, decoding the fields needed to inspect what a
device under test actually radiates:

```
ADV <type> <AdvA> rssi=<dBm> len=<n> data=<hex> [txp=<dBm>]
```

- `<type>` — `pub` or `rnd`, the advertiser address type from the scan-result
  event (the PDU header TxAdd bit).
- `<AdvA>` — the 6-byte advertising address.
- `data=<hex>` — the raw AdvData.
- `txp=<dBm>` — the decoded TX Power Level (`0x0A`) AD value, printed only when
  the advertisement carries one.

It only receives; it never transmits.

## Two-board validation setup

A cheap repeatable rig for checking the replay emit path uses two ESP32-S3
boards: one runs the firmware under test (the emitter), the other runs
adv-observer. Watch the observer's serial output to confirm the emitter's
advertising address, address type, AdvData, and TX Power AD field are what the
emit path intends.

## Build and flash

```sh
. $IDF_PATH/export.sh
idf.py -B build set-target esp32s3      # first build only
idf.py -B build build
idf.py -B build -p /dev/ttyACM2 flash monitor   # use the observer board's port
```

In the automated three-board rig (`tools/onair-test/test_rssi_power.py`) the
observer is the third board, conventionally `/dev/ttyACM2`; flash whichever port
your observer board enumerates as.
