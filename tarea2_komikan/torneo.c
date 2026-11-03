/*
 * torneo.c  -  Torneo de Komikan Concurrente (Tarea 2, Sistemas Operativos)
 * Uso:   ./torneo N M [config.txt]
 *     N = numero total de jugadores (clientes) que llegan al servidor
 *     M = maximo de mesas de juego activas al mismo tiempo
 *     config.txt = (opcional) parametros iniciales
 *     (archivo que empieza con SUITE_START, ver test_cases.txt)
 * Compilacion:  gcc -Wall -Wextra -std=c17 -O2 -pthread -o torneo torneo.c
 */

#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE    
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*  Constantes   */

enum {
    GRID = 5, NGRID = 25,
    C_TOP = 25, C_LEFT = 26, C_RIGHT = 27,       /* casillas del trono        */
    NCELLS = 28,
    NDOGS = 12, NPIECES = 13,                    /* pieza 0 = Puma, 1..12 = Perros */
    MAX_JUMPS = 256
};
#define PUMA 0
#define DOG_P(i) (1 + (i))

enum { SIDE_NONE = 0, SIDE_PUMA = 1, SIDE_DOGS = 2 };
enum { ST_OFF = 0, ST_ALIVE, ST_CAPTURED };
enum { W_NONE = 0, W_PUMA, W_DOGS, W_DRAW };
enum { RS_NONE = 0, RS_CAPTURES, RS_TRAPPED, RS_ABANDON, RS_MAXMOVES, RS_SIGINT,
       RS_SCENARIO, RS_STRESS };
enum { MV_OK = 0, MV_OK_CAPTURE, MV_REJ_OCCUPIED, MV_REJ_ADJ, MV_REJ_FROM,
       MV_REJ_DEAD, MV_REJ_NOJUMP, MV_REJ_RANGE, MV_OVER };
enum { REQ_NONE = 0, REQ_PENDING, REQ_DONE };
enum { P_WAITING = 0, P_ASSIGNED, P_UNMATCHED, P_CANCELLED };

static const char *RS_STR[] = {
    "-", "el Puma capturo los perros necesarios", "el Puma quedo acorralado",
    "abandono del oponente", "limite de movimientos", "interrupcion (SIGINT)",
    "escenario completado", "prueba de estres completada"
};
static const char *W_STR[] = { "SIN GANADOR", "PUMA", "PERROS", "EMPATE" };
static const char *MV_STR[] = {
    "ok", "ok (captura)", "casilla destino ocupada", "no es adyacente ni salto valido",
    "la ficha no esta en la casilla origen", "ficha capturada o fuera de juego",
    "no hay perro que saltar", "casilla fuera del tablero", "partida terminada"
};

/*  Utilidades: tiempo, RNG, log   */

static atomic_int g_stop_v = 0;                   /* SIGINT/SIGTERM recibido (atomico lock-free: seguro en handler) */
#define STOPPED() (atomic_load(&g_stop_v) != 0)
#define SET_STOP(v) atomic_store(&g_stop_v, (v))
static _Atomic(sem_t *) g_sem_signal = NULL;      /* para despertar al matchmaker desde el handler */
static int g_stop_announced = 0;                  /* protegido por g_log_mtx   */

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static void abs_deadline(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_REALTIME, ts);    /* pthread_cond_timedwait usa REALTIME */
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

static void sleep_ms(int ms) {     /* duerme ms, corta si hay SIGINT */
    long long end = now_ms() + ms;
    while (!STOPPED()) {
        long long left = end - now_ms();
        if (left <= 0) break;
        if (left > 50) left = 50;
        struct timespec ts = { 0, (long)left * 1000000L };
        nanosleep(&ts, NULL);
    }
}

typedef struct { uint64_t s; } Rng;   /* splitmix64: reproducible por semilla */
static void rng_seed(Rng *r, uint64_t seed) { r->s = seed; }
static uint64_t rng_next(Rng *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static int rng_range(Rng *r, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(rng_next(r) % (uint64_t)(hi - lo + 1));
}

static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_log_fp = NULL;
static long long g_t0 = 0;

static void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void LOG(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    pthread_mutex_lock(&g_log_mtx);
    printf("[%6lld ms] ", now_ms() - g_t0);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    if (g_log_fp) {
        fprintf(g_log_fp, "[%6lld ms] ", now_ms() - g_t0);
        vfprintf(g_log_fp, fmt, ap2);
        fprintf(g_log_fp, "\n");
        fflush(g_log_fp);
    }
    pthread_mutex_unlock(&g_log_mtx);
    va_end(ap2);
    va_end(ap);
}

static void LOG_RAW(const char *text) {   /* bloque multilinea atomico */
    pthread_mutex_lock(&g_log_mtx);
    fputs(text, stdout);
    fflush(stdout);
    if (g_log_fp) { fputs(text, g_log_fp); fflush(g_log_fp); }
    pthread_mutex_unlock(&g_log_mtx);
}

static void announce_sigint(void) {    /* mensaje unico al detectar Ctrl+C */
    int first = 0;
    pthread_mutex_lock(&g_log_mtx);
    if (!g_stop_announced) { g_stop_announced = 1; first = 1; }
    pthread_mutex_unlock(&g_log_mtx);
    if (first)
        LOG("[SIGINT] Senal recibida: se detiene el ingreso de jugadores y se cierran las partidas en orden");
}

static void on_signal(int sig) {    /* async-signal-safe: solo bandera + sem_post */
    (void)sig;
    SET_STOP(1);
    sem_t *s = atomic_load(&g_sem_signal);
    if (s) sem_post(s);
}

/*  Tablero: geometria, adyacencias y saltos                                 */
/*
 * Coordenadas (x,y): x = columnas, y = filass (y=4 es la fila de arriba,
 * pegada al trono). Casillas del trono: TRONO_TOP, TRONO_LEFT, TRONO_RIGHT.
 * Conexiones del trono (editar aqui si la figura del enunciado es distinta):
 *     (2,4)-TOP, TOP-LEFT, TOP-RIGHT, LEFT-(1,4), RIGHT-(3,4)
 * Se permiten diagonales en la grilla (config DIAGONALS 0 las desactiva).
 */

static int g_adj[NCELLS][8];
static int g_nadj[NCELLS];
typedef struct { int from, mid, to; } Jump;
static Jump g_jumps[MAX_JUMPS];
static int g_njumps;

static int cell_xy(int x, int y) { return y * GRID + x; }
static int cell_vx(int c) { return c < NGRID ? c % GRID : (c == C_TOP ? 2 : (c == C_LEFT ? 1 : 3)); }
static int cell_vy(int c) { return c < NGRID ? c / GRID : (c == C_TOP ? 6 : 5); }

static void add_edge(int a, int b) {
    g_adj[a][g_nadj[a]++] = b;
    g_adj[b][g_nadj[b]++] = a;
}
static void add_jump(int f, int m, int t) {
    if (g_njumps < MAX_JUMPS) g_jumps[g_njumps++] = (Jump){ f, m, t };
}

static void build_tables(int diag) {
    memset(g_nadj, 0, sizeof g_nadj);
    g_njumps = 0;
    for (int y = 0; y < GRID; y++)
        for (int x = 0; x < GRID; x++) {
            int c = cell_xy(x, y);
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    if (!diag && dx && dy) continue;
                    int nx = x + dx, ny = y + dy, jx = x + 2 * dx, jy = y + 2 * dy;
                    if (nx >= 0 && nx < GRID && ny >= 0 && ny < GRID)
                        g_adj[c][g_nadj[c]++] = cell_xy(nx, ny);
                    if (jx >= 0 && jx < GRID && jy >= 0 && jy < GRID)
                        add_jump(c, cell_xy(nx, ny), cell_xy(jx, jy));
                }
        }
    add_edge(C_TOP, cell_xy(2, 4));
    add_edge(C_TOP, C_LEFT);
    add_edge(C_TOP, C_RIGHT);
    add_edge(C_LEFT, cell_xy(1, 4));
    add_edge(C_RIGHT, cell_xy(3, 4));
    add_jump(C_TOP, cell_xy(2, 4), cell_xy(2, 3));      /* salto vertical por el eje central */
    add_jump(cell_xy(2, 3), cell_xy(2, 4), C_TOP);
}

static bool is_adjacent(int a, int b) {
    for (int i = 0; i < g_nadj[a]; i++) if (g_adj[a][i] == b) return true;
    return false;
}
static const Jump *find_jump(int from, int to) {
    for (int i = 0; i < g_njumps; i++)
        if (g_jumps[i].from == from && g_jumps[i].to == to) return &g_jumps[i];
    return NULL;
}

