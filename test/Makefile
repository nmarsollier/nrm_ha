# NRM-HA — test suite runner
#
#   make test           runs both suites (Alpaca + REST)
#   make test-alpaca    Alpaca only (port 11111)
#   make test-rest      REST only (port 80)
#   make flash          flash in TEST MODE (enables the debug seam)

HOST ?= 192.168.7.1
PORT ?= /dev/cu.usbserial-1410

.PHONY: test test-alpaca test-rest flash

test: test-alpaca test-rest

test-alpaca:
	@echo "== Alpaca =="
	@cd alpaca && python3 -m pytest -q

test-rest:
	@echo "== REST validation =="
	@cd api && python3 test_blackbox.py $(HOST)
	@echo "== REST state machine (requires TEST MODE) =="
	@cd api && python3 -m pytest test_state_machine.py -q

# Flash the board in TEST MODE: compiles the debug seam (/api/debug/*) that the
# REST state machine uses to reach the ERROR state.
flash:
	cd .. && source ~/.espressif/v6.0.1/esp-idf/export.sh && idf.py -DNRM_TEST_MODE=1 -p $(PORT) flash
