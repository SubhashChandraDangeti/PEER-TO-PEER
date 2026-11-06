#!/usr/bin/env python3
"""Audit regressions: stalled peers, logout, session replacement and metadata."""
import filecmp
import hashlib
import os
import shutil
import signal
import socket
import sys
import time

from p2plib import Conn, Proc, Results, TRACKER_BIN, recv_frame, send_frame, tmpdir, wait_port, write_tracker_info
from test_download import Client


def main():
    d = tmpdir("p2p-regressions-")
    info = write_tracker_info(d, 19800)
    tracker = Proc([TRACKER_BIN, info, "1"], os.path.join(d, "tracker.log"))
    clients = []
    frozen = None
    r = Results("audit regressions")
    try:
        assert wait_port("127.0.0.1:19800")
        time.sleep(4.5)
        clients = [Client(d, info, n) for n in (11, 12, 13)]
        a, b, c = clients
        for client, user in zip(clients, ("a", "b", "c")):
            assert client.cmd(f"create_user {user} pw", "[OK]")
            assert client.cmd(f"login {user} pw", "logged in")
        assert a.cmd("create_group g", "created")
        for client, user in ((b, "b"), (c, "c")):
            assert client.cmd("join_group g", "[OK]")
            assert a.cmd(f"accept_request g {user}", "member")
        src = os.path.join(d, "sample.bin")
        content = os.urandom(1000)
        with open(src, "wb") as f:
            f.write(content)
        assert a.cmd(f"upload_file g {src}", "uploaded")
        assert b.cmd(f"upload_file g {src}", "[OK]")

        # A accepts a connection but cannot answer. C must retry on B.
        frozen = a.p
        frozen.kill(signal.SIGSTOP)
        assert c.cmd("download_file g sample.bin .", "queued")
        r.eq(c.wait_done("g", "sample.bin", timeout=25), "C", "silent seeder times out and retries")
        r.check(filecmp.cmp(src, os.path.join(c.dir, "sample.bin"), shallow=False), "retry is byte-exact")
        frozen.kill(signal.SIGCONT)
        frozen = None

        # Logout must stop the peer server from handing out known file names.
        r.check(a.cmd("logout", "logged out"), "logout completes")
        host, port = a.addr.split(":")
        with socket.create_connection((host, int(port)), timeout=5) as peer:
            send_frame(peer, "GET_PIECE|g|sample.bin|0")
            kind, body = recv_frame(peer)
            r.check(kind == 1 and body.startswith(b"NOT_FOUND|"), "logged-out client no longer serves pieces")

        # A forged whole-file hash cannot hide conflicting size/piece metadata.
        raw = Conn("127.0.0.1:19800")
        try:
            r.eq(raw.code(f"LOGIN|a|pw|{a.addr}"), "OK", "new session")
            digest = hashlib.sha1(content).hexdigest()
            r.eq(raw.code(f"UPLOAD_FILE|g|sample.bin|1001|{digest}|{digest}"),
                 "ALREADY_EXISTS", "same whole hash with conflicting size rejected")
            r.eq(raw.code(f"UPLOAD_FILE|g|sample.bin|1000|{digest}|{'0' * 40}"),
                 "ALREADY_EXISTS", "conflicting piece hashes rejected")
            raw.req(f"UPLOAD_FILE|g|sample.bin|1000|{digest}|{digest}")
            replacement = Conn("127.0.0.1:19800")
            try:
                reply = replacement.req(f"LOGIN|a|pw|{a.addr}")
                token = reply.split("|")[2]
                r.eq(raw.code("LIST_GROUPS"), "NOT_LOGGED_IN", "replaced session loses access")
                r.check(c.cmd("list_files g", "sample.bin"), "remaining shares available")
                r.eq(replacement.code(f"RESUME|a|{token}|bad-address"), "INVALID_ACTION", "resume address validated")
                r.check("a@" not in replacement.req("DOWNLOAD_FILE|g|sample.bin"), "own seeder excluded")
                # Query as B to verify the stale A seeder was removed.
                query = Conn("127.0.0.1:19800")
                try:
                    query.req(f"LOGIN|b|pw|{b.addr}")
                    r.check("a@" not in query.req("DOWNLOAD_FILE|g|sample.bin"), "new login drops old process shares")
                finally:
                    query.close()
            finally:
                replacement.close()
        finally:
            raw.close()
    finally:
        if frozen:
            frozen.kill(signal.SIGCONT)
        for client in clients:
            client.p.quit()
        tracker.quit()
    ok = r.done()
    if ok:
        shutil.rmtree(d)
    else:
        print("logs kept in", d)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
