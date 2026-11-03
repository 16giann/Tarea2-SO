# Torneo de Komikan Concurrente — Tarea 2, Sistemas Operativos

**Integrantes:** Gianfranco Caleni · Gabriel Guaiquipan
**Lenguaje:** C (C17) · **Sincronización:** `pthread_mutex_t`, `pthread_cond_t`, `sem_t` POSIX · **Plataformas:** Linux y macOS

## 1. Compilación
Con las banderas exigidas por la rúbrica (compila sin warnings):
```bash
gcc -Wall -Wextra -std=c17 -pthread -o torneo torneo.c
```
o simplemente `make`. Otros objetivos: `make test` (suite), `make tsan` (data races), `make asan` (memoria/UB), `make clean`.

## 2. Ejecución
```bash
./torneo N M [config.txt]
```
| Parámetro | Significado |
|---|---|
| `N` | número total de jugadores/clientes que llegan al servidor (≥ 0) |
| `M` | máximo de mesas activas simultáneamente (≥ 1) |
| `config.txt` | opcional: parámetros iniciales **o** una suite de pruebas |

Ejemplos:
```bash
./torneo 6 2                      # 6 jugadores, 2 mesas, valores por defecto
./torneo 20 2 config.txt          # 20 jugadores, 2 mesas: se ve la cola de espera pasiva
./torneo 2 1 config_acorralado.txt # el Puma parte acorralado: ganan los Perros
./torneo 12 4 test_cases.txt      # suite de 12 pruebas (si el archivo empieza con SUITE_START)
```
Los jugadores se emparejan en orden de llegada (el 1.º del par es el Puma, el 2.º los Perros). Si N es impar, el último queda "sin pareja" y se informa. Al terminar se imprime un resumen (victorias, máximo de mesas simultáneas, invariantes del tablero, recursos liberados).

## 3. Formato del archivo de configuración
Líneas `CLAVE valor(es)` (también `CLAVE=valor`), `#` = comentario, todas opcionales. Ver `config.txt` (comentado).

| Clave | Descripción | Defecto |
|---|---|---|
| `SEED n` | semilla aleatoria (reproducibilidad de retardos y decisiones) | 42 |
| `DELAY_MIN ms` / `DELAY_MAX ms` | retardo mínimo/máximo por ciclo de decisión | 50 / 300 |
| `PUMA_INIT x y` | casilla inicial del Puma (o `TRONO_TOP/LEFT/RIGHT`) | `2 2` |
| `DOG id x y` | posición inicial del perro `id` (0–11). Si se define alguno, solo juegan los listados | 12 perros en filas inferiores |
| `MAX_MOVES n` | tope de movimientos por partida (empate); 0 = sin tope | 300 |
| `BOARD_EVERY k` | imprime el tablero cada k movimientos aceptados (1 = cada movimiento) | 10 |
| `VERBOSE 0/1` | registrar cada movimiento (aceptado/rechazado) | 1 |
| `DIAGONALS 0/1` | movimientos diagonales en la grilla | 1 |
| `WIN_CAPTURES n` | capturas para que gane el Puma | 6 |
| `ARRIVAL_MS ms` | ventana de llegada aleatoria de los jugadores | 200 |
| `LOG_FILE ruta` | además de consola, escribe el registro a un archivo | — |
| `FAULT_PUMA ms` / `FAULT_DOG id ms` | inyecta una caída abrupta (para probar resiliencia) | — |

**Suite de pruebas:** un archivo que empieza con `SUITE_START` (formato de `test_cases.txt`) con eventos `MOVE`, `DISCONNECT`, `SPAWN_PLAYERS`, `SIGNAL_SIGINT`, `BURST_RANDOM_MOVES`. En los escenarios con `MOVE`, cada ficha parte de su primer `FROM`; los perros no mencionados no participan.

## 4. Reglas implementadas
- Tablero de 5×5 intersecciones (`x`=columna 0–4, `y`=fila 0–4; `y=4` es la fila pegada al trono) + trono de 3 casillas (`TRONO_TOP`, `TRONO_LEFT`, `TRONO_RIGHT`).
- Puma: 1 hilo; se mueve a casillas adyacentes libres y captura saltando sobre un perro hacia la casilla libre siguiente en línea. **Gana con 6 capturas.**
- Perros: 12 hilos; se mueven a casillas adyacentes libres; no capturan. **Ganan si el Puma queda sin movimientos** (ni pasos ni saltos).
- Sin turnos: cada ficha repite *decidir → esperar retardo aleatorio → intentar mover*.
- Supuestos (la figura del enunciado no estaba disponible): adyacencia de 8 vecinos en la grilla; conexiones del trono `TOP–(2,4)`, `TOP–LEFT`, `TOP–RIGHT`, `LEFT–(1,4)`, `RIGHT–(3,4)`. Se cambian en `build_tables()`. Se agregó un tope de movimientos (empate) para garantizar que toda partida termine.

## 5. Decisiones de sincronización (resumen; detalle en informe)
- **Juez como único escritor del tablero.** Las 13 fichas de una mesa envían solicitudes a una cola FIFO (protegida por el mutex de la mesa) y se bloquean en su propia `pthread_cond_t`. El Juez las valida y aplica en orden de llegada: la primera ficha que llega a una casilla libre se la queda; las demás reciben `RECHAZADO`. Elimina las condiciones de carrera sobre el tablero.
- **Límite de mesas:** `sem_t` POSIX nombrado con valor `M`. El Matchmaker hace `sem_wait` antes de abrir cada mesa; si no hay mesa libre queda bloqueado en el kernel (espera **pasiva**, ~0,8 % de CPU medido con N=20, M=2). El Juez hace `sem_post` al cerrar. Se usa semáforo *nombrado* porque macOS no implementa `sem_init`, y se hace `sem_unlink` de inmediato para no dejar huérfanos.
- **Prevención de deadlock:** un solo mutex por mesa que nadie mantiene mientras espera otro recurso; jerarquía de locks mesa → log.
- **Livelock/starvation:** retardos aleatorios por ficha con decisión rehecha en cada ciclo; solicitudes y emparejamiento en orden FIFO; tope de movimientos por partida.
- **Fallos de jugador:** `pthread_cleanup_push` libera el mutex retenido y avisa al Juez, que declara ganador al oponente por abandono, destruye las primitivas de la mesa y libera el semáforo.
- **SIGINT/SIGTERM:** el manejador solo asigna un `atomic_int` y hace `sem_post` (async-signal-safe). Se detiene el ingreso, se cierran las partidas en orden, se cancelan los jugadores en cola, se hace `join` de todos los hilos y se liberan mutex, conds, semáforo y memoria.

## 6. Verificación realizada
- `make test`: 12/12 pruebas OK (repetida varias veces).
- ThreadSanitizer: 0 avisos · AddressSanitizer/UBSan/LeakSanitizer: 0 errores y 0 leaks (incluyendo Ctrl+C real con `kill -INT`).
- Comando recomendado para comprobarlo en macOS: `leaks --atExit -- ./torneo 6 2 config.txt` (debe indicar `0 leaks`). En Linux: `valgrind --leak-check=full ./torneo 6 2 config.txt`.

## 7. Estructura
```
torneo.c        código fuente completo (secciones: utilidades, tablero, config, reglas, fichas, Juez, matchmaker, suite, main)
Makefile        compilación y objetivos de prueba
config.txt      ejemplo de configuración (comentado)
config_acorralado.txt  ejemplo donde el Puma parte acorralado (ganan los Perros)
test_cases.txt  suite de 12 pruebas
informe
```
