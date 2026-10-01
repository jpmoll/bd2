#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "isam_core.h"

/* ================== FORMATOS EN DISCO ================== */

typedef struct {
    int32_t n;              /* cuántas entradas hay en la página */
    int32_t ptr_overflow;   /* nodo cabeza de su cadena de overflow, o -1 */
} CabeceraPagina;

#define CAP_PAGINA ((ISAM_TAM_PAGINA - sizeof(CabeceraPagina)) / sizeof(IsamEntrada))

typedef struct {
    CabeceraPagina cab;
    IsamEntrada    ent[CAP_PAGINA];
    unsigned char  relleno[ISAM_TAM_PAGINA - sizeof(CabeceraPagina)
                           - CAP_PAGINA * sizeof(IsamEntrada)];
} Pagina;

_Static_assert(sizeof(Pagina) == ISAM_TAM_PAGINA, "Pagina debe medir TAM_PAGINA");

typedef struct {
    IsamEntrada e;
    int32_t     siguiente;  /* posición del siguiente nodo, o -1 */
    int32_t     activo;     /* 0 = eliminado lógicamente */
} NodoOverflow;

struct IsamHandle {
    int      fd_datos;
    int      fd_overflow;
    int32_t  np;            /* número de páginas */
    int32_t *clave_min;     /* índice disperso en memoria: clave_min[i] de la página i */
    long     accesos;
};

#define ARCH_DATOS    "datos.dat"
#define ARCH_INDICE   "indice.dat"
#define ARCH_OVERFLOW "overflow.dat"

static void ruta(char *buf, size_t len, const char *dir, const char *nombre) {
    snprintf(buf, len, "%s/%s", dir, nombre);
}

/* ================== DATOS: E/S de páginas ================== */
/* pread/pwrite sin buffer de stdio: varios procesos (backends) pueden
 * usar los mismos archivos y siempre ven lo último que se escribió. */

static bool leer_pagina(IsamHandle *h, int32_t b, Pagina *p) {
    h->accesos++;
    return pread(h->fd_datos, p, ISAM_TAM_PAGINA, (off_t)b * ISAM_TAM_PAGINA)
           == (ssize_t)ISAM_TAM_PAGINA;
}

static bool escribir_pagina(IsamHandle *h, int32_t b, const Pagina *p) {
    h->accesos++;
    return pwrite(h->fd_datos, p, ISAM_TAM_PAGINA, (off_t)b * ISAM_TAM_PAGINA)
           == (ssize_t)ISAM_TAM_PAGINA;
}

/* ================== OVERFLOW: E/S de nodos ================== */

static bool leer_nodo(IsamHandle *h, int32_t pos, NodoOverflow *n) {
    h->accesos++;
    return pread(h->fd_overflow, n, sizeof(*n), (off_t)pos * sizeof(*n))
           == (ssize_t)sizeof(*n);
}

static bool escribir_nodo(IsamHandle *h, int32_t pos, const NodoOverflow *n) {
    h->accesos++;
    return pwrite(h->fd_overflow, n, sizeof(*n), (off_t)pos * sizeof(*n))
           == (ssize_t)sizeof(*n);
}

/* ================== ÍNDICE: búsqueda binaria sobre clave_min ================== */

/* Última página i con clave_min[i] <= clave (o < clave si `estricto`).
 * Si ninguna cumple, página 0. */
static int32_t ubicar_pagina(const IsamHandle *h, int32_t clave, bool estricto) {
    int32_t lo = 0, hi = h->np;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        bool cumple = estricto ? (h->clave_min[mid] < clave)
                               : (h->clave_min[mid] <= clave);
        if (cumple) lo = mid + 1; else hi = mid;
    }
    return lo > 0 ? lo - 1 : 0;
}

/* ================== CONSTRUCCIÓN ================== */

static int cmp_entrada(const void *a, const void *b) {
    const IsamEntrada *x = a, *y = b;
    if (x->clave != y->clave) return x->clave < y->clave ? -1 : 1;
    if (x->bloque != y->bloque) return x->bloque < y->bloque ? -1 : 1;
    if (x->offset != y->offset) return x->offset < y->offset ? -1 : 1;
    return 0;
}

