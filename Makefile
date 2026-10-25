# Thin wrapper over CMake so the one-word contract is `make check`.
# Override any of these on the command line, e.g. make PY=/path/to/env/bin/python KALSHI_ROOT=/path/to/Kalshi
PY    ?= python3
REF   ?= $(CURDIR)/ref/current
KALSHI_ROOT ?= $(abspath $(CURDIR)/../Kalshi)
BUILD ?= build
JOBS  ?= $(shell nproc)

.PHONY: parity config check deps configure build vectors unit lint clean

deps:
	sudo dnf install -y gcc-c++ cmake openssl-devel libcurl-devel zlib-devel
	@g++ --version | head -1
	@cmake --version | head -1

configure:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DKALSHI_ROOT_DEFAULT=$(abspath $(KALSHI_ROOT)) -DKP_REF_DEFAULT=$(abspath $(REF)) \
	  -DKP_PYTHON=$(PY)

build: configure
	cmake --build $(BUILD) -j$(JOBS)

# Regenerated, not committed: generation is deterministic (fixed seeds) and ~180 MB of derived
# data does not belong in a repo. Regenerating every time also means a vector set can never
# outlive the Python it was taken from.
vectors:
	KP_REF=$(REF) $(PY) tools/gen_golden.py --out tests/data/golden
	@for g in tools/golden/gen_*.py; do [ -f "$$g" ] || continue; \
	   KP_REF=$(REF) $(PY) $$g --out tests/data/golden || exit 1; done

# Regenerate the mechanical half of the config from the reference. `config_fresh` fails until you do.
config:
	KP_REF=$(REF) $(PY) tools/gen_config_hpp.py > include/kp/config_gen.hpp

# The Python side of the bar. The C++ tests assert against constants lifted from the reference;
# if the reference itself moved, those constants are stale and C++ would pass while diverging.
pytests:
	cd $(REF) && $(PY) -m strategy.test_quoting
	cd $(REF) && $(PY) -m strategy.test_orders
	cd $(REF) && $(PY) -m strategy.test_verdict

unit: build
	ctest --test-dir $(BUILD) --output-on-failure

# The bans, as grep. Each one is a "helpful cleanup" that would silently break bit-identity.
lint:
# Strips // comments first, so the bans check CODE and not the prose explaining them.
	@for f in $$(find src include -name '*.cpp' -o -name '*.hpp'); do \
	   sed 's|//.*||' $$f | grep -n 'std::round\|std::cbrt\|std::reduce\|execution::par' \
	     | sed "s|^|$$f:|"; \
	 done | grep . && (echo "BAN: use pycompat::py_round; pow(x,1/3) is not std::cbrt"; exit 1) \
	 || true
	@! grep -rn 'ffast-math\|march=native\|-Ofast\|fno-signed-zeros' CMakeLists.txt cmake/ \
	    | grep -v ':[[:space:]]*#' \
	  || (echo "BAN: unsafe FP flags actually set"; exit 1)
	@echo "  lint ok"

# THE END-TO-END GATE: scripted sessions through run_04.cycle (Python) and kp::app::cycle (C++),
# every decision, order, journal row and log line compared exactly. See tools/replay/.
PARITY_DIR ?= build/parity
parity: build
	@mkdir -p $(PARITY_DIR)
	@for sc in fast rest live; do \
	   KP_REF=$(REF) $(PY) tools/replay/make_session.py --scenario $$sc --out $(PARITY_DIR)/$$sc.json >/dev/null && \
	   KP_REF=$(REF) $(PY) tools/replay/py_replay.py $(PARITY_DIR)/$$sc.json $(PARITY_DIR)/$$sc.py.jsonl && \
	   $(BUILD)/apps/replay $(PARITY_DIR)/$$sc.json $(PARITY_DIR)/$$sc.cxx.jsonl $(KALSHI_ROOT) && \
	   printf "  %-5s " $$sc && \
	   { $(PY) tools/replay/compare.py $(PARITY_DIR)/$$sc.py.jsonl $(PARITY_DIR)/$$sc.cxx.jsonl \
	       > $(PARITY_DIR)/$$sc.compare.txt; rc=$$?; tail -1 $(PARITY_DIR)/$$sc.compare.txt; [ $$rc -eq 0 ]; } \
	   || exit 1; done
	@echo "=== REPLAY PARITY GREEN ==="

check: vectors build unit lint
	@echo "=== PARITY GREEN ==="

clean:
	rm -rf $(BUILD) tests/data/golden
