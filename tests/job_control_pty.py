#!/usr/bin/env python3
"""PTY integration tests for interactive Unix job control using only stdlib."""

import os
import pty
import re
import select
import signal
import sys
import tempfile
import time

SHELL = os.path.abspath(sys.argv[1])


class PtyShell:
    def __init__(self, executable: str):
        pid, fd = pty.fork()
        if pid == 0:
            os.execv(executable, [executable])
        self.pid = pid
        self.fd = fd
        self.buffer = b""

    def send(self, data: bytes) -> None:
        os.write(self.fd, data)

    def line(self, text: str) -> None:
        self.send(text.encode() + b"\n")

    def control(self, code: int) -> None:
        self.send(bytes([code]))

    def expect(self, pattern: bytes, timeout: float = 5.0) -> re.Match[bytes]:
        regex = re.compile(pattern, re.DOTALL)
        deadline = time.monotonic() + timeout
        while True:
            match = regex.search(self.buffer)
            if match:
                self.buffer = self.buffer[match.end():]
                return match
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(
                    f"timed out waiting for {pattern!r}; buffered={self.buffer!r}"
                )
            readable, _, _ = select.select([self.fd], [], [], remaining)
            if not readable:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                chunk = b""
            if not chunk:
                raise AssertionError(
                    f"shell exited while waiting for {pattern!r}; buffered={self.buffer!r}"
                )
            self.buffer += chunk

    def close(self) -> None:
        try:
            os.write(self.fd, b"exit 0\n")
        except OSError:
            pass
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
            if pid == self.pid:
                return
            time.sleep(0.02)
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass


session = PtyShell(SHELL)
try:
    session.expect(rb"\$ ")

    # Background launch + jobs listing.
    session.line("sleep 5 &")
    first = session.expect(rb"\[(\d+)\] (\d+)\r?\n")
    job1 = first.group(1)
    session.expect(rb"\$ ")
    session.line("jobs")
    session.expect(rb"\[" + job1 + rb"\]\+ Running\s+sleep 5\r?\n")
    session.expect(rb"\$ ")

    # Foreground -> Ctrl+Z -> jobs -> bg -> fg -> Ctrl+C.
    session.line("fg %" + job1.decode())
    session.expect(rb"sleep 5\r?\n")
    time.sleep(0.1)
    session.control(0x1A)  # Ctrl+Z / SIGTSTP
    session.expect(rb"\[" + job1 + rb"\] Stopped\s+sleep 5\r?\n")
    session.expect(rb"\$ ")

    session.line("jobs")
    session.expect(rb"\[" + job1 + rb"\]\+ Stopped\s+sleep 5\r?\n")
    session.expect(rb"\$ ")

    session.line("bg %" + job1.decode())
    session.expect(rb"\[" + job1 + rb"\] sleep 5 &\r?\n")
    session.expect(rb"\$ ")

    session.line("fg %" + job1.decode())
    session.expect(rb"sleep 5\r?\n")
    time.sleep(0.1)
    session.control(0x03)  # Ctrl+C / SIGINT
    session.expect(rb"\$ ")

    # Ctrl+C at the shell prompt must discard, not execute, the current line.
    with tempfile.TemporaryDirectory() as td:
        marker = os.path.join(td, "should-not-exist")
        session.send(("touch " + marker).encode())
        time.sleep(0.1)
        session.control(0x03)
        session.expect(rb"\$ ")
        if os.path.exists(marker):
            raise AssertionError("Ctrl+C at prompt executed the discarded command")

    # A multi-process pipeline is managed as one process group/job.
    session.line("sleep 5 | cat &")
    second = session.expect(rb"\[(\d+)\] (\d+)\r?\n")
    job2 = second.group(1)
    session.expect(rb"\$ ")
    session.line("jobs")
    session.expect(rb"\[" + job2 + rb"\]\+ Running\s+sleep 5 \| cat\r?\n")
    session.expect(rb"\$ ")
    session.line("fg %" + job2.decode())
    session.expect(rb"sleep 5 \| cat\r?\n")
    time.sleep(0.1)
    session.control(0x03)
    session.expect(rb"\$ ")

    # Finished background jobs are reaped and reported at the next safe point.
    session.line("sleep 0.2 &")
    session.expect(rb"\[(\d+)\] (\d+)\r?\n")
    session.expect(rb"\$ ")
    time.sleep(0.4)
    session.line("echo job-control-alive")
    session.expect(rb"\[\d+\] Done\s+sleep 0.2\r?\n")
    session.expect(rb"job-control-alive\r?\n")
    session.expect(rb"\$ ")

    session.line("exit 0")
    os.waitpid(session.pid, 0)
    session.pid = -1
finally:
    if session.pid > 0:
        session.close()

print("job-control PTY tests passed")
