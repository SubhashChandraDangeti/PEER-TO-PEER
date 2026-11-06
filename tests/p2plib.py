"""p2plib.py - shared helpers for the automated tests.

Speaks the wire protocol directly (4-byte big-endian length, 1-byte type,
body) so tests can send exactly the bytes they want, including malformed
ones, and start/stop trackers and clients as real processes.
"""
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKER_BIN = os.path.join(ROOT, "tracker", "tracker")
CLIENT_BIN = os.path.join(ROOT, "client", "client")
TEXT, PIECE = 1, 2


def send_frame(sock, body, ftype=TEXT):
    if isinstance(body, str):
        body = body.encode()
    sock.sendall(struct.pack(">IB", len(body) + 1, ftype) + body)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def recv_frame(sock):
    length, ftype = struct.unpack(">IB", recv_exact(sock, 5))
    return ftype, recv_exact(sock, length - 1)


class Conn:
    """A raw protocol connection to a tracker (acts like one client)."""

    def __init__(self, addr, timeout=10):
        host, port = addr.split(":")
        self.sock = socket.create_connection((host, int(port)), timeout=timeout)

    def req(self, body):
        send_frame(self.sock, body)
        _, data = recv_frame(self.sock)
        return data.decode()

    def code(self, body):
        return self.req(body).split("|")[0]

    def close(self):
        self.sock.close()


def write_tracker_info(dirname, base_port):
    path = os.path.join(dirname, "tracker_info.txt")
    with open(path, "w") as f:
        f.write(f"127.0.0.1:{base_port}:{base_port + 100}\n")
        f.write(f"127.0.0.1:{base_port + 1}:{base_port + 101}\n")
    return path


class Proc:
    """A tracker or client process with a writable stdin and a log file."""

    def __init__(self, args, log_path, cwd=None):
        self.log_path = log_path
        self.log = open(log_path, "w")
        self.p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.log,
                                  stderr=subprocess.STDOUT, cwd=cwd, text=True, bufsize=1)

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def output(self):
        self.log.flush()
        with open(self.log_path) as f:
            return f.read()

    def wait_for(self, text, timeout=15, start=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            out = self.output()
            if text in out[start:]:
                return True
            if self.p.poll() is not None:
                return text in self.output()[start:]
            time.sleep(0.05)
        return False

    def alive(self):
        return self.p.poll() is None

    def kill(self, sig=signal.SIGKILL):
        if self.alive():
            self.p.send_signal(sig)
            if sig in (signal.SIGKILL, signal.SIGTERM):
                self.p.wait(timeout=10)

    def quit(self, timeout=15):
        if self.alive():
            try:
                self.send("quit")
                self.p.wait(timeout=timeout)
            except Exception:
                self.kill()


def wait_port(addr, timeout=10):
    host, port = addr.split(":")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            socket.create_connection((host, int(port)), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


class Results:
    def __init__(self, name):
        self.name, self.passed, self.failed = name, 0, []

    def check(self, cond, label):
        if cond:
            self.passed += 1
        else:
            self.failed.append(label)
            print(f"  FAIL: {label}")

    def eq(self, got, want, label):
        self.check(got == want, f"{label}: got {got!r}, want {want!r}")

    def done(self):
        print(f"{self.name}: {self.passed} passed, {len(self.failed)} failed")
        return len(self.failed) == 0


def tmpdir(prefix):
    return tempfile.mkdtemp(prefix=prefix)
