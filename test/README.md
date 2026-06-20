# Afterglow tests

Testing is split so the privacy-critical logic is exercised fast on every push,
and the hardware-coupled paths are exercised on the real target.

## Layout

```
test/
  host/            host-native unit tests (plain CMake + CTest, no ESP-IDF)
    test_eviction.c    combined eviction model
    test_txwalk.c      RSSI random walk / signal-level variance
    test_subset.c      composite subset-selection score
    test_meshguard.c   mesh TTL / origin / LRU-seen guards
    separability/
      detector.[ch]          two-sample statistical metrics
      test_separability.c    generated-vs-reference distribution checks
      test_eligibility.c     replay-eligibility predicate
  target/          on-target ESP-IDF + Unity, driven by pytest-embedded
```

The portable, privacy-critical math lives in `components/afterglow_core`
(zero ESP-IDF dependencies). The host tests compile **the exact sources that
ship on-device**, so a green host run reflects on-device behavior; the
`[core]` Unity case re-checks one property on the chip to prove the port.

## Run the host tests

```sh
cmake -S test/host -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host -j
ctest --test-dir build/host --output-on-failure
```

No external dependencies — just CMake + a C compiler + libm.

## Run the on-target tests

```sh
idf.py -C test/target set-target esp32s3 build
pytest test/target --embedded-services idf,qemu          # under QEMU
pytest test/target --embedded-services idf --port <dev>  # on a real XIAO
```

## What the separability harness asserts

The metrics in `separability/detector.c` are standard two-sample statistical
tools. The tests generate a stream with `afterglow_core` alongside a reference
device stream and assert each metric stays **within** a configured bound:

- **KS test on RSSI** — the power walk keeps the generated signal distribution
  close to the reference moving device's.
- **RSSI stability** — output at constant power has near-zero RSSI variance; the
  walk keeps variance comparable to the reference moving device.
- **Interval regularity (CV)** — a fixed-cadence stream has CV ≈ 0; the
  generated cadence matches the reference interval *with* jitter.
- **1-D clustering** — no single feature splits the pooled stream into two
  clean groups.

These are **necessary, not sufficient** checks along the listed feature axes.

## Eligibility predicate

`test_eligibility.c` covers the replay-eligibility predicate
(`ag_replay_eligible`): with the broadcast-only policy enabled, only
broadcast-only (non-connectable, non-scannable) advertisements are eligible,
non-reproducible address classes are refused, and Wi-Fi eligibility obeys its
master switch.
