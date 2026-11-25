# Segunda pasada de corroboración — unificación NRM-HA / BFT

Versión 1.0 · 25 de noviembre de 2026 · Verificación de los documentos contra el código real de ambos repos, y de la exigencia de «refactor sin cambios de comportamiento».

Complementa [PR0_inventario.md](PR0_inventario.md).

## 0. Veredicto en una línea

Los dos documentos son **consistentes entre sí** y **describen fielmente ambos firmwares**; el diseño **no elimina ninguna capacidad de ninguna montura**. Pero hay **un punto que contradice tu instrucción de «refactor puro, sin cambios de comportamiento»**: el PRD/arquitectura incluyen **7 correcciones deliberadas que SÍ cambian comportamiento**. Hay que decidir si entran o se separan.

## 1. Consistencia PRD ↔ Arquitectura

**Consistentes.** Verifiqué punto por punto:

- **Tabla de diferencias (PRD §4) ≡ perfiles (Arquitectura §4.2)**: reducción 300/500, DIR LOW/HIGH, límites RA ±100/±100, DEC −135/+50 vs −150/+150, GoTo 4/6, manual 1/1, rampa 10°/10° vs 5°/5°, offset 0/90. Coinciden con el código (verificado abajo).
- **Brechas (PRD §6) ≡ hallazgos (Arquitectura §2)**: las mismas 7 brechas, mismas referencias de archivo. Coinciden.
- **Entregas (PRD §7: E0–E6) vs PRs (Arquitectura §12: PR0–PR9)**: descomposiciones distintas pero **compatibles** (E0=PR0, E1=PR1, E2=PR2+3, E3=PR7, E4=PR4+5, E5=PR8, E6=PR9; PR6 «política de trayectoria BFT» está implícita en RF05/brechas). La arquitectura §12 autoriza explícitamente reajustar los límites entre PRs.
- **Cifras de inventario (PRD §3 «100 rutas, 56 idénticas»)**: incluyen no-fuente. Acotado a `.c`/`.h` son **97 comunes / 55 idénticos**. Diferencia numérica, no funcional (ya notada en PR0).

**Única tensión real**: ambos documentos describen las brechas como **«correcciones de seguridad documentadas»** (PRD §6) y la arquitectura advierte «no se debe describir como paridad exacta con toda la ejecución antigua» (§8.2). Esto choca con «solo refactor, cero cambio de comportamiento». Detalle en §4.

## 2. Los firmwares son como se describen

Confirmado leyendo el código real de ambos repos. Muestras verificadas (con evidencia):

| Afirmación del documento | Verificado |
|---|---|
| Geometría NRM = alemana sur, offset 90°, pier-side AUTO asimétrico/EAST/WEST | `mount_coordinate_tools.c` NRM: `HOME_RA_OFFSET_DEG 90`, `PierSideRequirement`, política asimétrica por signo de `dec_axis` |
| Geometría BFT = horquilla, ambos hemisferios, vectorial, mínimo recorrido + compensación | `mount_coordinate_tools.c` BFT: `pole_sign(lat)`, `mount_select_axis_solution` con `quantize_within` y avance sideral por candidato |
| Horizonte BFT validado **solo al aceptar**, no en la tarea de movimiento | `motors_path_above_horizon` solo en `mount_slew_to_coordinates.c`; `mount_home.c` no lo gatea |
| Reducción 300 vs 500 | `config.h`: `TOTAL_GEAR_REDUCTION 300.0f` / `500.0f` |
| DIR positivo HIGH (NRM) / LOW (BFT) | `motors_hw.c`: `? 1 : 0` vs `? 0 : 1` |
| Ventana de rampa 5° (NRM) / 10° (BFT) | `motors_internal.h` NRM `ACCEL_DIST_CDS 500`; BFT `motors_ramp.c` `ACCEL_DIST_CDS 1000` |
| GoTo máx 6 vs 4°/s (rates 1/3/4.5 vs 0.5/1/2.5) | `motors_get_slewing_speed.c` |
| Envolvente DEC ±150 software (NRM) vs física −135/+50 (BFT) | `motors_limits.c` |
| STOP: NRM aborta + barrera; BFT drena sin barrera | `motors_stop.c` + `motors_motion_stop*.c` |
| NVS: mismas claves `ra_min/…/dec_max` en ambos → no identifica origen | `motors_limits.c` ambos |
| `meridian_flip` true/false; `pier_side` east/west vs unknown; `guide_truncated` solo NRM | `proto_capabilities.c`, `proto_state.c` |
| `side` (east/west) en goto solo NRM; `query` solo NRM | `proto_action.c`, `proto_query.c` |
| UART TMC: NRM `break` a 8 bytes; BFT espera deadline | `tmc_uart.c` |
| Caso singular de rampa `log(v/vmin)/(v-vmin)` en `v==vmin` | `motors_slew_duration.c` NRM **y** `motors_ramp.c` BFT (ambos) |
| BFT valida guía contra polo y límite DEC; NRM no | `mount_pulse_guide.c` |
| BFT `mount_settings_validate` + latitud fija con movimiento; NRM no | `mount_settings_update.c` |

