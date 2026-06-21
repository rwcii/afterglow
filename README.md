# Afterglow

Open-source ESP32-S3 firmware that defeats **passive** 2.4 GHz location tracking
by turning each device into a roaming relay for the BLE and Wi-Fi beacons it
observes. It captures a representative subset of nearby beacons and rebroadcasts
them — at their original cadence, with realistic signal-level variation — for a
randomized window after the source device has departed. When several units are
present they absorb subsets of each other's pools on contact, forming an
emergent, leaderless privacy mesh. The aggregate effect: a passive observer can
no longer reliably conclude that the presence of an identifier means the
corresponding device is physically there.

> **Status:** P1–P4 implemented and building clean for the ESP32-S3 (ESP-IDF
> v5.3.x). Host unit/statistical tests pass in CI; on-hardware flash and
> operational testing are in progress. Wi-Fi beacon replay and the mesh ship
> **disabled by default** pending field validation.

## Intended use & boundaries

Afterglow is a **personal privacy tool**. Please read this before building or
running it:

- **Not a jammer or DoS tool.** It does not flood the airspace or overwhelm
  receivers; it emits a conservative, subset-only volume of ordinary-looking
  advertisement traffic.
- **Not connection impersonation.** It replays presence/advertisement data
  only; it does not intercept connections or sessions.
- **Not for covert deployment** in shared spaces without the awareness of the
  people affected.
- **RF transmission is regulated.** Beacon replay and RF transmission are
  subject to local law; in some jurisdictions certain RF behaviors can implicate
  jamming or unauthorized-transmission rules. **You are responsible for
  compliance in your region.** Wi-Fi beacon replay and the mesh ship **disabled
  by default** (`replay.wifi_beacons_enabled=false`, `mesh.enabled=false`).

## Hardware

- **Initial testbed:** Seeed Studio XIAO ESP32-S3 (Xtensa LX7 dual-core 240 MHz,
  8 MB flash, 8 MB PSRAM, Wi-Fi 4 + Bluetooth 5.0 LE).
- The ESP32-S3 has **one shared 2.4 GHz radio** for Wi-Fi and BLE — they
  time-slice, they do not run truly concurrently. Afterglow's default radio
  backend (`radio_single`) is built around a strict serial time-division
  scheduler. A future `radio_dual` backend (a companion BLE radio over UART/SPI)
  can lift this ceiling without changing the rest of the firmware — the radio
  HAL (`components/radio_backend`) is the swap point.

## Build & flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.3.x**
(the version the firmware is built and CI-verified against).

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.defaults` pins the project-relevant options (PSRAM, Bluedroid with
BLE 4.2 legacy advertising, Wi-Fi/BLE coexistence, 8 MB flash). The generated
`sdkconfig` is gitignored.

Host-native unit and statistical tests run without ESP-IDF:

```sh
cmake -S test/host -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host -j
ctest --test-dir build/host --output-on-failure
```

## Architecture

The firmware is organized as ESP-IDF components, one per module, grouped by the
phase that introduced them:

| Phase | Focus | Key components |
|---|---|---|
| **P1** | Capture | `radio_backend`, `radio_single`, `capture`, `pool`, `classifier` |
| **P2** | Replay | `replay`, `radio_single` (TX path) |
| **P3** | Pool & lifecycle | `pool` (eviction), `txentropy`, `lifecycle` |
| **P4** | Mesh & hardening | `mesh` |

Cross-cutting: `config` (NVS-backed tunables, conservative defaults) and
`entropy` (RNG + slow runtime drift of boot constants).

Each component is split into **portable logic** (`*_logic.c`, compiled and
tested host-native against thin ESP-IDF shims under `test/host/shim/`) and a
thin **hardware wrapper** that supplies the clock, RNG, and radio. The
`afterglow_core` component is fully portable and carries no ESP-IDF
dependencies.

A primary design constraint is that the firmware must **not introduce its own
fingerprint** — any predictable artifact (fixed replay window, altered interval,
static eviction threshold, stable signal level on a stationary node) is avoided
in the relay behavior.

## License

[MIT](LICENSE) © 2026 Robert Capps.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the branch flow (`feature/*` →
`develop` → `main`), local checks, and commit conventions. Please keep the
hardware-agnostic abstractions (the radio HAL) clean so the firmware can be
ported beyond the XIAO, and respect the intended-use boundaries above.
