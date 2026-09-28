# NRM-HA test suite

Functional acceptance suite for an equatorial mount against real hardware,
oriented to **ASCOM Alpaca** (the interface N.I.N.A. consumes). It is controlled
and observed through that same interface; REST is used only as a diagnostic
complement.

## Structure

Two entry points, no overlap:

```text
test/api/            REST entry-point validation (port 80)
  test_blackbox.py     Rejection of REST inputs without partial effects
  test_state_machine.py State-transition matrix + fault injection (debug seam)
  conftest.py          helpers

test/alpaca/         Functional acceptance of the Alpaca entry point (port 11111)
  client.py          Alpaca client (GET/PUT, ErrorNumber + HTTP, transactions)
  oracle.py          Independent oracle (spherical angular distance + slew-time model)
  profile.json       Versioned profile (ASCOM version, capabilities, budgets)
  conftest.py        Fixtures, bounded polling, axis-position/goto-duration helpers
  test_con.py        Discovery, connection, identity
  test_val.py        Contract, invalid inputs, errors
  test_cfg.py        Site, time, coordinates
  test_trk.py        Tracking (rate coherence, limits)
  test_stp.py        Stop and cancellation (tracking, park)
  test_got.py        Slewing (GOTO) with duration assertion
  test_got_time.py   GOTO duration vs the ramp model
  test_got_speed.py  GOTO at each speed rate + small gotos (1–5°)
  test_got_seq.py    Consecutive gotos, start-position header, NINA-style, from home
  test_got_sites.py  GOTO across different site lat/long
  test_got_tracking.py GOTO × tracking combos
  test_got_limits.py GOTO limits (beyond / return / exact)
  test_got_flip.py   Meridian flip
  test_got_guide.py  PulseGuide issued mid-GOTO
  test_man.py        Manual movement (MoveAxis) + axis limits
  test_gui.py        Autoguiding (PulseGuide)
  test_guide_effect.py PulseGuide position/time effect (limits, flip, tracking)
  test_mer.py        Pier side / meridian flip (same-object)
  test_prk.py        Park / unpark (rejects every command while parked)
  test_ref.py        Reference / home (positions, tracking, mid-goto)
  test_limits.py     Runtime-changed limits are enforced
  test_net.py        Network and interoperability
```

## Requirements

- NRM-HA board reachable (default `http://192.168.7.1:11111`, configurable in
  `profile.json`).
- Python 3 + `pytest`.

```sh
python3 -m pip install pytest
```

## Running

```sh
make -C test test          # both suites (Alpaca + REST)
make -C test test-alpaca   # Alpaca only
make -C test test-rest     # REST only
make -C test flash         # flash in TEST MODE (enables the debug seam)
```

Manual equivalent:

```sh
# Alpaca (port 11111)
cd test/alpaca
python3 -m pytest -q            # the whole Alpaca suite
python3 -m pytest --lf          # only the ones that failed last time
python3 -m pytest test_got.py   # one family
python3 -m pytest -k "park"     # by name

# REST (port 80)
python3 test/api/test_blackbox.py 192.168.7.1
cd test/api && python3 -m pytest test_state_machine.py
```

The REST state machine (`test_state_machine.py`) requires the board flashed in
**TEST MODE** (it uses the debug seam `/api/debug/power` and `/api/debug/fault`
to reach the ERROR state). Everything else runs against normal firmware.

## Diagnostics (TEST MODE)

Several REST/Alpaca command handlers expose an `X-NRM-Snapshot` response header
(only when built with `NRM_TEST_MODE`), carrying the motors state at the instant
the command was received — `ra`/`dec` (steps), `st`/`tr`/`g` (status/tracking/
guiding), `ras`/`decs` (speeds) and `t` (µs). A black-box harness reads this
header to make deterministic measurements (e.g. post-STOP or post-pulse
movement) without the HTTP round-trip latency. See `oracle.py` for the slew-time
model and `conftest.py` for `parse_snapshot`/`assert_goto_duration`.

## Skill

There is a project skill, **`nrm-test`** (`.claude/skills/nrm-test/SKILL.md`),
that encapsulates this whole workflow: how to run the suites, how to recover the
board when the USB NCM link is down, how to diagnose a failure (transport vs
device vs assertion, serial capture for crashes) and how to fix it without
weakening the test expectations.

In Claude Code, invoke it with:

```
/nrm-test
```

or just ask to "run the tests and fix any failure" and the skill triggers by its
description.

## Results

- **133 passed, 0 skipped** (Alpaca) plus **7 REST validation checks** and
  **51 state-machine tests**. There are no `skip`-marked cases: what can be
  exercised via Alpaca runs; what requires physical acceptance (encoder, camera,
  power bench, N.I.N.A.) stays out of this automated suite and is neither
  declared passed nor pending.
- Each case validates `ErrorNumber` in addition to HTTP 200, and uses an
  independent oracle (angular distance, slew-time ramp model) for the reported
  position and duration, not the value of the API itself.

## Profile

`profile.json` is the versioned profile. `null` fields block the tests that use
them; a pending value is not a PASS. `budgets.reported_position_deg` is the
tolerance for the reported position (not physical pointing).
