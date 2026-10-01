/*
 * Prueba del núcleo ISAM SIN PostgreSQL.
 * Compara cada resultado contra una búsqueda de fuerza bruta sobre un
 * arreglo de referencia. Cubre: construcción, igualdad (existente e
 * inexistente), rango, inserción (página y overflow), claves repetidas,
 * vacuum, y reabrir los archivos desde disco.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/isam_core.h"

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FALLO (%s:%d): %s\n", __FILE__, __LINE__, msg); exit(1); } } while (0)

typedef struct { IsamEntrada *v; size_t n, cap; } Lista;

static void lista_add(Lista *l, const IsamEntrada *e) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 1024;
        l->v = realloc(l->v, l->cap * sizeof(IsamEntrada));
        CHECK(l->v, "realloc");
    }
    l->v[l->n++] = *e;
}

static void emitir(const IsamEntrada *e, void *ctx) { lista_add(ctx, e); }

static int cmp(const void *a, const void *b) {
    const IsamEntrada *x = a, *y = b;
    if (x->clave != y->clave) return x->clave < y->clave ? -1 : 1;
    if (x->bloque != y->bloque) return x->bloque < y->bloque ? -1 : 1;
    return 0;
}

/* referencia */
static IsamEntrada *ref; static char *viva; static size_t nref, capref;

static void ref_add(IsamEntrada e) {
    if (nref == capref) {
        capref = capref ? capref * 2 : 4096;
        ref = realloc(ref, capref * sizeof *ref);
        viva = realloc(viva, capref);
    }
    ref[nref] = e; viva[nref] = 1; nref++;
}

static void verificar(IsamHandle *h, int lo, int hi) {
    Lista got = {0}, want = {0};
    CHECK(isam_rango(h, lo, hi, emitir, &got) == 0, "isam_rango");
    for (size_t i = 0; i < nref; i++)
        if (viva[i] && ref[i].clave >= lo && ref[i].clave <= hi) lista_add(&want, &ref[i]);
    qsort(got.v, got.n, sizeof(IsamEntrada), cmp);
    qsort(want.v, want.n, sizeof(IsamEntrada), cmp);
    if (got.n != want.n) {
        fprintf(stderr, "rango [%d,%d]: obtuvo %zu, esperaba %zu\n", lo, hi, got.n, want.n);
        exit(1);
    }
    for (size_t i = 0; i < got.n; i++)
        CHECK(got.v[i].clave == want.v[i].clave && got.v[i].bloque == want.v[i].bloque,
              "contenido distinto");
    free(got.v); free(want.v);
}

static void verificar_todo(IsamHandle *h, int maxclave) {
    for (int i = 0; i < 300; i++) { int k = rand() % maxclave; verificar(h, k, k); }       /* igualdad */
    for (int i = 0; i < 100; i++) verificar(h, -5 - i, -1);                              /* ausentes */
    for (int i = 0; i < 100; i++) { int a = rand() % maxclave, b = a + rand() % 3000; verificar(h, a, b); }
    verificar(h, -1000000, maxclave * 2);                                               /* todo */
}

static bool muerta(const IsamEntrada *e, void *ctx) { (void)ctx; return e->bloque % 3 == 0; }

static void escenario(int fill, int n_inicial, int n_insert, int maxclave) {
    char dir[] = "/tmp/isam_test_XXXXXX";
    CHECK(mkdtemp(dir), "mkdtemp");
    nref = 0;
    unsigned id = 1;

    IsamEntrada *e = malloc((size_t)n_inicial * sizeof *e);
    for (int i = 0; i < n_inicial; i++) {
        IsamEntrada x = { rand() % maxclave, id++, 1, 0 };   /* claves con repetidas */
        e[i] = x; ref_add(x);
    }
    CHECK(isam_construir(dir, e, (size_t)n_inicial, fill) == 0, "construir");
    free(e);

    IsamHandle *h = isam_abrir(dir);
    CHECK(h, "abrir");
    verificar_todo(h, maxclave);

    for (int i = 0; i < n_insert; i++) {
        IsamEntrada x = { rand() % maxclave, id++, 1, 0 };
        CHECK(isam_insertar(h, &x) == 0, "insertar");
        ref_add(x);
    }
    /* claves menores que todas las existentes (van a la página 0) */
    for (int i = 0; i < 50; i++) {
        IsamEntrada x = { -(rand() % 1000) - 1, id++, 1, 0 };
        CHECK(isam_insertar(h, &x) == 0, "insertar menor");
        ref_add(x);
    }
    verificar_todo(h, maxclave);

    IsamEstadisticas st;
    CHECK(isam_estadisticas(h, &st) == 0, "estadisticas");
    printf("  fill=%3d%%  paginas=%ld  en_paginas=%ld  paginas_con_overflow=%ld  nodos_overflow=%ld\n",
           fill, st.paginas, st.entradas_en_paginas, st.paginas_con_overflow, st.nodos_overflow);

    /* reabrir desde disco */
    isam_cerrar(h);
    h = isam_abrir(dir);
    CHECK(h, "reabrir");
    verificar_todo(h, maxclave);

    /* vacuum */
    long elim, rest;
    CHECK(isam_vacuum(h, muerta, NULL, &elim, &rest) == 0, "vacuum");
    for (size_t i = 0; i < nref; i++) if (ref[i].bloque % 3 == 0) viva[i] = 0;
    CHECK((size_t)(elim + rest) == nref, "conteo vacuum");
    verificar_todo(h, maxclave);

    isam_cerrar(h);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    CHECK(system(cmd) == 0, "limpiar");
}

int main(void) {
    srand(12345);
    printf("Escenario 1: 70%% de llenado (inserciones caben en las páginas)\n");
    escenario(70, 20000, 3000, 100000);
    printf("Escenario 2: 100%% de llenado (todo va a overflow)\n");
    escenario(100, 20000, 3000, 100000);
    printf("Escenario 3: muchas claves repetidas (cruzan límites de página)\n");
    escenario(70, 20000, 3000, 50);
    printf("Escenario 4: tabla vacía al construir (todo a la página 0)\n");
    escenario(70, 0, 1500, 100000);
    printf("OK: todas las pruebas pasaron\n");
    return 0;
}
