# NRM-HA / BFT test suite

Functional acceptance suite for an equatorial mount against real hardware,
driven through the **CDC-ACM serial protocol**.  One suite validates both mount
profiles:

- **NRM-HA** — German equatorial (meridian flip, pier side east/west).
- **BFT** — equatorial fork (no flip, pier side unknown).

Which tests run is decided by the capabilities the connected mount declares via
`CAPABILITIES` — a test states its requirements with `@pytest.mark.requires(...)`
and `@pytest.mark.geometry(...)` and is skipped when the mount does not satisfy
them.  `profiles.py` holds the per-mount *expected* values, used only to validate
identity and `CAPABILITIES`; every other expectation is derived from
`CAPABILITIES` and the independent oracle in `oracle.py`.

## Structure

```text
test/host/           host harness (no hardware): compiles the production
                     mount/motors C (scheduler, queue, motion loop, RMT
                     encoding) with RTOS/RMT/time mocked
  harness.py         ctypes bindings + STEP observability + polling helpers
  test_mount_guard.py    redundant manual-zero guard + guide-rate validation
  test_phd_calibration.py  PHD2-style guide sequence, RMT fault injection
  test_fork.py       coordinate/guide math (mount_coordinate_tools + GOTO),
                     compiled with motors/clock mocked against an oracle
  mount_stubs.c      motors/clock mocks for the test_fork.py compile

test/proto/          serial protocol acceptance suite
  client.py          serial client (framing, request/response, error taxonomy)
  oracle.py          independent oracle (spherical angular distance + slew-time model)
  profiles.py        per-mount expected values (identity + CAPABILITIES validation)
  profile.json       versioned profile (budgets/tolerances)
  conftest.py        fixtures, capability-driven selection, polling/axis helpers
  test_capabilities.py  identity + capabilities (validated against profiles.py)
  test_state.py         snapshot coherence and fields
  test_cfg.py           site, time, coordinates
  test_val.py           contract, invalid inputs, errors
  test_json.py          structural rejection of malformed JSON frames
  test_trk.py           tracking (rate coherence, limits)
  test_trk_preserve.py  no motion (move/MoveAxis/guide) ever loses tracking
  test_stp.py           stop and cancellation
  test_got*.py          slewing (GOTO): duration, speed, sequence, sites, limits, guide
  test_got_flip.py      German meridian flip + pier side (NRM-HA only)
  test_got_meridian.py  fork meridian crossing without flip (BFT only)
  test_man.py           manual movement (MoveAxis) + axis limits
  test_gui.py           autoguiding (PulseGuide) + guide-rate validation
  test_guide_effect.py  PulseGuide position/time effect (coarse + default rate)
  test_phd_calibration.py  PHD2-style guide calibration sequence (real speed)
  test_mer.py           pier side / meridian flip (NRM-HA only)
  test_prk.py           park / unpark
  test_ref.py           reference / home
  test_limits.py        runtime-changed limits
  test_actions.py       action family
  test_session.py       long mixed session
```

## Requirements

- An NRM-HA or BFT board connected over USB, flashed with the firmware (CDC-ACM).
  The suite auto-detects the serial port by USB VID/PID and the profile from
  `CAPABILITIES`.
- Python 3 + `pytest`.

```sh
python3 -m pip install pytest
```

## Running

```sh
make -C test test-host     # host harness (no hardware, compiles production C)
make -C test test-cdc      # serial protocol suite (auto-detect profile)
make -C test test-cdc NRM_TEST_PROFILE=nrm_ha   # NRM-HA only (verify identity)
make -C test test-cdc NRM_TEST_PROFILE=bft      # BFT only (verify identity)
make -C test test-cdc-all  # common + NRM-HA + BFT (requires both mounts)
make -C test flash         # flash the firmware
```

Manual equivalent:

```sh
cd test/host
python3 -m unittest discover -s . -v    # host harness

cd test/proto
python3 -m pytest -q            # the whole suite (auto-detect profile)
NRM_TEST_PROFILE=bft python3 -m pytest -q   # BFT only
python3 -m pytest --lf          # only the ones that failed last time
python3 -m pytest test_got.py   # one family
python3 -m pytest -k "park"     # by name
```

`test-cdc-all` runs the two suites in sequence and stops at the first failure,
so it reports success only when *both* mounts ran.  With `NRM_TEST_PROFILE` set,
the suite aborts if the connected device reports a different `device_id`.

The host harness runs the *production* mount/motors C — the command queue,
the motion task, `tracking_loop_rmt`, and the RMT encoding — with only RTOS,
time and the RMT/GPIO peripherals mocked.  It observes the actual STEP
transmissions per axis (not a stub that fakes movement), so a suppressed DEC
output that still ACKs `ok` makes the nominal sequence fail.  It requires only
`cc` and Python 3 (no ESP-IDF, no board).


## Skill

There is a project skill, **`nrm-test`** (`.claude/skills/nrm-test/SKILL.md`),
that encapsulates this whole workflow: how to run the suite, how to recover the
board when the serial link is down, how to diagnose a failure (transport vs
device vs assertion, serial capture for crashes) and how to fix it without
weakening the test expectations.

In Claude Code, invoke it with:

```
/nrm-test
```

or just ask to "run the tests and fix any failure" and the skill triggers by its
description.

## Results

- Tests are skipped only when the connected mount does not declare the required
  capability or geometry (`@pytest.mark.requires` / `geometry`) — a legitimate
  deselection, never a way to hide an unimplemented feature.  What cannot be
  exercised over the protocol (encoder, camera, power bench, N.I.N.A.) stays out
  of the automated suite and is neither declared passed nor pending.
- Each case uses an independent oracle (angular distance, slew-time ramp model)
  for the reported position and duration, not the value of the protocol itself.

## Profiles

- `profiles.py` is the per-mount *expected* values (identity, geometry,
  mechanical scale, speeds, ramp, limits).  It is read only by the `profile`
  fixture and `test_capabilities.py` to validate what the firmware declares —
  a wrong value there fails the suite instead of shifting the assertions.
- `profile.json` is the versioned run profile (serial port, tolerance budgets).
  `null` fields block the tests that use them; a pending value is not a PASS.
  `budgets.reported_position_deg` is the tolerance for the reported position
  (not physical pointing).
