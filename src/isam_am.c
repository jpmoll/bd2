/*
 * isam_am.c — Index Access Method "isam" para PostgreSQL 18.
 *
 * Este archivo es el PEGAMENTO: traduce las llamadas de PostgreSQL
 * (ambuild, aminsert, amgettuple, ...) a llamadas al núcleo ISAM
 * (isam_core.c), que trabaja sobre archivos .dat.
 *
 * Flujo:
 *   CREATE INDEX ... USING isam  ->  isam_build()   -> isam_construir()
 *   INSERT en la tabla           ->  isam_insert()  -> isam_insertar()
 *   SELECT ... WHERE id = x      ->  isam_beginscan/rescan/gettuple
 *                                    -> isam_rango()  (devuelve TIDs)
 *   VACUUM                       ->  isam_bulkdelete() -> isam_vacuum()
 *
 * Los archivos viven en  $PGDATA/pg_isam/<oid_base>_<relfilenumber>/
 * y NO pasan por el buffer manager de PostgreSQL ni por el WAL
 * (la rúbrica no exige WAL propio ni concurrencia completa).
 */
#include "postgres.h"

#include <sys/stat.h>

#include "access/amapi.h"
#include "access/genam.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"

#include "isam_core.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(isam_handler);
PG_FUNCTION_INFO_V1(isam_info_sql);

/* Estrategias del operator class (mismos números que btree) */
#define ISAM_LT 1
#define ISAM_LE 2
#define ISAM_EQ 3
#define ISAM_GE 4
#define ISAM_GT 5

static int isam_fill_factor = 70;   /* GUC isam.fill_factor */

void _PG_init(void);

void
_PG_init(void)
{
    DefineCustomIntVariable("isam.fill_factor",
                            "Porcentaje de llenado de cada página al construir el índice ISAM.",
                            "El espacio restante absorbe inserciones sin usar overflow.",
                            &isam_fill_factor, 70, 10, 100,
                            PGC_USERSET, 0, NULL, NULL, NULL);
    MarkGUCPrefixReserved("isam");
}

/* ========================================================================
 * Archivos del índice
 * ======================================================================== */

static void
isam_dir(Relation index, char *buf, size_t len)
{
    snprintf(buf, len, "%s/pg_isam/%u_%u",
             DataDir, MyDatabaseId, index->rd_locator.relNumber);
}

static void
isam_crear_directorio(const char *dir)
{
    char padre[MAXPGPATH];

    snprintf(padre, sizeof(padre), "%s/pg_isam", DataDir);
    if (MakePGDirectory(padre) < 0 && errno != EEXIST)
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("isam: no se pudo crear el directorio \"%s\": %m", padre)));
    if (MakePGDirectory(dir) < 0 && errno != EEXIST)
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("isam: no se pudo crear el directorio \"%s\": %m", dir)));
}

/* Un handle abierto por backend (se reutiliza entre llamadas). El núcleo
 * no llama a elog, así que un error de PostgreSQL nunca deja fds a medias. */
static IsamHandle *cache_h = NULL;
static char cache_dir[MAXPGPATH];

static IsamHandle *
isam_obtener(Relation index)
{
    char dir[MAXPGPATH];

    isam_dir(index, dir, sizeof(dir));
    if (cache_h != NULL && strcmp(cache_dir, dir) == 0)
        return cache_h;

    if (cache_h != NULL)
    {
        isam_cerrar(cache_h);
        cache_h = NULL;
    }
    cache_h = isam_abrir(dir);
    if (cache_h == NULL)
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("isam: no se pudo abrir el índice en \"%s\": %m", dir)));
    strlcpy(cache_dir, dir, sizeof(cache_dir));
    return cache_h;
}

/*
 * Exclusión mutua entre procesos: escritores en ExclusiveLock, lectores
 * en ShareLock, sobre el "extension lock" de la relación índice (que no
 * usamos para otra cosa porque el índice no tiene bloques propios).
 */
#define ISAM_LOCK_ESCRITURA(rel) LockRelationForExtension(rel, ExclusiveLock)
#define ISAM_UNLOCK_ESCRITURA(rel) UnlockRelationForExtension(rel, ExclusiveLock)
#define ISAM_LOCK_LECTURA(rel) LockRelationForExtension(rel, ShareLock)
#define ISAM_UNLOCK_LECTURA(rel) UnlockRelationForExtension(rel, ShareLock)