## 3. Sin pérdida de funcionalidad (inventario de capacidades)

Ningún documento propone **eliminar** una capacidad. Cada diferencia se resuelve por perfil/estrategia, no por supresión:

| Capacidad | NRM | BFT | Destino en el diseño |
|---|---|---|---|
| Geometría propia | alemana + offset 90° | horquilla vectorial | `geometry_german` / `geometry_fork` (no se fuerza algoritmo común — §5.3) |
| Selección de solución | pier-side (AUTO/EAST/WEST) | mínimo recorrido | «No forzar un algoritmo de elección común» (arquitectura §5.3) |
| Flip de meridiano | sí (`meridian_flip:true`, campo `side`) | no (cruce continuo sin flip) | capacidad por perfil; BFT rechaza flip explícito |
| Consulta lado destino | `destination_side_of_pier` / `query` | — | contrato común; BFT responde no aplicable |
| Horizonte geométrico | — | `motors_path_above_horizon` | geometría BFT + revalidación |
| Tratamiento de polos | degenerado | explícito + cuantización | por perfil |
| Guiado con validación de polo/límite | — | sí | se conserva al portar BFT |
| STOP drenaje (BFT) / barrera (NRM) | barrera | drenaje | unificados (ambas propiedades) |
| `guide_truncated` | sí | — | campo común aditivo (no quita nada) |
| `motors_reset_hardware_fault` (hook de test) | sí | — | conservar para el harness |

**Conclusión §3**: no se pierde funcionalidad. El riesgo no es de pérdida sino de **cambios involuntarios** durante el refactor (§4).

## 4. Punto crítico: «refactor puro» vs «correcciones deliberadas»

Tu instrucción: *«refactor de código pero no funcional… no hay cambios de comportamiento»*. El PRD/arquitectura **sí** incluyen cambios de comportamiento, presentados como correcciones de seguridad. Son:

| # | Corrección | Montura | Cambio de comportamiento |
|---|---|---|---|
| C1 | Revalidar horizonte al ejecutar/comprometer | BFT | GoTos antes aceptados pueden rechazarse en ejecución |
| C2 | Recuperación manual con suelo congelado (HOME deja de ser la vía de recuperación) | BFT | HOME debajo del horizonte pasa a rechazarse (hoy no se gatea) |
| C3 | STOP/park: drenaje + barrera + no publicar READY/PARKED antes de reposo | ambas | Cambia el instante en que se reporta el estado |
| C4 | HOME no encola si STOP falló | ambas | HOME puede ahora fallar (hoy sigue de largo) |
| C5 | Reset de límites solo tras reposo confirmado | ambas | Reset puede ahora fallar |
| C6 | Rampa `v==vmin` → límite analítico `1/vmin` | ambas | Caso borde que hoy da NaN → duración finita |
| C7 | NVS por perfil + migración explícita | ambas | Primer arranque con datos legados inhibe movimiento hasta importar |