static void cell_str(int c, char *buf, size_t n) {
    if (c == C_TOP) snprintf(buf, n, "TRONO_TOP");
    else if (c == C_LEFT) snprintf(buf, n, "TRONO_LEFT");
    else if (c == C_RIGHT) snprintf(buf, n, "TRONO_RIGHT");
    else if (c >= 0 && c < NGRID) snprintf(buf, n, "(%d,%d)", c % GRID, c / GRID);
    else snprintf(buf, n, "?");
}
static void piece_str(int pid, char *buf, size_t n) {
    if (pid == PUMA) snprintf(buf, n, "PUMA");
    else snprintf(buf, n, "DOG %d", pid - 1);
}

/*  Config */

typedef struct {
    uint64_t seed;
    int dmin, dmax;                /* retardo por ciclo de decision (ms)        */
    int puma_cell;
    int dog_cell[NDOGS];           /* -1 = sin posicion explicita               */
    int layout_custom;             /* 1 si el archivo definio algun DOG         */
    int max_moves;                 /* 0 = sin limite                            */
    int board_every;               /* imprimir tablero cada k movimientos (0 = nunca) */
    int verbose;                   /* 1 = log de cada movimiento                */
    int diag;                      /* diagonales en la grilla                   */
    int win_captures;
    int arrival_ms;                /* ventana de llegada de los jugadores       */
    int fault_puma_ms;             /* inyeccion de fallos (-1 = ninguno)        */
    int fault_dog_id, fault_dog_ms;
    char log_file[256];
} Config;

static void config_default(Config *c) {
    memset(c, 0, sizeof *c);
    c->seed = 42; c->dmin = 50; c->dmax = 300;
    c->puma_cell = cell_xy(2, 2);
    for (int i = 0; i < NDOGS; i++) c->dog_cell[i] = -1;
    c->max_moves = 300; c->board_every = 10; c->verbose = 1; c->diag = 1;
    c->win_captures = 6; c->arrival_ms = 200;
    c->fault_puma_ms = -1; c->fault_dog_id = -1; c->fault_dog_ms = -1;
}

static int parse_ll(const char *s, long long *v) {
    char *e; errno = 0;
    long long x = strtoll(s, &e, 10);
    if (e == s || *e || errno) return -1;
    *v = x; return 0;
}

static int parse_cell(char **tok, int *i, int n, int *cell) {
    if (*i >= n) return -1;
    if (!strncasecmp(tok[*i], "TRONO_", 6)) {
        const char *s = tok[*i] + 6;
        if (!strcasecmp(s, "TOP")) *cell = C_TOP;
        else if (!strcasecmp(s, "LEFT")) *cell = C_LEFT;
        else if (!strcasecmp(s, "RIGHT")) *cell = C_RIGHT;
        else return -1;
        (*i)++; return 0;
    }
    long long x, y;
    if (*i + 1 >= n || parse_ll(tok[*i], &x) || parse_ll(tok[*i + 1], &y)) return -1;
    if (x < 0 || x >= GRID || y < 0 || y >= GRID) return -1;
    *cell = cell_xy((int)x, (int)y);
    *i += 2; return 0;
}

/* Aplica tokens "CLAVE valor" a la configuracion. Devuelve 0 si ok. */
static int config_apply(Config *c, char **tok, int n, char *err, size_t en) {
    int i = 0;
    while (i < n) {
        const char *k = tok[i++];
        long long v, w;
#define NEED_INT(var) do { if (i >= n || parse_ll(tok[i], &(var))) { \
            snprintf(err, en, "valor invalido para %s", k); return -1; } i++; } while (0)
        if (!strcasecmp(k, "SEED")) { NEED_INT(v); c->seed = (uint64_t)v; }
        else if (!strcasecmp(k, "DELAY_MIN")) { NEED_INT(v); c->dmin = (int)v; }
        else if (!strcasecmp(k, "DELAY_MAX")) { NEED_INT(v); c->dmax = (int)v; }
        else if (!strcasecmp(k, "MAX_MOVES")) { NEED_INT(v); c->max_moves = (int)v; }
        else if (!strcasecmp(k, "BOARD_EVERY")) { NEED_INT(v); c->board_every = (int)v; }
        else if (!strcasecmp(k, "VERBOSE")) { NEED_INT(v); c->verbose = (int)v; }
        else if (!strcasecmp(k, "DIAGONALS")) { NEED_INT(v); c->diag = v != 0; }
        else if (!strcasecmp(k, "WIN_CAPTURES")) { NEED_INT(v); c->win_captures = (int)v; }
        else if (!strcasecmp(k, "ARRIVAL_MS")) { NEED_INT(v); c->arrival_ms = (int)v; }
        else if (!strcasecmp(k, "FAULT_PUMA")) { NEED_INT(v); c->fault_puma_ms = (int)v; }
        else if (!strcasecmp(k, "FAULT_DOG")) {
            NEED_INT(v); NEED_INT(w);
            if (v < 0 || v >= NDOGS) { snprintf(err, en, "FAULT_DOG id fuera de rango"); return -1; }
            c->fault_dog_id = (int)v; c->fault_dog_ms = (int)w;
        }
        else if (!strcasecmp(k, "LOG_FILE")) {
            if (i >= n) { snprintf(err, en, "falta ruta en LOG_FILE"); return -1; }
            snprintf(c->log_file, sizeof c->log_file, "%s", tok[i++]);
        }
        else if (!strcasecmp(k, "PUMA_INIT")) {
            if (parse_cell(tok, &i, n, &c->puma_cell)) { snprintf(err, en, "PUMA_INIT invalido"); return -1; }
        }
        else if (!strcasecmp(k, "DOG")) {
            NEED_INT(v);
            int cell;
            if (v < 0 || v >= NDOGS || parse_cell(tok, &i, n, &cell)) {
                snprintf(err, en, "DOG invalido"); return -1;
            }
            c->dog_cell[v] = cell; c->layout_custom = 1;
        }
        else {                                    /* clave desconocida: se ignora el resto de la linea */
            fprintf(stderr, "aviso: clave de configuracion desconocida '%s' (ignorada)\n", k);
            break;
        }
#undef NEED_INT
    }
    return 0;
}

static int config_validate(Config *c) {
    if (c->dmin < 0) c->dmin = 0;
    if (c->dmax < c->dmin) {
        fprintf(stderr, "error: DELAY_MAX (%d) < DELAY_MIN (%d)\n", c->dmax, c->dmin);
        return -1;
    }
    return 0;
}

static int normalize_and_tokenize(char *line, char **tok, int max) {
    for (char *p = line; *p; p++) if (*p == '=' || *p == ':' || *p == ',') *p = ' ';
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    int n = 0; char *save = NULL;
    for (char *t = strtok_r(line, " \t\r\n", &save); t && n < max; t = strtok_r(NULL, " \t\r\n", &save))
        tok[n++] = t;
    return n;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}

static bool text_is_suite(const char *text) {
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        return strncasecmp(p, "SUITE_START", 11) == 0;
    }
    return false;
}

static int config_load_text(Config *c, char *text) {
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tok[64]; char err[128] = "";
        int n = normalize_and_tokenize(line, tok, 64);
        if (n == 0) continue;
        if (config_apply(c, tok, n, err, sizeof err)) {
            fprintf(stderr, "error en config: %s\n", err);
            return -1;
        }
    }
    return config_validate(c);
}

/*  Estructuras del juego    */

typedef struct Tournament Tournament;
typedef struct Player Player;

typedef struct { int status, from, to, result, cap; } Req;
typedef struct {
    int game_id, winner, reason, captures, inv_ok;
    long moves_ok, attempts, rejected;
    long long ms;
} GameResult;

typedef struct Game {
    int id;
    Config cfg;
    pthread_mutex_t mtx;                 /* protege TODO el estado de la mesa    */
    pthread_cond_t cv_judge;             /* despierta al Juez (nueva solicitud, salida, fallo) */
    pthread_cond_t cv_start;             /* largada de los hilos de escenario    */
    pthread_cond_t cv_piece[NPIECES];    /* despierta a UNA ficha (respuesta / fin) */
    int go;
    int board[NCELLS];                   /* id de pieza que ocupa la casilla, -1 = libre */
    int pos[NPIECES];
    int pstate[NPIECES];
    Req req[NPIECES];                    /* una solicitud pendiente por ficha como maximo */
    int fifo[NPIECES], fhead, flen;      /* orden de llegada                     */
    int over, winner, reason, captures;
    long moves_ok, attempts, rejected;
    int participants;                    /* hilos que aun juegan en esta mesa    */
    int fault_side, fault_piece;
    long burst_limit;
    long long t_start;
    Player *pp, *pd;
    Tournament *T;
    GameResult *out;
} Game;

static const int DEFAULT_ORDER[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 14, 11, 13, 15, 19, 16, 18, 20, 24, 21, 23, 22, 17, 12
};

static bool place_piece(Game *g, int pid, int cell) {
    if (cell < 0 || cell >= NCELLS || g->board[cell] != -1) return false;
    if (g->pos[pid] >= 0) g->board[g->pos[pid]] = -1;
    g->board[cell] = pid; g->pos[pid] = cell; g->pstate[pid] = ST_ALIVE;
    return true;
}

