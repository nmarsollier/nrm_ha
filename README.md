# NRM-HA — Harmonic Drive Equatorial Mount

Equatorial mount with harmonic drives for astrophotography, controlled by an
ESP32-S3 over a USB CDC-ACM serial protocol. A desktop gateway (Go) translates
that protocol to Alpaca/ASCOM for N.I.N.A. and PHD2.

## Hardware

- **Board**: ESP32-S3-WROOM-1 N16R8 
- **Motor drivers**: Integrated closed-loop ISS42 (64 microsteps via DIP switches, specs in [`MOTOR.txt`](MOTOR.txt))
- **Motors**: 2× NEMA 17 Closed Loop (0.44 Nm torque, integrated driver)
- **Harmonic Drives**: 100:1 reduction
- **Belt reduction**: 3:1 (GT2 9mm, 20T → 60T)
- **Total reduction**: 300:1 on both axes
- **Power**: 12V 5A supply → LM2596 (12V→5.5V for ESP32-S3). Motors powered directly from 12V.
- **Power sense**: 10k/10k divider on the 5.5V rail → GPIO 1 (ADC). When the 12V switch is off (USB-only), the mount enters ERROR and refuses to move the motors.
- **LED**: PWM indicator (GPIO 42) — three states: dim (~10%) at idle, bright (100%) during slewing, beacon when motors does not have energy.
- **Buzzer**: passive event beeper (GPIO 41, 2 kHz) — beeps on boot and on goto/move-axis start & end.
- **Outputs**: STEP/DIR/LED/buzzer all pass through a UMC2003 Darlington array (open-collector sinking).

### Harmonic Drives

