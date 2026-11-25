# PR0 — Inventario de diferencias y línea base de la unificación

Versión 1.0 · 25 de noviembre de 2026 · Estado: inventario completo a nivel de código fuente (`.c`/`.h`).

Documentos de referencia:
- [PRD del firmware unificado](../../../Downloads/unificacion/PRD_Firmware_Unificado_NRM_HA_BFT.md)
- [Arquitectura detallada](../../../Downloads/unificacion/Arquitectura_Firmware_Unificado_NRM_HA_BFT.md)

Fuentes inspeccionadas:
- NRM-HA: `~/Dev/nrm_ha` (rama `unificacion`, commit `dd2b71e`), raíz `main/`.
- BFT: `~/Dev/bft_firmware` (rama `main`, commit `f74563c`), raíz `main/`.

## 1. Línea base de pruebas (host, sin hardware)

Ejecutadas sobre los árboles reales:

| Suite | Comando | Resultado |
|---|---|---|
| NRM host | `make -C test/host test` | **64 aprobadas** (`Ran 64 tests ... OK`) |
| BFT host | `python3 -m unittest discover -s test/host -v` | **62 aprobadas** (`Ran 62 tests ... OK`) |

Coinciden con las cifras del PRD (64 NRM / 62 BFT). No se ejecutó compilación ESP-IDF ni pruebas físicas.

## 2. Manifiesto de fuentes

Clasificación de los 106 caminos fuente únicos (`main/**/*.c` y `*.h`):

| Categoría | Cantidad | Significado |
|---|---|---|
| Idénticos byte a byte | 55 | Núcleo ya compartido (TMC, buzzer, LED update, USB, utils, gran parte de mount/motors/proto) |
| Presentes en ambos, contenido distinto | 42 | Áreas a reconciliar |
| Solo NRM | 3 | `motors_slew_duration.c`, `mount_destination_side_of_pier.c`, `proto_query.c` |
| Solo BFT | 6 | `motors_horizon.c`, `motors_is_quiescent.c`, `motors_motion_idle.c`, `motors_motion_stop.c`, `motors_motion_stop_graceful.c`, `motors_ramp.c` |

Nota metodológica: la arquitectura cita «100 rutas comunes, 56 idénticas byte por byte» y «92 archivos compartidos no idénticos»; esas cifras incluyen archivos no fuente (`.md`, `CMakeLists.txt`, `idf_component.yml`, tests). Este inventario acota a `.c`/`.h`, por lo que da **55 idénticos** y **97 comunes** (55 idénticos + 42 distintos). La diferencia no es funcional.

### 2.1 Idénticos (55)

`buzzer/*` (5), `led/led_update.c`, `main.c`, `motors/` (home, init, is_pulse_guiding, is_slewing, microsteps, power, start_tracking, tools, unpark, get_move_axis_speed, get_tracking_speed), `mount/` (init, move_axis, park, set_tracking, settings_load, settings_save, stop, unpark), `proto/` (proto.h, control, frame, init, json, session), `runtime/*`, `tmc/*` salvo `tmc_uart.c` (11), `usb_cdc/*` (5), `utils/*` (3).

### 2.2 Solo NRM (3)

- `motors/motors_slew_duration.c` — rampa embebida NRM (ventana 5°/5°).
- `mount/mount_destination_side_of_pier.c` — consulta de lado de destino sin efectos laterales.
- `proto/proto_query.c` — handler `query`.

### 2.3 Solo BFT (6)

- `motors/motors_horizon.c` — fórmula geométrica de horizonte + `motors_path_above_horizon`.
- `motors/motors_is_quiescent.c`, `motors_motion_idle.c`, `motors_motion_stop.c`, `motors_motion_stop_graceful.c` — drenaje y espera de reposo separadas del stop.
- `motors/motors_ramp.c` — rampa extraída (ventana 10°/10°).

## 3. Matriz de diferencias funcionales → destino

### 3.1 Identidad y electrónica (RF01/RF02)

| Diferencia | BFT | NRM | Destino |
|---|---|---|---|
| `MOUNT_NAME` / `MOUNT_DEVICE_ID` | `BFT` / `bft` | `NRM-HA` / `nrm-ha` | Perfil |
| Reducción total | 500:1 (`TOTAL_GEAR_REDUCTION 500.0f`) | 300:1 (`300.0f`) | Perfil mecánico por eje |
| Buzzer GPIO | `GPIO_NUM_4` | `GPIO_NUM_6` | BSP común — pendiente de verificación física |
| Flash declarada | 16 MB | 8 MB | BSP común — pendiente de verificación física |
| STEP/DIR RA/DEC | 10/9, 15/7 | 10/9, 15/7 | Igual — BSP común |
| UART TMC RA/DEC | 1 (12/13), 2 (17/18) | 1 (12/13), 2 (17/18) | Igual — BSP común |
| LED GPIO | 5 | 5 | Igual |
| `RMT_MEM_BLOCK_DMA` | 1536 | 1024 | BSP común — preservar NRM (no elegir por ser mayor) |

