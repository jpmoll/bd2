-- PRUEBAS DE CORRECCIÓN DENTRO DE POSTGRESQL
-- Compara el resultado usando el índice ISAM contra el recorrido secuencial.
-- Cualquier diferencia lanza una excepción y detiene el script.
--
--   psql -U postgres -f /scripts/test_pg.sql        (o test/test_pg.sql en el repo)
\set ON_ERROR_STOP on
\pset pager off
CREATE EXTENSION IF NOT EXISTS isam;

SELECT setseed(0.42);
DROP TABLE IF EXISTS t, claves;
CREATE TABLE t (id integer, v integer);
-- 100 000 filas, claves repetidas (rango 0..60000) y 500 NULL
INSERT INTO t SELECT (random() * 60000)::int, g FROM generate_series(1, 100000) g;
INSERT INTO t SELECT NULL, g FROM generate_series(1, 500) g;
ANALYZE t;
CREATE INDEX t_isam ON t USING isam (id);

-- Cuenta con índice o sin índice, forzando un plan nuevo cada vez (EXECUTE)
CREATE OR REPLACE FUNCTION contar(sql text, con_indice boolean) RETURNS bigint
LANGUAGE plpgsql AS $$
DECLARE r bigint;
BEGIN
    PERFORM set_config('enable_seqscan',    (NOT con_indice)::text, true);
    PERFORM set_config('enable_indexscan',  con_indice::text, true);
    PERFORM set_config('enable_bitmapscan', con_indice::text, true);
    EXECUTE sql INTO r;
    RETURN r;
END $$;

CREATE OR REPLACE FUNCTION comparar(etapa text, n int) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE a int; b int; sql text; c1 bigint; c2 bigint; i int;
BEGIN
    FOR i IN 1..n LOOP
        a := (random() * 70000)::int - 5000;
        CASE i % 4
            WHEN 0 THEN sql := format('SELECT count(*) FROM t WHERE id = %s', a);
            WHEN 1 THEN b := a + (random() * 3000)::int;
                        sql := format('SELECT count(*) FROM t WHERE id BETWEEN %s AND %s', a, b);
            WHEN 2 THEN sql := format('SELECT count(*) FROM t WHERE id < %s', a);
            ELSE        sql := format('SELECT count(*) FROM t WHERE id >= %s AND id < %s', a, a + 40);
        END CASE;
        c1 := contar(sql, true);
        c2 := contar(sql, false);
        IF c1 <> c2 THEN
            RAISE EXCEPTION '[%] DIFERENCIA en "%": indice=% secuencial=%', etapa, sql, c1, c2;
        END IF;
    END LOOP;
    RAISE NOTICE '[%] % consultas iguales (indice = secuencial)', etapa, n;
END $$;

-- Que el índice realmente se use (no que el test pase sin tocarlo)
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT * FROM t WHERE id = 123;
RESET enable_seqscan;

-- 1) Después de CREATE INDEX
SELECT comparar('construccion', 400);

-- 2) Inserciones (dispersas y a la última página) -> páginas y overflow
INSERT INTO t SELECT (random() * 60000)::int, g FROM generate_series(1, 20000) g;
INSERT INTO t SELECT 60001 + g, g FROM generate_series(1, 3000) g;   -- crecientes
SELECT isam_info('t_isam') AS estructura;
SELECT comparar('inserciones', 400);

-- 3) Rollback: las entradas de filas abortadas no deben aparecer
BEGIN;
INSERT INTO t SELECT 500, g FROM generate_series(1, 1000) g;
ROLLBACK;
SELECT comparar('rollback', 100);

-- 4) Join con búsquedas parametrizadas (amrescan con otra clave en cada vuelta)
CREATE TABLE claves AS SELECT (random() * 60000)::int AS id FROM generate_series(1, 500);
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_seqscan = off;
SET enable_material = off;
DO $$
DECLARE c1 bigint; c2 bigint;
BEGIN
    SELECT count(*) INTO c1 FROM claves k JOIN t ON t.id = k.id;
    SET LOCAL enable_indexscan = off; SET LOCAL enable_bitmapscan = off; SET LOCAL enable_seqscan = on;
    SELECT count(*) INTO c2 FROM claves k JOIN t ON t.id = k.id;
    IF c1 <> c2 THEN RAISE EXCEPTION 'join: indice=% secuencial=%', c1, c2; END IF;
    RAISE NOTICE '[join] nested loop con indice = secuencial (% filas)', c1;
END $$;
EXPLAIN (COSTS OFF) SELECT count(*) FROM claves k JOIN t ON t.id = k.id;
RESET ALL;

-- 5) = ANY(array)
DO $$
DECLARE c1 bigint; c2 bigint;
BEGIN
    c1 := contar('SELECT count(*) FROM t WHERE id = ANY (ARRAY[5, 77, 1234, 59999, 60500, -3])', true);
    c2 := contar('SELECT count(*) FROM t WHERE id = ANY (ARRAY[5, 77, 1234, 59999, 60500, -3])', false);
    IF c1 <> c2 THEN RAISE EXCEPTION 'ANY: indice=% secuencial=%', c1, c2; END IF;
    RAISE NOTICE '[ANY] igual (% filas)', c1;
END $$;

-- 6) DELETE + VACUUM: el índice no debe devolver filas borradas
DELETE FROM t WHERE id BETWEEN 10000 AND 20000;
UPDATE t SET id = id + 1 WHERE id BETWEEN 30000 AND 31000;
VACUUM t;
SELECT isam_info('t_isam') AS estructura;
SELECT comparar('delete_update_vacuum', 400);

-- 7) Índice creado sobre tabla VACÍA y luego llenado solo con inserciones
DROP TABLE IF EXISTS vacia;
CREATE TABLE vacia (id integer);
CREATE INDEX vacia_isam ON vacia USING isam (id);
INSERT INTO vacia SELECT (random() * 1000000)::int FROM generate_series(1, 5000);
DO $$
DECLARE c1 bigint; c2 bigint;
BEGIN
    c1 := contar('SELECT count(*) FROM vacia WHERE id BETWEEN 1000 AND 300000', true);
    c2 := contar('SELECT count(*) FROM vacia WHERE id BETWEEN 1000 AND 300000', false);
    IF c1 <> c2 THEN RAISE EXCEPTION 'tabla vacia: indice=% secuencial=%', c1, c2; END IF;
    RAISE NOTICE '[tabla vacia] igual (% filas)', c1;
END $$;

-- 8) Rangos vacíos / extremos de INTEGER
SELECT count(*) AS debe_ser_0 FROM t WHERE id > 2147483647;
SELECT count(*) AS debe_ser_0 FROM t WHERE id < -2147483648;
SELECT count(*) AS debe_ser_0 FROM t WHERE id = 5 AND id = 6;

\echo
\echo 'TODAS LAS PRUEBAS PASARON'