static Game *game_create(const Config *cfg, int id, bool empty_board, int participants) {
    Game *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->id = id; g->cfg = *cfg;
    pthread_mutex_init(&g->mtx, NULL);
    pthread_cond_init(&g->cv_judge, NULL);
    pthread_cond_init(&g->cv_start, NULL);
    for (int i = 0; i < NPIECES; i++) {
        pthread_cond_init(&g->cv_piece[i], NULL);
        g->pos[i] = -1; g->pstate[i] = ST_OFF;
    }
    for (int c = 0; c < NCELLS; c++) g->board[c] = -1;
    place_piece(g, PUMA, cfg->puma_cell);
    if (!empty_board) {
        if (cfg->layout_custom) {
            for (int i = 0; i < NDOGS; i++)
                if (cfg->dog_cell[i] >= 0 && !place_piece(g, DOG_P(i), cfg->dog_cell[i]))
                    fprintf(stderr, "aviso: Perro %d no puede ocupar su casilla inicial\n", i);
        } else {
            int placed = 0;
            for (size_t k = 0; k < sizeof DEFAULT_ORDER / sizeof *DEFAULT_ORDER && placed < NDOGS; k++)
                if (place_piece(g, DOG_P(placed), DEFAULT_ORDER[k])) placed++;
        }
    }
    g->participants = participants;
    g->t_start = now_ms();
    return g;
}

static void game_destroy(Game *g) {
    pthread_mutex_destroy(&g->mtx);
    pthread_cond_destroy(&g->cv_judge);
    pthread_cond_destroy(&g->cv_start);
    for (int i = 0; i < NPIECES; i++) pthread_cond_destroy(&g->cv_piece[i]);
    free(g);
}

