# Afterglow

**Defeats passive Bluetooth/Wi-Fi location tracking by rebroadcasting nearby devices' beacons after they've left.**

Your phone, watch, earbuds, and laptop constantly announce themselves over Bluetooth LE and Wi-Fi. Anyone with a cheap receiver — a retail analytics box, a venue "footfall" sensor, a hobbyist sniffer, a stalker — can log those identifiers and build a record of where you were and when. Afterglow attacks the assumption that record depends on: *if I saw the identifier, the device was here.*

Afterglow is open-source firmware for a small ESP32-S3 board you carry with you. It captures a representative subset of the beacons around it and rebroadcasts them — at their original cadence, with realistic signal-level variation — for a randomized window after the source device has moved on. When several Afterglow units cross paths they exchange subsets of each other's pools, forming an emergent, leaderless privacy mesh. The aggregate effect: a passive observer can no longer reliably conclude that seeing an identifier means the corresponding device is physically present.

> **Status:** P1–P4 implemented and building clean for the ESP32-S3 (ESP-IDF v5.3.x). Host unit/statistical tests pass in CI; on-hardware flash and operational testing are in progress. Wi-Fi beacon replay and the mesh ship **disabled by default** pending field validation.

## Who this is for

- People who want to reduce the value of passive BLE/Wi-Fi presence logs kept by retail, transit, and venue analytics systems.
- Anyone concerned about being tracked by inexpensive Bluetooth or Wi-Fi sniffers.
- Privacy researchers and makers who want a hardware-agnostic base for beacon-replay experiments.

If you're worried about someone *reading your messages* or *intercepting your connections*, this is not the tool for that — see the boundaries below.

## How it works

1. **Capture.** Afterglow listens for BLE advertisements and Wi-Fi beacons/probes and keeps a bounded pool of what it sees.
2. **Classify and select.** A classifier picks a representative subset so replay volume stays low and ordinary-looking.
3. **Replay.** After a source device departs, its beacons are rebroadcast for a randomized window, preserving the original interval and adding plausible signal-level drift.
4. **Age out.** Entries are evicted on a randomized, non-fixed schedule so the replay itself has no telltale signature.
5. **Mesh (optional).** When two Afterglow units meet, each absorbs part of the other's pool, spreading identifiers across a wider area than any single carrier walked.

A primary design constraint is that the firmware must **not introduce its own fingerprint**. Any predictable artifact — fixed replay window, altered interval, static eviction threshold, stable signal level on a stationary node — is avoided in the relay behavior.

## Intended use & boundaries

Afterglow is a **personal privacy tool**. Please read this before building or running it:

- **Not a jammer or DoS tool.** It does not flood the airspace or overwhelm receivers; it emits a conservative, subset-only volume of ordinary-looking advertisement traffic.
- **Not connection impersonation.** It replays presence/advertisement data only; it does not intercept connections or sessions.
- **Not for covert deployment** in shared spaces without the awareness of the people affected.
- **RF transmission is regulated.** Beacon replay and RF transmission are subject to local law; in some jurisdictions certain RF behaviors can implicate jamming or unauthorized-transmission rules. **You are responsible for compliance in your region.** Wi-Fi beacon replay and the mesh ship **disabled by default** (`replay.wifi_beacons_enabled=false`, `mesh.enabled=false`).

## Hardware

- **Initial testbed:** Seeed Studio XIAO ESP32-S3 (Xtensa LX7 dual-core 240 MHz, 8 MB flash, 8 MB PSRAM, Wi-Fi 4 + Bluetooth 5.0 LE). Small enough to carry in a pocket or bag.
- The ESP32-S3 has **one shared 2.4 GHz radio** for Wi-Fi and BLE — they time-slice, they do not run truly concurrently. Afterglow's default radio backend (`radio_single`) is built around a strict serial time-division scheduler. A future `radio_dual` backend (a companion BLE radio over UART/SPI) can lift this ceiling without changing the rest of the firmware — the radio HAL (`components/radio_backend`) is the swap point.

## Build & flash

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.3.x** (the version the firmware is built and CI-verified against).

```
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.defaults` pins the project-relevant options (PSRAM, Bluedroid with BLE 4.2 legacy advertising, Wi-Fi/BLE coexistence, 8 MB flash). The generated `sdkconfig` is gitignored.

Host-native unit and statistical tests run without ESP-IDF:

```
cmake -S test/host -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host -j
ctest --test-dir build/host --output-on-failure
```

## Configuration

Runtime tunables are stored in NVS with conservative defaults (see the `config` component). The two features that transmit beyond baseline BLE replay are off until you opt in:

| Key | Default | Effect |
| --- | --- | --- |
| `replay.wifi_beacons_enabled` | `false` | Enables Wi-Fi beacon replay in addition to BLE |
| `mesh.enabled` | `false` | Enables pool exchange with other Afterglow units |

## Architecture

The firmware is organized as ESP-IDF components, one per module, grouped by the phase that introduced them:

| Phase  | Focus            | Key components                                                   |
| ------ | ---------------- | ---------------------------------------------------------------- |
| **P1** | Capture          | `radio_backend`, `radio_single`, `capture`, `pool`, `classifier` |
| **P2** | Replay           | `replay`, `radio_single` (TX path)                               |
| **P3** | Pool & lifecycle | `pool` (eviction), `txentropy`, `lifecycle`                      |
| **P4** | Mesh & hardening | `mesh`                                                           |

Cross-cutting: `config` (NVS-backed tunables, conservative defaults) and `entropy` (RNG + slow runtime drift of boot constants).

Each component is split into **portable logic** (`*_logic.c`, compiled and tested host-native against thin ESP-IDF shims under `test/host/shim/`) and a thin **hardware wrapper** that supplies the clock, RNG, and radio. The `afterglow_core` component is fully portable and carries no ESP-IDF dependencies.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the branch flow (`feature/*` → `develop` → `main`), local checks, and commit conventions. Please keep the hardware-agnostic abstractions (the radio HAL) clean so the firmware can be ported beyond the XIAO, and respect the intended-use boundaries above.

## License

[MIT](LICENSE) © 2026 Robert Capps.
