# Reglas de Desarrollo del Proyecto NRM-HA

Mantener este archivo en formato simple, para que pueda leerse y editarse rapidamente

## Definicion del proyecto

- Logica para manejar montura ecuatorial con harmonic drives para astrofotografia
- Montura NRM-HA con harmonic drives de reduccion 100:1 y poleas HTD3M 3:1 (15T→45T)
- Reduccion total: 300:1 en ambos ejes
- Utiliza una placa ESP32-S3 44 pines
- Utiliza 2 motores Nema 17 paso a paso con drivers TMC2209 (modo STEP/DIR + configuracion UART)
- Los motores se alimentan directo de fuente 12V, la placa ESP32-S3 via LM2596 (12V→5.5V)
- Estructura metalica en hierro 1/8, dos cuerpos (RA y DEC)
- La montura posee 2 botones fisicos, Stop y Home
- Posee una pantalla minimalista OLED de 0.98 inch, se muestra verticalmente

### Harmonic Drives

- Harmonic Drive de reduccion 100:1 (https://www.ebay.com/itm/286960016334)
- Reduccion de poleas 3:1 en la entrada del harmonico: HTD3M 15T → HTD3M 45T, correa 171mm
- Reduccion Total en ambos Ejes: 300:1
- El cuerpo DEC se enrosca a la estructura RA a traves de la salida del Harmonic
claude- Los cables que controlan el eje DEC van por fuera de la montura

### Motores Nema 17 y Drivers TMC2209

- Motores Nema 17 paso a paso bifasicos, 1.8° por paso
- Driver TMC2209 en modo STEP/DIR, configurado por UART single-wire
- Microstepping: 32 (MRES=3) con interpolacion a 256 (intpol)
- Cada driver tiene su propio canal UART (un GPIO por driver), ambos con
  MS1 y MS2 a GND → direccion UART 0x00
- Corriente: irun=14, ihold=8 (escala TMC2209 0–31, ver main/tmc/tmc_init.c)
- Chopper: StealthChop (silencioso) en reposo y < ~1°/s, SpreadCycle por encima;
  en_SpreadCycle=0 + TPWMTHRS=281. Referencia: main/tmc/README.md

## Placa ESP32-S3

### Identificacion

- **Modelo**: ESP32-S3 44 pines
- https://www.amazon.com/dp/B0F5QCK6X5
- **SoC**: ESP32-S3 (Xtensa 32-bit LX7, doble nucleo)
- **Chip USB-Serial**: Integrado en el SoC (USB Serial/JTAG nativo)
- **Flash**: 16 MB
- **PSRAM**: 8 MB (octal)

### Especificaciones tecnicas

- **CPU**: Tensilica Xtensa 32-bit LX7, doble nucleo, hasta 240 MHz
- **Alimentacion**: 5.5V via LM2596
- **Logica I/O**: 3.3V (led y buzzer conectados directo a 3.3V, anodo comun; el GPIO hunde corriente para activarlos)
- **GPIO digitales**: 45 (configurables)
- **ADC**: 2 conversores SAR ADC de 12 bits, hasta 20 canales
- **UART**: 3 controladores UART
- **I2C**: 2 controladores I2C
- **SPI**: 4 controladores SPI
- **PWM**: 8 canales LEDC independientes
- **USB**: USB OTG 1.1 (nativo, sin chip externo)

### Pinout definitivo NRM-HA

| GPIO | Funcion     | Notas                                             |
|------|-------------|---------------------------------------------------|
| 1    | UART DEC TX | TMC2209 DEC TX (al PDN_UART via 1 kΩ)             |
| 2    | UART RA TX  | TMC2209 RA TX (al PDN_UART via 1 kΩ)              |
| 4    | I2C SCL     | Acelerometro ADXL345 (I2C, pull-ups internos)     |
| 5    | I2C SDA     | Acelerometro ADXL345 (I2C, pull-ups internos)     |
| 6    | LED         | LEDC PWM, indicador de estado (directo, anodo 3V3)|
| 7    | BUZZER      | Buzzer pasivo 2 kHz (directo, a 3V3)              |
| 9    | UART DEC RX | TMC2209 DEC RX (directo al PDN_UART)              |
| 10   | DIR DEC     | Direccion declinacion (directo)                   |
| 11   | STEP DEC    | Pulso STEP declinacion (directo, RMT)             |
| 12   | UART RA RX  | TMC2209 RA RX (directo al PDN_UART)               |
| 13   | DIR RA      | Direccion ascension recta (directo)               |
| 14   | STEP RA     | Pulso STEP ascension recta (directo, RMT)         |

**Salidas directas (sin level shifting):**
En esta placa no se usa UMC2003/ULN2003. Las salidas STEP/DIR van directo a los
drivers TMC2209 (entradas 3.3 V tolerantes). El buzzer se conecta entre 3.3 V y
su GPIO (active-low, el GPIO hunde corriente) y usa LEDC con `output_invert`. El
LED se maneja en modo fuente (active-high): a mayor duty, mas brillo, sin
`output_invert`.

Cada TMC2209 se comunica por UART single-wire: un par TX/RX por driver. El TX
va al pin PDN_UART a traves de una resistencia de 1 kΩ (serie) y el RX directo
a ese mismo pin; para leer se flota el TX (modo input) mientras el driver responde.
Ambos drivers llevan MS1 y MS2 a GND → direccion 0x00.

El pin EN de cada TMC2209 va **sin conectar** (habilitado por defecto).

**Alimentacion:**
| Pin  | Funcion                                                 |
|------|---------------------------------------------------------|
| 12V  | Alimentacion de potencia (VM) de los TMC2209             |
| 3V3  | Alimentacion del LED y buzzer (anodo comun)              |
| GND  | Tierra comun (placa y drivers comparten la misma tierra) |

**Uso futuro:**
GPIO libres: 8, 15, 16, 17, 18, 21, 38.

### Pines con restricciones (NO USAR)

| GPIO    | Restriccion                                                        |
|---------|--------------------------------------------------------------------|
| 0       | Boot: LOW en reset = modo flash (ROM boot)                         |
| 3       | Strapping JTAG (LOW al boot)                                       |
| 19, 20  | USB D-/D+ (nativo, consola + USB Net)                              |
| 26–32   | SPI flash                                                          |
| 33–37   | PSRAM octal (8 MB)                                                 |
| 39–42   | JTAG (MTCK/MTDO/MTDI/MTMS), reservados                             |
| 43      | UART0 TX, consola debug (USB-Serial nativo)                        |
| 44      | UART0 RX, consola debug (USB-Serial nativo)                        |
| 45      | Strapping (VDD_SPI voltage)                                        |
| 46      | Strapping: modo de arranque (con GPIO0). GPIO46=1 + GPIO0=0 invalido |

## Arquitectura

Capas del sistema, de afuera hacia adentro:

```
Cliente Web (Alpine.js) → REST API → Mount (orquestacion) → Motors → STEP/DIR (hardware)
```

- **www/** — UI Web embebida programada con Alpine.js. Compila con `node www/build.js`, genera `www/dist/`.
- **REST API** (`main/rest/`) — Expone endpoints HTTP para control de la montura.
- **Mount** (`main/mount/`) — Orquestacion logica del montaje: estado, coordenadas, sincronizacion.
- **Runtime** (`main/runtime/`) — Inicializacion y ciclo de vida del sistema.
- **Motors** (`main/motors/`) — Control de motores de alto nivel y ejecucion hardware: GPIO DIR, RMT para STEP.
- **TMC** (`main/tmc/`) — Configuracion y verificacion de los drivers TMC2209 por UART single-wire (microstepping, corriente, chop mode). Referencia tecnica completa (datasheet rev 1.09): `main/tmc/README.md`.
- **LED** (`main/led/`) — Control PWM del LED externo en GPIO 6. Estados: tenue (normal), brillante (slewing), respiracion (error).
- **Buzzer** (`main/buzzer/`) — Buzzer pasivo de eventos en GPIO 7 (2 kHz via LEDC). Beeps de arranque, inicio y fin de goto/move axis.
- **Accelerometer** (`main/accelerometer/`) — Acelerometro ADXL345 por I2C (GPIO5 SDA / GPIO4 SCL). Mide `tilt` y `heading` para alineacion polar y limites de RA/DEC. Ver `main/accelerometer/README.md`.
- **USB Net** (`main/usb_net/`) — Interfaz de red USB Ethernet via TinyUSB en modo NCM.
- **Tools** (`main/tools/`) — Utilidades transversales (parser, validacion).

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

- USB Net depende de TinyUSB (componente gestionado `espressif/esp_tinyusb`) y esp_netif
- REST API y Alpaca se enlazan a INADDR_ANY, accesibles por USB Net
- Motors es autocontenido: controla GPIOs DIR y RMT para STEP; el modulo TMC (UART) configura y verifica los drivers TMC2209
