-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plprofiler" to load this file. \quit

-- Register functions.
CREATE FUNCTION pl_profiler(
    OUT func_oid oid,
    OUT line_number int8,
    OUT exec_count int8,
    OUT total_time int8,
    OUT longest_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_callgraph(
    OUT stack text[],
    OUT call_count int8,
    OUT us_total int8,
    OUT us_children int8,
    OUT us_self int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_get_source(oid, int8)
RETURNS text
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
  SELECT func_oid, line_number,
		pl_profiler_get_source(func_oid, line_number) as line,
		exec_count, total_time, longest_time
    FROM pl_profiler()
   ORDER BY func_oid, line_number;

CREATE VIEW pl_callgraph AS
  SELECT *
    FROM pl_callgraph();

GRANT SELECT ON pl_profiler TO PUBLIC;
GRANT EXECUTE ON FUNCTION pl_profiler_reset() TO PUBLIC;
GRANT EXECUTE ON FUNCTION pl_profiler_enable(bool) TO PUBLIC;


