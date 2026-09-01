# NRM-HA — Harmonic Drive Equatorial Mount

Equatorial mount with harmonic drives for astrophotography, controlled by an ESP32-S3, compatible with N.I.N.A. (Alpaca / ASCOM) and its own REST API.

This firmware runs on an ESP32-S3 44-pin board, driving two NEMA 17 closed-loop stepper motors with integrated drivers. It exposes a full ASCOM Alpaca interface on port 11111 so that N.I.N.A. and other clients can discover and control the mount directly.

## Hardware

- **Board**: ESP32-S3 44-pin (16 MB Flash, 8 MB PSRAM)
- **Motor drivers**: Integrated closed-loop ISS42 (64 microsteps via DIP switches, specs in [`MOTOR.txt`](MOTOR.txt))
- **Motors**: 2× NEMA 17 Closed Loop (0.44 Nm torque, integrated driver)
- **Harmonic Drives**: 100:1 reduction
- **Belt reduction**: 3:1 (HTD3M 15T → 45T, 171mm belt)
- **Total reduction**: 300:1 on both axes
- **Power**: 12V 5A supply → Mini DC 360 (12V→5.5V for ESP32-S3). Motors powered directly from 12V.
- **LED**: PWM indicator (GPIO 10) — three states: dim (~10%) at idle, bright (100%) during slewing, slow breathing on error.
- **Buzzer**: passive event beeper (GPIO 9, 2 kHz) — beeps on boot and on goto/move-axis start & end.
- **Accelerometer**: 2× ADXL345 on I2C (GPIO 2 SDA / GPIO 1 SCL) — tilt + rotation for polar alignment and axis limits (see `main/accelerometer/README.md`).
- **Outputs**: STEP/DIR/LED/buzzer all pass through a UMC2003 Darlington array (open-collector sinking).

### Harmonic Drives

- Harmonic Drive 100:1 reduction (https://www.ebay.com/itm/286960016334)
- Belt reduction 3:1 at harmonic input: HTD3M 15T → HTD3M 45T, 171mm belt
- Total reduction on both axes: 300:1
- DEC body threads onto the RA structure through the Harmonic output
- DEC control cables pass through the Harmonic center

### NEMA 17 Closed Loop Motors

- NEMA 17 Closed Loop with integrated ISS42 driver (specs in [`MOTOR.txt`](MOTOR.txt))
- https://www.amazon.com/dp/B0FHHWT8Q8
- Configured at 64 microsteps via DIP switches
- Torque: 0.44 Nm
- Hardware torque limiting (SW6 ON)

### Pin mapping

| GPIO | Function  | Notes                                              |
|------|-----------|----------------------------------------------------|
| 10   | LED (PWM) | External status indicator (via UMC2003)                          |
| 9    | Buzzer    | Event beeper, 2 kHz PWM (via UMC2003)                          |
| 2    | I2C SDA   | ADXL345 accelerometer (I2C)                             |
| 1    | I2C SCL   | ADXL345 accelerometer (I2C)                             |
| 14   | RA STEP   | Right ascension step pulse (via UMC2003)             |
| 13   | RA DIR    | Right ascension axis direction (via UMC2003)         |
| 12   | DEC STEP  | Declination step pulse (via UMC2003)                 |
| 11   | DEC DIR   | Declination axis direction (via UMC2003)             |

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
N.I.N.A. / ASCOM client
Alpaca REST API  (port 11111)  ◄── also: UDP discovery on 32227
REST API  (port 80)  ── serves embedded SPA at /
  Mount  (orchestration, coordinates, settings)
  Motors  (move / track, STEP/DIR GPIO, RMT pulse generation)

USB Net  (CDC-NCM gadget, 192.168.7.1, DHCP server)
LED  (GPIO 10 PWM: dim / bright / breathing)
Buzzer  (GPIO 9, 2 kHz PWM beeps: boot / motion start / motion end)
Accelerometer  (ADXL345 I2C: tilt / heading for polar align + limits)
Runtime  (init sequence + periodic loop)
```

## USB Ethernet (CDC-NCM)

The ESP32-S3 acts as a USB Ethernet gadget via its native USB-OTG peripheral. Connect the mount to a laptop with a USB-C cable and it appears as a network adapter — the mount's only network interface.

| Property        | Value                    |
|-----------------|--------------------------|
| Protocol        | CDC-NCM (Linux/macOS/Windows) |
| ESP32-S3 IP     | `192.168.7.1` (static)   |
| Host IP         | `192.168.7.2` – `192.168.7.10` (DHCP) |
| REST API        | `http://192.168.7.1/api/status` |
| Alpaca API      | `http://192.168.7.1:11111` |
| UDP Discovery   | `192.168.7.1:32227`      |

USB Ethernet is the mount's only network interface — all servers bind to `INADDR_ANY`.

### OS-specific notes

- **Windows 10/11**: CDC-NCM is supported natively; the device appears as a USB Ethernet adapter.
- **macOS**: CDC-NCM is supported natively (AppleUSBNCM); the device appears as "Mount USB Ethernet".
- **Linux**: CDC-NCM is handled by the `cdc_ncm` kernel module (loaded automatically).

## Setup

### Requirements

| Tool    | Version      | Purpose                       |
|---------|--------------|-------------------------------|
| ESP-IDF | v6.0.1       | Firmware build system         |
| Python  | 3.10+ (venv) | Required by ESP-IDF tools     |
| CMake   | 4.x          | Build system                  |
| Ninja   | 1.x          | Build executor                |
| Node.js | 22+          | Web UI build (`www/build.js`) |
| npm     | 9+           | UI dependencies (Alpine.js)   |

### macOS install

```sh
# ESP-IDF v6.0.1
mkdir -p ~/.espressif
git clone --depth 1 --branch v6.0.1 https://github.com/espressif/esp-idf.git ~/.espressif/v6.0.1/esp-idf
export IDF_TOOLS_PATH="$HOME/.espressif/tools"
cd ~/.espressif/v6.0.1/esp-idf && bash install.sh esp32s3

# build tools + Node.js
brew install cmake ninja node
cd www && npm install
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

### Web UI

The SPA lives in `www/src/` (HTML, CSS, JS). Rebuild the embedded UI with:

```sh
node www/build.js
idf.py build
```

The resulting `www/dist/index.html` is embedded into the firmware via `EMBED_TXTFILES`.

## Project conventions

- Language: **C** (C23), snake_case
- One `.c` file per use case within each module
- Public API: `module.h` — Internal API: `module_internal.h`
- Function prefix matches module name (`motors_`, `mount_`, `alpaca_bridge_`, …)
- Dependencies: REST → Mount → Motors (no reverse deps)