static void glog(const Game *g, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void glog(const Game *g, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("[Mesa %d] %s", g->id, buf);
}

static void board_render(const Game *g, char *buf, size_t n) {
    char s[NCELLS];
    for (int c = 0; c < NCELLS; c++) {
        int p = g->board[c];
        s[c] = p < 0 ? '.' : (p == 0 ? 'P' : "0123456789AB"[p - 1]);
    }
    size_t o = 0;
    o += (size_t)snprintf(buf + o, n - o, "            TRONO\n%*s[%c]\n%*s[%c]%*s[%c]\n",
                          12, "", s[C_TOP], 9, "", s[C_LEFT], 3, "", s[C_RIGHT]);
    for (int y = GRID - 1; y >= 0 && o < n; y--) {
        o += (size_t)snprintf(buf + o, n - o, "  y=%d ", y);
        for (int x = 0; x < GRID && o < n; x++)
            o += (size_t)snprintf(buf + o, n - o, " %c ", s[cell_xy(x, y)]);
        o += (size_t)snprintf(buf + o, n - o, "\n");
    }
    if (o < n) o += (size_t)snprintf(buf + o, n - o, "      ");
    for (int x = 0; x < GRID && o < n; x++) o += (size_t)snprintf(buf + o, n - o, " %d ", x);
    if (o < n) snprintf(buf + o, n - o, "\n      (P=Puma, 0-9 A B = Perros 0..11)\n");
}

static void log_board(const Game *g, const char *title) {
    char b[1024], out[1280];
    board_render(g, b, sizeof b);
    snprintf(out, sizeof out, "[Mesa %d] --- %s (capturas=%d, movimientos=%ld) ---\n%s",
             g->id, title, g->captures, g->moves_ok, b);
    LOG_RAW(out);
}

/*  Reglas del juego (se ejecutan SIEMPRE con g->mtx tomado, por el Juez)    */

static bool puma_has_moves(const Game *g) {
    int p = g->pos[PUMA];
    if (p < 0) return false;
    for (int i = 0; i < g_nadj[p]; i++)
        if (g->board[g_adj[p][i]] == -1) return true;
    for (int j = 0; j < g_njumps; j++)
        if (g_jumps[j].from == p && g->board[g_jumps[j].mid] > 0 && g->board[g_jumps[j].to] == -1)
            return true;
    return false;
}

static int check_invariants(const Game *g) {
    int ok = 1, caps = 0;
    for (int c = 0; c < NCELLS; c++) {
        int p = g->board[c];
        if (p >= 0 && (p >= NPIECES || g->pos[p] != c)) ok = 0;
    }
    for (int p = 0; p < NPIECES; p++) {
        if (g->pstate[p] == ST_ALIVE && (g->pos[p] < 0 || g->board[g->pos[p]] != p)) ok = 0;
        if (g->pstate[p] != ST_ALIVE && g->pos[p] != -1) ok = 0;
        if (g->pstate[p] == ST_CAPTURED) caps++;
    }
    if (caps != g->captures) ok = 0;
    return ok;
}

static void set_over(Game *g, int winner, int reason) {
    if (g->over) return;
    g->over = 1; g->winner = winner; g->reason = reason;
    glog(g, "FIN DE PARTIDA -> %s (%s)", W_STR[winner], RS_STR[reason]);
    for (int p = 0; p < NPIECES; p++) pthread_cond_broadcast(&g->cv_piece[p]);
    pthread_cond_broadcast(&g->cv_start);
}

static void check_victory(Game *g) {
    if (g->over) return;
    if (g->captures >= g->cfg.win_captures) { set_over(g, W_PUMA, RS_CAPTURES); return; }
    if (!puma_has_moves(g)) { set_over(g, W_DOGS, RS_TRAPPED); return; }
    if (g->cfg.max_moves > 0 && g->moves_ok >= g->cfg.max_moves) set_over(g, W_DRAW, RS_MAXMOVES);
}

static int validate_and_apply(Game *g, int pid, int from, int to, int *cap) {
    *cap = -1;
    if (g->over) return MV_OVER;
    if (from < 0 || from >= NCELLS || to < 0 || to >= NCELLS) return MV_REJ_RANGE;
    if (g->pstate[pid] != ST_ALIVE) return MV_REJ_DEAD;
    if (g->pos[pid] != from) return MV_REJ_FROM;
    if (is_adjacent(from, to)) {                         /* paso simple */
        if (g->board[to] != -1) return MV_REJ_OCCUPIED;
        g->board[from] = -1; g->board[to] = pid; g->pos[pid] = to;
        return MV_OK;
    }
    if (pid != PUMA) return MV_REJ_ADJ;                   /* los perros no saltan */
    const Jump *j = find_jump(from, to);
    if (!j) return MV_REJ_ADJ;
    if (g->board[j->mid] <= 0) return MV_REJ_NOJUMP;      /* debe haber un PERRO en medio */
    if (g->board[to] != -1) return MV_REJ_OCCUPIED;
    int dog = g->board[j->mid];
    g->board[j->mid] = -1; g->pos[dog] = -1; g->pstate[dog] = ST_CAPTURED;
    g->board[from] = -1; g->board[to] = PUMA; g->pos[PUMA] = to;
    g->captures++;
    *cap = dog;
    return MV_OK_CAPTURE;
}

static void process_request(Game *g, int pid) {          /* g->mtx tomado */
    Req *r = &g->req[pid];
    int cap, res = validate_and_apply(g, pid, r->from, r->to, &cap);
    char who[16], a[16], b[16];
    piece_str(pid, who, sizeof who); cell_str(r->from, a, sizeof a); cell_str(r->to, b, sizeof b);
    g->attempts++;
    r->result = res; r->cap = cap; r->status = REQ_DONE;
    if (res == MV_OK || res == MV_OK_CAPTURE) {
        g->moves_ok++;
        if (g->cfg.verbose) {
            if (res == MV_OK_CAPTURE)
                glog(g, "MOVE %s FROM %s TO %s -> OK, CAPTURA DOG %d (capturas=%d)", who, a, b, cap - 1, g->captures);
            else
                glog(g, "MOVE %s FROM %s TO %s -> OK", who, a, b);
        }
        check_victory(g);
        if (g->cfg.board_every > 0 && g->moves_ok % g->cfg.board_every == 0 && !g->over)
            log_board(g, "Tablero");
    } else {
        g->rejected++;
        if (g->cfg.verbose)
            glog(g, "MOVE %s FROM %s TO %s -> RECHAZADO (%s)", who, a, b, MV_STR[res]);
    }
    pthread_cond_signal(&g->cv_piece[pid]);
    if (!g->over && g->burst_limit > 0 && g->attempts >= g->burst_limit)
        set_over(g, W_DRAW, RS_STRESS);
}

/*  Primitivas para las fichas   */

static void cleanup_unlock(void *m) { pthread_mutex_unlock((pthread_mutex_t *)m); }

typedef struct { Game *g; int side; int pid; } FaultCtx;

/* Se ejecuta SOLO si el hilo muere de forma abrupta (pthread_exit). */
static void fault_cleanup(void *arg) {
    FaultCtx *f = arg;
    Game *g = f->g;
    pthread_mutex_lock(&g->mtx);
    if (!g->over && !g->fault_side) {
        char who[16]; piece_str(f->pid, who, sizeof who);
        g->fault_side = f->side; g->fault_piece = f->pid;
        glog(g, "[FALLO DETECTADO] el hilo de %s termino abruptamente", f->side == SIDE_PUMA ? "PUMA" : "PERROS");
    }
    pthread_cond_signal(&g->cv_judge);
    pthread_mutex_unlock(&g->mtx);
}

/* Se ejecuta SIEMPRE al salir un participante (normal o abrupto). */
static void participant_leave(void *arg) {
    Game *g = arg;
    pthread_mutex_lock(&g->mtx);
    g->participants--;
    pthread_cond_signal(&g->cv_judge);
    pthread_mutex_unlock(&g->mtx);
}

static void crash_locked(Game *g, int pid, const char *why) {   /* simula caida abrupta CON el mutex tomado */
    char who[16]; piece_str(pid, who, sizeof who);
    glog(g, "[FALLO] %s aborta abruptamente (%s) reteniendo el mutex de la mesa", who, why);
    pthread_exit(NULL);                      /* los cleanup handlers liberan el mutex y avisan al Juez */
}

static void maybe_fault_locked(Game *g, int pid) {
    long long el = now_ms() - g->t_start;
    if (pid == PUMA && g->cfg.fault_puma_ms >= 0 && el >= g->cfg.fault_puma_ms)
        crash_locked(g, pid, "fallo inyectado por configuracion");
    if (pid > 0 && g->cfg.fault_dog_ms >= 0 && pid - 1 == g->cfg.fault_dog_id && el >= g->cfg.fault_dog_ms)
        crash_locked(g, pid, "fallo inyectado por configuracion");
}

/* Duerme ms con g->mtx tomado; se interrumpe si termina la partida. true = sigue en juego */
static bool piece_sleep_locked(Game *g, int pid, int ms) {
    struct timespec ts;
    abs_deadline(&ts, ms);
    while (!g->over && !STOPPED()) {
        if (pthread_cond_timedwait(&g->cv_piece[pid], &g->mtx, &ts) == ETIMEDOUT) break;
    }
    return !g->over && !STOPPED();
}

/* Envia un movimiento al Juez y espera su veredicto. */
static int submit_move(Game *g, int pid, int from, int to) {
    pthread_mutex_lock(&g->mtx);
    if (g->over) { pthread_mutex_unlock(&g->mtx); return MV_OVER; }
    Req *r = &g->req[pid];
    r->status = REQ_PENDING; r->from = from; r->to = to;
    g->fifo[(g->fhead + g->flen) % NPIECES] = pid;
    g->flen++;
    pthread_cond_signal(&g->cv_judge);
    while (r->status != REQ_DONE) pthread_cond_wait(&g->cv_piece[pid], &g->mtx);
    int res = r->result;
    r->status = REQ_NONE;
    pthread_mutex_unlock(&g->mtx);
    return res;
}

/* IA simple de las fichas */

static int cheb(int a, int b) {
    int dx = cell_vx(a) - cell_vx(b), dy = cell_vy(a) - cell_vy(b);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;
}

#ifndef COH_W
#define COH_W 8
#endif
/* Elige (from,to). from = -1 si no hay nada que hacer. g->mtx tomado. */
static void choose_move(Game *g, int pid, Rng *rng, int *from, int *to) {
    int cur = g->pos[pid];
    *from = -1; *to = -1;
    if (cur < 0 || g->pstate[pid] != ST_ALIVE) return;

    if (g->burst_limit > 0) {   /* modo estres: intentos al azar */
        if (pid == PUMA && rng_range(rng, 0, 3) == 0) {
            int cand[MAX_JUMPS], nc = 0;
            for (int j = 0; j < g_njumps; j++) if (g_jumps[j].from == cur) cand[nc++] = g_jumps[j].to;
            if (nc) { *from = cur; *to = cand[rng_range(rng, 0, nc - 1)]; return; }
        }
        if (g_nadj[cur]) { *from = cur; *to = g_adj[cur][rng_range(rng, 0, g_nadj[cur] - 1)]; }
        return;
    }

    if (pid == PUMA) {
        int cand[MAX_JUMPS], nc = 0;
        for (int j = 0; j < g_njumps; j++)                 /* 1) prefiere capturar */
            if (g_jumps[j].from == cur && g->board[g_jumps[j].mid] > 0 && g->board[g_jumps[j].to] == -1)
                cand[nc++] = g_jumps[j].to;
        if (nc) { *from = cur; *to = cand[rng_range(rng, 0, nc - 1)]; return; }
        int best = -1, bscore = -1000;                      /* 2) maximiza su movilidad */
        for (int i = 0; i < g_nadj[cur]; i++) {
            int n = g_adj[cur][i];
            if (g->board[n] != -1) continue;
            int mob = 0;
            for (int k = 0; k < g_nadj[n]; k++) if (g->board[g_adj[n][k]] == -1) mob++;
            int sc = mob * 10 + rng_range(rng, 0, 9);
            if (sc > bscore) { bscore = sc; best = n; }
        }
        if (best >= 0) { *from = cur; *to = best; }
        return;
    }

    /* Perro: se acerca al Puma sin exponerse. Como entre decidir y actuar pasa un retardo,
     * considera seguras solo las casillas que el Puma no podria saltar desde su posicion
     * actual NI desde cualquiera de las casillas libres a las que podria moverse. */
    int pc = g->pos[PUMA];
    int qs[9], nq = 0;
    qs[nq++] = pc;
    for (int i = 0; i < g_nadj[pc]; i++) if (g->board[g_adj[pc][i]] == -1) qs[nq++] = g_adj[pc][i];

    int unprot = 0;    /* mover este perro dejaria sin respaldo a otro perro? */
    for (int k = 0; k < nq && !unprot; k++)
        for (int j = 0; j < g_njumps; j++)
            if (g_jumps[j].from == qs[k] && g_jumps[j].to == cur && g->board[g_jumps[j].mid] > 0) { unprot = 1; break; }

    int best = -1, bscore = -100000, ubest = -1, uscore = -100000;
    for (int i = 0; i < g_nadj[cur]; i++) {
        int n = g_adj[cur][i];
        if (g->board[n] != -1) continue;
        int unsafe = unprot;
        for (int k = 0; k < nq && !unsafe; k++)
            for (int j = 0; j < g_njumps; j++)
                if (g_jumps[j].from == qs[k] && g_jumps[j].mid == n) {
                    int land = g_jumps[j].to;
                    if (g->board[land] == -1 || land == cur || land == pc) { unsafe = 1; break; }
                }
        int coh = 0;    /* cohesion: prefiere ir junto a otros perros (quedan respaldados) */
        for (int k = 0; k < g_nadj[n]; k++) if (g_adj[n][k] != cur && g->board[g_adj[n][k]] > 0) coh++;
        int sc = -cheb(n, pc) * 10 + coh * COH_W + rng_range(rng, 0, 12);
        if (!unsafe) { if (sc > bscore) { bscore = sc; best = n; } }
        else if (sc > uscore) { uscore = sc; ubest = n; }
    }
    /* Impaciencia: cuanto mas larga la partida, mas riesgo aceptan (evita empates por estancamiento) */
    int risk = 20 + (int)(g->moves_ok / 5);
    if (risk > 75) risk = 75;
    if (best < 0 && ubest >= 0 && rng_range(rng, 0, 99) < risk) best = ubest;
    if (best >= 0) { *from = cur; *to = best; }
}

/* Ciclo de decision de una ficha: decide -> espera retardo aleatorio -> actua */
static void piece_loop(Game *g, int pid) {
    Rng rng;
    rng_seed(&rng, g->cfg.seed * 1000003ULL + (uint64_t)g->id * 7919ULL + (uint64_t)pid * 104729ULL + 1ULL);
    for (;;) {
        int from = -1, to = -1;
        bool run = true;
        pthread_mutex_lock(&g->mtx);
        pthread_cleanup_push(cleanup_unlock, &g->mtx);
        if (g->over || STOPPED() || g->pstate[pid] == ST_CAPTURED) {
            run = false;
        } else {
            maybe_fault_locked(g, pid);    /* puede morir AQUI, con el mutex tomado */
            choose_move(g, pid, &rng, &from, &to);
            int d = rng_range(&rng, g->cfg.dmin, g->cfg.dmax);
            if (d < 1) d = 1;
            run = piece_sleep_locked(g, pid, d);
        }
        pthread_cleanup_pop(1);
        if (!run) return;
        if (from < 0) continue;
        if (submit_move(g, pid, from, to) == MV_OVER) return;
    }
}

typedef struct { Game *g; int pid; } DogCtx;

static void *dog_thread(void *arg) {
    DogCtx *d = arg;
    FaultCtx fc = { d->g, SIDE_DOGS, d->pid };
    pthread_cleanup_push(fault_cleanup, &fc);
    piece_loop(d->g, d->pid);
    pthread_cleanup_pop(0);
    return NULL;
}

static void dogs_run(Game *g) {    /* el "jugador Perros" lanza 12 hilos */
    pthread_t th[NDOGS];
    DogCtx ctx[NDOGS];
    bool started[NDOGS] = { false };
    for (int i = 0; i < NDOGS; i++) {
        if (g->pstate[DOG_P(i)] != ST_ALIVE) continue;
        ctx[i].g = g; ctx[i].pid = DOG_P(i);
        if (pthread_create(&th[i], NULL, dog_thread, &ctx[i]) == 0) started[i] = true;
        else {
            FaultCtx fc = { g, SIDE_DOGS, DOG_P(i) };
            glog(g, "error: no se pudo crear el hilo del Perro %d", i);
            fault_cleanup(&fc);
        }
    }
    for (int i = 0; i < NDOGS; i++) if (started[i]) pthread_join(th[i], NULL);
}

/* Un "lado" completo (Puma o Perros). Lo ejecuta un hilo Jugador. */
static void run_side(Game *g, int side) {
    FaultCtx fc = { g, side, side == SIDE_PUMA ? PUMA : -1 };
    pthread_cleanup_push(participant_leave, g);
    pthread_cleanup_push(fault_cleanup, &fc);
    if (side == SIDE_PUMA) piece_loop(g, PUMA);
    else dogs_run(g);
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(1);
}

/*  Juez / Arbitro  */

typedef struct { int max_active, active, games_done, n_inv_bad, unmatched, cancelled; } TStats;

typedef struct Tournament {
    Config cfg;
    int N, M;
    sem_t *sem;                         /* semaforo POSIX: mesas libres        */
    pthread_mutex_t mtx;                /* protege las estadisticas            */
    int active, max_active, games_started, games_done;
    int n_puma, n_dogs, n_draw, n_abandon, n_sigint, n_inv_bad;
    int unmatched, cancelled;
    pthread_mutex_t qmtx;               /* protege la cola de espera           */
    pthread_cond_t qcv;
    Player **queue;
    int qhead, qlen, arrived, closed;
    Player *players;
    pthread_t mm_tid;
    bool mm_started;
    pthread_t *jtids;
    int njudges;
} Tournament;

struct Player {
    int id, role, state;
    Game *game;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    pthread_t tid;
    bool started;
    Tournament *T;
};

static void assign_player(Player *p, Game *g, int role) {
    pthread_mutex_lock(&p->mtx);
    p->game = g; p->role = role; p->state = P_ASSIGNED;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mtx);
}

