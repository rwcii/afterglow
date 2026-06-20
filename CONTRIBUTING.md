# Contributing to Afterglow

## Branch model

```
feature/*  ──(squash PR)──▶  develop  ──(PR)──▶  main
```

- **`main`** — release branch. Always builds and passes CI. Protected.
- **`develop`** — integration branch. Protected.
- **`feature/*`** (also `fix/*`, `chore/*`) — short-lived working branches off `develop`.

### Rules (enforced by branch protection)

- **No direct pushes** to `main` or `develop`. All changes land via pull request.
- **`feature/*` → `develop`**: merged with **squash** (one commit per feature on `develop`).
- **`develop` → `main`**: merged via PR (a normal merge, promoting integrated work to release).
- **Branches are deleted automatically** when their PR merges.
- **CI must pass** before merge: host tests + the esp32s3 firmware build.
- At least **one approving review** is required.

## Typical workflow

```sh
git switch develop && git pull
git switch -c feature/my-change

# ...work, commit...

git push -u origin feature/my-change
gh pr create --base develop --fill        # open the PR into develop
# CI runs; on green + approval, squash-merge. The branch auto-deletes.
```

Promoting a release:

```sh
gh pr create --base main --head develop --title "Release: <summary>"
```

## Commit messages

- Imperative subject (`Add ...`, `Fix ...`), wrapped body explaining the *why*.
- **No automated attribution** — no `Co-Authored-By`, no tool/assistant signatures.
- Commits are **signed** (SSH signing is configured repo-wide).

## Local checks before opening a PR

```sh
# Host-native unit tests (no ESP-IDF needed):
cmake -S test/host -B build/host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host -j
ctest --test-dir build/host --output-on-failure

# Firmware build for the target (needs ESP-IDF v5.3.x):
. "$IDF_PATH/export.sh"
idf.py set-target esp32s3
idf.py build
```

## Project layout

- `components/afterglow_core/` — portable, host-tested algorithm core (zero ESP-IDF deps).
- `components/*/` — each component is split into portable `*_logic.c` (host-tested)
  and a thin hardware wrapper that supplies clock/RNG/radio.
- `test/host/` — host-native unit + statistical tests; `test/host/shim/` provides
  thin ESP-IDF stubs so component logic compiles off-target.
- `test/target/` — on-target Unity suite (QEMU / real board).
