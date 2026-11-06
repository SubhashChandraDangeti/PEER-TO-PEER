# `make` builds tracker and client, `make test` runs unit tests,
# `make integration` runs the download and failover suites.
all:
	$(MAKE) -C tracker
	$(MAKE) -C client

unit:
	$(MAKE) -C tests unit_tests

test: unit
	./tests/unit_tests

integration: all
	python3 tests/test_tracker.py
	python3 tests/test_download.py
	python3 tests/test_failover.py
	python3 tests/test_regressions.py

clean:
	$(MAKE) -C tracker clean
	$(MAKE) -C client clean
	$(MAKE) -C tests clean

.PHONY: all unit test integration clean
