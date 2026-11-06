#!/usr/bin/env python3
"""test_tracker.py - every tracker command, the permission matrix,
password storage, and robustness against malformed input.

Talks to one tracker directly over the wire protocol.
"""
import os
import random
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from p2plib import Conn, Proc, Results, TRACKER_BIN, tmpdir, wait_port, write_tracker_info, send_frame, recv_frame

H = "a" * 40          # a well-formed SHA-1 hex string
ADDR_A = "127.0.0.1:20001"


def login(addr, user, pw="pw1", listen=ADDR_A):
    c = Conn(addr)
    r = c.req(f"LOGIN|{user}|{pw}|{listen}")
    assert r.startswith("OK"), r
    return c, r.split("|")[2]


def main():
    d = tmpdir("p2p-t2-")
    info = write_tracker_info(d, 18000)
    t = Proc([TRACKER_BIN, info, "1"], os.path.join(d, "t1.log"))
    addr = "127.0.0.1:18000"
    assert wait_port(addr), "tracker did not start"
    time.sleep(4.5)  # alone: waits for its partner, then becomes primary
    R = Results("tracker commands + permissions")

    # ---------------------------------------------------------- accounts
    anon = Conn(addr)
    R.eq(anon.code("CREATE_USER|alice|pw1"), "OK", "create alice")
    R.eq(anon.code("CREATE_USER|alice|other"), "ALREADY_EXISTS", "duplicate user")
    for u in ("bob", "carol", "eve"):
        anon.req(f"CREATE_USER|{u}|pw1")
    R.eq(anon.code("CREATE_USER|bad name|pw"), "INVALID_ACTION", "space in name")
    R.eq(anon.code("CREATE_USER|../x|pw"), "INVALID_ACTION", "path in name")
    # Unknown user and wrong password: identical reply (no username probing)
    r1 = anon.req(f"LOGIN|nosuchuser|pw1|{ADDR_A}")
    r2 = anon.req(f"LOGIN|alice|wrong|{ADDR_A}")
    R.eq(r1, r2, "unknown user and wrong password give the same reply")
    R.eq(r1.split("|")[0], "AUTH_FAILED", "auth failed code")
    # Anonymous user may not do anything else
    for cmd in ("CREATE_GROUP|g", "JOIN_GROUP|g", "LIST_GROUPS", "LIST_FILES|g", "LOGOUT",
                "DOWNLOAD_FILE|g|f", f"UPLOAD_FILE|g|f|1|{H}|{H}"):
        R.eq(anon.code(cmd), "NOT_LOGGED_IN" if cmd != "LOGOUT" else "NOT_LOGGED_IN", f"anon {cmd}")

    alice, alice_tok = login(addr, "alice")
    R.check(len(alice_tok) == 64, "token is 256 bits (64 hex)")
    R.eq(alice.code(f"LOGIN|alice|pw1|{ADDR_A}"), "INVALID_ACTION", "double login on same connection")

    # ------------------------------------------------------------ groups
    R.eq(alice.code("CREATE_GROUP|Team"), "OK", "create group")
    R.eq(alice.code("CREATE_GROUP|team"), "ALREADY_EXISTS", "group names case-insensitive")
    bob, _ = login(addr, "bob", listen="127.0.0.1:20002")
    carol, _ = login(addr, "carol", listen="127.0.0.1:20003")
    eve, _ = login(addr, "eve", listen="127.0.0.1:20005")
    R.eq(bob.code("JOIN_GROUP|nosuch"), "NOT_FOUND", "join missing group")
    R.eq(bob.code("JOIN_GROUP|team"), "OK", "bob requests")
    R.eq(bob.code("JOIN_GROUP|team"), "ALREADY_EXISTS", "duplicate request")
    R.eq(carol.code("JOIN_GROUP|team"), "OK", "carol requests")
    R.eq(alice.code("JOIN_GROUP|team"), "ALREADY_EXISTS", "owner already member")
    R.eq(bob.code("LIST_REQUESTS|team"), "INVALID_ACTION", "non-member list_requests")
    r = alice.req("LIST_REQUESTS|team")
    R.check(r.startswith("OK") and "bob" in r and "carol" in r, "owner lists requests")
    R.eq(alice.code("ACCEPT_REQUEST|team|eve"), "NOT_FOUND", "accept without request")
    R.eq(alice.code("ACCEPT_REQUEST|team|bob"), "OK", "accept bob")
    R.eq(bob.code("ACCEPT_REQUEST|team|carol"), "INVALID_ACTION", "member (not owner) accepts")
    R.eq(alice.code("ACCEPT_REQUEST|team|carol"), "OK", "accept carol")
    R.eq(alice.code("LEAVE_GROUP|team"), "INVALID_ACTION", "owner cannot leave")
    R.eq(eve.code("LEAVE_GROUP|team"), "INVALID_ACTION", "non-member leave")
    R.check("team" in eve.req("LIST_GROUPS"), "any logged-in user lists groups")

    # ------------------------------------------------------------- files
    up = f"UPLOAD_FILE|team|report.pdf|10|{H}|{'b' * 40}"
    R.eq(eve.code(up), "INVALID_ACTION", "non-member upload")
    R.eq(alice.code(up), "OK", "member upload")
    R.eq(alice.code(f"UPLOAD_FILE|team|x.bin|NOTNUM|{H}|{H}"), "INVALID_ACTION", "non-numeric size (old crash)")
    R.eq(alice.code(f"UPLOAD_FILE|team|x.bin|10|{H}|{H},{H}"), "INVALID_ACTION", "hash count mismatch")
    R.eq(alice.code(f"UPLOAD_FILE|team|x.bin|{4 * 2**30 + 1}|{H}|"), "INVALID_ACTION", "over 4 GiB")
    R.eq(alice.code(f"UPLOAD_FILE|team|x.bin|10|zz|{H}"), "INVALID_ACTION", "bad hash")
    R.eq(bob.code(f"UPLOAD_FILE|team|report.pdf|10|{'c' * 40}|{'b' * 40}"), "ALREADY_EXISTS",
         "same name, different content")
    R.eq(bob.code(up), "OK", "same name, same content -> extra seeder")
    R.eq(eve.code("LIST_FILES|team"), "INVALID_ACTION", "non-member list_files")
    R.check("report.pdf:10:2" in carol.req("LIST_FILES|team"), "member lists file with 2 seeders")
    R.eq(eve.code("DOWNLOAD_FILE|team|report.pdf"), "INVALID_ACTION", "non-member download")
    r = carol.req("DOWNLOAD_FILE|team|report.pdf")
    R.check(r.startswith("OK") and "alice@127.0.0.1:20001" in r and "bob@127.0.0.1:20002" in r,
            "download lists both seeders with addresses")
    R.eq(carol.code("DOWNLOAD_FILE|team|nosuch"), "NOT_FOUND", "download missing file")
    R.eq(carol.code("STOP_SHARE|team|report.pdf"), "NOT_FOUND", "stop_share by non-seeder")

    # F17 case 2: Bob's stop_share removes only Bob; Alice still shares.
    R.eq(bob.code("STOP_SHARE|team|report.pdf"), "OK", "bob stop_share")
    r = carol.req("DOWNLOAD_FILE|team|report.pdf")
    R.check("alice@" in r and "bob@" not in r, "after bob stops, alice still listed")

    R.eq(carol.code("ADD_SEEDER|team|report.pdf"), "OK", "carol becomes seeder")
    # Leaving a group removes your seeder entries in it.
    R.eq(carol.code("LEAVE_GROUP|team"), "OK", "carol leaves")
    r = bob.req("DOWNLOAD_FILE|team|report.pdf")
    R.check("carol@" not in r, "leaver no longer a seeder")
    R.eq(carol.code("LIST_FILES|team"), "INVALID_ACTION", "leaver lost access")

    # Logout stops sharing (F6), and the old connection loses its session.
    R.eq(alice.code("LOGOUT"), "OK", "alice logout")
    R.eq(bob.code("DOWNLOAD_FILE|team|report.pdf"), "NOT_FOUND", "no seeders after owner logout")
    R.eq(alice.code("LIST_GROUPS"), "NOT_LOGGED_IN", "commands after logout")

    # Disconnect also stops sharing.
    alice, alice_tok = login(addr, "alice")
    alice.req("ADD_SEEDER|team|report.pdf")
    R.check("alice@" in bob.req("DOWNLOAD_FILE|team|report.pdf"), "alice seeding again")
    alice.close()
    time.sleep(0.3)
    R.eq(bob.code("DOWNLOAD_FILE|team|report.pdf"), "NOT_FOUND", "disconnect removes seeder")

    # Session token resume (used by failover) and last-login-wins.
    a1, tok = login(addr, "alice")
    a2 = Conn(addr)
    R.eq(a2.code(f"RESUME|alice|{tok}|{ADDR_A}"), "OK", "resume with token")
    R.eq(a2.code(f"RESUME|alice|{'0' * 64}|{ADDR_A}"), "AUTH_FAILED", "resume with wrong token")
    a3, _ = login(addr, "alice")  # newer login replaces the old session
    R.eq(a1.code("LIST_GROUPS"), "NOT_LOGGED_IN", "older session replaced by newer login")
    R.eq(a3.code("LIST_GROUPS"), "OK", "newest session works")

    # Password storage: the log never contains passwords or tokens.
    log = t.output()
    R.check("pw1" not in log and tok not in log, "no passwords or tokens in tracker log")
    t.send("state")
    time.sleep(0.3)
    R.check("4 users" in t.output(), "console state command")

    # --------------------------------------------- robustness / fuzzing
    rnd = random.Random(42)
    commands = ["CREATE_USER", "LOGIN", "UPLOAD_FILE", "DOWNLOAD_FILE", "ACCEPT_REQUEST", "LIST_FILES", "RESUME"]
    sent = 0
    for i in range(300):
        s = socket.create_connection(("127.0.0.1", 18000), timeout=5)
        kind = i % 6
        try:
            if kind == 0:   # random bytes, no valid header
                s.sendall(bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 64))))
            elif kind == 1:  # huge length claim
                s.sendall(struct.pack(">IB", rnd.choice([0, 0xFFFFFFFF, 2**21]), 1) + b"x")
            elif kind == 2:  # truncated frame
                s.sendall(struct.pack(">IB", 100, 1) + b"LOGIN|a")
            elif kind == 3:  # PIECE frame to the tracker
                send_frame(s, b"\x00\x01\x02", 2)
                recv_frame(s)
            else:            # random fields for real commands
                fields = [rnd.choice(commands)] + [
                    "".join(chr(rnd.randrange(0, 256)) for _ in range(rnd.randrange(0, 12)))
                    for _ in range(rnd.randrange(0, 8))]
                send_frame(s, "|".join(fields).encode("utf-8", "surrogateescape"))
                recv_frame(s)
            sent += 1
        except (ConnectionError, OSError, struct.error):
            sent += 1
        finally:
            s.close()
    time.sleep(0.3)
    R.check(t.alive(), f"tracker alive after {sent} malformed messages")
    probe = Conn(addr)
    R.eq(probe.code("LIST_GROUPS"), "NOT_LOGGED_IN", "tracker still answers after fuzzing")

    t.quit()
    R.check(not t.alive(), "console quit stops the tracker")
    ok = R.done()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
