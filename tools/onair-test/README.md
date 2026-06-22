# onair-test

Automated on-air tests for the BLE replay path and mesh, plus an on-air
separability evaluator. No human in the loop: ESP32-S3 boards play stimulus /
observer / device-under-test roles over the air, and the host harness asserts
ground truth from their serial output.

The harnesses share a small framework under `rig/` (one canonical `Board`, role
and port resolution, stabilization helpers, and the separability evaluator).
Tests declare the board roles they need with the `rig_roles` marker and the
`rig` fixture opens only those — so the whole suite **green-skips cleanly with
no hardware attached** and runs for real on a rig with the boards configured.

## Shared rig framework (`rig/`)

- `board.py` — the canonical `Board` (settle window, bounded writes, buffered
  pump, regex `wait`, line reader, and `reset_pulse()` for a clean reboot).
- `roles.py` — `Roles` (DUT, STIMULUS, OBSERVER, PEER_A, PEER_B) and `RigConfig`,
  which resolves each role to a serial port from the environment. The canonical
  `AG_<ROLE>_PORT` names are honoured, as are the back-compat aliases the older
  harnesses used (`AG_STIM_PORT`, `AG_OBS_PORT`, `AG_MESH_A_PORT`, `AG_MESH_B_PORT`).
- `stabilize.py` — `assert_eventually`, `collect_until`, `retry`, and `quorum`
  (require K of N independent runs to pass) so timing flakiness is handled once.
- `ports.py` — opt-in serial-port reclaim via `fuser -k` (`AG_RIG_FUSER_K=1`) and
  a flash helper.
- `separability.py` — a pure-Python port of the host `detector.c` primitives plus
  `score_real_vs_ghost` / `assert_indistinguishable` for the on-air evaluator.

## Separability evaluator and the parity gate

`test_separability_onair.py` scores how close the device-emitted clone's radiated
signal is to the real source's, along the same feature axes as the host
`test_separability` (RSSI distribution KS, signal-level variance, cadence
regularity, 1-D cluster separation). The Python primitives are locked to the C
reference by `test_separability_parity.py`, a **host-only test that runs in CI
and gates every PR**: it reproduces fixed inputs, runs both, and asserts the
Python output matches the C-computed vectors in
`test/host/separability/parity/expected_vectors.json`. Regenerate those vectors
(only when `detector.c` changes) with:

```sh
cmake -S test/host -B build/host && cmake --build build/host -j
./build/host/gen_vectors > test/host/separability/parity/expected_vectors.json
```

## What it checks

Two boards advertise / replay (`test_onair.py`); a third passive observer adds
RSSI/power validation (`test_rssi_power.py`). Mesh discovery and fragmented data
transfer run on two peer boards (`test_mesh_discovery.py`,
`test_mesh_transfer.py`); ambient-variance validation uses the DUT + observer
(`test_ambient_variance.py`).

## Physical setup

Two or three identical **ESP32-S3** boards (XIAO ESP32-S3 or any S3 dev board),
each connected to the host by its **own USB cable** — there is **no interconnect
between boards** (no GPIO/UART wiring; they communicate only over the air). Place
them within a meter or so of each other so all three are comfortably in BLE
range.

- Replay-path test (`test_onair.py`): 2 boards — **DUT** + **stimulus**.
- RSSI/power test (`test_rssi_power.py`): 3 boards — **DUT** + **stimulus** +
  **observer**.

Each board enumerates as a serial port (`/dev/ttyACM*` on Linux,
`/dev/cu.usbmodem*` on macOS). Set each role's port via its `AG_*_PORT` env var
(see Run below); a test green-skips if any role it declares is unconfigured, so
nothing fails when boards are absent. On Linux a freshly-enumerated board may
come up owned by `root:dialout`; if flashing fails with a permission error, add
your user to the `dialout` group (then re-login) or `sudo chmod 666
/dev/ttyACM<n>`.

A note on results: the RSSI/power sweep measures a ghost as it competes for the
single replay radio against every other replay-eligible source the DUT has
captured. In a busy RF environment the swept ghost is serviced sparsely, so a
single run typically populates only a subset of the power ladder — the harness
asserts the monotonic RSSI-vs-power trend over the levels it does observe rather
than requiring every level. A quiet RF environment gives denser coverage.

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
- **Separability (on-air):** the device-emitted clone's radiated signal is not
  separable from the real source's along the configured feature axes.

## Self-hosted on-air rig runner (DORMANT)

`.github/workflows/onair-rig.yml` defines a status-only job that runs this suite
on a self-hosted runner. It is **dormant**: it targets the runner label
`afterglow-rig`, which is not registered anywhere, so the job is never picked up.
It is also `continue-on-error` and not a required check, so it cannot gate a PR.
On a live runner it runs the suite with a 3-of-5 quorum (`AG_RIG_QUORUM_K=3`,
`AG_RIG_QUORUM_N=5`, `AG_RIG_RETRIES=1`).

To activate it (only after the dev box migrates to dedicated non-Docker
hardware), register a self-hosted runner on the rig host and give it the
matching labels. **These steps are documented, not run by CI:**

```sh
# On the rig host (Linux x86_64, Python 3.12, gh, 3x ESP32-S3 on ACM0..2):
./config.sh --url https://github.com/rwcii/afterglow \
    --token <RUNNER_TOKEN> \
    --labels self-hosted,linux,x64,afterglow-rig \
    --name afterglow-rig-01 --work _work
sudo ./svc.sh install && sudo ./svc.sh start
```

Then set the port-role env for the runner service to match the board layout
(`AG_DUT_PORT`, `AG_STIMULUS_PORT`, `AG_OBSERVER_PORT`, `AG_PEER_A_PORT`,
`AG_PEER_B_PORT`) and ensure the service account is in the `dialout` group. Once
the `afterglow-rig` label exists, the workflow goes live automatically (nightly
at 07:00 UTC, on manual dispatch, and on PRs labelled `run-onair-rig`).
