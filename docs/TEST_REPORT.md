# Test report

Verified on 2026-11-06 on Ubuntu Linux x86-64 with g++ 13.3.0,
OpenSSL 3.0.13 and Python 3.12.3. Tests use local TCP sockets and real
tracker/client processes. Both application binaries rebuilt without warnings.

| Suite | Passed | Coverage |
|---|---:|---|
| `make test` | 62 | Framing, partial transmissions, validation, piece arithmetic and hash vectors |
| `tests/test_tracker.py` | 65 | Commands, permissions, sessions and 300 malformed messages |
| `tests/test_download.py` | 74 | Byte-exact transfers, boundary sizes, multiple peers, download queues, corrupt and disconnected seeders |
| `tests/test_failover.py` | 28 | Redirects, tracker crashes, promotion, recovery, freeze and partition reconciliation |
| `tests/test_regressions.py` | 12 | Silent peers, logout stopping local sharing, session replacement and conflicting metadata |
| `tests/test_big.py` | 4 | 2.5 GiB transfer, SHA-1 comparison and bounded memory usage |
| `tests/test_chaos.py` | 44 | 20 fault-injection rounds; 38 byte-exact completions, two expected failures on killed clients, no hangs; trackers finish in sync |

**Total: 289 passed, zero failed.** Optional generated media fixtures were
skipped because ffmpeg was unavailable. Valgrind was not installed, and
ThreadSanitizer was not run; these results do not establish absence of leaks
or data races.

For the 2.5 GiB test, hashing/upload took 6.1 seconds and download took
10.9 seconds. Peak resident memory was 15.5 MB for the uploader and
12.9 MB for the downloader. Performance varies by machine.

## Reproduce

```sh
make
make test
make integration
sh tests/run_all.sh
python3 tests/test_big.py
python3 tests/test_leaks.py  # requires valgrind; otherwise reports a skip
```

`make integration` includes tracker, download, failover and regression suites.
The full runner also includes chaos. `BIG=1 sh tests/run_all.sh` adds the
large-file test, and `LEAKS=1` requests valgrind. Install ffmpeg to enable the
additional generated video, audio and image fixtures. Network tests require
permission to bind local sockets. See [manual tests](../tests/MANUAL_TESTS.md)
for interactive demonstrations.
