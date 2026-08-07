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

- Motores Nema 17 Closed Loop con driver integrado
- https://www.amazon.com/dp/B0FHHWT8Q8
- Configurados a 64 microsteps via DIP switches
- Torque: 0.44 Nm
- Limitado de torque por hardware (SW6 en ON)
- Los motores emiten señal ALARM (activo bajo) cuando detectan error de posicion

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
| 14   | EN- ambos motores | Enable global RA y DEC (GPIO LOW = enabled, via BC337)|
| 13   | STEP- RA          | Pulso STEP eje ascension recta (via BC337)         |
| 12   | DIR- RA           | Direccion eje ascension recta (via BC337)           |
| 11   | STEP- DEC         | Pulso STEP eje declinacion (via BC337)              |
| 10   | DIR- DEC          | Direccion eje declinacion (via BC337)               |
| 9    | ALARM- RA         | Input con pull-up, activo bajo = falla             |
| 46   | ALARM- DEC        | Input con pull-up, activo bajo = falla             |
| 4    | LED externo       | LEDC PWM, indicador de estado                      |

**Level shifting (BC337 NPN):**
Los drivers integrados usan optoacopladores en STEP/DIR/EN que requieren
5 V / ~10 mA. La ESP32-S3 tiene logica de 3.3 V y no tolera 5 V. Cada senal
de salida usa un transistor BC337 como switch:

```
  GPIO ──[1kΩ]── Base (centro)
                  │
  5V ──→ COM+ driver ──→ [opto] ──→ COM- driver ──→ Colector (izquierda)
                                                     │
                                                   Emisor (derecha) ──→ GND
```

Visto de frente (letras legibles), patas hacia abajo: izquierda=Colector,
centro=Base, derecha=Emisor. 5 transistores + 5 resistencias 1kΩ en total.

**IMPORTANTE — Logica de EN-:**
El driver invierte la logica: EN+ = 5V + EN- = LOW (GND) → motor LIBRE
(deshabilitado). Por lo tanto con el BC337:
  GPIO LOW  → transistor OFF → EN- flotando → motor HABILITADO
  GPIO HIGH → transistor ON  → EN- = GND    → motor DESHABILITADO

**Alimentacion:**
| Pin  | Funcion           |
|------|-------------------|
| 5V   | Terminales + de datos de ambos motores (COM+) |
| GND  | Tierra comun                                   |

**Uso futuro:**
| GPIO | Funcion     | Notas                              |
|------|-------------|------------------------------------|
| 5    | I2C SDA     | Sensor / display                   |
| 6    | I2C SCL     | Sensor / display                   |
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
Cliente Web (Alpine.js) → REST API → Mount (orquestacion) → Motors → STEP/DIR/EN/ALARM (hardware)
```

- **www/** — UI Web embebida programada con Alpine.js. Compila con `node www/build.js`, genera `www/dist/`.
- **REST API** (`main/rest/`) — Expone endpoints HTTP para control de la montura.
- **Mount** (`main/mount/`) — Orquestacion logica del montaje: estado, coordenadas, sincronizacion.
- **Runtime** (`main/runtime/`) — Inicializacion y ciclo de vida del sistema.
- **Motors** (`main/motors/`) — Control de motores de alto nivel y ejecucion hardware: GPIO DIR/EN/ALARM, RMT para STEP.
- **LED** (`main/led/`) — Control PWM del LED externo en GPIO 4. Estados: tenue (normal), brillante (slewing), respiracion (error).
- **Network** (`main/network/`) — Conectividad WiFi.
- **USB Net** (`main/usb_net/`) — Interfaz de red USB Ethernet via TinyUSB en modo ECM/RNDIS.
- **Tools** (`main/tools/`) — Utilidades transversales (parser, validacion).

### Comportamiento de ALARM

- Los pines ALARM- (GPIO 11 RA, GPIO 46 DEC) son monitoreados durante el movimiento
- Si cualquiera de los dos se pone LOW, la montura entra en estado ERROR irrecuperable
- En estado ERROR se deshabilitan los motores inmediatamente
- No se acepta ningun comando de movimiento en estado ERROR
- Solo un reboot puede sacar la montura del estado ERROR

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
- Motors es autocontenido: controla GPIOs, RMT, y monitorea ALARM
