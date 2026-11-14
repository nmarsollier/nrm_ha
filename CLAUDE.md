# Reglas de Desarrollo del Proyecto NRM-HA

Mantener este archivo en formato simple, para que pueda leerse y editarse rapidamente

## Proyecto hermano (gateway Go)

Este firmware se comunica por USB CDC-ACM con una app de escritorio Go que expone
Alpaca/UI. Su código vive en **otro repo local**:

- **`~/Dev/nrm_ha_go`** — gateway Go (módulo `github.com/nmarsollier/nrm_ha_go`).
  Documentación en su `README.md` y `CLAUDE.md`.

Cuando se necesite buscar referencias de código del gateway, leer su protocolo de
consumo o consultar cómo se implementa el lado Alpaca/UI, mirar ahí.

## Definicion del proyecto

- Logica para manejar montura ecuatorial con harmonic drives para astrofotografia
- Montura NRM-HA con harmonic drives de reduccion 100:1 y poleas GT2 9mm 3:1 (20T→60T)
- Reduccion total: 300:1 en ambos ejes
- Utiliza una placa ESP32-S3 44 pines
- Utiliza 2 motores Nema 17 controlados por drivers TMC2209 (microsteps variables: 64 en tracking, 8 en slew)
- Los motores se alimentan directo de fuente 12V (VM de los TMC2209), la placa ESP32-S3 via LM2596 (12V→5.5V)
- Estructura metalica en hierro 1/8, dos cuerpos (RA y DEC)
- La montura posee 2 botones fisicos, Stop y Home
- Posee una pantalla minimalista OLED de 0.98 inch, se muestra verticalmente

### Harmonic Drives

