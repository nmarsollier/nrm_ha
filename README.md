# NRM-HA — Harmonic Drive Equatorial Mount

Equatorial mount with harmonic drives for astrophotography, controlled by an ESP32-S3, compatible with N.I.N.A. (Alpaca / ASCOM) and its own REST API.

This firmware runs on an ESP32-S3 44-pin board, driving two NEMA 17 closed-loop stepper motors with integrated drivers. It exposes a full ASCOM Alpaca interface on port 11111 so that N.I.N.A. and other clients can discover and control the mount directly.

## Hardware

- **Board**: ESP32-S3 44-pin (16 MB Flash, 8 MB PSRAM)
- **Motor drivers**: Integrated closed-loop (64 microsteps via DIP switches)
- **Motors**: 2× NEMA 17 Closed Loop (0.44 Nm torque, integrated driver)
- **Harmonic Drives**: 100:1 reduction
- **Belt reduction**: 3:1 (HTD3M 15T → 45T, 171mm belt)
- **Total reduction**: 300:1 on both axes
- **Power**: 12V 5A supply → Mini DC 360 (12V→5.5V for ESP32-S3). Motors powered directly from 12V.
- **LED**: PWM indicator (GPIO 4) — three states: dim (~10%) at idle, bright (100%) during slewing, slow breathing on error.

### Harmonic Drives

- Harmonic Drive 100:1 reduction (https://www.ebay.com/itm/286960016334)
- Belt reduction 3:1 at harmonic input: HTD3M 15T → HTD3M 45T, 171mm belt
- Total reduction on both axes: 300:1
- DEC body threads onto the RA structure through the Harmonic output
- DEC control cables pass through the Harmonic center

### NEMA 17 Closed Loop Motors

- NEMA 17 Closed Loop with integrated driver
- https://www.amazon.com/dp/B0FHHWT8Q8
- Configured at 64 microsteps via DIP switches
- Torque: 0.44 Nm
- Hardware torque limiting (SW6 ON)
- Motors emit ALARM signal (active low) on position error

### Pin mapping

| GPIO | Function   | Notes                                              |
|------|------------|----------------------------------------------------|
| 4    | LED (PWM)  | External status indicator                          |
| 10   | DEC DIR    | Declination axis direction (via BC337)             |
| 11   | DEC STEP   | Declination step pulse (via BC337)                 |
| 12   | RA DIR     | Right ascension axis direction (via BC337)         |
| 13   | RA STEP    | Right ascension step pulse (via BC337)             |
| 14   | MOTORS EN- | Shared enable (GPIO LOW = enabled, via BC337)      |
| 46   | ALARM- RA  | Input with pull-up, active low = fault             |
| 3    | ALARM- DEC | Input with pull-up, active low = fault             |

### Level shifting (BC337 NPN)

The integrated drivers use optocouplers on STEP/DIR/EN that require 5V / ~10mA. The ESP32-S3 has 3.3V logic and is not 5V tolerant. Each output signal uses a BC337 transistor as a switch:

```
GPIO ──[1kΩ]── Base (center)
                 │
5V ──→ COM+ driver ──→ [opto] ──→ COM- driver ──→ Collector (left)
                                                    │
                                                  Emitter (right) ──→ GND
```

Facing the flat side (label readable), pins down: left=Collector, center=Base, right=Emitter. 5 transistors + 5 1kΩ resistors total.

**EN- logic:**
The driver inverts the logic: EN+ = 5V + EN- = LOW (GND) → motor FREE (disabled). With the BC337:
  GPIO LOW  → transistor OFF → EN- floating → motor ENABLED
  GPIO HIGH → transistor ON  → EN- = GND    → motor DISABLED

**Power connections:**
| Pin | Purpose                                    |
|-----|--------------------------------------------|
| 5V  | COM+ data terminals for both motors        |
| GND | Common ground                              |

### ALARM behavior

The closed-loop motors emit an ALARM signal (active low) when they detect a position error (stall, overcurrent, lost steps). If either ALARM pin goes LOW, the mount enters an unrecoverable ERROR state: motors are immediately disabled and all motion commands are rejected until reboot.

## Architecture

```
N.I.N.A. / ASCOM client
Alpaca REST API  (port 11111)  ◄── also: UDP discovery on 32227
REST API  (port 80)  ── serves embedded SPA at /
  Mount  (orchestration, coordinates, settings)
  Motors  (move / track, STEP/DIR/EN GPIO, RMT pulse generation, ALARM monitoring)

Network  (WiFi station + setup AP fallback)
USB Net  (RNDIS/ECM gadget, 192.168.7.1, DHCP server)
LED  (GPIO 4 PWM: dim / bright / breathing)
Runtime  (init sequence + periodic loop)
```

## USB Ethernet (RNDIS/ECM)

The ESP32-S3 acts as a USB Ethernet gadget via its native USB-OTG peripheral. Connect the mount to a laptop with a USB-C cable and it appears as a network adapter — no WiFi needed in the field.

| Property        | Value                    |
|-----------------|--------------------------|
| Protocol        | ECM (Linux/macOS) / RNDIS (Windows) |
| ESP32-S3 IP     | `192.168.7.1` (static)   |
| Host IP         | `192.168.7.2` – `192.168.7.10` (DHCP) |
| REST API        | `http://192.168.7.1/api/status` |
| Alpaca API      | `http://192.168.7.1:11111` |
| UDP Discovery   | `192.168.7.1:32227`      |

WiFi and USB Ethernet work simultaneously — all servers bind to `INADDR_ANY`.

### OS-specific notes

- **Windows 10/11**: RNDIS driver is built-in. The device appears as "Mount USB Ethernet" in Network adapters.
- **macOS**: CDC-ECM is natively supported. The interface appears as `usb0`.
- **Linux**: CDC-ECM is handled by the `cdc_ether` kernel module (loaded automatically).

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
