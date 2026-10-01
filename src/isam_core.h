#ifndef ISAM_CORE_H
#define ISAM_CORE_H

/*
 * NÚCLEO ISAM (C puro, sin dependencias de PostgreSQL).
 *
 * Tres archivos .dat por índice, dentro de una carpeta:
 *   datos.dat     área primaria: páginas de 4096 bytes, ordenadas por clave
 *   indice.dat    índice disperso: la clave mínima de cada página
 *   overflow.dat  nodos encadenados para lo que no cabe en su página
 *
 * Cada entrada guarda (clave, TID del heap de PostgreSQL), NO la fila.
 * PostgreSQL ya guarda la fila en su heap; el índice solo dice dónde está.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ISAM_TAM_PAGINA 4096

typedef struct {
    int32_t  clave;
    uint32_t bloque;    /* ItemPointer: número de bloque del heap */
    uint16_t offset;    /* ItemPointer: número de ítem dentro del bloque */
    uint16_t _pad;
} IsamEntrada;          /* 12 bytes */

typedef struct IsamHandle IsamHandle;

typedef void (*IsamEmitir)(const IsamEntrada *e, void *ctx);
typedef bool (*IsamEsMuerta)(const IsamEntrada *e, void *ctx);

typedef struct {
    long paginas;
    long entradas_en_paginas;
    long paginas_con_overflow;
    long nodos_overflow;
    long capacidad_pagina;
    long bytes_archivos;     /* datos.dat + overflow.dat + indice.dat (aprox.) */
} IsamEstadisticas;

/* CONSTRUCCIÓN: ordena `e` (in situ) y escribe los 3 archivos en `dir`
 * (la carpeta ya debe existir). `fill_pct` = % de cada página que se llena
 * al construir; el resto queda libre para inserciones sin overflow. */
int  isam_construir(const char *dir, IsamEntrada *e, size_t n, int fill_pct);

IsamHandle *isam_abrir(const char *dir);
void        isam_cerrar(IsamHandle *h);

/* INSERCIÓN: página si hay espacio, si no overflow. 0 = ok, -1 = error. */
int  isam_insertar(IsamHandle *h, const IsamEntrada *e);

/* BÚSQUEDA por rango inclusivo [lo, hi]; igualdad = (k, k).
 * Llama a `f` por cada coincidencia. 0 = ok, -1 = error de E/S. */
int  isam_rango(IsamHandle *h, int32_t lo, int32_t hi, IsamEmitir f, void *ctx);

/* VACUUM: elimina las entradas para las que `f` devuelve true. */
int  isam_vacuum(IsamHandle *h, IsamEsMuerta f, void *ctx,
                 long *eliminadas, long *restantes);

int  isam_estadisticas(IsamHandle *h, IsamEstadisticas *out);
long isam_accesos(IsamHandle *h);   /* lecturas+escrituras de página/nodo */

#endif
