#!/usr/bin/env python3
"""Stress lifecycle invariants of the shell without relying on timing-only guesses."""

from __future__ import annotations

import os
import selectors
import signal
import subprocess
import sys
import time

SHELL = os.path.abspath(sys.argv[1])


def fd_count(pid: int) -> int:
    return len(os.listdir(f"/proc/{pid}/fd"))


def child_pids(pid: int) -> list[int]:
    path = f"/proc/{pid}/task/{pid}/children"
    try:
        data = open(path, encoding="utf-8").read().strip()
    except FileNotFoundError:
        return []
    return [int(value) for value in data.split()] if data else []


def wait_for_marker(proc: subprocess.Popen[bytes], marker: bytes, timeout: float = 20.0) -> bytes:
    assert proc.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout
    collected = bytearray()

    while time.monotonic() < deadline:
        events = selector.select(timeout=0.2)
        for key, _ in events:
            chunk = os.read(key.fileobj.fileno(), 65536)
            if not chunk:
                raise RuntimeError(
                    f"shell exited before marker {marker!r}; output={bytes(collected)!r}"
                )
            collected.extend(chunk)
            if marker in collected:
                return bytes(collected)
    raise TimeoutError(f"timed out waiting for {marker!r}; output={bytes(collected)!r}")


proc = subprocess.Popen(
    [SHELL],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    start_new_session=True,
)
assert proc.stdin is not None
baseline_fds = fd_count(proc.pid)

try:
    commands: list[str] = []

    # Repeated fork/exec and pipe construction catches descriptor lifecycle
    # mistakes that small regression tests rarely expose.
    commands.extend("/bin/true" for _ in range(300))
    commands.extend("printf x | cat > /dev/null" for _ in range(200))

    # Exercise a materially longer pipeline than ordinary feature tests.
    long_pipeline = "printf x" + " | cat" * 48 + " > /dev/null"
    commands.append(long_pipeline)

    # Launch many short background jobs, then wait long enough for SIGCHLD
    # handling/event-hook reaping before inspecting /proc.
    commands.extend("sleep 0.01 &" for _ in range(100))
    commands.append("sleep 0.5")
    commands.append("echo __LIFECYCLE_QUIESCENT__")

    proc.stdin.write(("\n".join(commands) + "\n").encode())
    proc.stdin.flush()
    wait_for_marker(proc, b"__LIFECYCLE_QUIESCENT__")

    # The shell should return to its steady-state descriptor footprint after
    # hundreds of pipelines. Allow one descriptor of implementation variance
    # across Readline/libc versions, but reject monotonic leakage.
    after_fds = fd_count(proc.pid)
    if after_fds > baseline_fds + 1:
        raise AssertionError(
            f"file descriptor leak: baseline={baseline_fds}, after={after_fds}"
        )

    deadline = time.monotonic() + 3.0
    remaining: list[int] = []
    while time.monotonic() < deadline:
        remaining = child_pids(proc.pid)
        if not remaining:
            break
        time.sleep(0.05)
    if remaining:
        raise AssertionError(f"unreaped child processes remain: {remaining}")

    proc.stdin.write(b"echo __LIFECYCLE_ALIVE__\nexit 0\n")
    proc.stdin.flush()
    output = wait_for_marker(proc, b"__LIFECYCLE_ALIVE__")
    if b"__LIFECYCLE_ALIVE__" not in output:
        raise AssertionError("shell did not survive stress workload")

    return_code = proc.wait(timeout=5)
    if return_code != 0:
        stderr = proc.stderr.read() if proc.stderr else b""
        raise AssertionError(f"shell exited with {return_code}: {stderr!r}")
finally:
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=5)

print("lifecycle stress test passed")
