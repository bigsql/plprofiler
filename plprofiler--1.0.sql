-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plprofiler" to load this file. \quit

-- Register functions.
CREATE FUNCTION pl_profiler(
    OUT func_oid oid,
    OUT line_number int8,
    OUT line text,
    OUT exec_count int8,
    OUT total_time int8,
    OUT longest_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_enable(enabled bool)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE VIEW pl_profiler AS
  SELECT * 
    FROM pl_profiler()
   ORDER BY func_oid, line_number;

GRANT SELECT ON pl_profiler TO PUBLIC;
GRANT EXECUTE ON FUNCTION pl_profiler_reset() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pl_profiler_enable(bool) TO PUBLIC;


