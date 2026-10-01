\echo Use "CREATE EXTENSION isam" to load this file. \quit

CREATE FUNCTION isam_handler(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD isam TYPE INDEX HANDLER isam_handler;

-- Una sola columna INTEGER, con las 5 estrategias de comparación.
CREATE OPERATOR CLASS int4_ops DEFAULT FOR TYPE int4 USING isam AS
    OPERATOR 1 <  (int4, int4),
    OPERATOR 2 <= (int4, int4),
    OPERATOR 3 =  (int4, int4),
    OPERATOR 4 >= (int4, int4),
    OPERATOR 5 >  (int4, int4);

-- Estadísticas del índice (páginas, overflow) para la demo.
CREATE FUNCTION isam_info(regclass)
RETURNS text
AS 'MODULE_PATHNAME', 'isam_info_sql'
LANGUAGE C STRICT;