**Interpretación honesta de lo que pedís:** si «cero cambio de comportamiento» es literal, entonces C1–C7 **deben separarse** del refactor (diferirse o tratarse como PRs de *fix* explícitos y aprobados aparte). Si «sin cambios de comportamiento» significa «no pierdan funcionalidad ni cambien lo que cada montura puede hacer» (lectura compatible con tu «no se pierde funcionalidad»), entonces C1–C7 son correcciones de seguridad acotadas, ya documentadas, y el resto del trabajo es refactor puro.

**No puedo resolver esto solo**: es una decisión de producto. Ver §6.

## 5. Tests: ambas builds + unificación

### 5.1 Ambas builds corren sus tests

- NRM host: **64** (`make -C test/host test`, harness `libnrm_host.so` + `harness.py` ctypes).
- BFT host: **62** (`python3 -m unittest discover -s test/host -v`, C compilado por test).
- Proto: NRM y BFT tienen suites `pytest` separadas (CDC-ACM, requieren placa — no se ejecutan sin hardware).

Los harness host son **arquitectónicamente distintos** (lib compartida + ctypes vs compilación por test). Unificarlos es trabajo real (arquitectura §11.2: «compilar bibliotecas host distintas por perfil»). Ningún documento propone descartar tests.

### 5.2 Comparación de suites (para unificar)

**Proto — idénticos (6, unificables directo):** `oracle.py`, `profile.json`, `test_actions.py`, `test_got_guide.py`, `test_prk.py`, `test_trk_preserve.py`.

**Proto — específicos por montura (4, no unificables):** `test_got_flip.py` (NRM), `test_got_meridian.py` (BFT).

**Proto — difieren (33):** la mayoría difieren solo en valores dependientes del perfil (velocidades, límites, pier) — candidatos a **parametrizarse por `profile.json`** en lugar de duplicarse.

**Host — difieren estructuralmente:** NRM (harness.py + host_*.c + 6 tests propios) vs BFT (C por test + phd_sim.c + 3 tests propios). `test_fork.py` y `test_phd_calibration.py` existen en ambos pero **difieren**.

### 5.3 Qué significa «correr todos los tests»

- Objetivo alcanzable: el build NRM corre la suite NRM, el build BFT corre la suite BFT, y las piezas idénticas se comparten (una sola copia).
- No es alcanzable sin decidir: ejecutar la **misma** suite proto contra ambas monturas porque los valores esperados difieren por perfil (velocidades, límites, pier). La vía es perfil en `profile.json` (ya versionado e idéntico hoy: 156 bytes).

## 6. Decisión tomada (25-nov-2026)

Reducción del alcance de la primera unificación, según criterio del operador:

**Incluidas en esta fase** (cambios separados, cada uno con pruebas):
- **C4** — HOME no encola si STOP no confirmó parada. Verificar cómo se informa y recupera el fallo.
- **C5** — Reset de límites solo tras reposo confirmado; conservar valores previos si falla.
- **C6** — Caso singular `v == vmin` → límite analítico `1/vmin` (corrige estimador de duración y, de rebote, la compensación de llegada de GoTo).
- **C3** — READY/PARKED solo después del reposo, **junto con** la reconciliación de STOP (drenaje + barrera), verificando ambos perfiles y el contrato de respuesta (timeouts, sin bloqueos).

**Diferidas a una entrega posterior de «seguridad de trayectoria»:**
- **C1** — revalidar horizonte al ejecutar/comprometer.
- **C2** — recuperación manual con suelo congelado y rechazo de HOME bajo horizonte. **No cambiar ahora** la recuperación mediante HOME por una interpretación documental (el comentario de `mount_home.c` BFT sigue siendo el comportamiento vigente).

