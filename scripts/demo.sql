-- DEMO ETAPA 1: ISAM como index access method de PostgreSQL
-- Ejecutar:  psql -U postgres -f /scripts/demo.sql
\set ON_ERROR_STOP on
\pset pager off

CREATE EXTENSION IF NOT EXISTS isam;

-- 1) Tabla principal (esquema de transporte.sql adaptado a PostgreSQL)
DROP TABLE IF EXISTS coordenadas;
CREATE TABLE coordenadas (
    id_coordenada integer       NOT NULL,
    latitud       numeric(10,7) NOT NULL,
    longitud      numeric(10,7) NOT NULL,
    id_ruta       integer       NOT NULL,
    sentido       text          NOT NULL DEFAULT 'IDA' CHECK (sentido IN ('IDA','VUELTA')),
    id_paradero   integer,
    orden         integer       NOT NULL
);

INSERT INTO coordenadas VALUES
 (1, -16.3658450, -71.5016580, 1, 'IDA', 253, 1),
 (2, -16.3656690, -71.5023130, 1, 'IDA', NULL, 2),
 (3, -16.3656900, -71.5025040, 1, 'IDA', NULL, 3),
 (4, -16.3657520, -71.5025880, 1, 'IDA', NULL, 4),
 (5, -16.3659800, -71.5024950, 1, 'IDA', NULL, 5),
 (6, -16.3662680, -71.5024150, 1, 'IDA', NULL, 6),
 (7, -16.3664310, -71.5023080, 1, 'IDA', 254, 7),
 (8, -16.3666290, -71.5020560, 1, 'IDA', NULL, 8),
 (9, -16.3668370, -71.5015610, 1, 'IDA', NULL, 9);

-- Relleno determinista (sin random): ids pares hasta 20 000 (los impares quedan
-- libres para insertarlos después, repartidos entre todas las páginas)
INSERT INTO coordenadas
SELECT g,
       round((-16.3658450 - (g % 1000) * 0.00001)::numeric, 7),
       round((-71.5016580 - (g % 700)  * 0.00001)::numeric, 7),
       (g - 1) / 200 + 1,
       CASE WHEN ((g - 1) / 100) % 2 = 0 THEN 'IDA' ELSE 'VUELTA' END,
       NULL,
       (g - 1) % 200 + 1
FROM generate_series(10, 20000, 2) g;

ANALYZE coordenadas;

-- 2) CONSTRUCCIÓN: el índice se arma recorriendo el heap
\echo
\echo '== CREATE INDEX ... USING isam =='
CREATE INDEX coordenadas_isam ON coordenadas USING isam (id_coordenada);
SELECT isam_info('coordenadas_isam') AS estructura;

-- Forzamos al planner a usar índices para ver el ISAM en el plan
SET enable_seqscan = off;

-- 3) BÚSQUEDA por igualdad (clave existente)
\echo
\echo '== Igualdad: clave existente (id_coordenada = 7) =='
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT * FROM coordenadas WHERE id_coordenada = 7;
SELECT * FROM coordenadas WHERE id_coordenada = 7;

-- 4) BÚSQUEDA por igualdad (clave inexistente)
\echo
\echo '== Igualdad: clave inexistente (id_coordenada = -50) =='
SELECT count(*) AS filas FROM coordenadas WHERE id_coordenada = -50;

-- 5) BÚSQUEDA por rango
\echo
\echo '== Rango: BETWEEN 100 AND 110 =='
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT * FROM coordenadas WHERE id_coordenada BETWEEN 100 AND 110;
SELECT id_coordenada, id_ruta, orden FROM coordenadas
WHERE id_coordenada BETWEEN 100 AND 110 ORDER BY 1;

-- 6) INSERCIÓN dispersa: claves repartidas por todo el rango. Caben en el 30 %
--    de espacio libre que dejó la construcción -> no hay overflow.
\echo
\echo '== Inserción dispersa: 2500 filas (caben en el espacio libre de las páginas) =='
INSERT INTO coordenadas
SELECT g, -16.37, -71.51, 999, 'IDA', NULL, 1
FROM generate_series(11, 20000, 8) g;
SELECT isam_info('coordenadas_isam') AS estructura;

-- 7) OVERFLOW: reconstruir con las páginas llenas al 100 % y luego insertar claves
--    NUEVAS CRECIENTES (todas caen en la última página -> cadena de overflow).
--    Es la debilidad clásica de ISAM con inserciones concentradas.
\echo
\echo '== Overflow: reconstruir al 100 %, insertar 1000 filas con ids crecientes =='
SET isam.fill_factor = 100;
REINDEX INDEX coordenadas_isam;
SELECT isam_info('coordenadas_isam') AS antes;
INSERT INTO coordenadas
SELECT g, -16.37, -71.51, 998, 'IDA', NULL, g - 30000
FROM generate_series(30001, 31000) g;
SELECT isam_info('coordenadas_isam') AS despues;

-- 8) VERIFICACIÓN: el índice y el seq scan deben dar lo mismo
\echo
\echo '== Verificación: índice vs. recorrido secuencial =='
SELECT count(*) AS con_indice FROM coordenadas WHERE id_coordenada BETWEEN 15000 AND 30500;
SET enable_indexscan = off;  SET enable_bitmapscan = off;  RESET enable_seqscan;
SELECT count(*) AS sin_indice FROM coordenadas WHERE id_coordenada BETWEEN 15000 AND 30500;
RESET enable_indexscan; RESET enable_bitmapscan;

-- 9) ELIMINACIÓN + VACUUM (el índice deja de apuntar a filas muertas)
\echo
\echo '== DELETE + VACUUM =='
DELETE FROM coordenadas WHERE id_coordenada BETWEEN 30001 AND 31000;
VACUUM coordenadas;
SET enable_seqscan = off;
SELECT count(*) AS deberia_ser_0 FROM coordenadas WHERE id_coordenada BETWEEN 30001 AND 31000;
SELECT isam_info('coordenadas_isam') AS estructura;