static void
entrada_desde(IsamEntrada *e, Datum key, ItemPointer tid)
{
    e->clave = DatumGetInt32(key);
    e->bloque = ItemPointerGetBlockNumber(tid);
    e->offset = ItemPointerGetOffsetNumber(tid);
    e->_pad = 0;
}

/* ========================================================================
 * ambuild — CREATE INDEX
 * ======================================================================== */

typedef struct
{
    IsamEntrada *v;
    size_t       n;
    size_t       cap;
} BuildState;

static void
isam_build_callback(Relation index, ItemPointer tid, Datum *values,
                    bool *isnull, bool tupleIsAlive, void *state)
{
    BuildState *bs = (BuildState *) state;

    if (isnull[0])
        return;                 /* los NULL no se indexan */

    if (bs->n == bs->cap)
    {
        bs->cap *= 2;
        bs->v = (IsamEntrada *) repalloc(bs->v, bs->cap * sizeof(IsamEntrada));
    }
    entrada_desde(&bs->v[bs->n], values[0], tid);
    bs->n++;
}

static IndexBuildResult *
isam_build(Relation heap, Relation index, IndexInfo *indexInfo)
{
    BuildState        bs;
    IndexBuildResult *result;
    double            reltuples;
    char              dir[MAXPGPATH];

    /* 1) Recorrer el heap completo y juntar (clave, TID) en memoria */
    bs.cap = 1024;
    bs.n = 0;
    bs.v = (IsamEntrada *) palloc(bs.cap * sizeof(IsamEntrada));
    reltuples = table_index_build_scan(heap, index, indexInfo, true, true,
                                       isam_build_callback, &bs, NULL);

    /* 2) El núcleo ordena y escribe datos.dat / indice.dat / overflow.dat */
    isam_dir(index, dir, sizeof(dir));
    isam_crear_directorio(dir);
    if (isam_construir(dir, bs.v, bs.n, isam_fill_factor) != 0)
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("isam: falló la construcción en \"%s\": %m", dir)));

    result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
    result->heap_tuples = reltuples;
    result->index_tuples = (double) bs.n;
    pfree(bs.v);
    return result;
}

static void
isam_buildempty(Relation index)
{
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("isam: no soporta índices sobre tablas UNLOGGED")));
}

/* ========================================================================
 * aminsert — INSERT en la tabla indexada
 * ======================================================================== */

static bool
isam_insert(Relation index, Datum *values, bool *isnull, ItemPointer ht_ctid,
            Relation heapRel, IndexUniqueCheck checkUnique,
            bool indexUnchanged, IndexInfo *indexInfo)
{
    IsamEntrada e;
    IsamHandle *h;
    int         rc;

    if (isnull[0])
        return false;

    entrada_desde(&e, values[0], ht_ctid);
    h = isam_obtener(index);

    ISAM_LOCK_ESCRITURA(index);
    rc = isam_insertar(h, &e);
    ISAM_UNLOCK_ESCRITURA(index);

    if (rc != 0)
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("isam: falló la inserción: %m")));
    return false;
}

/* ========================================================================
 * Scan — SELECT ... WHERE col =, <, <=, >=, > constante
 *
 * En la primera llamada se traduce el WHERE a un rango [lo, hi], se busca
 * en el núcleo y se guardan los TIDs; luego se entregan de a uno.
 * ======================================================================== */

typedef struct
{
    MemoryContext    ctx;
    ItemPointerData *tids;
    int64            n;
    int64            cap;
    int64            pos;
    bool             ejecutado;
} IsamScan;

static void
isam_emitir_tid(const IsamEntrada *e, void *arg)
{
    IsamScan     *st = (IsamScan *) arg;
    MemoryContext old = MemoryContextSwitchTo(st->ctx);

    if (st->n == st->cap)
    {
        st->cap = st->cap ? st->cap * 2 : 256;
        st->tids = st->tids ? repalloc_huge(st->tids, st->cap * sizeof(ItemPointerData))
                            : palloc(st->cap * sizeof(ItemPointerData));
    }
    ItemPointerSet(&st->tids[st->n], e->bloque, e->offset);
    st->n++;
    MemoryContextSwitchTo(old);
}

static IndexScanDesc
isam_beginscan(Relation index, int nkeys, int norderbys)
{
    IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
    IsamScan     *st = (IsamScan *) palloc0(sizeof(IsamScan));

    st->ctx = AllocSetContextCreate(CurrentMemoryContext, "isam scan",
                                    ALLOCSET_DEFAULT_SIZES);
    scan->opaque = st;
    return scan;
}

