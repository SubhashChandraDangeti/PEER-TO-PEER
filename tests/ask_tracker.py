#!/usr/bin/env python3
"""ask_tracker.py - send ONE raw protocol command to a tracker and print the
reply. A demo/debug tool: it shows the wire protocol and replies such as
NOT_PRIMARY that the client normally handles silently.

  python3 tests/ask_tracker.py 127.0.0.1:5001 "CREATE_USER|x|pw"
  python3 tests/ask_tracker.py 127.0.0.1:5000 "LIST_GROUPS"
"""
import socket
import struct
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)
host, port = sys.argv[1].split(":")
body = sys.argv[2].encode()
s = socket.create_connection((host, int(port)), timeout=10)
s.sendall(struct.pack(">IB", len(body) + 1, 1) + body)  # length, type=TEXT, body
hdr = b""
while len(hdr) < 5:
    hdr += s.recv(5 - len(hdr))
length, _ = struct.unpack(">IB", hdr)
data = b""
while len(data) < length - 1:
    data += s.recv(length - 1 - len(data))
print(data.decode())
