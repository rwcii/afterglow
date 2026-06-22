"""Pytest fixtures for the on-air rig.

A test declares the board roles it needs with the ``rig_roles`` marker:

    @pytest.mark.rig_roles("dut", "stimulus", "observer")
    def test_x(rig): ...

The ``rig`` fixture resolves each declared role to a serial port from the
environment, opens only the declared boards, and green-skips (not fails) when any
declared role has no port configured — so the on-air suite runs cleanly on host
CI with no hardware attached. An autouse fixture reclaims the declared ports
first when reclaim is opted into (AG_RIG_FUSER_K=1).
"""
import sys
from pathlib import Path

import pytest

# Make ``rig`` and the sibling test modules importable when pytest is pointed at
# this directory from anywhere (the package is not installed).
sys.path.insert(0, str(Path(__file__).resolve().parent))

from rig import Board, RigConfig, Roles, reclaim_port  # noqa: E402


def _declared_roles(request):
    marker = request.node.get_closest_marker("rig_roles")
    if marker is None:
        return []
    return [Roles(r) for r in marker.args]


@pytest.fixture(scope="module")
def rig_config():
    return RigConfig()


@pytest.fixture(autouse=True)
def reclaim_ports(request, rig_config):
    """Opt-in: reclaim the declared roles' ports before the test (AG_RIG_FUSER_K)."""
    for role in _declared_roles(request):
        port = rig_config.port_for(role)
        if port:
            reclaim_port(port)
    yield


def _open_roles(request, rig_config):
    """Generator that green-skips on missing ports, opens the declared roles'
    boards, yields {Roles: Board}, and closes them on teardown. Shared by the
    function-scoped and module-scoped rig fixtures so both have identical
    open/skip/teardown semantics."""
    roles = _declared_roles(request)
    if not roles:
        pytest.skip("test declares no rig_roles marker")

    missing = rig_config.missing(roles)
    if missing:
        names = ", ".join(
            f"{r.value} ({'/'.join(rig_config.env_names(r))})" for r in missing
        )
        pytest.skip(f"no board configured for rig role(s): {names}")

    boards = {}
    try:
        for role in roles:
            boards[role] = Board(rig_config.port_for(role))
        yield boards
    finally:
        for b in boards.values():
            try:
                b.close()
            except Exception:  # noqa: BLE001 - best-effort teardown
                pass


@pytest.fixture
def rig(request, rig_config):
    """Open the boards for the test's declared roles, or green-skip if any port is
    unconfigured. Yields {Roles: Board} and closes every opened board after."""
    yield from _open_roles(request, rig_config)


@pytest.fixture(scope="module")
def rig_module(request, rig_config):
    """Module-scoped variant of ``rig``: opens the declared roles' boards once for
    the whole test module and keeps the session open across its tests. Use this
    for a suite that builds up shared on-device state across its tests."""
    yield from _open_roles(request, rig_config)
