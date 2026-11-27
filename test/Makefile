# NRM-HA / BFT — test suite runner
#
#   make flash         flash the firmware
#   make test-cdc      serial protocol (CDC-ACM); auto-detects the profile
#   make test-cdc NRM_TEST_PROFILE=bft   run only the BFT tests
#   make test-cdc-all  run the common + NRM-HA + BFT suites (both mounts)
#   make test-host     host harness (production C, RTOS/RMT mocked)

# PORT auto-detects the board's USB-Serial/JTAG endpoint unless overridden
# (e.g. `make -C test flash PORT=/dev/cu.usbserial-XXXX`).
PORT   ?= $(shell cd proto && python3 -c "from client import detect_flash_port; print(detect_flash_port() or '')")

# CDC-ACM protocol ports.  SERIAL is the single-port default; test-cdc-all
# selects each device explicitly with SERIAL_NRM_HA / SERIAL_BFT.  When left
# empty the pytest fixture auto-detects by USB VID/PID.
SERIAL ?=
SERIAL_NRM_HA ?=
SERIAL_BFT ?=

# Active mount profile (nrm_ha|bft).  Empty = auto-detect from CAPABILITIES.
NRM_TEST_PROFILE ?=

.PHONY: flash test-cdc test-cdc-all test-host

# Flash the firmware.
flash:
	@test -n "$(PORT)" || { echo "No USB-Serial/JTAG port found — is the board connected and in boot mode? (override with PORT=/dev/cu.usbserial-XXXX)"; exit 1; }
	cd .. && source ~/.espressif/v6.0.1/esp-idf/export.sh && idf.py -p $(PORT) flash

# Serial protocol suite (one profile, or auto-detect).
test-cdc:
	@echo "== Serial protocol (CDC-ACM) =="
	@cd proto && NRM_SERIAL=$(SERIAL) NRM_TEST_PROFILE=$(NRM_TEST_PROFILE) python3 -m pytest -q

# Serial protocol suite for BOTH profiles (common + per-profile tests).  Both
# devices must be selected explicitly and distinctly; each identity is validated
# before its tests start.
test-cdc-all:
	@test -n "$(SERIAL_NRM_HA)" || { echo "test-cdc-all requires SERIAL_NRM_HA (e.g. SERIAL_NRM_HA=/dev/cu.usbmodemXXXX)"; exit 1; }
	@test -n "$(SERIAL_BFT)" || { echo "test-cdc-all requires SERIAL_BFT (e.g. SERIAL_BFT=/dev/cu.usbmodemYYYY)"; exit 1; }
	@test "$(SERIAL_NRM_HA)" != "$(SERIAL_BFT)" || { echo "SERIAL_NRM_HA and SERIAL_BFT must be different ports"; exit 1; }
	@$(MAKE) test-cdc NRM_TEST_PROFILE=nrm_ha SERIAL=$(SERIAL_NRM_HA)
	@$(MAKE) test-cdc NRM_TEST_PROFILE=bft SERIAL=$(SERIAL_BFT)

# Host harness: compiles the production mount/motors C (scheduler, queue,
# motion loop, RMT encoding) with RTOS/RMT/time mocked and runs the host suite.
test-host:
	@echo "== Host harness (production C, mocked RTOS/RMT) =="
	@$(MAKE) -C host test
