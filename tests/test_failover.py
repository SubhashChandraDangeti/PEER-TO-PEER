#!/usr/bin/env python3
"""test_failover.py - two trackers, replication and failover.

Scenarios:
  1. both start -> tracker 1 primary, tracker 2 backup, IN SYNC
  2. a client that asks the backup gets NOT_PRIMARY + the primary's address
  3. kill -9 the primary mid-download: backup promotes, clients carry on
     without logging in again, the download completes
  4. restart it: it rejoins as backup, loads a snapshot, IN SYNC
  5. kill the other one: roles swap again
  6. split: freeze the primary (SIGSTOP) -> backup promotes; make a change;
     resume (SIGCONT) -> tracker 1 wins, tracker 2 steps down and recopies;
     the change made during the split is lost (documented limitation)
"""
import os
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p2plib import Conn, Proc, Results, TRACKER_BIN, tmpdir, wait_port, write_tracker_info
from test_download import Client, sha1

BASE = 19100
T1, T2 = f"127.0.0.1:{BASE}", f"127.0.0.1:{BASE + 1}"


def start_tracker(d, info, n, tag=""):
    p = Proc([TRACKER_BIN, info, str(n)], os.path.join(d, f"t{n}{tag}.log"))
    assert wait_port(T1 if n == 1 else T2)
    return p


def check_sync(t):
    start = len(t.output())
    t.send("check_sync")
    for _ in range(80):
        out = t.output()[start:]
        for key in ("IN SYNC", "OUT OF SYNC", "PARTNER UNREACHABLE"):
            if key in out:
                return out.strip().splitlines()[-1]
        time.sleep(0.1)
    return "no answer"


def main():
    d = tmpdir("p2p-t4-")
    info = write_tracker_info(d, BASE)
    R = Results("failover")
    big = os.path.join(d, "big.bin")
    with open(big, "wb") as f:
        f.write(os.urandom(300 * 1024 * 1024))

    # 1. Startup: tracker 1 primary, tracker 2 backup.
    t1 = start_tracker(d, info, 1)
    t2 = start_tracker(d, info, 2)
    R.check(t2.wait_for("Recovery complete: IN SYNC", 10), "backup loads snapshot and verifies")
    R.check("role STARTING -> PRIMARY" in t1.output(), "tracker 1 became primary")
    R.check("role STARTING -> BACKUP" in t2.output(), "tracker 2 became backup")

    # 2. Backup redirects clients.
    c = Conn(T2)
    r = c.req("CREATE_USER|x|pw")
    R.eq(r, f"NOT_PRIMARY|this tracker is the backup|{T1}", "backup answers NOT_PRIMARY with primary address")
    c.close()

    A, B, C = Client(d, info, 1), Client(d, info, 2), Client(d, info, 3)
    for cl, u in ((A, "a"), (B, "b"), (C, "c")):
        cl.cmd(f"create_user {u} pw", "[")
        cl.cmd(f"login {u} pw", "logged in")
    A.cmd("create_group g", "created")
    for cl, u in ((B, "b"), (C, "c")):
        cl.cmd("join_group g", "[OK]")
        A.cmd(f"accept_request g {u}", "member")
    A.cmd(f"upload_file g {big}", "[OK] uploaded", timeout=120)
    B.cmd("download_file g big.bin .", "queued")
    R.eq(B.wait_done("g", "big.bin"), "C", "B downloads (2 seeders now)")
    R.check(check_sync(t1).startswith("IN SYNC"), "check_sync on primary: IN SYNC")
    R.check(check_sync(t2).startswith("IN SYNC"), "check_sync on backup: IN SYNC")

    # 3. Kill the primary in the middle of C's download.
    C.cmd("download_file g big.bin .", "queued")
    time.sleep(0.3)
    t1.kill(signal.SIGKILL)
    R.check(t2.wait_for("PROMOTING myself to primary", 10), "backup promotes when primary dies")
    R.eq(C.wait_done("g", "big.bin"), "C", "download completes through the failover")
    R.eq(sha1(os.path.join(C.dir, "big.bin")), sha1(big), "file intact after failover")
    R.check(A.cmd("list_files g", "big.bin"), "client keeps working without logging in again")
    R.check("3 seeder" in A.p.output().split("list_files")[-1] or A.cmd("list_files g", "3 seeder(s)"),
            "seeder list survived (A, B and C)")
    R.check(check_sync(t2).startswith("PARTNER UNREACHABLE"), "check_sync reports partner down")

    # 4. Restart tracker 1: it must rejoin as BACKUP (an empty tracker never wins).
    mark = len(t2.output())
    t1 = start_tracker(d, info, 1, "b")
    R.check(t1.wait_for("Recovery complete: IN SYNC", 15), "restarted tracker 1 recovers via snapshot")
    R.check("role STARTING -> BACKUP" in t1.output(), "restarted tracker 1 is backup, not primary")
    R.check("SPLIT-BRAIN" not in t2.output()[mark:], "no split-brain on a normal restart")
    A.cmd("create_group g2", "created")
    R.check(check_sync(t1).startswith("IN SYNC"), "new change replicated to the recovered tracker")

    # 5. Kill tracker 2 (now primary): tracker 1 promotes, clients follow.
    t2.kill(signal.SIGKILL)
    R.check(t1.wait_for("PROMOTING myself to primary", 10), "tracker 1 promotes")
    R.check(B.cmd("list_groups", "g2"), "client fails over back to tracker 1")
    t2 = start_tracker(d, info, 2, "b")
    R.check(t2.wait_for("Recovery complete: IN SYNC", 15), "tracker 2 recovers as backup")

    # 6. Split: freeze the primary. The backup sees silence for 6 s and promotes.
    os.kill(t1.p.pid, signal.SIGSTOP)
    R.check(t2.wait_for("PROMOTING myself to primary", 15), "frozen primary detected by heartbeat timeout")
    R.check(A.cmd("create_group splitgroup", "created", timeout=30), "client times out on frozen tracker, fails over")
    os.kill(t1.p.pid, signal.SIGCONT)
    R.check(t2.wait_for("SPLIT-BRAIN resolved: stepping down", 20), "tracker 2 steps down after the split")
    R.check(t2.wait_for("Recovery complete: IN SYNC", 15), "tracker 2 recopies tracker 1's state")
    R.check(A.cmd("list_groups", "2 group(s)", timeout=30), "change made during the split is gone (documented)")
    R.check(check_sync(t1).startswith("IN SYNC"), "both trackers IN SYNC after the split")
    R.check(C.cmd("list_files g", "big.bin"), "clients still logged in after the split")

    for cl in (A, B, C):
        cl.p.quit()
    t1.quit()
    t2.quit()
    R.check(not t1.alive() and not t2.alive(), "both trackers quit cleanly")
    ok = R.done()
    if not ok:
        print("logs kept in", d)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
