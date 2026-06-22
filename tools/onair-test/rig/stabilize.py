"""Stabilization helpers: bound timing flakiness once, in the framework.

On-air tests race a physical radio against a finite capture window. These
helpers centralise the patterns the harnesses need: poll until a condition
holds (``assert_eventually``), gather a stream until a predicate is satisfied or
a deadline passes (``collect_until``), retry a flaky callable (``retry``), and
require a quorum of independent runs to pass (``quorum``).
"""
import time


class QuorumError(AssertionError):
    """Raised when fewer than K of N quorum runs passed."""


def assert_eventually(predicate, timeout, interval=0.1, message="condition not met"):
    """Poll ``predicate`` until it returns truthy or ``timeout`` elapses.

    Returns the truthy value. Raises AssertionError with ``message`` on timeout.
    """
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(message)


def collect_until(step, done, timeout, interval=0.05):
    """Drive ``step()`` repeatedly until ``done()`` is truthy or the deadline.

    ``step`` advances the collection (e.g. drains serial lines into an
    accumulator); ``done`` inspects the accumulator. Returns True if ``done``
    was satisfied before the deadline, False if the window expired first.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        step()
        if done():
            return True
        time.sleep(interval)
    return False


def retry(fn, attempts, on_error=None):
    """Call ``fn`` up to ``attempts`` times, returning its first success.

    A run "fails" if it raises. ``attempts`` is total tries (so attempts=1 means
    no retry). Re-raises the last exception if every attempt fails. ``on_error``,
    if given, is called with (attempt_index, exception) after each failure.
    """
    if attempts < 1:
        raise ValueError("attempts must be >= 1")
    last_exc = None
    for i in range(attempts):
        try:
            return fn()
        except Exception as exc:  # noqa: BLE001 - re-raised below
            last_exc = exc
            if on_error is not None:
                on_error(i, exc)
    raise last_exc


def quorum(fn, k, n, retries=0, on_result=None):
    """Run ``fn`` ``n`` times (each up to ``1 + retries`` tries) and require that
    at least ``k`` runs pass.

    A run passes if any of its tries returns without raising. Returns the list of
    per-run pass/fail booleans. Raises QuorumError if fewer than ``k`` passed.
    ``on_result`` is called with (run_index, passed, exception_or_None).
    """
    if n < 1:
        raise ValueError("quorum n must be >= 1")
    if k < 1:
        raise ValueError("quorum k must be >= 1")
    if k > n:
        raise ValueError("quorum k must be <= n")
    passed = []
    for run in range(n):
        last_exc = None
        ok = False
        for _ in range(1 + retries):
            try:
                fn()
                ok = True
                last_exc = None
                break
            except Exception as exc:  # noqa: BLE001 - aggregated into quorum verdict
                last_exc = exc
        passed.append(ok)
        if on_result is not None:
            on_result(run, ok, last_exc)
        # Stop early once the verdict is decided: enough have passed to meet the
        # quorum, or too many have failed for it to still be reachable. Each run
        # is an expensive physical-radio capture, so this saves real time.
        n_pass = sum(passed)
        remaining = n - (run + 1)
        if n_pass >= k:
            break
        if n_pass + remaining < k:
            break
    n_pass = sum(passed)
    if n_pass < k:
        raise QuorumError(
            f"quorum not met: {n_pass}/{len(passed)} runs ran, "
            f"{n_pass} passed, needed {k} of {n}"
        )
    return passed
