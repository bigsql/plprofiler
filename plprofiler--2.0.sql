-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plprofiler" to load this file. \quit

-- Register functions.

CREATE FUNCTION pl_profiler_linestats(
    OUT func_oid oid,
    OUT line_number int8,
    OUT exec_count int8,
    OUT total_time int8,
    OUT longest_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_linestats() TO public;

CREATE FUNCTION pl_profiler_callgraph(
    OUT stack text[],
    OUT call_count int8,
    OUT us_total int8,
    OUT us_children int8,
    OUT us_self int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_callgraph() TO public;

CREATE FUNCTION pl_profiler_get_source(func oid, lineno int8)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_get_source(oid, int8) TO public;

CREATE FUNCTION pl_profiler_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_reset() TO public;

CREATE FUNCTION pl_profiler_enable(enabled bool)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_enable(bool) TO public;

CREATE FUNCTION pl_profiler_save_stats()
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_save_stats() TO public;

CREATE TABLE pl_profiler_linestats_data (
	func_oid		oid,
	line_number		int8,
	exec_count		int8,
	total_time		int8,
	longest_time	int8
);
GRANT INSERT, SELECT ON pl_profiler_linestats_data TO public;

CREATE TABLE pl_profiler_callgraph_data (
    stack			text[],
	call_count		int8,
	us_total		int8,
	us_children		int8,
	us_self			int8
);
GRANT INSERT, SELECT ON pl_profiler_callgraph_data TO public;

CREATE VIEW pl_profiler_linestats AS
	SELECT func_oid,
		   line_number,
		   pl_profiler_get_source(func_oid, line_number) AS line,
		   sum(exec_count) AS exec_count,
		   sum(total_time) AS total_time,
		   max(longest_time) AS longest_time
	FROM pl_profiler_linestats_data
	GROUP BY func_oid, line_number, line
	ORDER BY func_oid, line_number;
GRANT SELECT ON pl_profiler_linestats TO public;

CREATE VIEW pl_profiler_callgraph AS
	SELECT stack,
		   sum(call_count) AS call_count,
		   sum(us_total) AS us_total,
		   sum(us_children) AS us_children,
		   sum(us_self) AS us_self
	FROM pl_profiler_callgraph_data
	GROUP BY stack;
GRANT SELECT ON pl_profiler_callgraph TO public;

CREATE VIEW pl_profiler_linestats_current AS
  SELECT func_oid, line_number,
		pl_profiler_get_source(func_oid, line_number) as line,
		exec_count, total_time, longest_time
    FROM pl_profiler_linestats()
   ORDER BY func_oid, line_number;
GRANT SELECT ON pl_profiler_linestats_current TO public;

CREATE VIEW pl_profiler_callgraph_current AS
  SELECT stack, call_count, us_total, us_children, us_self
    FROM pl_profiler_callgraph();
GRANT SELECT ON pl_profiler_callgraph_current TO public;