int isam_construir(const char *dir, IsamEntrada *e, size_t n, int fill_pct) {
    char path[1024];
    int fd_d = -1, fd_i = -1, fd_o = -1, rc = -1;
    int32_t *cm = NULL;

    if (fill_pct < 10)  fill_pct = 10;
    if (fill_pct > 100) fill_pct = 100;
    size_t por_pagina = CAP_PAGINA * (size_t)fill_pct / 100;
    if (por_pagina < 1) por_pagina = 1;

    qsort(e, n, sizeof(IsamEntrada), cmp_entrada);

    size_t np = (n == 0) ? 1 : (n + por_pagina - 1) / por_pagina;
    cm = malloc(np * sizeof(int32_t));
    if (!cm) goto fin;

    ruta(path, sizeof path, dir, ARCH_DATOS);
    fd_d = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ruta(path, sizeof path, dir, ARCH_OVERFLOW);
    fd_o = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ruta(path, sizeof path, dir, ARCH_INDICE);
    fd_i = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd_d < 0 || fd_o < 0 || fd_i < 0) goto fin;

    Pagina p;
    for (size_t i = 0; i < np; i++) {
        size_t ini = i * por_pagina;
        size_t cnt = (n > ini) ? (n - ini < por_pagina ? n - ini : por_pagina) : 0;
        memset(&p, 0, sizeof p);
        p.cab.n = (int32_t)cnt;
        p.cab.ptr_overflow = -1;
        if (cnt) memcpy(p.ent, &e[ini], cnt * sizeof(IsamEntrada));
        cm[i] = cnt ? e[ini].clave : INT32_MIN;
        if (pwrite(fd_d, &p, sizeof p, (off_t)i * sizeof p) != (ssize_t)sizeof p) goto fin;
    }

    int32_t np32 = (int32_t)np;
    if (write(fd_i, &np32, sizeof np32) != (ssize_t)sizeof np32) goto fin;
    if (write(fd_i, cm, np * sizeof(int32_t)) != (ssize_t)(np * sizeof(int32_t))) goto fin;
    rc = 0;

fin:
    if (fd_d >= 0) close(fd_d);
    if (fd_o >= 0) close(fd_o);
    if (fd_i >= 0) close(fd_i);
    free(cm);
    return rc;
}

/* ================== ABRIR / CERRAR ================== */

IsamHandle *isam_abrir(const char *dir) {
    char path[1024];
    int fd_i = -1;
    IsamHandle *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->fd_datos = h->fd_overflow = -1;

    ruta(path, sizeof path, dir, ARCH_DATOS);
    h->fd_datos = open(path, O_RDWR);
    ruta(path, sizeof path, dir, ARCH_OVERFLOW);
    h->fd_overflow = open(path, O_RDWR);
    ruta(path, sizeof path, dir, ARCH_INDICE);
    fd_i = open(path, O_RDONLY);
    if (h->fd_datos < 0 || h->fd_overflow < 0 || fd_i < 0) goto error;

    int32_t np;
    if (read(fd_i, &np, sizeof np) != (ssize_t)sizeof np || np <= 0) goto error;
    h->clave_min = malloc((size_t)np * sizeof(int32_t));
    if (!h->clave_min) goto error;
    if (read(fd_i, h->clave_min, (size_t)np * sizeof(int32_t))
        != (ssize_t)((size_t)np * sizeof(int32_t))) goto error;
    h->np = np;
    close(fd_i);
    return h;

error:
    if (fd_i >= 0) close(fd_i);
    isam_cerrar(h);
    return NULL;
}

void isam_cerrar(IsamHandle *h) {
    if (!h) return;
    if (h->fd_datos >= 0)    close(h->fd_datos);
    if (h->fd_overflow >= 0) close(h->fd_overflow);
    free(h->clave_min);
    free(h);
}

/* ================== INSERCIÓN ================== */

