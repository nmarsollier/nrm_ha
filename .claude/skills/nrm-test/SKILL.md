---
name: nrm-test
description: Run the NRM-HA mount test suite and fix any failure. Use when the user asks to run the tests, diagnose a failure, or fix a bug the suite surfaces.
---

# Run and fix the NRM-HA tests

The NRM-HA mount has a functional acceptance suite against real hardware.
Two separate entry points, no overlap:

- `test/alpaca/` → **Alpaca** (port 11111), the interface N.I.N.A. consumes. Run with pytest.
- `test/api/` → **REST** (port 80). Input validation + state machine.

## 1. Run the tests

```sh
make -C test test          # both suites
make -C test test-alpaca   # Alpaca only
make -C test test-rest     # REST only
```

Manual equivalent:

```sh
cd test/alpaca && python3 -m pytest -q          # Alpaca
cd test/api && python3 test_blackbox.py 192.168.7.1   # REST validation
cd test/api && python3 -m pytest test_state_machine.py # REST state machine (TEST MODE)
```

Useful pytest variants: `--lf` (only last failed), `-k "park"` (by name),
`test_got.py` (one family), `-x` (stop at first failure).

## 2. Prerequisites

- Board flashed and reachable at `http://192.168.7.1`. The REST state machine
  requires **TEST MODE** (`idf.py -DNRM_TEST_MODE=1 ... flash`) because it uses the
  debug seam `/api/debug/power` and `/api/debug/fault`.
- `pytest` installed (`python3 -m pip install pytest`).
- ESP-IDF v6.0.1 tools at `~/.espressif/v6.0.1/esp-idf` (`idf.py`).

## 3. If the board does not answer (USB NCM down)

The USB-NCM link on macOS can stay down after a flash/reset. Recover in this order:

```sh
# 1. clean SoC reset (forces USB re-enumeration)
source ~/.espressif/v6.0.1/esp-idf/export.sh
python -m esptool --chip esp32s3 -p /dev/cu.usbserial-1410 --after hard-reset chip-id

# 2. if still down, re-flash
idf.py -DNRM_TEST_MODE=1 -p /dev/cu.usbserial-1410 flash
```

The firmware already forces a `tud_disconnect()` + `tud_connect()` on boot
(`main/usb_net/usb_net_init.c`), so it should come back on its own after flashing.
Verify with: `curl http://192.168.7.1/api/status`.

## 4. Diagnosing a failing test

1. **Read the exact message.** Distinguish:
   - `TransportError` → the board did not answer (see section 3).
   - `DeviceError: ErrorNumber=N` → the device rejected the operation (firmware bug).
   - `AssertionError` → the observed state/value does not match the expectation.
2. **Reproduce in isolation**: `python3 -m pytest test_xxx.py::test_name -v`.
3. **Confirm the real state** with `curl http://192.168.7.1/api/status` and, for
   Alpaca state, the corresponding property GETs.
4. **For crashes/resets** capture the serial with DTR/RTS de-asserted:

```sh
python3 - <<'EOF'
import serial
s = serial.Serial("/dev/cu.usbserial-1410", 115200, timeout=0.2)
s.dtr = False; s.rts = False   # avoid the auto-program reset
while True:
    b = s.read(4096)
    if b: print(b.decode(errors="replace"), end="")
EOF
```

   Look for: `Guru Meditation Error`, `task_wdt`, `rst:` (reset cause), or
   `channel can't be disabled in state 0` (RMT).

## 5. Fixing the bug

- **Do not change the test expectation to make a bug pass.** The test expresses the
  correct behavior; if it fails, fix the firmware in `main/`.
- Fixes go in the matching domain (`main/alpaca_bridge/`, `main/rest_alpaca/`,
  `main/motors/`, `main/mount/`, `main/power/`, `main/usb_net/`).
- Keep production changes separate from test changes.
- After the fix: `make -C test flash` (rebuild + flash), wait for the board to come
  back (`curl` until 200), then re-run only the failing test, then the whole suite.

## 6. Conventions the suite respects

- **No `@pytest.mark.skip`.** What cannot be simulated is not in the suite.
- **No catalog codes** (VAL-01, GOT-02, D-08...). Tests are described with clear
  names and docstrings.
- The celestial position is validated with **spherical angular distance** (an
  independent oracle in `test/alpaca/oracle.py`), not a direct RA/DEC subtraction.
- `ErrorNumber` is validated in addition to HTTP 200: an HTTP `ok` is not enough.
- The REST ERROR-state and latched-fault tests use the debug seam and require TEST
  MODE; everything else runs against normal firmware.