static void release_player(Player *p, int state) {
    pthread_mutex_lock(&p->mtx);
    p->state = state;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mtx);
}

static void tournament_game_done(Tournament *T, const GameResult *r) {
    pthread_mutex_lock(&T->mtx);
    T->active--; T->games_done++;
    if (!r->inv_ok) T->n_inv_bad++;
    if (r->reason == RS_ABANDON) T->n_abandon++;
    else if (r->reason == RS_SIGINT) T->n_sigint++;
    else if (r->winner == W_PUMA) T->n_puma++;
    else if (r->winner == W_DOGS) T->n_dogs++;
    else T->n_draw++;
    pthread_mutex_unlock(&T->mtx);
    sem_post(T->sem);        /* la mesa queda libre */
}

static void *judge_main(void *arg) {
    Game *g = arg;

    if (g->pp) {      /* publica la mesa a los dos jugadores */
        glog(g, "MESA ABIERTA: Jugador %d (Puma) vs Jugador %d (Perros)", g->pp->id, g->pd->id);
        assign_player(g->pp, g, SIDE_PUMA);
        assign_player(g->pd, g, SIDE_DOGS);
    }

    pthread_mutex_lock(&g->mtx);
    log_board(g, "Estado inicial");
    check_victory(g);     /* por si la disposicion inicial ya decide la partida */

    while (!g->over) {
        while (g->flen > 0 && !g->over) {   /* atiende solicitudes en orden de llegada */
            int pid = g->fifo[g->fhead];
            g->fhead = (g->fhead + 1) % NPIECES;
            g->flen--;
            process_request(g, pid);
        }
        if (g->over) break;
        if (g->fault_side) {                              /* un jugador cayo: gana el oponente */
            set_over(g, g->fault_side == SIDE_PUMA ? W_DOGS : W_PUMA, RS_ABANDON);
            break;
        }
        if (STOPPED()) { announce_sigint(); set_over(g, W_NONE, RS_SIGINT); break; }
        if (g->participants <= 0) { set_over(g, W_NONE, RS_SCENARIO); break; }
        struct timespec ts;
        abs_deadline(&ts, 100);
        pthread_cond_timedwait(&g->cv_judge, &g->mtx, &ts);
    }

    /* Cierre: responde pendientes, despierta a todos y espera que se retiren */
    while (g->flen > 0) {
        int pid = g->fifo[g->fhead];
        g->fhead = (g->fhead + 1) % NPIECES;
        g->flen--;
        g->req[pid].result = MV_OVER; g->req[pid].status = REQ_DONE;
        pthread_cond_signal(&g->cv_piece[pid]);
    }
    while (g->participants > 0) {
        for (int p = 0; p < NPIECES; p++) pthread_cond_broadcast(&g->cv_piece[p]);
        struct timespec ts;
        abs_deadline(&ts, 20);
        pthread_cond_timedwait(&g->cv_judge, &g->mtx, &ts);
    }

    GameResult res;
    memset(&res, 0, sizeof res);
    res.game_id = g->id; res.winner = g->winner; res.reason = g->reason; res.captures = g->captures;
    res.moves_ok = g->moves_ok; res.attempts = g->attempts; res.rejected = g->rejected;
    res.ms = now_ms() - g->t_start; res.inv_ok = check_invariants(g);
    glog(g, "RESULTADO: gana %s | motivo: %s | capturas=%d | movimientos=%ld | intentos=%ld | rechazados=%ld | %lld ms | invariantes=%s",
         W_STR[g->winner], RS_STR[g->reason], g->captures, g->moves_ok, g->attempts, g->rejected,
         res.ms, res.inv_ok ? "OK" : "FALLA");
    log_board(g, "Tablero final");
    pthread_mutex_unlock(&g->mtx);

    Tournament *T = g->T;
    if (g->out) *g->out = res;
    int gid = g->id;
    game_destroy(g);    /* destruye mutex/cond y libera memoria */
    if (T) {
        tournament_game_done(T, &res);
        LOG("[Mesa %d] CERRADA y liberada", gid);
    }
    return NULL;
}

/*  Jugadores, Matchmaker y Torneo  */

static void *player_main(void *arg) {
    Player *p = arg;
    Tournament *T = p->T;
    Rng r;
    rng_seed(&r, T->cfg.seed * 7919ULL + (uint64_t)p->id * 104729ULL + 17ULL);
    sleep_ms(rng_range(&r, 0, T->cfg.arrival_ms));        /* llegada aleatoria */

    bool enq = false;
    int qlen = 0;
    pthread_mutex_lock(&T->qmtx);
    T->arrived++;
    if (!STOPPED() && !T->closed) {
        T->queue[(T->qhead + T->qlen) % T->N] = p;
        T->qlen++; qlen = T->qlen; enq = true;
    }
    pthread_cond_signal(&T->qcv);
    pthread_mutex_unlock(&T->qmtx);
    if (!enq) {
        LOG("[Jugador %d] llega con el servidor cerrado: no es admitido", p->id);
        return NULL;
    }
    LOG("[Jugador %d] llega al servidor (en espera de pareja: %d)", p->id, qlen);

    pthread_mutex_lock(&p->mtx);
    while (p->state == P_WAITING) pthread_cond_wait(&p->cv, &p->mtx);   /* espera pasiva */
    int st = p->state, role = p->role;
    Game *g = p->game;
    pthread_mutex_unlock(&p->mtx);

    if (st == P_ASSIGNED) {
        int gid = g->id;
        LOG("[Jugador %d] juega como %s en la Mesa %d", p->id, role == SIDE_PUMA ? "PUMA" : "PERROS", gid);
        run_side(g, role);                                 /* tras esto NO se toca g */
        LOG("[Jugador %d] termino su partida (Mesa %d)", p->id, gid);
    } else if (st == P_UNMATCHED) {
        LOG("[Jugador %d] sin pareja: el torneo termino sin rival disponible", p->id);
    } else {
        LOG("[Jugador %d] cancelado por cierre del servidor", p->id);
    }
    return NULL;
}

static Player *queue_pop(Tournament *T) {    /* qmtx tomado */
    Player *p = T->queue[T->qhead];
    T->qhead = (T->qhead + 1) % T->N;
    T->qlen--;
    return p;
}

