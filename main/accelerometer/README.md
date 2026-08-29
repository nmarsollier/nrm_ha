# Acelerómetro (ADXL345)

Módulo que lee hasta dos acelerómetros ADXL345 por I2C, para medir la
inclinación y la rotación de los ejes de la montura. Destinado a
**alineación polar** y **límites de RA y DEC**.

**Estado**: implementado y verificado en banco (dos sensores leyendo).
Durante el desarrollo se puede flashear **sin sensores conectados**: si no
se encuentran, se ignoran y la montura funciona igual (sin errores).

## Hardware

| Señal | Pin     | Notas                    |
|-------|---------|--------------------------|
| SDA   | GPIO 2  | ADXL345                  |
| SCL   | GPIO 1  | ADXL345                  |

- Bus `I2C_NUM_0`, 100 kHz.
- Pull-ups internos habilitados. Para 100 kHz conviene añadir 4.7–10 kΩ
  externos (los internos ~45 kΩ son débiles).
- Dos direcciones según el pin SDO, ambos sensores en el mismo bus:

| SDO    | Dirección I2C |
|--------|---------------|
| → GND  | `0x53`        |
| → 3V3  | `0x1D`        |

## Qué mide (por sensor)

| Valor     | Significado |
|-----------|-------------|
| `x,y,z`   | aceleración en **g** (rango ±2 g, 3.9 mg/LSB). Un eje lee +1 g cuando apunta hacia abajo. |
| `tilt`    | ángulo del eje Z respecto a la **vertical**, 0–90° (`0°` = nivelado, `90°` = vertical). |
| `heading` | dirección de inclinación en el plano X-Y, 0–360°. Si el eje Z es paralelo al eje de rotación, `heading` = **ángulo de rotación** de ese eje. |

Fórmulas: `tilt = acos(|z| / |g|)` y `heading = atan2(y, x)` (normalizado a 0–360°).

## Regla de montaje

Lo más simple (sin calibración) es montar cada sensor con su **eje Z paralelo
al eje que se quiere medir**:

- **Sensor de RA** (fijo dentro de la caja DEC) → eje Z ∥ eje RA (apuntando al polo).
- **Sensor de DEC** (en el rotor del eje DEC) → eje Z ∥ eje DEC.

Pero **no es obligatorio**: cualquier posición fija funciona. La única
condición es que el sensor vaya **rígido** a la parte que rota (que no se
mueva respecto a ella). Con una calibración única — rotar el eje una cantidad
conocida para aprender la orientación del sensor — se calcula el ángulo igual,
sin importar cómo quede montado.

**Restricción real** (independiente de la orientación del sensor): el eje de
rotación no puede quedar **paralelo a la vertical** (gravedad), porque el
acelerómetro no detecta giro alrededor de esa dirección. A latitud ~33° el eje
DEC queda siempre lejos de la vertical (como mínimo ~33°), así que no hay
degeneración.

## Funciones previstas (para más adelante)

1. **Alineación polar (RA)**
   - El `tilt` del sensor de RA mide la inclinación del eje polar respecto a la vertical.
   - `latitud = 90° − tilt`. Ejemplo: latitud 33° → `tilt = 57°`.

2. **Límites de RA**
   - El `heading` del sensor de RA = ángulo de rotación del eje RA. El eje
     polar es fijo en el espacio, así que el `heading` es limpio.
   - Falta calibrar el **offset**: llevar el eje al home, anotar el `heading`
     y usarlo como cero.

3. **Límites de DEC**
   - El `heading` del sensor de DEC depende de **DEC y RA juntos** (el eje DEC
     gira alrededor del eje RA cuando RA rota).
   - Para aislar el ángulo DEC hay que corregir con el ángulo RA, que la
     montura ya conoce (lo está trackeando).

## Pendiente (cuando se arme la montura)

- Identificar qué dirección física (`0x53` / `0x1D`) corresponde al sensor de
  RA y cuál al de DEC (mover un eje a mano y ver qué dirección cambia).
- Calibración de offset del `heading` (home → cero).
- Exponer `tilt` / `heading` por REST para que la lógica de límites de la
  montura los consuma.
- (Opcional) Promediar N muestras y calibrar el offset del acelerómetro para
  mayor precisión.

## Detalles técnicos

- Verificación de identidad: `DEVID (0x00) == 0xE5` antes de dar el sensor por válido.
- Configuración: `DATA_FORMAT (0x31) = 0x00` (±2 g, 10-bit) y
  `POWER_CTL (0x2D) = 0x08` (modo medida).
- Lectura: escribir `0x32` (DATAX0) y leer 6 bytes con auto-incremento
  (el bit MB 0x40 es solo de SPI, no se usa en I2C).
- Cadencia de lectura: **500 ms** (`ACCEL_READ_PERIOD_US`).
- El log de cada lectura sale con tag `ACCELEROMETER_UPDATE` a nivel INFO:

  ```
  addr 0x1D: x=-0.70g y=-0.16g z=0.63g | tilt=49.0 deg heading=193.0 deg
  ```

## Archivos

```
main/accelerometer/
  accelerometer.h            API pública (init / update)
  accelerometer_internal.h   pines, direcciones, registros, estado
  accelerometer_init.c       bus I2C + probe + configuración
  accelerometer_read.c       lectura I2C + conversión a g / tilt / heading
  accelerometer_update.c     refresco 500 ms + log
```
