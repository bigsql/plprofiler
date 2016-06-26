-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plprofiler" to load this file. \quit

-- Register functions.

CREATE FUNCTION pl_profiler_linestats(
	IN  filter_zero bool,
    OUT func_oid oid,
    OUT line_number int8,
    OUT exec_count int8,
    OUT total_time int8,
    OUT longest_time int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_linestats(bool) TO public;

CREATE FUNCTION pl_profiler_callgraph(
	IN  filter_zero bool,
    OUT stack oid[],
    OUT call_count int8,
    OUT us_total int8,
    OUT us_children int8,
    OUT us_self int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_callgraph(bool) TO public;

CREATE FUNCTION pl_profiler_get_stack(stack oid[])
RETURNS text[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_get_stack(oid[]) TO public;

CREATE FUNCTION pl_profiler_get_import_stack(stack oid[])
RETURNS text[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_get_stack(oid[]) TO public;

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

CREATE FUNCTION pl_profiler_source_lines(
	IN  func_oid oid,
	OUT source text,
	OUT line_number bigint)
RETURNS SETOF RECORD
AS $$
    SELECT *, row_number() OVER ()
		FROM regexp_split_to_table((SELECT prosrc
										FROM pg_catalog.pg_proc
										WHERE oid = func_oid),
									E'\\n')
$$ LANGUAGE sql;
GRANT EXECUTE ON FUNCTION pl_profiler_source_lines(oid) TO public;

CREATE VIEW pl_profiler_all_source AS
	SELECT P.oid AS func_oid,
		   S.line_number,
		   S.source
		FROM pg_catalog.pg_proc P,
			 pg_catalog.pg_language L,
			 pl_profiler_source_lines(P.oid) S
		WHERE L.lanname = 'plpgsql'
		  AND P.prolang = L.oid;
GRANT SELECT ON pl_profiler_all_source TO public;

CREATE VIEW pl_profiler_all_functions AS
	SELECT P.oid AS func_oid,
		   N.nspname AS func_schema,
		   P.proname AS func_name,
		   pg_catalog.pg_get_function_result(P.oid) AS func_result,
		   pg_catalog.pg_get_function_arguments(P.oid) AS func_arguments
		FROM pg_catalog.pg_proc P,
			 pg_catalog.pg_language L,
			 pg_catalog.pg_namespace N
		WHERE L.lanname = 'plpgsql'
		  AND P.prolang = L.oid
		  AND N.oid = P.pronamespace;
GRANT SELECT ON pl_profiler_all_functions TO public;

CREATE TABLE pl_profiler_linestats_data (
	func_oid		oid,
	line_number		int8,
	exec_count		int8,
	total_time		int8,
	longest_time	int8
);

CREATE TABLE pl_profiler_callgraph_data (
    stack			oid[],
	call_count		int8,
	us_total		int8,
	us_children		int8,
	us_self			int8
);

CREATE VIEW pl_profiler_linestats AS
	SELECT L.func_oid,
		   L.line_number,
		   coalesce(S.source, '') AS line,
		   sum(exec_count) AS exec_count,
		   sum(total_time) AS total_time,
		   max(longest_time) AS longest_time
	FROM pl_profiler_linestats_data L
	LEFT JOIN pl_profiler_all_source S
		ON S.func_oid = L.func_oid AND S.line_number = L.line_number
	GROUP BY L.func_oid, L.line_number, S.source
	ORDER BY L.func_oid, L.line_number;

CREATE VIEW pl_profiler_callgraph AS
	SELECT pl_profiler_get_stack(stack) AS stack,
		   sum(call_count) AS call_count,
		   sum(us_total) AS us_total,
		   sum(us_children) AS us_children,
		   sum(us_self) AS us_self
	FROM pl_profiler_callgraph_data
	GROUP BY stack;

CREATE VIEW pl_profiler_linestats_current AS
  WITH S AS (
    SELECT * FROM pl_profiler_all_source
  )
  SELECT L.func_oid, L.line_number,
		coalesce(S.source, '') AS line,
		L.exec_count, L.total_time, L.longest_time
    FROM pl_profiler_linestats(false) L
	LEFT JOIN S
		ON S.func_oid = L.func_oid AND S.line_number = L.line_number
   ORDER BY L.func_oid, L.line_number;
GRANT SELECT ON pl_profiler_linestats_current TO public;

CREATE VIEW pl_profiler_callgraph_current AS
	SELECT pl_profiler_get_stack(stack) AS stack, call_count,
		   us_total, us_children, us_self
    FROM pl_profiler_callgraph(false);
GRANT SELECT ON pl_profiler_callgraph_current TO public;

CREATE TABLE pl_profiler_saved (
	s_id			serial						PRIMARY KEY,
    s_name			text						NOT NULL UNIQUE,
	s_options		text						NOT NULL DEFAULT ''
);

CREATE TABLE pl_profiler_saved_functions (
	f_s_id			integer						NOT NULL
												REFERENCES pl_profiler_saved
												ON DELETE CASCADE,
	f_funcoid		int8						NOT NULL,
	f_schema		text						NOT NULL,
	f_funcname		text						NOT NULL,
	f_funcresult	text						NOT NULL,
	f_funcargs		text						NOT NULL,
	PRIMARY KEY (f_s_id, f_funcoid)
);

CREATE TABLE pl_profiler_saved_linestats (
	l_s_id			integer						NOT NULL
												REFERENCES pl_profiler_saved
												ON DELETE CASCADE,
	l_funcoid		int8						NOT NULL,
	l_line_number	int4						NOT NULL,
	l_source		text,
	l_exec_count	bigint,
	l_total_time	bigint,
	l_longest_time	bigint,
	PRIMARY KEY (l_s_id, l_funcoid, l_line_number)
);

CREATE TABLE pl_profiler_saved_callgraph (
	c_s_id			integer						NOT NULL
												REFERENCES pl_profiler_saved
												ON DELETE CASCADE,
	c_stack			text[]						NOT NULL,
	c_call_count	bigint,
	c_us_total		bigint,
	c_us_children	bigint,
	c_us_self		bigint,
	PRIMARY KEY (c_s_id, c_stack)
);
