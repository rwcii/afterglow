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
- **`develop` → `main`**: merged via PR using a **merge commit** (the *Create a merge commit* button) — **never squash for this step**. A merge commit carries `develop`'s commits onto `main` with shared history, so the two branches stay reconciled automatically. Squashing `develop` into `main` would create a new commit on `main` that `develop` cannot reach, leaving the branches permanently "diverged" (a non-zero `main..develop` count) even when their code is byte-identical — and forcing a manual catch-up. Use merge-commit and this never happens.
- **Branches are deleted automatically** when their PR merges — but **do NOT delete `develop`**; it is permanent.
- **CI must pass** before merge: host tests + the esp32s3 firmware build.
- **`feature/*` → `develop`** requires **one approving review**; **`develop` → `main`** requires none (it promotes already-reviewed integrated work).

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
# Merge this PR with the "Create a merge commit" button — NOT squash (see Branch model).
```

### Checking whether `main` and `develop` agree

The commit-ahead count is **not** a reliable signal of whether code is missing —
a squash or merge-commit can leave a non-zero count while the code is identical.
Always check the *tree* (actual file content), not the commit count:

```sh
git fetch origin
git diff --stat origin/main origin/develop   # empty output ⇒ identical code, nothing missing
git rev-list --count origin/main..origin/develop   # commit-count only; can be non-zero even when code matches
```

If `git diff --stat origin/main origin/develop` is empty, the branches contain
the same code and **there is nothing to fix**, regardless of the ahead-count.

### Recovering from a diverged history (rare)

If a `develop → main` promotion was ever squashed (or done outside the merge-commit
flow), `develop` and `main` may diverge in history while their code stays identical.
To realign, reset `develop` to `main`'s tip (lossless when the tree diff is empty):

```sh
# Owner-only: develop is protected. Temporarily allow force-push on develop,
# then restore protection immediately after.
git fetch origin
git push --force-with-lease origin origin/main:refs/heads/develop
```

This is a deliberate, owner-gated step — do not improvise it mid-task. Prefer
preventing the situation by always using merge-commit for `develop → main`.

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
