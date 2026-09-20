#!/usr/bin/env python3
"""PTY test ensuring pinfo --watch behaves like a foreground job."""

import os
import pty
import re
import select
import signal
import sys
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

    def line(self, text: str) -> None:
        os.write(self.fd, text.encode() + b"\n")

    def control(self, code: int) -> None:
        os.write(self.fd, bytes([code]))

    def expect(self, pattern: bytes, timeout: float = 5.0):
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
                chunk = os.read(self.fd, 8192)
            except OSError:
                chunk = b""
            if not chunk:
                raise AssertionError(
                    f"shell exited while waiting for {pattern!r}; buffered={self.buffer!r}"
                )
            self.buffer += chunk

    def close(self) -> None:
        try:
            self.line("exit 0")
        except OSError:
            pass
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            try:
                pid, _ = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return
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
    session.line("pinfo --watch self --interval 50")
    session.expect(rb"=== sample 1 ===\r?\nProcess \d+ \([^)]+\)")
    time.sleep(0.1)
    session.control(0x03)  # Ctrl+C must interrupt the pinfo child job.
    session.expect(rb"\$ ")

    session.line("echo observability-alive")
    session.expect(rb"observability-alive\r?\n")
    session.expect(rb"\$ ")

    session.line("exit 0")
    os.waitpid(session.pid, 0)
    session.pid = -1
finally:
    if session.pid > 0:
        session.close()

print("process-observability PTY test passed")
