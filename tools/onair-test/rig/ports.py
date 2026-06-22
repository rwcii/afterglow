"""Serial-port housekeeping for the rig: opt-in reclaim and a flash helper.

A previous run (or a stray monitor) can hold a board's serial port open and make
the next open fail. ``reclaim_port`` optionally kills whatever holds a port via
``fuser -k`` — opt-in because killing processes is intrusive; it is enabled with
AG_RIG_FUSER_K=1. ``flash`` wraps the idf.py flash invocation used to load
firmware onto a board for a role.
"""
import os
import shlex
import subprocess


def fuser_k_enabled(env=None):
    """True if port reclaim via ``fuser -k`` is opted into (AG_RIG_FUSER_K=1)."""
    env = env if env is not None else os.environ
    return env.get("AG_RIG_FUSER_K", "0") not in ("0", "", "false", "False")


def reclaim_port(port, env=None):
    """If reclaim is opted in, kill any process holding ``port`` via ``fuser -k``.

    No-op when AG_RIG_FUSER_K is unset, when ``port`` is falsy/absent, or when
    ``fuser`` is unavailable. Never raises on a port that nobody holds.
    """
    if not port or not fuser_k_enabled(env):
        return False
    if not os.path.exists(port):
        return False
    try:
        # `fuser -k <port>` exits nonzero when no process holds the port; that is
        # the common, healthy case, so a nonzero exit is not an error here.
        subprocess.run(
            ["fuser", "-k", port],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return True
    except FileNotFoundError:
        return False


def flash(build_dir, port, idf_path=None, check=True):
    """Flash the firmware in ``build_dir`` to the board on ``port`` via idf.py.

    Returns the completed process. Requires ESP-IDF on PATH (or pass an explicit
    ``idf_path`` to the idf.py script). Intended for a future runner that flashes
    boards itself; the current tests assume firmware is already flashed.
    """
    idf = idf_path or "idf.py"
    cmd = [idf, "-B", build_dir, "-p", port, "flash"]
    return subprocess.run(cmd, check=check)


def flash_command(build_dir, port, idf_path=None):
    """Return the shell string for the flash invocation (for logging/docs)."""
    idf = idf_path or "idf.py"
    return shlex.join([idf, "-B", build_dir, "-p", port, "flash"])
