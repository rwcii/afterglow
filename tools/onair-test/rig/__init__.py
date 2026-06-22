"""Shared on-air rig framework.

Centralises the bits the on-air harnesses previously duplicated: the serial
``Board`` class, role/port resolution, stabilization helpers (eventually /
collect-until / retry / quorum), port reclaim + flash, the shared line regexes
and small numeric helpers, and the separability evaluator.
"""
from .board import Board, BAUD
from .roles import Roles, RigConfig
from .stabilize import (
    assert_eventually,
    collect_until,
    retry,
    quorum,
    QuorumError,
)
from .ports import reclaim_port, fuser_k_enabled, flash, flash_command
from .separability import ADV_RE, EMIT_RE


def median(xs):
    """Median of a numeric sequence (lower-of-two for even length is averaged)."""
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


__all__ = [
    "Board",
    "BAUD",
    "Roles",
    "RigConfig",
    "assert_eventually",
    "collect_until",
    "retry",
    "quorum",
    "QuorumError",
    "reclaim_port",
    "fuser_k_enabled",
    "flash",
    "flash_command",
    "ADV_RE",
    "EMIT_RE",
    "median",
]
