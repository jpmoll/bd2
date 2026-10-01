# ISAM como Index Access Method de PostgreSQL 18 — Proyecto BDII (Etapa 1)

Índice **ISAM** (Indexed Sequential Access Method) implementado en C y conectado
a PostgreSQL como un *index access method* real: se crea con
`CREATE INDEX ... USING isam` y el planner/executor de PostgreSQL lo usa en las
consultas. El índice se guarda en archivos `.dat` propios.

Alcance de esta etapa (rúbrica §5): **construcción, búsqueda (igualdad y rango),
inserción con overflow**, integración con PostgreSQL y primera comparación con B-tree.
Sin nodos / distribución (eso es la Etapa 2).

## Reproducir desde cero

Requisitos: Docker.

```bash
docker compose up -d --build          # compila la extensión dentro de postgres:18.6
docker compose exec db psql -U postgres -f /scripts/test_pg.sql      # corrección: índice = recorrido secuencial
docker compose exec db psql -U postgres -f /scripts/demo.sql         # demo paso a paso
docker compose exec db psql -U postgres -v n=100000 -v q=1000 -v reps=5 -f /scripts/comparacion.sql
docker compose down                   # entorno limpio (no hay volumen)
```

Probado compilando contra PostgreSQL 18.6 (fuente oficial, con `--enable-cassert`); el
`Dockerfile` no se pudo ejecutar en el entorno donde se desarrolló, así que el primer
`docker compose up --build` es la prueba real de ese archivo (si el tag `postgres:18.6`
no existiera, cambiar `PG_VERSION` en el `Dockerfile`).

El `docker build` ejecuta además `make test-core`: pruebas del núcleo ISAM
**sin PostgreSQL** (compara contra fuerza bruta). También se puede correr local:
`make test-core`.

Compilación sin Docker (necesita `postgresql-server-dev-18`):
`make && sudo make install` y luego `CREATE EXTENSION isam;`.

## Uso

```sql
CREATE EXTENSION isam;
CREATE INDEX coordenadas_isam ON coordenadas USING isam (id_coordenada);  -- columna INTEGER

SELECT * FROM coordenadas WHERE id_coordenada = 7;                -- igualdad
SELECT * FROM coordenadas WHERE id_coordenada BETWEEN 100 AND 200; -- rango
SELECT isam_info('coordenadas_isam');                              -- páginas / overflow / tamaño

SET isam.fill_factor = 70;   -- % de llenado de cada página al construir (10–100)
```

## Cómo está organizado (y el flujo PostgreSQL → estructura)

```
src/isam_am.c     PEGAMENTO con PostgreSQL (handler, ambuild, aminsert, scan, vacuum)
src/isam_core.c   NÚCLEO ISAM en C puro (datos / índice / overflow) — sin #include de PostgreSQL
sql/isam--1.0.sql CREATE ACCESS METHOD + operator class (<, <=, =, >=, >) para int4
test/test_core.c  pruebas del núcleo contra fuerza bruta
scripts/          demo.sql, comparacion.sql
```

| Operación SQL | Función de PostgreSQL (`isam_am.c`) | Función del núcleo (`isam_core.c`) |
|---|---|---|
| `CREATE INDEX` | `isam_build` (recorre el heap con `table_index_build_scan`) | `isam_construir` |
| `INSERT` | `isam_insert` | `isam_insertar` |
| `WHERE id = x` / rangos | `isam_beginscan` → `isam_rescan` → `isam_gettuple` / `isam_getbitmap` | `isam_rango` |
| `VACUUM` | `isam_bulkdelete` | `isam_vacuum` |

**Diferencia clave respecto al esqueleto original:** un index AM no guarda la fila
completa. Guarda `(clave, TID)`, donde el TID (bloque, offset) es la dirección de la
fila en el heap de PostgreSQL. Por eso ya no existen `Registro`, `bus.c` ni
`coordenada.c`: PostgreSQL conserva la fila y el ISAM solo la localiza.

### Estructura en disco
`$PGDATA/pg_isam/<oid_db>_<relfilenumber>/` contiene tres archivos:

- **`datos.dat`** — área primaria: páginas de 4096 bytes (340 entradas de 12 bytes +
  cabecera con cantidad y puntero a overflow), ordenadas por clave entre y dentro
  de páginas. Se llena al `isam.fill_factor` (70 % por defecto) para absorber las
  primeras inserciones sin overflow.
- **`indice.dat`** — índice **disperso**: la clave mínima de cada página. En memoria se
  consulta con búsqueda binaria → O(log P) para ubicar la página.
- **`overflow.dat`** — nodos de 20 bytes `{entrada, siguiente, activo}`. Si la página
  destino está llena, la entrada va a una lista encadenada de ESA página
  (inserción al frente, O(1)). La cadena no está ordenada: se recorre completa.

### Complejidad
- Búsqueda: O(log P) para el índice + 1 página + largo de la cadena de overflow.
- Rango: ubica la página inicial y escanea páginas consecutivas hasta superar el límite.
- Inserción: O(log P) + corrimiento dentro de una página, u O(1) en overflow.
- Limitación clásica de ISAM: el índice es **estático**. Si muchas inserciones caen en
  la misma página, la cadena de overflow crece y la búsqueda degrada. Se corrige
  reconstruyendo (`REINDEX`), no se rebalancea como un B-tree.

## Decisiones y limitaciones (alcance académico, rúbrica §5.3)

- Clave: una columna `INTEGER`. Soporta `=`, `<`, `<=`, `>=`, `>`. Claves repetidas permitidas;
  los `NULL` no se indexan.
- Los `.dat` no pasan por `shared_buffers` ni por el WAL → sin recuperación ante caídas.
  Concurrencia simple: escritores exclusivos / lectores compartidos (un lock por índice).
- No se devuelven resultados ordenados (`amcanorder = false`); si hay `ORDER BY`, PostgreSQL ordena.
- Estimación de costos: la genérica de PostgreSQL. Como el índice no tiene bloques
  propios, el planner lo ve "barato"; para forzarlo en pruebas: `SET enable_seqscan = off`.
- `DROP INDEX` / `REINDEX` no borran la carpeta vieja en `pg_isam/` (PostgreSQL no avisa a
  un AM que se borre su archivo): quedan huérfanas hasta limpiarlas a mano.
- No soporta tablas `UNLOGGED`.
- **Comparación con B-tree: no es manzanas con manzanas.** El B-tree escribe WAL y usa
  `shared_buffers`; el ISAM escribe directo a archivos sin WAL, lo que le da ventaja
  en inserciones y construcción. Hay que decirlo en el análisis, no ocultarlo. Además,
  los tiempos obtenidos con builds de depuración (`--enable-cassert`) no son
  representativos: los oficiales deben salir del contenedor Docker.
- `isam.fill_factor` se define al cargar la librería: en una sesión nueva usar `LOAD 'isam';`
  antes de `SHOW isam.fill_factor` (los scripts ya lo hacen).

## Código reutilizado y herramientas externas (declarar en la defensa)

- API de index access methods de PostgreSQL 18 (documentación oficial, cap. "Index Access
  Method Interface Definition") y como referencia de forma, `contrib/bloom` del propio PostgreSQL.
- Diseño inicial del esqueleto ISAM del grupo (módulos datos / índice / overflow, índice
  disperso, overflow encadenado) — reescrito aquí sobre `pread/pwrite` y entradas `(clave, TID)`.
- Asistencia de IA (Claude, Anthropic) en la escritura y revisión del código del núcleo,
  del pegamento con PostgreSQL y de los scripts. El grupo debe poder explicar cada función.

## Qué falta para la Etapa 2

- Datasets D1–D3 a 100 k / 500 k / 1 M y las cargas W1–W4 completas (`comparacion.sql`
  ya implementa W1–W3 y es la base; falta W4, rango, y 3 tamaños).
- Carga "desfavorable" para el ISAM (inserciones concentradas → overflow largo).
- Distribución en 3 nodos: cada nodo tendría su propia carpeta `pg_isam/` local.
