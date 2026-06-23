"""Rig roles and environment-driven port configuration.

Each on-air test declares which board roles it needs (DUT, stimulus, observer,
and the two mesh peers). ``RigConfig`` resolves each role to a serial port from
the environment, honouring the historical per-harness env var names as aliases
so existing run commands keep working.
"""
import enum
import os


class Roles(str, enum.Enum):
    """The board roles a test can declare."""

    DUT = "dut"
    STIMULUS = "stimulus"
    OBSERVER = "observer"
    PEER_A = "peer_a"
    PEER_B = "peer_b"


# Each role resolves to the first environment variable that is set, in order.
# The canonical AG_<ROLE>_PORT name comes first; the remaining names are the
# back-compat aliases the individual harnesses used before the shared rig.
_ROLE_ENV = {
    Roles.DUT: ("AG_DUT_PORT",),
    Roles.STIMULUS: ("AG_STIMULUS_PORT", "AG_STIM_PORT"),
    Roles.OBSERVER: ("AG_OBSERVER_PORT", "AG_OBS_PORT"),
    Roles.PEER_A: ("AG_PEER_A_PORT", "AG_MESH_A_PORT"),
    Roles.PEER_B: ("AG_PEER_B_PORT", "AG_MESH_B_PORT"),
}


class RigConfig:
    """Resolve declared roles to serial ports from the environment."""

    def __init__(self, env=None):
        self.env = env if env is not None else os.environ

    def port_for(self, role):
        """Return the configured port for a role, or None if no env var is set."""
        role = Roles(role)
        for name in _ROLE_ENV[role]:
            val = self.env.get(name)
            if val:
                return val
        return None

    def env_names(self, role):
        """The environment variable names consulted for a role (canonical first)."""
        return _ROLE_ENV[Roles(role)]

    def require_port(self, role):
        """Return the configured port for a role, or raise a clear error naming the
        env var(s) to set. Used by the standalone run() entry points so an unset
        port surfaces as an actionable message rather than an opaque serial error."""
        role = Roles(role)
        port = self.port_for(role)
        if not port:
            names = "/".join(_ROLE_ENV[role])
            raise SystemExit(
                f"no serial port configured for rig role '{role.value}'; "
                f"set {names}"
            )
        return port

    def resolve(self, roles):
        """Map each requested role to a port; missing roles map to None."""
        return {Roles(r): self.port_for(r) for r in roles}

    def missing(self, roles):
        """The subset of requested roles that have no configured port."""
        return [Roles(r) for r in roles if self.port_for(r) is None]