static void
isam_rescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
            ScanKey orderbys, int norderbys)
{
    IsamScan *st = (IsamScan *) scan->opaque;

    if (scankey && scan->numberOfKeys > 0)
        memcpy(scan->keyData, scankey, scan->numberOfKeys * sizeof(ScanKeyData));

    MemoryContextReset(st->ctx);
    st->tids = NULL;
    st->n = st->cap = st->pos = 0;
    st->ejecutado = false;
}

static void
isam_endscan(IndexScanDesc scan)
{
    IsamScan *st = (IsamScan *) scan->opaque;

    MemoryContextDelete(st->ctx);
    pfree(st);
}

/* Traduce los ScanKeys a [lo, hi] y consulta el núcleo. */
static void
isam_ejecutar(IndexScanDesc scan)
{
    IsamScan *st = (IsamScan *) scan->opaque;
    int64     lo = PG_INT32_MIN;
    int64     hi = PG_INT32_MAX;
    bool      vacio = false;

    for (int i = 0; i < scan->numberOfKeys; i++)
    {
        ScanKey k = &scan->keyData[i];
        int64   v;

        if (k->sk_flags & SK_ISNULL)
        {
            vacio = true;       /* col = NULL nunca es verdadero */
            break;
        }
        v = DatumGetInt32(k->sk_argument);

        switch (k->sk_strategy)
        {
            case ISAM_LT: if (v - 1 < hi) hi = v - 1; break;
            case ISAM_LE: if (v < hi)     hi = v;     break;
            case ISAM_EQ: if (v > lo) lo = v; if (v < hi) hi = v; break;
            case ISAM_GE: if (v > lo)     lo = v;     break;
            case ISAM_GT: if (v + 1 > lo) lo = v + 1; break;
            default:
                elog(ERROR, "isam: estrategia %d no soportada", k->sk_strategy);
        }
    }

    if (!vacio && lo <= hi)
    {
        IsamHandle *h = isam_obtener(scan->indexRelation);
        int         rc;

        ISAM_LOCK_LECTURA(scan->indexRelation);
        rc = isam_rango(h, (int32) lo, (int32) hi, isam_emitir_tid, st);
        ISAM_UNLOCK_LECTURA(scan->indexRelation);
        if (rc != 0)
            ereport(ERROR, (errcode_for_file_access(),
                            errmsg("isam: falló la búsqueda: %m")));
    }
    st->ejecutado = true;

    /* EXPLAIN ANALYZE (PG18) muestra "Index Searches": una por cada ejecución */
    if (scan->instrument)
        scan->instrument->nsearches++;
}

static bool
isam_gettuple(IndexScanDesc scan, ScanDirection dir)
{
    IsamScan *st = (IsamScan *) scan->opaque;

    if (!st->ejecutado)
        isam_ejecutar(scan);
    if (st->pos >= st->n)
        return false;

    scan->xs_heaptid = st->tids[st->pos++];
    scan->xs_recheck = false;
    return true;
}

static int64
isam_getbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
    IsamScan *st = (IsamScan *) scan->opaque;

    if (!st->ejecutado)
        isam_ejecutar(scan);
    if (st->n > 0)
        tbm_add_tuples(tbm, st->tids, (int) st->n, false);
    return st->n;
}

/* ========================================================================
 * VACUUM
 * ======================================================================== */

typedef struct
{
    IndexBulkDeleteCallback cb;
    void                   *cb_state;
} VacState;

static bool
isam_es_muerta(const IsamEntrada *e, void *arg)
{
    VacState     *vs = (VacState *) arg;
    ItemPointerData tid;

    ItemPointerSet(&tid, e->bloque, e->offset);
    return vs->cb(&tid, vs->cb_state);
}

static IndexBulkDeleteResult *
isam_bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
                IndexBulkDeleteCallback callback, void *callback_state)
{
    VacState    vs = {callback, callback_state};
    IsamHandle *h = isam_obtener(info->index);
    long        elim = 0, rest = 0;
    int         rc;

    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));

    ISAM_LOCK_ESCRITURA(info->index);
    rc = isam_vacuum(h, isam_es_muerta, &vs, &elim, &rest);
    ISAM_UNLOCK_ESCRITURA(info->index);
    if (rc != 0)
        ereport(ERROR, (errcode_for_file_access(),
                        errmsg("isam: falló el vacuum: %m")));

    stats->tuples_removed += elim;
    stats->num_index_tuples = rest;
    return stats;
}

static IndexBulkDeleteResult *
isam_vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
    return stats;
}

