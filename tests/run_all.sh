#!/bin/sh
# Run every automated suite. Usage: tests/run_all.sh   (from the project root)
#   BIG=1     also run the 2.5 GB test        LEAKS=1   also run valgrind
#   ROUNDS=n  chaos rounds (default 20)
set -e
cd "$(dirname "$0")/.."
make -s
make -s test
python3 tests/test_tracker.py
python3 tests/test_download.py
python3 tests/test_failover.py
python3 tests/test_regressions.py
python3 tests/test_chaos.py
[ -n "$BIG" ] && python3 tests/test_big.py
[ -n "$LEAKS" ] && python3 tests/test_leaks.py
echo "ALL SUITES PASSED"
