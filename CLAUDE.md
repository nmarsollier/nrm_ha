# Reglas de Desarrollo del Proyecto NRM-HA

Mantener este archivo en formato simple, para que pueda leerse y editarse rapidamente

## Definicion del proyecto

- Logica para manejar montura ecuatorial con harmonic drives para astrofotografia
- Montura NRM-HA con harmonic drives de reduccion 100:1 y poleas HTD3M 3:1 (15T→45T)
- Reduccion total: 300:1 en ambos ejes
- Utiliza una placa ESP32-S3 44 pines
- Utiliza 2 motores Nema 17 Closed Loop con driver integrado, configurados a 64 microsteps
- Los motores se alimentan directo de fuente 12V, la placa ESP32-S3 via Mini DC 360 (12V→5.5V)
- Estructura metalica en hierro 1/8, dos cuerpos (RA y DEC)
- La montura posee 2 botones fisicos, Stop y Home
- Posee una pantalla minimalista OLED de 0.98 inch, se muestra verticalmente

### Harmonic Drives

- Harmonic Drive de reduccion 100:1 (https://www.ebay.com/itm/286960016334)
- Reduccion de poleas 3:1 en la entrada del harmonico: HTD3M 15T → HTD3M 45T, correa 171mm
- Reduccion Total en ambos Ejes: 300:1
- El cuerpo DEC se enrosca a la estructura RA a traves de la salida del Harmonic
- Los cables que controlan el eje DEC se pasan por dentro del Harmonic

### Motores Nema 17 Closed Loop

- Motores Nema 17 Closed Loop con driver integrado ISS42 (especificaciones en MOTOR.txt)
- https://www.amazon.com/dp/B0FHHWT8Q8
- Configurados a 64 microsteps via DIP switches
- Torque: 0.44 Nm
- Limitado de torque por hardware (SW6 en ON)

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
- **WiFi**: 802.11 b/g/n (2.4 GHz)
- **Bluetooth**: v5.0 BLE + Bluetooth Mesh
- **Alimentacion**: 5.5V via Mini DC 360 (convierte 12V a 5.5V)
- **Logica I/O**: 3.3V (no tolera 5V)
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
| 14   | STEP- RA          | Pulso STEP eje ascension recta (via UMC2003)         |
| 13   | DIR- RA           | Direccion eje ascension recta (via UMC2003)           |
| 12   | STEP- DEC         | Pulso STEP eje declinacion (via UMC2003)              |
| 11   | DIR- DEC          | Direccion eje declinacion (via UMC2003)               |
| 4    | LED externo       | LEDC PWM, indicador de estado (via UMC2003)                      |
| 5    | BUZZER            | Buzzer pasivo 2 kHz (via UMC2003)                  |
| 18   | I2C SDA           | Acelerometro ADXL345 (I2C, pull-ups internos)       |
| 8    | I2C SCL           | Acelerometro ADXL345 (I2C, pull-ups internos)       |

**Level shifting (UMC2003, array Darlington):**
Los drivers integrados usan optoacopladores en STEP/DIR que requieren
5 V / ~10 mA. La ESP32-S3 tiene logica de 3.3 V y no tolera 5 V. Todas las
salidas (STEP, DIR de ambos ejes, el LED y el buzzer) pasan por un UMC2003, un array
Darlington de 7 canales con salidas de colector abierto (sinking).

Cada GPIO va directo a una entrada del UMC2003 (resistencia de base interna,
no requiere resistor externo). La salida hunde corriente hacia GND:

```
GPIO (3.3V) → INx UMC2003        OUTx → carga → +V

  GPIO HIGH → OUTx a GND (carga activa)
  GPIO LOW  → OUTx en alta impedancia
```

Las salidas son "negativas" (active-low): la carga (opto del driver o el LED
con su resistencia) se conecta entre +V y la salida. El pin COM (anodo comun
de los diodos de proteccion) se ata al +V. No hay cargas inductivas en esta
revision (los drivers se controlan por optoacopladores, que son LED), asi
que los diodos no son estrictamente necesarios.

EN+ / EN- van **sin conectar** (habilitado por defecto). El ISS42 invierte la
logica: EN+ a +V y EN- a GND lo *deshabilita*.
Los pines ALARM no se conectan en esta revision.

**Alimentacion:**
| Pin  | Funcion           |
|------|-------------------|
| 12V  | Alimentacion de potencia (DC+) y entradas de control (PU+/DR+). El ISS42 acepta 5-24 V en control |
| GND  | Tierra comun (placa, UMC2003 y drivers comparten la misma tierra) |

**Uso futuro:**
| GPIO | Funcion     | Notas                              |
|------|-------------|------------------------------------|
| 6    | I2C SCL (2do bus) | Display / sensor adicional        |
| 1    | Hall limit  | Sensor de fin de carrera           |
| 2    | Hall limit  | Sensor de fin de carrera           |

### Pines con restricciones (NO USAR)

| GPIO | Restriccion                                 |
|------|---------------------------------------------|
| 0    | Boot: LOW en reset = modo flash (ROM boot)  |
| 3    | Strapping JTAG (LOW al boot)                |
| 43   | UART0 TX, consola debug (USB-Serial nativo) |
| 44   | UART0 RX, consola debug (USB-Serial nativo) |
| 45   | Strapping (VDD_SPI voltage)                 |

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
- **LED** (`main/led/`) — Control PWM del LED externo en GPIO 4. Estados: tenue (normal), brillante (slewing), respiracion (error).
- **Buzzer** (`main/buzzer/`) — Buzzer pasivo de eventos en GPIO 5 (2 kHz via LEDC). Beeps de arranque, inicio y fin de goto/move axis.
- **Accelerometer** (`main/accelerometer/`) — Acelerometro ADXL345 por I2C (GPIO18 SDA / GPIO8 SCL). Mide `tilt` y `heading` para alineacion polar y limites de RA/DEC. Ver `main/accelerometer/README.md`.
- **Network** (`main/network/`) — Conectividad WiFi.
- **USB Net** (`main/usb_net/`) — Interfaz de red USB Ethernet via TinyUSB en modo ECM/RNDIS.
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

- Network es autocontenido y expone WiFi STA + AP fallback
- USB Net depende de TinyUSB (componente gestionado `espressif/esp_tinyusb`) y esp_netif
- REST API y Alpaca se enlazan a INADDR_ANY, accesibles tanto por WiFi como por USB Net
- Motors es autocontenido: controla GPIOs DIR y RMT para STEP