### 3.2 Mecánica y signo (RF03)

| Diferencia | BFT | NRM | Destino |
|---|---|---|---|
| DIR positivo | LOW (`0`) | HIGH (`1`) | `dir_positive_level` por eje (datos de perfil hasta medir giro real) |
| Microsteps | 64 fino / 8 slew | 64 fino / 8 slew | Igual — política TMC común |
| Paso motor | 1.8° | 1.8° | Igual |

### 3.3 Geometría (RF03 / PR2-PR3)

| Diferencia | BFT (horquilla) | NRM (alemana) | Destino |
|---|---|---|---|
| Modelo | Vectorial, puro `(axis, lat, lon, t)` | Trigonométrico, lee estado global, offset 90° | Estrategia geométrica (`geometry_fork` vs `geometry_german`) |
| Hemisferio | Norte y sur (signo por `lat`) | Formulada para el sur | Estrategia; no generalizar NRM sin pruebas |
| Offset RA | 0° | 90° (`HOME_RA_OFFSET_DEG`) | Perfil geométrico (preservar 90°) |
| Selección de solución | Mínimo recorrido `|dR|+|dD|` + compensación de llegada por candidato | Pier-side (AUTO asimétrico / EAST / WEST forzado) | Selección por estrategia |
| Tratamiento de polos | Explícito (mantiene R, mínimo viaje DEC) | Degenerado (RA singular en dec_axis≈0) | Preservar por perfil |
| Cuantización | `quantize_within` (hacia el interior, clamp a paso representable) | No cuantiza destino | Preservar por perfil |
| Firmas | `equatorial_to_axis(eq, cur, lat, lon, t, out)`, `axis_to_equatorial(axis, lat, lon, t)` | `equatorial_to_axis(eq, cur, t, required, out)`, `axis_to_equatorial(axis)` | Contrato común con entradas explícitas (contexto inmutable) |

### 3.4 GoTo (RF03 / RF04)

| Diferencia | BFT | NRM | Destino |
|---|---|---|---|
| Selección de solución | `mount_select_axis_solution` (mín. recorrido + compensación) | `equatorial_to_axis` con `required` + bloqueo de lado en el instante de petición | Contrato `geometry_plan_goto` por estrategia |
| Validación de horizonte | Sí, al aceptar (`motors_path_above_horizon`) | No | Geometría BFT + revalidación en ejecución |
| Compensación de llegada | Por candidato (sidereal × duración rampa) | Re-solvía en el instante de llegada con lado bloqueado | Preservar comportamiento por perfil |

### 3.5 Parada y movimiento (RF04 / PR4-PR5)

| Diferencia | BFT | NRM | Destino |
|---|---|---|---|
| STOP | `motors_motion_stop_graceful` (bump generación, `active=false`, `graceful=true`, notifica) — drenaje de lotes comprometidos | `motors_motion_stop` (aborto) + `motors_motion_wait_idle` (barrera) | Unificar: drenaje normal + barrera de finalización + aborto urgente |
| `motors_stop()` | Devuelve `MOTOR_OK` sin barrera | Espera idle; devuelve `MOTOR_ERR_NOT_READY` si no reposa | Unificar solicitud/finalización/publicación |
| Reposo/quiescencia | `motors_is_quiescent` + `motors_motion_idle` | (ausente como archivo; barrera en `motors_stop`) | `is_quiescent` común |
| Rampa | `motors_ramp.c` (extraída, 10°/10°) | `motors_slew_duration.c` (embebida, 5°/5°) + estimador logarítmico | Común parametrizado + límite analítico `1/vmin` |
| `motors_home()` | — | No propaga el fallo de `motors_stop()` | No encolar HOME sin parada confirmada (brecha §6) |

### 3.6 Límites y persistencia (RF05 / RF07)

| Diferencia | BFT | NRM | Destino |
|---|---|---|---|
| Envolvente física | `PHYSICAL_RA ±100`, `PHYSICAL_DEC −135/+50` (hard) | `LIMITS_MECHANICAL ±180` (envolvente software) | Envolvente de perfil + límites operativos |
| Defaults | RA ±100, DEC −135/+50 | RA ±100, DEC ±150 | Perfil |
| Validación | `limits_clamp_to_physical` (nunca ampliar) | `limits_are_valid` (finito, ordenado, dentro ±180) + `apply_limits_if_valid` | Separar envolvente/operativos/persistencia; validar finitud/orden/pertenencia |
| Reset de límites | `bool`, espera idle, devuelve falso en timeout | `void`, confía en `motors_stop()` | Reset solo tras reposo confirmado (brecha §6) |
| NVS | `NVS_NS` keys `ra_min/ra_max/dec_min/dec_max` | `NVS_NS` keys idénticas | Namespaces por perfil (`cfg_nrm_ha`/`cfg_bft`); migración explícita |

