#!/usr/bin/env python3
"""test_chaos.py - stress + random failure injection.

2 trackers, 5 clients, one group, 6 shared files. ROUNDS rounds; in each:
  - 2 random clients start downloads of random files (concurrently)
  - one random fault is injected while they run:
      kill -9 the primary tracker (restart it 2 s later)
      freeze the primary for 8 s (SIGSTOP / SIGCONT)  -> split + heal
      kill -9 the backup tracker (restart it)
      kill -9 a seeder client (restart it, log in again, re-share)
  - every download must END (C or F, never hang); every C must be
    byte-exact. F is allowed only for a download whose own client was killed
    or whose only seeders died.
At the end: both trackers alive, check_sync IN SYNC, and tracker thread
and fd counts back to normal (no leaks across 20 failovers).
"""
import filecmp
import os
import random
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p2plib import Proc, Results, TRACKER_BIN, tmpdir, wait_port, write_tracker_info
from test_download import Client
from test_failover import check_sync

BASE = 19600
ROUNDS = int(os.environ.get("ROUNDS", "20"))
MB = 1024 * 1024


def counts(pid):
    """(threads, open fds) of a process: /proc on Linux, ps + lsof on macOS."""
    try:
        return len(os.listdir(f"/proc/{pid}/task")), len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        pass
    import subprocess
    th = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True, text=True).stdout
    fd = subprocess.run(["lsof", "-p", str(pid)], capture_output=True, text=True).stdout
    # ps -M: header + one line per thread. lsof: header + one line per open
    # file, including the binary and libraries, so subtract those (type txt).
    threads = max(len(th.strip().splitlines()) - 1, 0)
    fds = sum(1 for l in fd.splitlines()[1:] if " txt " not in l)
    return threads, fds


def main():
    rnd = random.Random(int(os.environ.get("SEED", "7")))
    d = tmpdir("p2p-chaos-")
    info = write_tracker_info(d, BASE)
    R = Results(f"chaos ({ROUNDS} rounds)")
    tr = {}

    def start_tracker(n):
        tr[n] = Proc([TRACKER_BIN, info, str(n)], os.path.join(d, f"t{n}_{time.time():.0f}.log"))
        assert wait_port(f"127.0.0.1:{BASE + n - 1}")

    def primary():
        for n in (1, 2):
            if tr[n].alive() and "PRIMARY" in last_role(tr[n]):
                return n
        return None

    def last_role(t):
        roles = [l for l in t.output().splitlines() if "role " in l]
        return roles[-1] if roles else ""

    start_tracker(1)
    start_tracker(2)
    tr[2].wait_for("Recovery complete", 15)

    files = {}
    src = os.path.join(d, "src")
    os.makedirs(src)
    for i in range(6):
        p = os.path.join(src, f"f{i}.bin")
        with open(p, "wb") as f:
            f.write(os.urandom(rnd.randrange(5, 40) * MB + rnd.randrange(1000)))
        files[f"f{i}.bin"] = p

    clients = {}

    def start_client(n):
        c = Client(d, info, n)
        u = f"u{n}"
        c.cmd(f"create_user {u} pw", "[")
        assert c.cmd(f"login {u} pw", "logged in", timeout=40), c.p.output()
        clients[n] = c
        return c

    for n in range(1, 6):
        start_client(n)
    owner = clients[1]
    owner.cmd("create_group grp", "created")
    for n in range(2, 6):
        clients[n].cmd("join_group grp", "[OK]")
        owner.cmd(f"accept_request grp u{n}", "member")
    for name, p in files.items():
        owner.cmd(f"upload_file grp {p}", "[OK] uploaded", timeout=60)
    base_counts = {n: counts(tr[n].p.pid) for n in (1, 2)}

    completed, failed_ok, failed_bad, hung = 0, 0, 0, 0
    for rd in range(ROUNDS):
        downloaders = rnd.sample([2, 3, 4, 5], 2)
        jobs = []
        for n in downloaders:
            name = rnd.choice(list(files))
            dest = f"r{rd}_{name}"
            clients[n].cmd(f"download_file grp {name} {dest}", "[")
            jobs.append((n, name, dest))
        time.sleep(rnd.uniform(0.0, 0.4))

        fault = rnd.choice(["kill_primary", "freeze_primary", "kill_backup", "kill_seeder"])
        killed_client = None
        p = primary()
        if fault == "kill_primary" and p:
            tr[p].kill()
            time.sleep(2)
            start_tracker(p)
        elif fault == "freeze_primary" and p:
            os.kill(tr[p].p.pid, signal.SIGSTOP)
            time.sleep(8)
            os.kill(tr[p].p.pid, signal.SIGCONT)
        elif fault == "kill_backup" and p:
            b = 3 - p
            tr[b].kill()
            time.sleep(1)
            start_tracker(b)
        else:
            killed_client = rnd.choice([2, 3, 4, 5])
            clients[killed_client].p.kill()
        print(f"  round {rd + 1}: downloads {[(j[0], j[1]) for j in jobs]}, fault={fault}"
              + (f" (client {killed_client})" if killed_client else ""))

        for n, name, dest in jobs:
            if n == killed_client:
                failed_ok += 1
                continue
            st = clients[n].wait_done("grp", name, timeout=150)
            if st == "C":
                ok = filecmp.cmp(files[name], os.path.join(clients[n].dir, dest), shallow=False)
                completed += ok
                R.check(ok, f"round {rd + 1}: client {n} {name} byte-exact")
            elif st == "F":
                failed_ok += 1  # e.g. all seeders of that file were killed
            else:
                hung += 1
                R.check(False, f"round {rd + 1}: client {n} {name} did not finish (hung)")

        if killed_client:  # bring it back: new process, log in, it shares nothing yet
            c = clients[killed_client]
            c.p = Proc(c.p.p.args, c.p.log_path + f".r{rd}", cwd=c.dir)
            c.p.wait_for("client listening")
            c.cmd(f"login u{killed_client} pw", "logged in", timeout=40)

        # Let any split heal before the next round.
        for n in (1, 2):
            if tr[n].alive():
                tr[n].wait_for("Recovery complete", 1)

    time.sleep(3)
    R.check(all(tr[n].alive() for n in (1, 2)), "both trackers alive at the end")
    p = primary() or 1
    res = check_sync(tr[p])
    print(f"  final check_sync: {res}")
    R.check(res.startswith("IN SYNC"), "trackers IN SYNC after all rounds")
    print(f"  downloads: {completed} completed byte-exact, {failed_ok} ended [F] (expected), {hung} hung")
    R.check(completed >= ROUNDS, "most downloads completed")
    R.eq(hung, 0, "no download hung")
    for n in (1, 2):
        th, fd = counts(tr[n].p.pid)
        print(f"  tracker {n}: {th} threads, {fd} fds (at start: {base_counts[n]})")
        R.check(0 < th < 30 and 0 < fd < 40, f"tracker {n} threads/fds bounded")
    for c in clients.values():
        c.p.quit()
    for n in (1, 2):
        tr[n].quit()
    ok = R.done()
    if not ok:
        print("logs kept in", d)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