**C7 — NVS por perfil: diseño ahora, activación después.** Diseñar schema y namespaces (`cfg_nrm_ha`/`cfg_bft`) en esta fase, pero **activar la migración únicamente con un procedimiento probado** (herramienta + respaldo + rollback). Nunca borrar ni reinterpretar silenciosamente la configuración existente (namespace legado `mount` intacto).

Mapa a los PRs de la arquitectura §12: C6 → PR4 (rampas); C3/C4/C5 → PR5 (STOP/admisión/drenaje/barrera); C7 (diseño) → PR7; C1/C2 → entrega posterior.

## 7. Adenda — verificación en profundidad de `motors_motion_task.c` y concurrencia

Análisis completo de las dos tareas de movimiento, los auxiliares de parada y los archivos de rampa. Confirma §2–§4 y añade tres matices de comportamiento:

### 7.1 Confirmaciones (sin cambios al veredicto)

- **STOP**: NRM aborta (generación++ + `aborted=true` + `rmt_reset` acotado a 100 ms/canal) y espera idle (500 ms); BFT drena asíncrono (generación++ + `graceful=true`, sin espera) y cuenta cada lote exactamente una vez antes del deadline de 500 ms. El aborto urgente (`motors_motion_stop`) existe en ambos; BFT lo reserva a fallos de reconfig y faults, NRM lo usa como STOP público.
- **Horizonte**: confirmado — `motors_path_above_horizon` se invoca **solo** en `mount_slew_to_coordinates.c` (aceptación); ninguna de las dos tareas de movimiento referencia altitud ni horizonte en ejecución. NRM no tiene horizonte en ningún punto.
- **MoveAxis**: en ambos el cambio de velocidad es un **aborto** (no transición suave); NRM vía setpoint versionado, BFT vía peek de la cabeza de cola. Ambos envenenan `position_uncertain`.
- **Rampa**: ecuación idéntica; ventana 5°/5° (NRM, constantes en `motors_internal.h`) vs 10°/10° (BFT, en `motors_ramp.c`); el caso singular `log(v/vmin)/(v-vmin)` en `v==vmin` está **sin guarda en ambos**.
- **Concurrencia**: solo `volatile` + spinlock; sin mutexes ni `_Atomic`. No hay lock retenido durante UART/NVS/cola/RMT (anti-patrón ausente en ambos).

### 7.2 Matices nuevos (relevantes para «cero cambios de comportamiento»)

1. **Restauración de tracking tras STOP — divergencia real.** NRM cancela `resume_tracking` al parar (`motors_motion_stop` pone `aborted=true`); BFT **no** lo cancela en el STOP graceful/PARK, de modo que un GoTo detenido puede «resucitar» tracking y sobreescribir el estado READY/PARKED que el propio STOP fijó. Unificar STOP (C3) obliga a decidir este comportamiento: es exactamente el tipo de cambio involuntario que querés evitar si lo hacés sin explicitarlo.

2. **`position_uncertain` con semántica distinta.** NRM lo enciende ante **cualquier** SLEW/MOVE_AXIS abortado (incluso si no se emitió nada); BFT solo cuando quedan lotes sin confirmar tras el drenaje. Mismo nombre, distinto alcance. Es un ejemplo de «mismo flag, distinta verdad».

3. **Escrituras de contadores sin lock en NRM.** BFT protege **toda** escritura de `ra_steps`/`dec_steps` con el spinlock; NRM las deja sin lock en los caminos calientes (aunque el lector de snapshot sí toma el lock), una lectura rasgada cross-core posible. Unificar adopta el criterio BFT (un fix) o reproduce el de NRM (un defecto). Es material para el §6 de la arquitectura («`volatile` no establece coherencia»).

Los tres son diferencias de comportamiento entre las dos monturas de hoy; unificar es elegir uno de los dos comportamientos (o documentar el cambio). Refuerzan la necesidad de decidir C1–C7 antes de implementar.