**Hallazgo clave (RF07):** ambos perfiles persisten los límites con las **mismas claves** (`ra_min`, `ra_max`, `dec_min`, `dec_max` en el namespace de límites) pero con significados físicos distintos (NRM DEC ±150, BFT DEC −135/+50). Un namespace legado compartido no identifica el origen — confirma la necesidad de namespaces por perfil e importación explícita.

### 3.7 Protocolo (RF06 / PR8)

| Diferencia | BFT | NRM | Destino |
|---|---|---|---|
| `meridian_flip` (capacidades) | `false` | `true` | Capacidad por perfil |
| `pier_side` (estado) | `"unknown"` | `"east"`/`"west"` | Capacidad por perfil |
| `guide_truncated` (estado) | ausente | presente | Campo común aditivo (RF06) |
| Handler `query` | ausente | presente (`proto_query.c`, `destination_side_of_pier`) | Contrato común; BFT rechaza lo no soportado |
| Conversión en estado | Pura (`mount_get_settings` + `gettimeofday` coherentes) | Global (`axis_to_equatorial` + `mount_get_lst`) | Captura coherente de posición/sitio/hora |
| Guía | — | `MOUNT_MAX_GUIDE_RATE_DPS` y validación de tasa | `MOUNT_GUIDE_RATE_MAX_DPS` común + `mount_guide_rate_is_valid` |

### 3.8 TMC (RF02 / §9)

| Diferencia | BFT | NRM | Destino |
|---|---|---|---|
| Lectura UART | Espera el deadline completo (buffer 32 B) | `break` al acumular 8 bytes | Parser común que valide framing/registro/CRC; 8 bytes no garantizan trama válida |
| Resto del driver | Idéntico | Idéntico | Compartido |

### 3.9 Latitud fija durante movimiento (RF05 / §8)

BFT posee `mount_settings_validate` y rechaza cambiar la latitud con movimiento activo (`motors_is_quiescent`). NRM no tiene esa compuerta. Destino: política común de contexto inmutable mientras haya trabajo pendiente.

## 4. Discrepancias documentación ↔ código (verificadas)

| Documento | Afirmación | Hallazgo en código |
|---|---|---|
| Arquitectura §2 | Validación de horizonte BFT «no revalidada en `motors_motion_task.c`» | Confirmado: `motors_path_above_horizon` se llama solo en `mount_slew_to_coordinates.c` (aceptación), no en la tarea de movimiento |
| Arquitectura §2 | `MOUNT.md` rechaza HOME bajo horizonte; comentario contrario en `mount_home.c` | Pendiente de confirmación fina en el detalle del comentario; adoptar política segura §8 |
| Arquitectura §2 | STOP NRM devuelve error sin reposo, pero `motors_home()` y reset de límites no propagan | Confirmado: `motors_limits.c` NRM llama `motors_stop()` y **sí** propaga; la brecha real es que el reset es `void` y confía en el resultado; `motors_home()` (idéntico en ambos) no propaga `motors_stop()` |
| Arquitectura §2 | Estimador logarítmico `log(v/vmin)/(v-vmin)` singular en `v==vmin` | Confirmado en `motors_slew_duration.c` NRM; falta límite analítico `1/vmin` |
| PRD §3 | «100 rutas comunes, 56 idénticas byte por byte» | 97 comunes `.c/.h`, 55 idénticos (ver §2, nota metodológica) |

## 5. Brechas que forman parte de la migración (PRD §6)

| # | Brecha | Estado en código |
|---|---|---|
| 1 | Horizonte BFT: validación solo al aceptar, no al ejecutar/comprometer | Confirmado (§4) |
| 2 | Recuperación BFT: `MOUNT.md` vs comentario en `mount_home.c` | Adoptar recuperación manual controlada (§8 arquitectura) |
| 3 | STOP/park: drenaje BFT vs espera NRM; escrituras tempranas READY/PARKED | Confirmado (motors_stop.c difieren; estado se publica antes de reposo) |
| 4 | HOME NRM: `motors_home()` no propaga resultado de `motors_stop()` | Confirmado (archivo idéntico en ambos) |
| 5 | Reset de límites NRM puede continuar tras STOP fallido | Confirmado (reset `void`, sin espera de idle) |
| 6 | Rampas: caso singular `v==vmin` | Confirmado (falta `1/vmin`) |
| 7 | Concurrencia: `volatile` no establece coherencia | Confirmado (locks por `portENTER_CRITICAL`, sin modelo de propiedad formalizado) |

## 6. Plan de PRs (referencia, sin cambios de firmware en PR0)

Se adopta el plan de la arquitectura §12 (PR1..PR9). El siguiente paso concreto es **PR1**: BSP común (`main/board/`), selección obligatoria `MOUNT_PROFILE`, datos mecánicos (`main/profiles/`) y adaptador transitorio de macros en `config/config.h`, preservando el comportamiento NRM.

## 7. Resultado

- Inventario completo de diferencias funcionales a nivel de código fuente.
- Línea base reproducible: 64 (NRM) / 62 (BFT).
- Cada diferencia funcional tiene destino o decisión registrada (§3).
- Sin cambios de firmware en esta entrega (solo documentación).