static bool acquire_table(Tournament *T, const Player *a, const Player *b) {
    if (sem_trywait(T->sem) == 0) return !STOPPED();
    LOG("[MATCHMAKER] pareja (J%d Puma, J%d Perros) en cola: las %d mesas estan ocupadas -> espera pasiva",
        a->id, b->id, T->M);
    for (;;) {
        if (sem_wait(T->sem) == 0) break;      /* bloqueado en el kernel, sin busy-wait */
        if (errno == EINTR) { if (STOPPED()) return false; continue; }
        return false;
    }
    return !STOPPED();
}

static void *matchmaker_main(void *arg) {
    Tournament *T = arg;
    int next_game = 1;
    for (;;) {
        Player *a = NULL, *b = NULL;
        pthread_mutex_lock(&T->qmtx);
        while (!STOPPED() && T->qlen < 2 && T->arrived < T->N) {
            struct timespec ts;
            abs_deadline(&ts, 100);
            pthread_cond_timedwait(&T->qcv, &T->qmtx, &ts);
        }
        if (!STOPPED() && T->qlen >= 2) {
            a = queue_pop(T); b = queue_pop(T);
        } else {
            T->closed = 1;      /* nadie mas entra */
            pthread_mutex_unlock(&T->qmtx);
            if (STOPPED()) announce_sigint();
            for (;;) {         /* libera a los que quedaron en cola */
                pthread_mutex_lock(&T->qmtx);
                if (!T->qlen) { pthread_mutex_unlock(&T->qmtx); break; }
                Player *p = queue_pop(T);
                pthread_mutex_unlock(&T->qmtx);
                if (STOPPED()) { release_player(p, P_CANCELLED); T->cancelled++; }
                else { release_player(p, P_UNMATCHED); T->unmatched++; }
            }
            break;
        }
        pthread_mutex_unlock(&T->qmtx);

        if (!acquire_table(T, a, b)) {    /* SIGINT mientras esperaba mesa */
            release_player(a, P_CANCELLED); release_player(b, P_CANCELLED);
            T->cancelled += 2;
            continue;
        }
        Game *g = game_create(&T->cfg, next_game, false, 2);
        if (!g) {
            LOG("[MATCHMAKER] error: sin memoria para crear la mesa");
            sem_post(T->sem);
            release_player(a, P_CANCELLED); release_player(b, P_CANCELLED);
            T->cancelled += 2;
            continue;
        }
        g->pp = a; g->pd = b; g->T = T;
        pthread_mutex_lock(&T->mtx);
        T->active++; T->games_started++;
        if (T->active > T->max_active) T->max_active = T->active;
        int act = T->active;
        pthread_mutex_unlock(&T->mtx);
        LOG("[MATCHMAKER] Mesa %d asignada a J%d (Puma) y J%d (Perros) | mesas activas: %d/%d",
            next_game, a->id, b->id, act, T->M);
        if (pthread_create(&T->jtids[T->njudges], NULL, judge_main, g) != 0) {
            LOG("[MATCHMAKER] error: no se pudo crear el Juez de la mesa %d", next_game);
            pthread_mutex_lock(&T->mtx); T->active--; pthread_mutex_unlock(&T->mtx);
            sem_post(T->sem);
            game_destroy(g);
            release_player(a, P_CANCELLED); release_player(b, P_CANCELLED);
            T->cancelled += 2;
            continue;
        }
        T->njudges++;
        next_game++;
    }
    return NULL;
}

static Tournament *tournament_start(int N, int M, const Config *cfg) {
    Tournament *T = calloc(1, sizeof *T);
    if (!T) return NULL;
    T->cfg = *cfg; T->N = N; T->M = M;
    pthread_mutex_init(&T->mtx, NULL);
    pthread_mutex_init(&T->qmtx, NULL);
    pthread_cond_init(&T->qcv, NULL);

    static int counter = 0;      /* semaforo POSIX NOMBRADO (macOS no tiene sem_init) */
    char name[48];
    snprintf(name, sizeof name, "/komikan_%d_%d", (int)getpid(), counter++);
    sem_unlink(name);
    T->sem = sem_open(name, O_CREAT | O_EXCL, 0600, (unsigned int)M);
    if (T->sem == SEM_FAILED) {
        fprintf(stderr, "error: sem_open: %s\n", strerror(errno));
        pthread_mutex_destroy(&T->mtx); pthread_mutex_destroy(&T->qmtx); pthread_cond_destroy(&T->qcv);
        free(T);
        return NULL;
    }
    sem_unlink(name);       /* se desvincula YA: el semaforo desaparece solo aunque el proceso muera -> cero huerfanos */
    atomic_store(&g_sem_signal, T->sem);

    int cap = N > 0 ? N : 1;
    T->queue = calloc((size_t)cap, sizeof *T->queue);
    T->players = calloc((size_t)cap, sizeof *T->players);
    T->jtids = calloc((size_t)(cap / 2 + 1), sizeof *T->jtids);

    LOG("[SERVIDOR] Torneo iniciado: N=%d jugadores, M=%d mesas maximas, seed=%llu, retardo=%d..%d ms",
        N, M, (unsigned long long)cfg->seed, cfg->dmin, cfg->dmax);

    if (pthread_create(&T->mm_tid, NULL, matchmaker_main, T) == 0) T->mm_started = true;
    for (int i = 0; i < N; i++) {
        Player *p = &T->players[i];
        p->id = i + 1; p->state = P_WAITING; p->T = T;
        pthread_mutex_init(&p->mtx, NULL);
        pthread_cond_init(&p->cv, NULL);
        if (pthread_create(&p->tid, NULL, player_main, p) == 0) p->started = true;
        else {
            fprintf(stderr, "error: no se pudo crear el hilo del jugador %d\n", p->id);
            pthread_mutex_lock(&T->qmtx);
            T->arrived++;
            pthread_cond_signal(&T->qcv);
            pthread_mutex_unlock(&T->qmtx);
        }
    }
    return T;
}

/* Espera el fin, libera TODO y devuelve las estadisticas por referencia. */
static void tournament_join(Tournament *T, TStats *stats_out) {
    if (T->mm_started) pthread_join(T->mm_tid, NULL);
    for (int i = 0; i < T->njudges; i++) pthread_join(T->jtids[i], NULL);
    for (int i = 0; i < T->N; i++) if (T->players[i].started) pthread_join(T->players[i].tid, NULL);

    LOG("[SERVIDOR] ============ RESUMEN DEL TORNEO ============");
    LOG("[SERVIDOR] Jugadores: %d | Mesas maximas: %d | Partidas iniciadas: %d | terminadas: %d",
        T->N, T->M, T->games_started, T->games_done);
    LOG("[SERVIDOR] Gana Puma: %d | Ganan Perros: %d | Abandono: %d | Empate/limite: %d | Interrumpidas (SIGINT): %d",
        T->n_puma, T->n_dogs, T->n_abandon, T->n_draw, T->n_sigint);
    LOG("[SERVIDOR] Jugadores sin pareja: %d | cancelados: %d", T->unmatched, T->cancelled);
    LOG("[SERVIDOR] Maximo de mesas simultaneas observado: %d (limite %d) -> %s",
        T->max_active, T->M, T->max_active <= T->M ? "OK" : "VIOLACION DEL LIMITE");
    LOG("[SERVIDOR] Invariantes del tablero: %s", T->n_inv_bad == 0 ? "OK en todas las mesas" : "FALLAS DETECTADAS");

    if (stats_out) {
        stats_out->max_active = T->max_active; stats_out->active = T->active;
        stats_out->games_done = T->games_done; stats_out->n_inv_bad = T->n_inv_bad;
        stats_out->unmatched = T->unmatched; stats_out->cancelled = T->cancelled;
    }

    atomic_store(&g_sem_signal, NULL);    /* el handler ya no debe tocar el semaforo */
    sem_close(T->sem);
    for (int i = 0; i < T->N; i++) {
        pthread_mutex_destroy(&T->players[i].mtx);
        pthread_cond_destroy(&T->players[i].cv);
    }
    pthread_mutex_destroy(&T->mtx);
    pthread_mutex_destroy(&T->qmtx);
    pthread_cond_destroy(&T->qcv);
    free(T->queue); free(T->players); free(T->jtids);
    LOG("[SERVIDOR] Recursos liberados: mutex, variables de condicion, semaforo POSIX, hilos y memoria");
    free(T);
}

/*  SUITE de pruebas (formato test_cases.txt) */

typedef enum { A_MOVE, A_PLACE, A_DISCONNECT } ActKind;
typedef struct { ActKind kind; int from, to, cap_dog, cap_at; } Action;
typedef struct { Action *a; int n, cap; } Script;
typedef struct {
    int id;
    char name[160];
    Config cfg;
    Script sc[NPIECES];
    int spawn_n, spawn_m, wait_q, sigint, burst_n;
} TestCase;