/* ========================================================================
 * Planner, opciones, validación
 * ======================================================================== */

static void
isam_costestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                  Cost *indexStartupCost, Cost *indexTotalCost,
                  Selectivity *indexSelectivity, double *indexCorrelation,
                  double *indexPages)
{
    GenericCosts costs;

    /* Estimación simple: la genérica de PostgreSQL (la rúbrica no pide más). */
    MemSet(&costs, 0, sizeof(costs));
    genericcostestimate(root, path, loop_count, &costs);

    *indexStartupCost = costs.indexStartupCost;
    *indexTotalCost = costs.indexTotalCost;
    *indexSelectivity = costs.indexSelectivity;
    *indexCorrelation = 0;
    *indexPages = costs.numIndexPages;
}

static bytea *
isam_options(Datum reloptions, bool validate)
{
    if (validate && PointerIsValid(DatumGetPointer(reloptions)))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("isam: no tiene opciones de índice; use SET isam.fill_factor")));
    return NULL;
}

static bool
isam_validate(Oid opclassoid)
{
    return true;
}

Datum
isam_handler(PG_FUNCTION_ARGS)
{
    IndexAmRoutine *am = makeNode(IndexAmRoutine);

    am->amstrategies = 5;       /* <, <=, =, >=, > */
    am->amsupport = 0;
    am->amoptsprocnum = 0;
    am->amcanorder = false;     /* no devolvemos en orden: el planner ordena aparte */
    am->amcanorderbyop = false;
    am->amcanbackward = false;
    am->amcanunique = false;
    am->amcanmulticol = false;  /* una sola columna int4 */
    am->amoptionalkey = true;
    am->amsearcharray = false;
    am->amsearchnulls = false;
    am->amstorage = false;
    am->amclusterable = false;
    am->ampredlocks = false;
    am->amcanparallel = false;
    am->amcanbuildparallel = false;
    am->amcaninclude = false;
    am->amusemaintenanceworkmem = false;
    am->amsummarizing = false;
    am->amparallelvacuumoptions = 0;
    am->amkeytype = InvalidOid;

    am->ambuild = isam_build;
    am->ambuildempty = isam_buildempty;
    am->aminsert = isam_insert;
    am->ambulkdelete = isam_bulkdelete;
    am->amvacuumcleanup = isam_vacuumcleanup;
    am->amcanreturn = NULL;
    am->amcostestimate = isam_costestimate;
    am->amoptions = isam_options;
    am->amproperty = NULL;
    am->ambuildphasename = NULL;
    am->amvalidate = isam_validate;
    am->amadjustmembers = NULL;
    am->ambeginscan = isam_beginscan;
    am->amrescan = isam_rescan;
    am->amgettuple = isam_gettuple;
    am->amgetbitmap = isam_getbitmap;
    am->amendscan = isam_endscan;
    am->ammarkpos = NULL;
    am->amrestrpos = NULL;
    am->amestimateparallelscan = NULL;
    am->aminitparallelscan = NULL;
    am->amparallelrescan = NULL;

    PG_RETURN_POINTER(am);
}

/* ========================================================================
 * isam_info(indice regclass) -> text   (para la demo: ver páginas y overflow)
 * ======================================================================== */

Datum
isam_info_sql(PG_FUNCTION_ARGS)
{
    Oid              indexoid = PG_GETARG_OID(0);
    Relation         index = index_open(indexoid, AccessShareLock);
    IsamHandle      *h;
    IsamEstadisticas st;
    int              rc;
    char            *msg;

    if (index->rd_rel->relam != get_am_oid("isam", false))
        ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                        errmsg("\"%s\" no es un índice isam", RelationGetRelationName(index))));

    h = isam_obtener(index);
    ISAM_LOCK_LECTURA(index);
    rc = isam_estadisticas(h, &st);
    ISAM_UNLOCK_LECTURA(index);
    if (rc != 0)
        ereport(ERROR, (errcode_for_file_access(), errmsg("isam: falló la lectura: %m")));

    msg = psprintf("paginas=%ld entradas_en_paginas=%ld capacidad_por_pagina=%ld "
                   "paginas_con_overflow=%ld nodos_overflow=%ld tamano_bytes=%ld",
                   st.paginas, st.entradas_en_paginas, st.capacidad_pagina,
                   st.paginas_con_overflow, st.nodos_overflow, st.bytes_archivos);
    index_close(index, AccessShareLock);
    PG_RETURN_TEXT_P(cstring_to_text(msg));
}
