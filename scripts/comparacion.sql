-- COMPARACIÓN PRELIMINAR (Etapa 1): sin índice vs B-tree vs ISAM
-- Dataset D2 (INTEGER aleatorio, único, reproducible) con cargas W1, W2, W3.
--
-- Ejecutar:  psql -U postgres -v n=100000 -v q=1000 -v reps=5 -f /scripts/comparacion.sql
--   n    = tamaño del dataset      (rúbrica: 100000, 500000, 1000000)
--   q    = búsquedas por carga     (rúbrica Etapa 2: 10000; aquí 1000 para que el
--                                   caso "sin índice" no tarde minutos)
--   reps = repeticiones            (rúbrica: mínimo 5)
--
-- Reproducibilidad: las claves son  (g * 48271) mod 2147483647  para g = 1, 2, 3...
-- (generador de Lehmer, sin semilla aleatoria: la misma fórmula produce los mismos
-- datos siempre y las claves son únicas). Claves ausentes (W2) = enteros negativos.
\set ON_ERROR_STOP on
\pset pager off
\if :{?n} \else \set n 100000 \endif
\if :{?q} \else \set q 1000 \endif
\if :{?reps} \else \set reps 5 \endif

CREATE EXTENSION IF NOT EXISTS isam;
LOAD 'isam';   -- carga la librería (define el parámetro isam.fill_factor)

\echo '== Condiciones del experimento =='
SELECT version();
SELECT :n AS n, :q AS q, :reps AS repeticiones,
       current_setting('shared_buffers') AS shared_buffers,
       current_setting('isam.fill_factor') AS isam_fill_factor;

DROP TABLE IF EXISTS d2;
CREATE TABLE d2 (id integer NOT NULL, payload text);

CREATE OR REPLACE FUNCTION ms(t0 timestamptz) RETURNS numeric
LANGUAGE sql AS $$ SELECT round((extract(epoch FROM clock_timestamp() - t0) * 1000)::numeric, 2) $$;

CREATE OR REPLACE FUNCTION correr(config text, n int, q int)
RETURNS TABLE (construccion_ms numeric, w1_exitosas_ms numeric,
               w2_ausentes_ms numeric, w3_inserciones_ms numeric, bytes_indice bigint)
LANGUAGE plpgsql AS $$
DECLARE
    t0 timestamptz;
    i int;
BEGIN
    DROP INDEX IF EXISTS d2_idx;
    TRUNCATE d2;
    INSERT INTO d2 SELECT ((g::bigint * 48271) % 2147483647)::int, 'x' || g
    FROM generate_series(1, n) g;

    -- Construcción
    t0 := clock_timestamp();
    IF config = 'btree' THEN
        CREATE INDEX d2_idx ON d2 USING btree (id);
    ELSIF config = 'isam' THEN
        CREATE INDEX d2_idx ON d2 USING isam (id);
    END IF;
    construccion_ms := ms(t0);
    ANALYZE d2;

    -- W1: q búsquedas exitosas por igualdad
    t0 := clock_timestamp();
    FOR i IN 1..q LOOP
        PERFORM 1 FROM d2 WHERE id = ((i::bigint * 48271) % 2147483647)::int;
    END LOOP;
    w1_exitosas_ms := ms(t0);

    -- W2: q búsquedas de claves ausentes
    t0 := clock_timestamp();
    FOR i IN 1..q LOOP
        PERFORM 1 FROM d2 WHERE id = -i;
    END LOOP;
    w2_ausentes_ms := ms(t0);

    -- W3: inserciones de un 10 % adicional (claves nuevas, mismas fórmulas)
    t0 := clock_timestamp();
    INSERT INTO d2 SELECT ((g::bigint * 48271) % 2147483647)::int, 'x' || g
    FROM generate_series(n + 1, n + n / 10) g;
    w3_inserciones_ms := ms(t0);

    IF config = 'btree' THEN
        bytes_indice := pg_relation_size('d2_idx');
    ELSIF config = 'isam' THEN
        bytes_indice := substring(isam_info('d2_idx') FROM 'tamano_bytes=([0-9]+)')::bigint;
    ELSE
        bytes_indice := 0;
    END IF;
    RETURN NEXT;
END $$;

DROP TABLE IF EXISTS resultados;
CREATE TABLE resultados (config text, rep int, construccion_ms numeric,
    w1_exitosas_ms numeric, w2_ausentes_ms numeric, w3_inserciones_ms numeric, bytes_indice bigint);

\echo
\echo '== Corriendo (puede tardar) =='
INSERT INTO resultados
SELECT c.config, r.rep, x.*
FROM (VALUES ('sin_indice'), ('btree'), ('isam')) AS c(config),
     generate_series(1, :reps) AS r(rep),
     LATERAL correr(c.config, :n, :q) AS x;

\echo
\echo '== MEDIANA de las repeticiones (ms; tamaño en bytes) =='
SELECT config,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY construccion_ms)    AS construccion,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY w1_exitosas_ms)     AS w1_exitosas,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY w2_ausentes_ms)     AS w2_ausentes,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY w3_inserciones_ms)  AS w3_inserciones,
       max(bytes_indice)                                               AS bytes_indice
FROM resultados GROUP BY config
ORDER BY CASE config WHEN 'sin_indice' THEN 1 WHEN 'btree' THEN 2 ELSE 3 END;

\echo
\echo 'Detalle por repetición: SELECT * FROM resultados ORDER BY config, rep;'