static void script_add(Script *s, Action a) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 4;
        s->a = realloc(s->a, (size_t)s->cap * sizeof *s->a);
    }
    s->a[s->n++] = a;
}

typedef struct { Game *g; int pid; const Script *sc; } ScriptCtx;

static void scenario_place_locked(Game *g, int pid, int cell, const char *why) {
    if (g->pstate[pid] == ST_CAPTURED || g->pos[pid] == cell) return;
    char who[16], c[16];
    piece_str(pid, who, sizeof who); cell_str(cell, c, sizeof c);
    if (g->board[cell] != -1) {
        glog(g, "[ESCENARIO] no se puede situar %s en %s: casilla ocupada", who, c);
        return;
    }
    place_piece(g, pid, cell);
    glog(g, "[ESCENARIO] %s situado en %s (%s)", who, c, why);
}

/* Un paso del guion: espera el retardo y ejecuta la accion preparatoria bajo el mutex.
 * (funcion aparte para que cleanup_push/pop no choque con variables locales vivas) */
static bool script_step(Game *g, int pid, const Action *a, int d) {
    bool run;
    pthread_mutex_lock(&g->mtx);
    pthread_cleanup_push(cleanup_unlock, &g->mtx);
    run = piece_sleep_locked(g, pid, d);
    if (run && a->kind == A_DISCONNECT) crash_locked(g, pid, "desconexion simulada por el escenario");
    if (run && a->kind == A_PLACE) {
        scenario_place_locked(g, pid, a->to, "posicionamiento del escenario");
        check_victory(g);
        if (g->cfg.board_every > 0) log_board(g, "Tablero");
    }
    if (run && a->kind == A_MOVE && a->cap_dog >= 0)
        scenario_place_locked(g, DOG_P(a->cap_dog), a->cap_at, "objetivo de captura del escenario");
    pthread_cleanup_pop(1);
    return run;
}

static void script_run(Game *g, int pid, const Script *sc) {
    Rng rng;
    rng_seed(&rng, g->cfg.seed * 1000003ULL + (uint64_t)pid * 104729ULL + 1ULL);
    pthread_mutex_lock(&g->mtx);                           /* largada simultanea */
    while (!g->go && !g->over) pthread_cond_wait(&g->cv_start, &g->mtx);
    pthread_mutex_unlock(&g->mtx);
    for (int i = 0; i < sc->n; i++) {
        const Action *a = &sc->a[i];
        int d = rng_range(&rng, g->cfg.dmin, g->cfg.dmax);
        if (!script_step(g, pid, a, d)) break;
        if (a->kind == A_MOVE && submit_move(g, pid, a->from, a->to) == MV_OVER) break;
    }
}

static void *script_thread(void *arg) {
    ScriptCtx *c = arg;
    FaultCtx fc = { c->g, c->pid == PUMA ? SIDE_PUMA : SIDE_DOGS, c->pid };
    pthread_cleanup_push(participant_leave, c->g);
    pthread_cleanup_push(fault_cleanup, &fc);
    script_run(c->g, c->pid, c->sc);
    pthread_cleanup_pop(0);
    pthread_cleanup_pop(1);
    return NULL;
}

typedef struct { Game *g; int side; } SideCtx;
static void *side_thread(void *arg) {
    SideCtx *s = arg;
    run_side(s->g, s->side);
    return NULL;
}

static int run_scenario(const TestCase *tc) {
    Config cfg = tc->cfg;
    cfg.verbose = 1; cfg.board_every = 1; cfg.max_moves = 0;
    SET_STOP(0);
    build_tables(cfg.diag);
    ScriptCtx ctx[NPIECES];
    pthread_t th[NPIECES];
    int nth = 0;
    for (int p = 0; p < NPIECES; p++) if (tc->sc[p].n > 0) nth++;
    Game *g = game_create(&cfg, 1, true, nth);
    if (!g) return 0;
    GameResult res;
    memset(&res, 0, sizeof res);
    g->out = &res;
    for (int p = 1; p < NPIECES; p++) {                   /* preparacion: cada ficha parte de su 1er FROM */
        if (tc->sc[p].n > 0 && tc->sc[p].a[0].kind == A_MOVE) {
            char who[16], c[16];
            piece_str(p, who, sizeof who); cell_str(tc->sc[p].a[0].from, c, sizeof c);
            if (place_piece(g, p, tc->sc[p].a[0].from)) glog(g, "[ESCENARIO] %s inicia en %s", who, c);
            else glog(g, "[ESCENARIO] %s no puede iniciar en %s (ocupada)", who, c);
        }
    }
    if (tc->sc[PUMA].n > 0 && tc->sc[PUMA].a[0].kind == A_MOVE && tc->sc[PUMA].a[0].from != g->pos[PUMA]) {
        char c[16]; cell_str(tc->sc[PUMA].a[0].from, c, sizeof c);
        place_piece(g, PUMA, tc->sc[PUMA].a[0].from);
        glog(g, "[ESCENARIO] el Puma inicia en %s (segun el primer evento)", c);
    }
    pthread_t jt;
    if (pthread_create(&jt, NULL, judge_main, g) != 0) { game_destroy(g); return 0; }
    int k = 0;
    for (int p = 0; p < NPIECES; p++) {
        if (tc->sc[p].n == 0) continue;
        ctx[k].g = g; ctx[k].pid = p; ctx[k].sc = &tc->sc[p];
        pthread_create(&th[k], NULL, script_thread, &ctx[k]);
        k++;
    }
    pthread_mutex_lock(&g->mtx);
    g->go = 1;
    pthread_cond_broadcast(&g->cv_start);
    pthread_mutex_unlock(&g->mtx);
    for (int i = 0; i < k; i++) pthread_join(th[i], NULL);
    pthread_join(jt, NULL);
    printf("    -> intentos=%ld aceptados=%ld rechazados=%ld ganador=%s motivo=%s\n",
           res.attempts, res.moves_ok, res.rejected, W_STR[res.winner], RS_STR[res.reason]);
    return res.inv_ok;
}

static int run_burst(const TestCase *tc) {
    Config cfg = tc->cfg;
    cfg.verbose = 0; cfg.board_every = 0; cfg.max_moves = 0;
    SET_STOP(0);
    build_tables(cfg.diag);
    Game *g = game_create(&cfg, 1, false, 2);
    if (!g) return 0;
    GameResult res;
    memset(&res, 0, sizeof res);
    g->out = &res; g->burst_limit = tc->burst_n;
    pthread_t jt, ts[2];
    SideCtx sc[2] = { { g, SIDE_PUMA }, { g, SIDE_DOGS } };
    pthread_create(&jt, NULL, judge_main, g);
    pthread_create(&ts[0], NULL, side_thread, &sc[0]);
    pthread_create(&ts[1], NULL, side_thread, &sc[1]);
    pthread_join(ts[0], NULL); pthread_join(ts[1], NULL); pthread_join(jt, NULL);
    printf("    -> intentos=%ld aceptados=%ld rechazados=%ld (13 hilos compitiendo con retardo %d ms)\n",
           res.attempts, res.moves_ok, res.rejected, cfg.dmin);
    return res.inv_ok && (res.attempts >= tc->burst_n || res.reason != RS_STRESS);
}

static int run_spawn(const TestCase *tc) {
    Config cfg = tc->cfg;
    cfg.verbose = 0; cfg.board_every = 0;
    if (cfg.max_moves <= 0 || cfg.max_moves > 100) cfg.max_moves = 100;   /* acota la duracion de la suite */
    SET_STOP(0);
    g_stop_announced = 0;
    build_tables(cfg.diag);
    Tournament *T = tournament_start(tc->spawn_n, tc->spawn_m, &cfg);
    if (!T) return 0;
    if (tc->sigint) {
        sleep_ms(400);                                       /* deja que haya partidas en curso */
        LOG("[TEST] enviando SIGINT al proceso (raise)");
        raise(SIGINT);
    }
    TStats st;
    tournament_join(T, &st);
    int ok = st.max_active <= tc->spawn_m && st.n_inv_bad == 0 && st.active == 0;
    if (!tc->sigint) {
        int accounted = st.games_done * 2 + st.unmatched + st.cancelled;
        ok = ok && accounted == tc->spawn_n;
    }
    SET_STOP(0);
    return ok;
}

static void free_tests(TestCase *tests, int n) {
    for (int i = 0; i < n; i++) for (int p = 0; p < NPIECES; p++) free(tests[i].sc[p].a);
    free(tests);
}

