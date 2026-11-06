#!/usr/bin/env python3
"""test_download.py - real tracker + 5 real clients.

Covers: byte-exact downloads across sizes and file types, multi-seeder
least-connections spread, concurrent downloads, the [Q] queue, a corrupt
seeder, a seeder killed mid-download, non-member access, stop_share and
logout. Set BIG=1 to add a 2.5 GB file (slow).
"""
import filecmp
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p2plib import CLIENT_BIN, TRACKER_BIN, Proc, Results, tmpdir, wait_port, write_tracker_info

BASE = 19000
MB = 1024 * 1024


class Client:
    def __init__(self, d, info, n, extra=()):
        # Linux routes all of 127.0.0.0/8 to loopback, so each client gets its
        # own address. macOS only has 127.0.0.1 unless aliases are added.
        ip = "127.0.0.1" if sys.platform == "darwin" else f"127.0.0.{n + 1}"
        self.addr = f"{ip}:{BASE + 500 + n}"
        self.dir = os.path.join(d, f"c{n}")
        os.makedirs(self.dir, exist_ok=True)
        self.p = Proc([CLIENT_BIN, self.addr, info, *extra], os.path.join(d, f"c{n}.log"), cwd=self.dir)
        assert self.p.wait_for("client listening"), self.p.output()

    def cmd(self, line, expect=None, timeout=20):
        start = len(self.p.output())
        self.p.send(line)
        if expect:
            ok = self.p.wait_for(expect, timeout=timeout, start=start)
            return ok
        time.sleep(0.3)
        return True

    def wait_done(self, group, name, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            start = len(self.p.output())
            self.p.send("show_downloads")
            time.sleep(0.4)
            out = self.p.output()[start:]
            if f"[C] [{group}] {name}" in out:
                return "C"
            if re.search(rf"\[F\] \[{group}\] {re.escape(name)}\b", out):
                return "F"
        return "timeout"


def make_files(src):
    os.makedirs(src, exist_ok=True)
    files = {}

    def rnd(name, size):
        p = os.path.join(src, name)
        with open(p, "wb") as f:
            left = size
            while left:
                n = min(left, 8 * MB)
                f.write(os.urandom(n))
                left -= n
        files[name] = p

    open(os.path.join(src, "empty.txt"), "w").close()
    files["empty.txt"] = os.path.join(src, "empty.txt")
    rnd("one_byte.bin", 1)
    rnd("exact_piece.bin", 512 * 1024)
    rnd("piece_plus_one.bin", 512 * 1024 + 1)
    rnd("random_10mb.bin", 10 * MB)
    rnd("random_100mb.bin", 100 * MB)
    for i in range(1, 4):  # big enough that 4 downloads overlap (queue test)
        rnd(f"queue_{i}.bin", 150 * MB)
    with open(os.path.join(src, "notes.txt"), "w") as f:
        f.write("line of text\n" * 50000)
    files["notes.txt"] = os.path.join(src, "notes.txt")
    # Media and archives made with standard tools (no copyrighted content).
    ff = shutil.which("ffmpeg")
    if ff:
        run = lambda *a: subprocess.run([ff, "-loglevel", "error", "-y", *a], check=True)
        run("-f", "lavfi", "-i", "testsrc=duration=20:size=640x360:rate=25", "-pix_fmt", "yuv420p",
            os.path.join(src, "video.mp4"))
        run("-f", "lavfi", "-i", "sine=frequency=440:duration=20", os.path.join(src, "audio.wav"))
        run("-f", "lavfi", "-i", "sine=frequency=660:duration=20", os.path.join(src, "audio.mp3"))
        run("-f", "lavfi", "-i", "testsrc=size=800x600", "-frames:v", "1", os.path.join(src, "image.png"))
        run("-f", "lavfi", "-i", "testsrc=size=800x600", "-frames:v", "1", os.path.join(src, "image.jpg"))
        for n in ("video.mp4", "audio.wav", "audio.mp3", "image.png", "image.jpg"):
            files[n] = os.path.join(src, n)
    shutil.make_archive(os.path.join(src, "archive"), "gztar", src, "notes.txt")
    files["archive.tar.gz"] = os.path.join(src, "archive.tar.gz")
    shutil.copy(CLIENT_BIN, os.path.join(src, "program.bin"))  # an executable
    files["program.bin"] = os.path.join(src, "program.bin")
    if os.environ.get("BIG"):
        rnd("big_2_5gb.bin", int(2.5 * 1024 * MB))
    return files


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(MB), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    d = tmpdir("p2p-t3-")
    info = write_tracker_info(d, BASE)
    t = Proc([TRACKER_BIN, info, "1"], os.path.join(d, "t1.log"))
    assert wait_port(f"127.0.0.1:{BASE}")
    time.sleep(4.5)
    R = Results("downloads")
    files = make_files(os.path.join(d, "src"))
    if os.environ.get("BIG"):
        files["big_2_5gb.bin"] = os.path.join(d, "src", "big_2_5gb.bin")

    A, B, C, D, E = (Client(d, info, n) for n in range(1, 6))
    for i, c in enumerate((A, B, C, D, E)):
        u = "abcde"[i]
        c.cmd(f"create_user {u} pw", "[")
        c.cmd(f"login {u} pw", "logged in")
    A.cmd("create_group team", "created")
    for c, u in ((B, "b"), (C, "c"), (D, "d")):
        c.cmd("join_group team", "[OK]")
        A.cmd(f"accept_request team {u}", "now a member")

    # A uploads everything.
    for name, path in files.items():
        R.check(A.cmd(f"upload_file team {path}", "[OK] uploaded", timeout=300), f"upload {name}")

    # E is not a member: cannot list or download.
    R.check(E.cmd("list_files team", "INVALID_ACTION"), "non-member list_files refused")
    E.cmd("download_file team notes.txt .", "queued")
    R.eq(E.wait_done("team", "notes.txt", 20), "F", "non-member download fails")

    # B and C download everything at the same time (concurrent downloads).
    # Queue the four largest first in one burst: only 3 can be active, so the
    # 4th must show [Q] while the others run.
    order = sorted(files, key=lambda n: -os.path.getsize(files[n]))
    start = len(B.p.output())
    for name in order[:4]:
        B.p.send(f"download_file team {name} .")
    time.sleep(0.1)  # let the 3 slot threads pick up their jobs
    B.p.send("show_downloads")
    time.sleep(0.5)
    out = B.p.output()[start:]
    R.check("[Q]" in out and out.count("[D]") == 3, "3 active [D] + extra download waits as [Q]: " + out.replace("\n", " / "))
    for name in order[4:]:
        B.cmd(f"download_file team {name} .", "queued")
    for name in order:
        C.cmd(f"download_file team {name} .", "queued")

    for name, path in files.items():
        for who, c in (("B", B), ("C", C)):
            st = c.wait_done("team", name, timeout=900)
            R.eq(st, "C", f"{who} download {name}")
            got = os.path.join(c.dir, name)
            R.check(os.path.exists(got) and filecmp.cmp(path, got, shallow=False), f"{who} {name} byte-exact")
    R.check(not any(f.endswith(".part") for f in os.listdir(B.dir)), "no .part files left")

    # D downloads from three seeders: least-connections spreads the pieces.
    D.cmd("download_file team random_100mb.bin .", "queued")
    R.eq(D.wait_done("team", "random_100mb.bin"), "C", "D 3-seeder download")
    m = re.search(r"random_100mb.bin complete: 200 pieces, per seeder: (.*)", D.p.output())
    R.check(m is not None, "D logs per-seeder spread")
    if m:
        counts = [int(x.split("=")[1]) for x in m.group(1).split(", ")]
        print(f"  spread over {len(counts)} seeders: {m.group(1)}")
        R.check(len(counts) == 3 and min(counts) >= 30, f"all 3 seeders used ({m.group(1)})")
    R.eq(sha1(os.path.join(D.dir, "random_100mb.bin")), sha1(files["random_100mb.bin"]), "D 100MB hash")

    # The downloaded executable still runs (one wrong byte would break it).
    prog = os.path.join(B.dir, "program.bin")
    os.chmod(prog, 0o755)
    r = subprocess.run([prog], capture_output=True, text=True)
    R.check("usage: ./client" in r.stderr, "downloaded executable runs")

    # Corrupt seeder: B stops sharing, a corrupting client F joins as seeder.
    F = Client(d, info, 7, extra=("--corrupt",))
    F.cmd("create_user f pw", "[")
    F.cmd("login f pw", "logged in")
    F.cmd("join_group team", "[OK]")
    A.cmd("accept_request team f", "now a member")
    F.cmd("upload_file team " + files["random_10mb.bin"], "[OK]")
    E_dir = os.path.join(d, "dl_corrupt")
    os.makedirs(E_dir)
    D.cmd(f"download_file team random_10mb.bin {E_dir}/r10.bin", "queued")
    R.eq(D.wait_done("team", "random_10mb.bin"), "C", "download with a corrupt seeder present")
    R.check(filecmp.cmp(files["random_10mb.bin"], f"{E_dir}/r10.bin", shallow=False),
            "corrupt pieces rejected, file intact")
    F.p.kill()

    # Seeder killed mid-download: E2 (new member) downloads 100MB, C dies.
    G = Client(d, info, 8)
    G.cmd("create_user g pw", "[")
    G.cmd("login g pw", "logged in")
    G.cmd("join_group team", "[OK]")
    A.cmd("accept_request team g", "now a member")
    G.cmd("download_file team random_100mb.bin .", "queued")
    time.sleep(0.15)
    C.p.kill()
    R.eq(G.wait_done("team", "random_100mb.bin"), "C", "download survives a seeder dying")
    R.eq(sha1(os.path.join(G.dir, "random_100mb.bin")), sha1(files["random_100mb.bin"]), "G 100MB hash")

    # stop_share and logout remove only that seeder.
    B.cmd("stop_share team notes.txt", "stopped sharing")
    R.check(D.cmd("list_files team", "notes.txt"), "file still listed after one seeder stops")
    A.cmd("logout", "logged out")
    time.sleep(0.3)
    for c in (A, B, D, E, G):
        c.p.quit()
    t.quit()
    ok = R.done()
    if ok and not os.environ.get("KEEP"):
        shutil.rmtree(d, ignore_errors=True)
    else:
        print("logs kept in", d)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
