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

> **Status:** scaffolding. Design is complete and grounded against verified
> ESP32-S3 capabilities; implementation proceeds bottom-up by phase (see below).

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

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) (v5.x).

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`sdkconfig.defaults` pins the project-relevant options (PSRAM, Bluedroid,
coexistence, 8 MB flash). The generated `sdkconfig` is gitignored.

## Architecture

The firmware is organized as ESP-IDF components, one per module. Build order is
bottom-up so the design is informed by what is actually observed in the field:

| Phase | Focus | Key components |
|---|---|---|
| **P1** | Capture | `radio_backend`, `radio_single`, `capture`, `pool`, `classifier` |
| **P2** | Replay | `replay`, `radio_single` (TX path) |
| **P3** | Pool & lifecycle | `pool` (eviction), `txentropy`, `lifecycle` |
| **P4** | Mesh & hardening | `mesh` |

Cross-cutting: `config` (NVS-backed tunables, conservative defaults) and
`entropy` (RNG + slow runtime drift of boot constants).

A primary design constraint is that the firmware must **not introduce its own
fingerprint** — any predictable artifact (fixed replay window, altered interval,
static eviction threshold, stable signal level on a stationary node) is avoided
in the relay behavior.

## License

[MIT](LICENSE) © 2026 Robert Capps.

## Contributing

Contributions welcome once the P1 capture foundation lands. Please keep the
hardware-agnostic abstractions (the radio HAL) clean so the firmware can be
ported beyond the XIAO, and respect the intended-use boundaries above.
