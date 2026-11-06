#!/usr/bin/env python3
"""test_big.py - a 2.5 GB file: crosses the 2 GiB (INT32_MAX) offset
boundary, so it proves 64-bit offsets, and measures the peak memory of the
downloading client to show it does not grow with file size.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p2plib import Proc, Results, TRACKER_BIN, tmpdir, wait_port, write_tracker_info
from test_download import Client, sha1

SIZE = int(2.5 * 1024**3)


def peak_rss_mb(pid):
    """Peak resident memory (Linux /proc). On macOS, current RSS via ps."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) / 1024
    except OSError:
        pass
    out = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout
    return int(out.strip() or 0) / 1024


def main():
    d = tmpdir("p2p-big-")
    info = write_tracker_info(d, 19300)
    t = Proc([TRACKER_BIN, info, "1"], os.path.join(d, "t1.log"))
    assert wait_port("127.0.0.1:19300")
    time.sleep(4.5)
    R = Results("2.5 GB file")
    src = os.path.join(d, "big.bin")
    with open(src, "wb") as f:
        for _ in range(SIZE // (64 * 1024**2)):
            f.write(os.urandom(64 * 1024**2))
        f.write(os.urandom(SIZE % (64 * 1024**2)))

    A, B = Client(d, info, 1), Client(d, info, 2)
    for c, u in ((A, "a"), (B, "b")):
        c.cmd(f"create_user {u} pw", "[")
        c.cmd(f"login {u} pw", "logged in")
    A.cmd("create_group big", "created")
    B.cmd("join_group big", "[OK]")
    A.cmd("accept_request big b", "member")
    t0 = time.time()
    R.check(A.cmd(f"upload_file big {src}", "[OK] uploaded", timeout=600), "upload 2.5 GB (5120 pieces)")
    t1 = time.time()
    B.cmd("download_file big big.bin .", "queued")
    R.eq(B.wait_done("big", "big.bin", timeout=1200), "C", "download 2.5 GB")
    t2 = time.time()
    R.eq(sha1(os.path.join(B.dir, "big.bin")), sha1(src), "2.5 GB SHA-1 matches")
    a_rss, b_rss = peak_rss_mb(A.p.p.pid), peak_rss_mb(B.p.p.pid)
    print(f"  hash+upload {t1 - t0:.1f}s, download {t2 - t1:.1f}s ({SIZE / 1024**2 / (t2 - t1):.0f} MB/s)")
    print(f"  peak memory: uploader {a_rss:.1f} MB, downloader {b_rss:.1f} MB")
    R.check(b_rss < 100, f"downloader memory stays small ({b_rss:.1f} MB for a 2560 MB file)")
    A.p.quit()
    B.p.quit()
    t.quit()
    ok = R.done()
    shutil.rmtree(d, ignore_errors=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