- Harmonic Drive 100:1 reduction (https://www.ebay.com/itm/286960016334)
- Belt reduction 3:1 at harmonic input: GT2 9mm, 20T → 60T
- Total reduction on both axes: 300:1
- DEC body threads onto the RA structure through the Harmonic output
- DEC control cables pass through the Harmonic center

### NEMA 17 Closed Loop Motors

- NEMA 17 Closed Loop with integrated ISS42 driver (specs in [`MOTOR.txt`](MOTOR.txt))
- https://www.amazon.com/dp/B0FHHWT8Q8
- Configured at 64 microsteps via DIP switches (12800 steps/rev)
- Torque: 0.44 Nm
- Resolution DIP switches (SW3-SW6): 64 microsteps = SW3 OFF, SW4 ON, SW5 OFF, SW6 ON

### Pin mapping

| GPIO | Function  | Notes                                              |
|------|-----------|----------------------------------------------------|
| 1    | PWR-SENSE | 12V rail present (ADC, 10k/10k divider on 5.5V)     |
| 42   | LED (PWM) | External status indicator (via UMC2003)                          |
| 41   | Buzzer    | Event beeper, 2 kHz PWM (via UMC2003)                          |
| 14   | RA STEP   | Right ascension step pulse (via UMC2003)             |
| 10   | RA DIR    | Right ascension axis direction (via UMC2003)         |
| 12   | DEC STEP  | Declination step pulse (via UMC2003)                 |
| 9    | DEC DIR   | Declination axis direction (via UMC2003)             |

### Level shifting (UMC2003 Darlington array)

The integrated ISS42 drivers use optocouplers on STEP/DIR that accept 5–24 V control signals (~10 mA). The ESP32-S3 has 3.3 V logic and is not 5 V tolerant. All outputs (STEP, DIR for both axes, LED and buzzer) pass through a UMC2003 — a 7-channel Darlington array with open-collector (sinking) outputs.

Each GPIO drives a UMC2003 input directly (internal base resistor, no external resistor needed). The output sinks current to ground:

```
GPIO (3.3 V) → INx UMC2003        OUTx → load → +V

  GPIO HIGH → OUTx to GND (load active)
  GPIO LOW  → OUTx high-impedance
```

Outputs are "active-low": the load (driver opto, or LED/buzzer with its resistor) connects between +V and the output. The COM pin (common anode of the protection diodes) ties to +V. There are no inductive loads in this revision, so the diodes aren't strictly needed.

**Power connections:**

| Pin  | Purpose                                                              |
|------|----------------------------------------------------------------------|
| 12 V | Motor power (DC+) and control inputs (PU+/DR+). ISS42 accepts 5–24 V on control inputs. |
| GND  | Common ground — shared by the board, the UMC2003 (pin 8) and both drivers. |

### Motor driver wiring (ISS42)

Terminals in order: `DC+, GND, AM-, AM+, EN-, EN+, DR-, DR+, PU-, PU+`.

| Terminal    | Connect to |
|-------------|------------|
| DC+ / GND   | 12 V motor power (shared ground) |
| PU+ (STEP+) | +12 V |
| PU- (STEP-) | UMC2003 output (sinks to GND) |
| DR+ (DIR+)  | +12 V |
| DR- (DIR-)  | UMC2003 output (sinks to GND) |
| EN+ / EN-   | **unconnected** — enabled by default |
| AM+ / AM-   | not connected (this revision) |

- **STEP (PU)**: rising edge, one microstep per low→high transition, pulse > 2.5 µs (firmware emits 6 µs for Darlington margin).
- **DIR (DR)**: level input, must be stable ≥ 50 µs before the STEP pulse.
- **ENABLE (EN)**: inverted — connecting EN+ to +V and EN- to GND *disables* the driver; leaving both floating *enables* it (default).
- **Common ground**: the UMC2003 GND (pin 8) must share the same ground as the driver's 12 V supply.

## Architecture

```
Desktop gateway (Go) — Alpaca + UI
  └─ USB CDC-ACM serial protocol
       └─ ESP32-S3: Proto → Mount → Motors → STEP/DIR (RMT)
```

- **usb_cdc** (`main/usb_cdc/`) — CDC-ACM transport (TinyUSB): init, RX task, TX.
- **proto** (`main/proto/`) — the serial protocol: framing, dispatch, one handler per message family, session (`boot_id`, `config_rev`, idempotency).
- **mount** (`main/mount/`) — orchestration: state, coordinates, settings.
- **motors** (`main/motors/`) — motion: STEP/DIR GPIO, RMT pulse generation.
- **power** (`main/power/`) — GPIO 1 ADC: 12 V rail sense (blocks motion when unpowered).
- **led** (`main/led/`) — GPIO 42 PWM: dim / bright / beacon.
- **buzzer** (`main/buzzer/`) — GPIO 41, 2 kHz PWM beeps.
- **runtime** (`main/runtime/`) — init sequence + periodic loop.

## Protocolo CDC-ACM (serial)

The ESP32-S3 speaks a small, deterministic serial protocol over USB CDC-ACM.
The desktop gateway (Go) is the only client and translates the protocol to
Alpaca/REST/UI. The ESP is the authority over the mount: it publishes a coherent
state and executes a small set of operations.

### Message families

| Family | Role |
|---|---|
| `capabilities` | identity + static capabilities (read once on connect) |
| `state` | coherent snapshot of the whole mount (on-demand read) |
| `config` | read/edit persistent config (site, time, guide rates) |
| `control` | desired continuous-motion state (tracking + manual rates) |
| `action` | operation with a start and end: `stop`, `goto`, `guide`, `home`, `park`, `unpark` (and `sync`, deferred) |

**Framing**: frame = `[u32 little-endian length][JSON UTF-8]` (no app CRC — USB is
reliable). ~2048 B max per frame, preallocated buffers.

The full protocol spec (format, per-family semantics, design decisions) lives in
[`main/proto/README.md`](main/proto/README.md).

### Tests

`test/proto/` drives the mount through the serial protocol (pytest).

```sh
make flash        # flash the firmware
make test-cdc     # run the serial protocol suite (auto-detects the CDC port)
```

## Setup

### Requirements

| Tool    | Version      | Purpose                       |
|---------|--------------|-------------------------------|
| ESP-IDF | v6.0.1       | Firmware build system         |
| Python  | 3.10+ (venv) | Required by ESP-IDF tools     |
| CMake   | 4.x          | Build system                  |
| Ninja   | 1.x          | Build executor                |

### macOS install

```sh
mkdir -p ~/.espressif
git clone --depth 1 --branch v6.0.1 https://github.com/espressif/esp-idf.git ~/.espressif/v6.0.1/esp-idf
export IDF_TOOLS_PATH="$HOME/.espressif/tools"
cd ~/.espressif/v6.0.1/esp-idf && bash install.sh esp32s3

brew install cmake ninja
```

Add to `~/.zshrc` (adjust paths to match your system):

```sh
export IDF_PATH="$HOME/.espressif/v6.0.1/esp-idf"
export IDF_TOOLS_PATH="$HOME/.espressif/tools"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/tools/python/v6.0.1/venv"
export PYTHON="$IDF_PYTHON_ENV_PATH/bin/python"
alias idf.py="$PYTHON $IDF_PATH/tools/idf.py"
```

### Build

```sh
idf.py set-target esp32s3
idf.py build flash monitor
```

## Project conventions

- Language: **C** (C23), snake_case
- One `.c` file per use case within each module
- Public API: `module.h` — Internal API: `module_internal.h`
- Function prefix matches module name (`motors_`, `mount_`, …)
- Dependencies: proto → mount → motors (no reverse deps)