- Harmonic Drive de reduccion 100:1 (https://www.ebay.com/itm/286960016334)
- Reduccion de poleas 3:1 en la entrada del harmonico: GT2 9mm 20T → 60T
- Reduccion Total en ambos Ejes: 300:1
- El cuerpo DEC se enrosca a la estructura RA a traves de la salida del Harmonic
- Los cables que controlan el eje DEC se pasan por dentro del Harmonic

### Motores Nema 17 con TMC2209

- Motores Nema 17 de 1.8° controlados por drivers TMC2209 (StealthChop/SpreadCycle)
- Los drivers se configuran por UART single-wire (TX por resistor 1kΩ, RX directo a PDN_UART)
- Microsteps variables via CHOPCONF.MRES: 64 en tracking/guiding, 8 en slews grandes
- Reduccion total 300:1 (3:1 poleas × 100:1 harmonico)
- Configuracion de referencia del TMC2209: ver `TMC.md` del repo bft_firmware (datasheet rev 1.09)
- Corrientes IRUN/IHOLD provisionales en `config.h` (a afinar para el Nema 17 final de 2.0A)

### Drivers TMC2209

- Driver inteligente por eje, alimentado por VM (12V), logica VCC_IO 3.3V (sin level shifting)
- STEP/DIR directos desde GPIO (3.3V CMOS, flanco ascendente en STEP)
- Sin pin ENABLE (habilitado por defecto) ni ALARM conectados
- UART single-wire por eje: TX (GPIO) → 1kΩ → PDN_UART; RX (GPIO) directo a PDN_UART
- Deteccion de energia: los drivers quedan mudos sin VM → `tmc2209_probe()` (lee DRV_STATUS)

## Placa ESP32-S3

### Identificacion

- **Modelo**: ESP32-S3 44 pines
- https://www.amazon.com/dp/B0F5QCK6X5
- **SoC**: ESP32-S3 (Xtensa 32-bit LX7, doble nucleo)
- **Chip USB-Serial**: Bridge QinHeng CH343 (VID 0x1A86) cableado a UART0 — flasheo + monitor; el protocolo CDC-ACM va por el USB nativo del SoC (VID 0x303A, /dev/cu.usbmodem*)
- **Flash**: 16 MB
- **PSRAM**: 8 MB (octal)

### Especificaciones tecnicas

- **CPU**: Tensilica Xtensa 32-bit LX7, doble nucleo, hasta 240 MHz
- **Alimentacion**: 5.5V via LM2596
- **Logica I/O**: 3.3V (no tolera 5V, LED y buzzer directos a GPIO 5 y 4)
- **GPIO digitales**: 45 (configurables)
- **ADC**: 2 conversores SAR ADC de 12 bits, hasta 20 canales
- **UART**: 3 controladores UART
- **I2C**: 2 controladores I2C
- **SPI**: 4 controladores SPI
- **PWM**: 8 canales LEDC independientes
- **USB**: USB OTG 1.1 (nativo, sin chip externo)

### Pinout definitivo NRM-HA

| GPIO | Funcion           | Notas                                              |
|------|-------------------|----------------------------------------------------|
| 10   | STEP- RA          | Pulso STEP eje ascension recta (directo al TMC2209)  |
| 9    | DIR- RA           | Direccion eje ascension recta (directo al TMC2209)    |
| 15   | STEP- DEC         | Pulso STEP eje declinacion (directo al TMC2209)       |
| 7    | DIR- DEC          | Direccion eje declinacion (directo al TMC2209)        |
| 12   | TMC RA TX         | UART1 TX → 1kΩ → PDN_UART del TMC de RA              |
| 13   | TMC RA RX         | UART1 RX directo a PDN_UART del TMC de RA             |
| 17   | TMC DEC TX        | UART2 TX → 1kΩ → PDN_UART del TMC de DEC             |
| 18   | TMC DEC RX        | UART2 RX directo a PDN_UART del TMC de DEC            |
| 5    | LED externo       | LEDC PWM, indicador de estado (directo a GPIO)        |
| 4    | BUZZER            | Buzzer activo (directo a GPIO, HIGH=beep)             |

**Conexion TMC2209 (sin level shifting):**
Los drivers TMC2209 aceptan logica 3.3V en STEP/DIR, asi que van
directos a los GPIO (no hace falta el UMC2003). Cada TMC se configura
por un UART single-wire dedicado: TX (con resistor serie de 1kΩ) y RX
(directo) sobre el pin PDN_UART. El pin de habilitacion (EN) no se
conecta (habilitado por defecto) y los pines de diagnostico (DIAG/INDEX)
no se usan en esta revision.

**Alimentacion:**
| Pin  | Funcion           |
|------|-------------------|
| 12V  | Alimentacion de potencia (VM de los TMC2209) y logica VCC_IO (3.3V de la placa) |
| GND  | Tierra comun (placa y drivers comparten la misma tierra) |

**Uso futuro:**
| GPIO | Funcion     | Notas                              |
|------|-------------|------------------------------------|
| 6    | I2C SCL (2do bus) | Display / sensor adicional  |
| 1    | Libre       | (ex PWR-SENSE, ya no se usa ADC)   |
| 14   | Libre       | (ex STEP- RA)                      |
| 42   | Libre       | (ex LED externo)                   |
| 41   | Libre       | (ex BUZZER)                        |

### Pines con restricciones (NO USAR)

| GPIO | Restriccion                                 |
|------|---------------------------------------------|
| 0    | Boot: LOW en reset = modo flash (ROM boot)  |
| 3    | Strapping JTAG (LOW al boot)                |
| 43   | UART0 TX, consola debug (USB-Serial nativo) |
| 44   | UART0 RX, consola debug (USB-Serial nativo) |
| 45   | Strapping (VDD_SPI voltage)                 |
| 46   | Strapping: modo de arranque (con GPIO0). GPIO46=1 + GPIO0=0 es invalido |

## Arquitectura

Capas del sistema, de afuera hacia adentro:

```
App de escritorio Go (Alpaca/UI) → USB CDC-ACM → Proto → Mount (orquestacion) → Motors → STEP/DIR (hardware)
```

- **usb_cdc** (`main/usb_cdc/`) — Transporte USB CDC-ACM via TinyUSB: init, RX task, TX.
- **proto** (`main/proto/`) — Protocolo serial: framing, dispatch, handlers por familia, sesion.
- **Mount** (`main/mount/`) — Orquestacion logica del montaje: estado, coordenadas, sincronizacion.
- **Runtime** (`main/runtime/`) — Inicializacion y ciclo de vida del sistema.
- **Motors** (`main/motors/`) — Control de motores de alto nivel y ejecucion hardware: GPIO DIR, RMT para STEP, microsteps variables y gating de energia.
- **TMC** (`main/tmc/`) — Driver TMC2209 por UART single-wire: init, probe, reinit y cambio de microsteps con verificacion.
- **LED** (`main/led/`) — Control PWM del LED externo en GPIO 5. Estados: tenue (normal), brillante (slewing), respiracion (error).
- **Buzzer** (`main/buzzer/`) — Buzzer activo de eventos en GPIO 4 (directo a GPIO, HIGH=beep). Beeps de arranque, inicio y fin de goto/move axis.
- **Utils** (`main/utils/`) — Utilidades transversales (parser, validacion).

## Reglas generales

- Nunca hacer commit o push a github sin autorizacion explicita
- Commits solo cuando la feature completa este verificada y compilando
- Siempre basarse en codigo desde el disco como unica fuente de verdad. El codigo fuente es la unica referencia valida para entender detalles de implementacion
- Antes de cualquier cambio leer el estado actual del archivo a modificar
- Proyecto programado en C, siguiendo estilo funcional cuando sea posible
- Modulos separados por dominio, cada dominio en una carpeta dentro de `main/`
- Cada dominio expone:
    - `dominio.h` — API publica (funciones que otros modulos pueden llamar)
    - `dominio_internal.h` — API interna (funciones compartidas solo entre archivos del mismo modulo)
- Un archivo `.c` por caso de uso dentro de cada modulo
- Las funciones publicas comienzan con el nombre del modulo, seguido de `_`
- Las variables de estado de un modulo nunca se acceden directamente desde afuera. Solo a traves de funciones publicas del modulo
- Respetar principios: DRY - YAGNI - KISS
- La documentacion en headers describe el problema de negocio o caso de uso que resuelve la funcion, no los detalles de implementacion
- Solo comentar codigo si es complejo de comprender para un humano
- El codigo y sus comentarios dentro de archivos con extension .c y .h se escriben en ingles
- Archivos markdown .md se escriben en idioma español
- Los tags de comentarios deben definirse como constante y su valor es el mismo nombre del archivo en mayuscular y sin la extension .c

## Convenciones de Codigo

- **Lenguaje**: C (no C++)
- **snake_case** para funciones y variables
- **UPPER_CASE** para macros y defines
- **Headers**: `#pragma once`, includes organizados: primeros los propios del modulo, luego librerias del framework
- **Logging**: usar `ESP_LOGI()`, `ESP_LOGW()`, `ESP_LOGE()` con tag estatico por archivo
- **Resultados entre capas**: usar los tipos `MotorResultCode` y `MountResult` definidos en el proyecto

## Relaciones entre modulos (dependencias)

- usb_cdc depende de TinyUSB (componente gestionado `espressif/esp_tinyusb`)
- proto depende de usb_cdc (transporte) y de mount/motors (estado y comandos)
- Motors depende de tmc (microsteps y probe de energia) y controla GPIOs DIR y RMT para STEP
- tmc depende de config (pines/registros) y del driver UART de ESP-IDF