int isam_insertar(IsamHandle *h, const IsamEntrada *e) {
    int32_t b = ubicar_pagina(h, e->clave, false);   /* índice -> página destino */
    Pagina p;
    if (!leer_pagina(h, b, &p)) return -1;

    if ((size_t)p.cab.n < CAP_PAGINA) {
        /* Hay espacio: insertar ordenado (corriendo a los mayores). */
        int i = p.cab.n;
        while (i > 0 && p.ent[i - 1].clave > e->clave) {
            p.ent[i] = p.ent[i - 1];
            i--;
        }
        p.ent[i] = *e;
        p.cab.n++;
        return escribir_pagina(h, b, &p) ? 0 : -1;
    }

    /* Página llena: nodo nuevo al final de overflow.dat, como nueva cabeza. */
    struct stat st;
    if (fstat(h->fd_overflow, &st) != 0) return -1;
    int32_t pos = (int32_t)(st.st_size / (off_t)sizeof(NodoOverflow));

    NodoOverflow nodo;
    memset(&nodo, 0, sizeof nodo);
    nodo.e = *e;
    nodo.siguiente = p.cab.ptr_overflow;
    nodo.activo = 1;
    if (!escribir_nodo(h, pos, &nodo)) return -1;

    p.cab.ptr_overflow = pos;
    return escribir_pagina(h, b, &p) ? 0 : -1;
}

/* ================== BÚSQUEDA (igualdad y rango) ================== */

int isam_rango(IsamHandle *h, int32_t lo, int32_t hi, IsamEmitir f, void *ctx) {
    if (lo > hi) return 0;

    /* Primera página que PUEDE tener `lo`: la última con clave_min < lo
     * (estricto, porque claves repetidas pueden empezar en la página anterior). */
    int32_t s = ubicar_pagina(h, lo, true);
    Pagina p;

    for (int32_t b = s; b < h->np; b++) {
        /* El área de datos está ordenada: si esta página empieza después
         * de `hi`, ninguna de las siguientes sirve. */
        if (b > s && h->clave_min[b] > hi) break;

        if (!leer_pagina(h, b, &p)) return -1;
        for (int i = 0; i < p.cab.n; i++) {
            if (p.ent[i].clave > hi) break;
            if (p.ent[i].clave >= lo) f(&p.ent[i], ctx);
        }

        /* La cadena de overflow NO está ordenada: se recorre completa. */
        int32_t pos = p.cab.ptr_overflow;
        while (pos != -1) {
            NodoOverflow nd;
            if (!leer_nodo(h, pos, &nd)) return -1;
            if (nd.activo && nd.e.clave >= lo && nd.e.clave <= hi) f(&nd.e, ctx);
            pos = nd.siguiente;
        }
    }
    return 0;
}

/* ================== VACUUM ================== */

int isam_vacuum(IsamHandle *h, IsamEsMuerta f, void *ctx, long *elim, long *rest) {
    long e = 0, r = 0;
    Pagina p;

    for (int32_t b = 0; b < h->np; b++) {
        if (!leer_pagina(h, b, &p)) return -1;

        int j = 0;
        for (int i = 0; i < p.cab.n; i++) {
            if (f(&p.ent[i], ctx)) e++;
            else { p.ent[j++] = p.ent[i]; r++; }
        }
        if (j != p.cab.n) {
            p.cab.n = j;
            if (!escribir_pagina(h, b, &p)) return -1;
        }

        int32_t pos = p.cab.ptr_overflow;
        while (pos != -1) {
            NodoOverflow nd;
            if (!leer_nodo(h, pos, &nd)) return -1;
            if (nd.activo) {
                if (f(&nd.e, ctx)) {
                    nd.activo = 0;
                    if (!escribir_nodo(h, pos, &nd)) return -1;
                    e++;
                } else r++;
            }
            pos = nd.siguiente;
        }
    }
    if (elim) *elim = e;
    if (rest) *rest = r;
    return 0;
}

/* ================== ESTADÍSTICAS ================== */

int isam_estadisticas(IsamHandle *h, IsamEstadisticas *out) {
    struct stat st;
    Pagina p;
    memset(out, 0, sizeof *out);
    out->paginas = h->np;
    out->capacidad_pagina = (long)CAP_PAGINA;
    for (int32_t b = 0; b < h->np; b++) {
        if (!leer_pagina(h, b, &p)) return -1;
        out->entradas_en_paginas += p.cab.n;
        if (p.cab.ptr_overflow != -1) out->paginas_con_overflow++;
    }
    if (fstat(h->fd_overflow, &st) != 0) return -1;
    out->nodos_overflow = (long)(st.st_size / (off_t)sizeof(NodoOverflow));
    out->bytes_archivos = (long)st.st_size
                        + (long)h->np * ISAM_TAM_PAGINA          /* datos.dat */
                        + (long)(h->np + 1) * (long)sizeof(int32_t); /* indice.dat */
    return 0;
}

long isam_accesos(IsamHandle *h) { return h->accesos; }
