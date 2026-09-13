# NRM-HA — Harmonic Drive Equatorial Mount

Equatorial mount with harmonic drives for astrophotography, controlled by an ESP32-S3, compatible with N.I.N.A. (Alpaca / ASCOM) and its own REST API.

This firmware runs on an ESP32-S3 44-pin board, driving two NEMA 17 stepper motors with TMC2209 drivers. It exposes a full ASCOM Alpaca interface on port 11111 so that N.I.N.A. and other clients can discover and control the mount directly.

## Hardware

- **Board**: ESP32-S3 44-pin (16 MB Flash, 8 MB PSRAM)
- **Motor drivers**: 2× TMC2209 in STEP/DIR mode, configured over single-wire UART (32 µsteps + 256 interpolation)
- **Motors**: 2× NEMA 17 stepper (1.8°/step)
- **Harmonic Drives**: 100:1 reduction
- **Belt reduction**: 3:1 (HTD3M 15T → 45T, 171mm belt)
- **Total reduction**: 300:1 on both axes
- **Power**: 12V 5A supply → Mini DC 360 (12V→5.5V for ESP32-S3). Motors powered directly from 12V.
- **LED**: PWM indicator (GPIO 6) — three states: dim (~10%) at idle, bright (100%) during slewing, slow breathing on error.
- **Buzzer**: passive event beeper (GPIO 1, 2 kHz) — beeps on boot and on goto/move-axis start & end.
- **Accelerometer**: 1× ADXL345 on I2C (GPIO 4 SDA / GPIO 5 SCL) — tilt + rotation for polar alignment and axis limits (see `main/accelerometer/README.md`).
- **Outputs**: direct 3.3 V logic — no level shifting. LED and buzzer are common-anode to 3.3 V (GPIO sinks).

### Harmonic Drives

- Harmonic Drive 100:1 reduction (https://www.ebay.com/itm/286960016334)
- Belt reduction 3:1 at harmonic input: HTD3M 15T → HTD3M 45T, 171mm belt
- Total reduction on both axes: 300:1
- DEC body threads onto the RA structure through the Harmonic output
- DEC control cables pass through the Harmonic center

### NEMA 17 Motors + TMC2209 Drivers

- NEMA 17 stepper, bipolar, 1.8°/step
- TMC2209 in STEP/DIR mode, configured over single-wire UART
- Microstepping: 32 (MRES=3) with 256-step interpolation (intpol)
- Each driver has its own UART (one GPIO per driver); both use MS1/MS2 to GND → address 0x00
- Run/hold current: irun=10, ihold=8 (TMC2209 0–31 scale, see `main/tmc/tmc_init.c`)

### Pin mapping

| GPIO | Function    | Notes                                       |
|------|-------------|---------------------------------------------|
| 2    | DEC UART TX | TMC2209 DEC TX (to PDN_UART via 1 kΩ)       |
| 21   | RA UART TX  | TMC2209 RA TX (to PDN_UART via 1 kΩ)        |
| 14   | RA STEP     | Right ascension step pulse (direct, RMT)    |
| 13   | RA DIR      | Right ascension axis direction (direct)     |
| 12   | RA UART RX  | TMC2209 RA RX (direct to PDN_UART)          |
| 11   | DEC STEP    | Declination step pulse (direct, RMT)        |
| 10   | DEC DIR     | Declination axis direction (direct)         |
| 9    | DEC UART RX | TMC2209 DEC RX (direct to PDN_UART)         |
| 6    | LED         | Status indicator (direct, anode to 3.3 V)   |
| 5    | I2C SCL     | ADXL345 accelerometer (I2C)                 |
| 4    | I2C SDA     | ADXL345 accelerometer (I2C)                 |
| 1    | Buzzer      | Event beeper, 2 kHz PWM (direct, to 3.3 V)  |

### Direct wiring (no level shifting)

This board does not use a UMC2003/ULN2003. STEP/DIR connect directly to the TMC2209 drivers (3.3 V tolerant inputs). The LED and buzzer sit between 3.3 V and their GPIO (common anode): the GPIO sinks current to drive them (active-low), so LEDC is configured with `output_invert`.

Each TMC2209 talks over a single-wire UART — a TX/RX pair per driver. TX connects through a 1 kΩ series resistor to PDN_UART and RX connects directly; TX is floated while reading the response. Both drivers tie MS1 and MS2 to GND → address 0x00.

**Power connections:**

| Pin   | Purpose                                         |
|-------|-------------------------------------------------|
| 12 V  | TMC2209 motor supply (VM)                       |
| 3.3 V | LED and buzzer common anode                     |
| GND   | Common ground — shared by board and drivers      |

### Motor driver wiring (TMC2209)

- **VM / GND** — 12 V motor power (shared ground).
- **STEP / DIR** — direct from ESP32-S3 GPIOs (RMT for STEP, GPIO for DIR).
- **PDN_UART** — single-wire UART, one GPIO per driver.
- **EN** — unconnected (enabled by default).
- **MS1 / MS2** — both to GND (UART address 0x00).

- **STEP**: rising edge, one microstep per transition; TMC2209 needs ≥ 100 ns HIGH/LOW (firmware emits 2 µs HIGH / 1 µs LOW).
- **DIR**: level input, stable before the STEP pulse.

## Architecture

```
N.I.N.A. / ASCOM client
Alpaca REST API  (port 11111)  ◄── also: UDP discovery on 32227
REST API  (port 80)  ── serves embedded SPA at /
  Mount  (orchestration, coordinates, settings)
  Motors  (move / track, STEP/DIR GPIO, RMT pulse generation)
  TMC  (TMC2209 UART config + verification)

USB Net  (CDC-NCM gadget, 192.168.7.1, DHCP server)
LED  (GPIO 6 PWM: dim / bright / breathing)
Buzzer  (GPIO 1, 2 kHz PWM beeps: boot / motion start / motion end)
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
