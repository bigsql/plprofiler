-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION plprofiler UPDATE TO '3.0'" to load this file. \quit

-- We don't preserve any

ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_linestats(bool);
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_callgraph(bool);
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_func_oids_current();
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_funcs_source(oid[]);
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_get_stack(oid[]);
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_reset();
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_enable(bool);
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_collect_data();

DROP FUNCTION pl_profiler_linestats(bool);
DROP FUNCTION pl_profiler_callgraph(bool);
DROP FUNCTION pl_profiler_func_oids_current();
DROP FUNCTION pl_profiler_funcs_source(oid[]);
DROP FUNCTION pl_profiler_get_stack(oid[]);
DROP FUNCTION pl_profiler_reset();
DROP FUNCTION pl_profiler_enable(bool);
DROP FUNCTION pl_profiler_collect_data();

ALTER EXTENSION plprofiler DROP TABLE pl_profiler_linestats_data;
ALTER EXTENSION plprofiler DROP TABLE pl_profiler_callgraph_data;

DROP TABLE pl_profiler_linestats_data;
DROP TABLE pl_profiler_callgraph_data;

-- Register functions.

CREATE FUNCTION pl_profiler_linestats_local(
    OUT func_oid oid,
    OUT line_number int8,
    OUT exec_count int8,
    OUT total_time int8,
    OUT longest_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C ROWS 1000000;
GRANT EXECUTE ON FUNCTION pl_profiler_linestats_local() TO public;

CREATE FUNCTION pl_profiler_linestats_shared(
    OUT func_oid oid,
    OUT line_number int8,
    OUT exec_count int8,
    OUT total_time int8,
    OUT longest_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C ROWS 1000000;

CREATE FUNCTION pl_profiler_callgraph_local(
    OUT stack oid[],
    OUT call_count int8,
    OUT us_total int8,
    OUT us_children int8,
    OUT us_self int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C ROWS 1000000;
GRANT EXECUTE ON FUNCTION pl_profiler_callgraph_local() TO public;

CREATE FUNCTION pl_profiler_callgraph_shared(
    OUT stack oid[],
    OUT call_count int8,
    OUT us_total int8,
    OUT us_children int8,
    OUT us_self int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C ROWS 1000000;

CREATE FUNCTION pl_profiler_func_oids_local()
RETURNS oid[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_func_oids_local() TO public;

CREATE FUNCTION pl_profiler_func_oids_shared()
RETURNS oid[]
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_funcs_source(
	IN  func_oids oid[],
	OUT func_oid oid,
	OUT line_number int8,
	OUT source text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C ROWS 1000000;
GRANT EXECUTE ON FUNCTION pl_profiler_funcs_source(oid[]) TO public;

CREATE FUNCTION pl_profiler_get_stack(stack oid[])
RETURNS text[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_get_stack(oid[]) TO public;

CREATE FUNCTION pl_profiler_reset_local()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_reset_local() TO public;

CREATE FUNCTION pl_profiler_reset_shared()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_enable(enabled bool)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_enable(bool) TO public;

CREATE FUNCTION pl_profiler_collect_data()
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_callgraph_overflow()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_functions_overflow()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE FUNCTION pl_profiler_lines_overflow()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Add new columns to table pl_profiler_saved

ALTER TABLE pl_profiler_saved
	ADD COLUMN s_callgraph_overflow bool,
	ADD COLUMN s_function_overflow bool,
	ADD COLUMN s_lines_overflow bool;

