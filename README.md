# NRM-HA — Harmonic Drive Equatorial Mount

Montura ecuatorial con harmonic drives para astrofotografia, controlada por un ESP32-S3, compatible con N.I.N.A. (Alpaca / ASCOM) y su propia REST API.

Este firmware corre en una placa ESP32-S3 44 pines, manejando dos motores NEMA 17 closed-loop con drivers integrados. Expone una interfaz ASCOM Alpaca completa en el puerto 11111 para que N.I.N.A. y otros clientes puedan descubrir y controlar la montura directamente.

## Hardware

- **Board**: ESP32-S3 44 pines (16 MB Flash, 8 MB PSRAM)
- **Motor drivers**: Integrados en los motores closed-loop (64 microsteps via DIP switches)
- **Motors**: 2× NEMA 17 Closed Loop (0.44 Nm torque, driver integrado)
- **Harmonic Drives**: 100:1 de reduccion
- **Reduccion poleas**: 3:1 (HTD3M 15T → 45T, correa 171mm)
- **Reduccion total**: 300:1 en ambos ejes
- **Power**: Fuente 12V 5A → Mini DC 360 (12V→5.5V para ESP32-S3). Motores alimentados directo de 12V.
- **LED**: PWM indicator (GPIO 4) — three states: dim (~10%) at idle, bright (100%) during slewing, slow breathing on error.

### Pin mapping

| GPIO | Function           | Notes                        |
|------|--------------------|------------------------------|
| 4    | LED (PWM)          | External status indicator    |
| 9    | DEC DIR            | Declination axis direction   |
| 10   | DEC STEP           | Declination step pulse       |
| 11   | ALARM- RA          | Input, internal pull-up, active low = fault |
| 12   | RA DIR             | Right ascension axis dir     |
| 13   | RA STEP            | Right ascension step pulse   |
| 14   | MOTORS EN-         | Shared enable for both axes (active low) |
| 46   | ALARM- DEC         | Input, internal pull-up, active low = fault |

### Comportamiento de ALARM

Los motores closed-loop emiten señal ALARM (activo bajo) cuando detectan error de posicion (stall, sobrecorriente). Ante la activacion de cualquiera de los pines ALARM, la montura entra en estado ERROR irrecuperable: deshabilita motores inmediatamente y rechaza todo comando de movimiento hasta reboot.

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
