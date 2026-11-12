# NRM-HA — test suite runner
#
#   make flash         flash the firmware
#   make test-cdc      serial protocol (CDC-ACM)
#   make test-host     host harness (production C, RTOS/RMT mocked)

# PORT auto-detects the board's USB-Serial/JTAG endpoint unless overridden
# (e.g. `make -C test flash PORT=/dev/cu.usbserial-XXXX`).  SERIAL is the
# CDC-ACM protocol port, auto-detected by the pytest fixture when left empty.
PORT   ?= $(shell cd proto && python3 -c "from client import detect_flash_port; print(detect_flash_port() or '')")
SERIAL ?=

.PHONY: flash test-cdc test-host

# Flash the firmware.
flash:
	@test -n "$(PORT)" || { echo "No NRM-HA USB-Serial/JTAG port found — is the board connected and in boot mode? (override with PORT=/dev/cu.usbserial-XXXX)"; exit 1; }
	cd .. && source ~/.espressif/v6.0.1/esp-idf/export.sh && idf.py -p $(PORT) flash

# Serial protocol suite.
test-cdc:
	@echo "== Serial protocol (CDC-ACM) =="
	@cd proto && NRM_SERIAL=$(SERIAL) python3 -m pytest -q

# Host harness: compiles the production mount/motors C (scheduler, queue,
# motion loop, RMT encoding) with RTOS/RMT/time mocked and runs the host suite.
test-host:
	@echo "== Host harness (production C, mocked RTOS/RMT) =="
	@$(MAKE) -C host test
