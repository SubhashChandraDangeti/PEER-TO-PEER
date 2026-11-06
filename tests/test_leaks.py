#!/usr/bin/env python3
"""test_leaks.py - run both trackers and two clients under valgrind through a
full scenario (accounts, group, upload, download, check_sync, logout), shut
everything down cleanly with `quit`, and require:
  - "definitely lost: 0 bytes" in every process
  - no invalid reads/writes (ERROR SUMMARY: 0 errors)
  - every process exits by itself after quit (all threads joined).
"""
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p2plib import CLIENT_BIN, TRACKER_BIN, Proc, Results, tmpdir, wait_port, write_tracker_info

BASE = 19400


def vg(d, name):
    return ["valgrind", "--leak-check=full", "--errors-for-leak-kinds=definite",
            f"--log-file={os.path.join(d, name + '.vg')}"]


def main():
    if not shutil.which("valgrind"):
        print("valgrind not installed; skipping")
        return
    d = tmpdir("p2p-vg-")
    info = write_tracker_info(d, BASE)
    R = Results("valgrind")
    t1 = Proc(vg(d, "t1") + [TRACKER_BIN, info, "1"], os.path.join(d, "t1.log"))
    t2 = Proc(vg(d, "t2") + [TRACKER_BIN, info, "2"], os.path.join(d, "t2.log"))
    R.check(t2.wait_for("Recovery complete", 60), "trackers paired under valgrind")
    src = os.path.join(d, "file.bin")
    with open(src, "wb") as f:
        f.write(os.urandom(3 * 1024 * 1024 + 17))

    cs = []
    for n, u in ((1, "a"), (2, "b")):
        addr = f"127.0.0.1:{BASE + 500 + n}"
        os.makedirs(os.path.join(d, u))
        c = Proc(vg(d, "c" + u) + [CLIENT_BIN, addr, info], os.path.join(d, f"c{u}.log"), cwd=os.path.join(d, u))
        R.check(c.wait_for("client listening", 60), f"client {u} started")
        c.send(f"create_user {u} pw")
        c.send(f"login {u} pw")
        R.check(c.wait_for("logged in", 60), f"{u} logged in")
        cs.append(c)
    a, b = cs
    a.send("create_group g")
    R.check(a.wait_for("group g created", 60), "create group")
    b.send("join_group g")
    R.check(b.wait_for("join request sent", 60), "join")
    a.send("accept_request g b")
    R.check(a.wait_for("now a member", 60), "accept")
    a.send(f"upload_file g {src}")
    R.check(a.wait_for("uploaded file.bin", 60), "upload")
    b.send("download_file g file.bin .")
    deadline = time.time() + 120
    while time.time() < deadline:
        b.send("show_downloads")
        time.sleep(1)
        if "[C] [g] file.bin" in b.output():
            break
    R.check("[C] [g] file.bin" in b.output(), "download completed under valgrind")
    t1.send("check_sync")
    R.check(t1.wait_for("IN SYNC", 30), "check_sync IN SYNC")
    a.send("logout")
    R.check(a.wait_for("logged out", 30), "logout")

    for p in (a, b, t2, t1):
        p.quit(timeout=60)
        R.check(not p.alive(), f"{os.path.basename(p.log_path)} exited after quit (threads joined)")
    for name in ("t1", "t2", "ca", "cb"):
        log = open(os.path.join(d, name + ".vg")).read()
        m = re.search(r"definitely lost: ([\d,]+) bytes", log)
        lost = m.group(1) if m else "0"  # no line at all = no leaks of any kind
        e = re.search(r"ERROR SUMMARY: (\d+) errors", log)
        print(f"  {name}: definitely lost {lost} bytes, {e.group(1) if e else '?'} errors")
        R.eq(lost, "0", f"{name} definitely lost")
        R.check(e is not None and e.group(1) == "0", f"{name} valgrind error summary is 0")
    ok = R.done()
    if ok:
        shutil.rmtree(d, ignore_errors=True)
    else:
        print("logs kept in", d)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