static int run_suite(char *text) {
    int cap = 8, nt = 0, declared = -1;
    TestCase *tests = calloc((size_t)cap, sizeof *tests);
    TestCase *cur = NULL;
    char *save = NULL;
    int lineno = 0;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        lineno++;
        char raw[512];
        snprintf(raw, sizeof raw, "%s", line);
        char *tok[64]; char err[128] = "";
        int n = normalize_and_tokenize(line, tok, 64);
        if (n == 0) continue;
        if (!strcasecmp(tok[0], "SUITE_START")) { long long v; if (n > 1 && !parse_ll(tok[1], &v)) declared = (int)v; continue; }
        if (!strcasecmp(tok[0], "SUITE_END")) break;
        if (!strcasecmp(tok[0], "TEST_CASE")) {
            if (nt == cap) { cap *= 2; tests = realloc(tests, (size_t)cap * sizeof *tests); }
            cur = &tests[nt++];
            memset(cur, 0, sizeof *cur);
            config_default(&cur->cfg);
            long long v = nt;
            if (n > 1) parse_ll(tok[1], &v);
            cur->id = (int)v;
            char *q1 = strchr(raw, '"'), *q2 = q1 ? strrchr(raw, '"') : NULL;
            if (q1 && q2 && q2 > q1) snprintf(cur->name, sizeof cur->name, "%.*s", (int)(q2 - q1 - 1), q1 + 1);
            else snprintf(cur->name, sizeof cur->name, "Test %d", cur->id);
            continue;
        }
        if (!strcasecmp(tok[0], "TEST_END")) { cur = NULL; continue; }
        if (!cur) continue;
        if (!strcasecmp(tok[0], "CONFIG")) {
            if (config_apply(&cur->cfg, tok + 1, n - 1, err, sizeof err))
                fprintf(stderr, "linea %d: %s\n", lineno, err);
            continue;
        }
        if (strcasecmp(tok[0], "EVENT") || n < 2) continue;
        const char *ev = tok[1];
        int i = 2;
        if (!strcasecmp(ev, "MOVE")) {
            long long id = 0;
            int pid, from, to;
            bool is_puma = !strcasecmp(tok[i], "PUMA");
            i++;
            if (i >= n || parse_ll(tok[i], &id)) { fprintf(stderr, "linea %d: MOVE invalido\n", lineno); continue; }
            i++;
            if (is_puma) pid = PUMA;
            else if (id >= 0 && id < NDOGS) pid = DOG_P((int)id);
            else { fprintf(stderr, "linea %d: id de perro fuera de rango\n", lineno); continue; }
            if (i >= n || strcasecmp(tok[i], "FROM")) { fprintf(stderr, "linea %d: falta FROM\n", lineno); continue; }
            i++;
            if (parse_cell(tok, &i, n, &from) || i >= n || strcasecmp(tok[i], "TO")) {
                fprintf(stderr, "linea %d: FROM/TO invalido\n", lineno); continue;
            }
            i++;
            if (parse_cell(tok, &i, n, &to)) { fprintf(stderr, "linea %d: TO invalido\n", lineno); continue; }
            Action a = { from == to ? A_PLACE : A_MOVE, from, to, -1, -1 };
            if (i < n && !strcasecmp(tok[i], "CAPTURE")) {          /* CAPTURE DOG k AT x y */
                long long dk;
                i++;
                if (i < n && !strcasecmp(tok[i], "DOG")) i++;
                if (i < n && !parse_ll(tok[i], &dk) && dk >= 0 && dk < NDOGS) {
                    i++;
                    if (i < n && !strcasecmp(tok[i], "AT")) i++;
                    int at;
                    if (!parse_cell(tok, &i, n, &at)) { a.cap_dog = (int)dk; a.cap_at = at; }
                }
            }
            script_add(&cur->sc[pid], a);
        } else if (!strcasecmp(ev, "DISCONNECT")) {
            int pid = -1;
            if (i < n && !strcasecmp(tok[i], "PLAYER_PUMA")) pid = PUMA;
            else if (i < n && !strcasecmp(tok[i], "PLAYER_DOG")) {
                long long id = 0;
                i++;
                if (i < n && !strcasecmp(tok[i], "ID")) i++;
                if (i < n && !parse_ll(tok[i], &id) && id >= 0 && id < NDOGS) pid = DOG_P((int)id);
            }
            if (pid < 0) { fprintf(stderr, "linea %d: DISCONNECT invalido\n", lineno); continue; }
            Action a = { A_DISCONNECT, -1, -1, -1, -1 };
            script_add(&cur->sc[pid], a);
        } else if (!strcasecmp(ev, "SPAWN_PLAYERS")) {
            long long c = 0, m = 0;
            for (; i + 1 < n; i += 2) {
                if (!strcasecmp(tok[i], "COUNT")) parse_ll(tok[i + 1], &c);
                else if (!strcasecmp(tok[i], "MESAS_MAX")) parse_ll(tok[i + 1], &m);
            }
            cur->spawn_n = (int)c; cur->spawn_m = m > 0 ? (int)m : 1;
        } else if (!strcasecmp(ev, "WAIT_QUEUED_PLAYERS")) cur->wait_q = 1;
        else if (!strcasecmp(ev, "SIGNAL_SIGINT")) cur->sigint = 1;
        else if (!strcasecmp(ev, "BURST_RANDOM_MOVES")) {
            long long c = 0;
            if (i + 1 < n && !strcasecmp(tok[i], "COUNT")) parse_ll(tok[i + 1], &c);
            cur->burst_n = (int)c;
        } else fprintf(stderr, "linea %d: evento desconocido %s\n", lineno, ev);
    }
    if (declared >= 0 && declared != nt)
        fprintf(stderr, "aviso: SUITE_START declara %d pruebas pero se encontraron %d\n", declared, nt);

    int passed = 0;
    int *results = calloc((size_t)(nt ? nt : 1), sizeof *results);
    for (int t = 0; t < nt; t++) {
        TestCase *tc = &tests[t];
        printf("\n================================================================\n");
        printf("TEST %d: %s\n", tc->id, tc->name);
        printf("  config: seed=%llu delay=%d..%d puma_init=", (unsigned long long)tc->cfg.seed, tc->cfg.dmin, tc->cfg.dmax);
        { char c[16]; cell_str(tc->cfg.puma_cell, c, sizeof c); printf("%s\n", c); }
        printf("================================================================\n");
        fflush(stdout);
        int ok = 0, has_script = 0;
        for (int p = 0; p < NPIECES; p++) if (tc->sc[p].n) has_script = 1;
        if (tc->spawn_n > 0) ok = run_spawn(tc);
        else if (tc->burst_n > 0) ok = run_burst(tc);
        else if (has_script) ok = run_scenario(tc);
        else { printf("  (prueba sin eventos ejecutables)\n"); ok = 1; }
        results[t] = ok;
        if (ok) passed++;
        printf(">>> TEST %d %s\n", tc->id, ok ? "OK" : "FALLO");
        fflush(stdout);
    }
    printf("\n================ RESUMEN DE LA SUITE ================\n");
    for (int t = 0; t < nt; t++) printf("  Test %2d  %-4s  %s\n", tests[t].id, results[t] ? "OK" : "FALLO", tests[t].name);
    printf("  Total: %d/%d pruebas OK\n", passed, nt);
    free(results);
    free_tests(tests, nt);
    return passed == nt ? 0 : 1;
}

/*  main                                                                     */

static void usage(const char *prog) {
    fprintf(stderr, "Uso: %s N M [config.txt]\n"
                    "  N: numero total de jugadores/clientes (>= 0)\n"
                    "  M: maximo de mesas activas simultaneas (>= 1)\n"
                    "  config.txt: opcional (parametros o suite de pruebas)\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) { usage(argv[0]); return 2; }
    long long N, M;
    if (parse_ll(argv[1], &N) || parse_ll(argv[2], &M) || N < 0 || M < 1 || N > 100000 || M > 100000) {
        usage(argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_t0 = now_ms();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    Config cfg;
    config_default(&cfg);
    int rc = 0;
    char *text = NULL;
    if (argc == 4) {
        text = read_file(argv[3]);
        if (!text) { fprintf(stderr, "error: no se pudo leer '%s': %s\n", argv[3], strerror(errno)); return 2; }
        if (text_is_suite(text)) {
            printf("Modo SUITE: se ejecutan las pruebas de '%s' (N y M se toman de cada prueba).\n", argv[3]);
            rc = run_suite(text);
            free(text);
            return rc;
        }
        if (config_load_text(&cfg, text)) { free(text); return 2; }
        free(text);
    }
    build_tables(cfg.diag);
    if (cfg.log_file[0]) {
        g_log_fp = fopen(cfg.log_file, "w");
        if (!g_log_fp) fprintf(stderr, "aviso: no se pudo abrir el log '%s'\n", cfg.log_file);
    }

    Tournament *T = tournament_start((int)N, (int)M, &cfg);
    if (!T) { if (g_log_fp) fclose(g_log_fp); return 1; }
    TStats st;
    tournament_join(T, &st);
    if (st.n_inv_bad) rc = 1;
    if (g_log_fp) { fclose(g_log_fp); g_log_fp = NULL; }
    return rc;
}
