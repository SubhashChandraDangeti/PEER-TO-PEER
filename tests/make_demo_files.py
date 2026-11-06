#!/usr/bin/env python3
"""make_demo_files.py - create one file of each type for the demo.

  python3 tests/make_demo_files.py demo_files          # small set
  python3 tests/make_demo_files.py demo_files --big    # + a 2.5 GB file

Everything is generated (no copyrighted media). Video, mp3 and jpg need
ffmpeg (brew install ffmpeg); without it those three are skipped and the
rest are made with plain Python.
"""
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import wave
import zlib
import math

out = sys.argv[1] if len(sys.argv) > 1 else "demo_files"
os.makedirs(out, exist_ok=True)
P = lambda n: os.path.join(out, n)

open(P("empty.txt"), "w").close()
with open(P("notes.txt"), "w") as f:
    for i in range(20000):
        f.write(f"line {i}: peer-to-peer file sharing demo text\n")
with open(P("random_20mb.bin"), "wb") as f:
    f.write(os.urandom(20 * 1024 * 1024))
with open(P("exactly_one_piece.bin"), "wb") as f:
    f.write(os.urandom(512 * 1024))

# 10 s, 440 Hz tone as 16-bit WAV
with wave.open(P("tone.wav"), "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(44100)
    w.writeframes(b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / 44100)))
                           for i in range(44100 * 10)))

# 400x300 gradient PNG, written by hand: each row is a filter byte (0)
# followed by RGB pixels; the rows are zlib-compressed into one IDAT chunk.
def png(path, wd, ht):
    raw = b"".join(b"\x00" + bytes(v for x in range(wd) for v in (x * 255 // wd, y * 255 // ht, 160))
                   for y in range(ht))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", wd, ht, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


png(P("gradient.png"), 400, 300)

with tarfile.open(P("archive.tar.gz"), "w:gz") as t:
    t.add(P("notes.txt"), arcname="notes.txt")
    t.add(P("tone.wav"), arcname="tone.wav")

client = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "client", "client")
if os.path.exists(client):
    shutil.copy(client, P("program.bin"))  # an executable: must still run after download

ff = shutil.which("ffmpeg")
if ff:
    run = lambda *a: subprocess.run([ff, "-loglevel", "error", "-y", *a], check=True)
    run("-f", "lavfi", "-i", "testsrc=duration=30:size=1280x720:rate=30", "-f", "lavfi",
        "-i", "sine=frequency=440:duration=30", "-pix_fmt", "yuv420p", "-shortest", P("video.mp4"))
    run("-f", "lavfi", "-i", "sine=frequency=660:duration=20", P("tone.mp3"))
    run("-f", "lavfi", "-i", "testsrc=size=1280x720", "-frames:v", "1", P("testcard.jpg"))
else:
    print("ffmpeg not found: skipping video.mp4, tone.mp3, testcard.jpg (brew install ffmpeg)")

if "--big" in sys.argv:
    with open(P("big_2_5gb.bin"), "wb") as f:
        for _ in range(40):
            f.write(os.urandom(64 * 1024 * 1024))
for n in sorted(os.listdir(out)):
    print(f"{os.path.getsize(P(n)):>12,}  {n}")
