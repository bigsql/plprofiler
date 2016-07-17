-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION plprofiler UPDATE TO '2.0'" to load this file. \quit

-- For the upgrade we just drop all 1.0 objects and build from scratch.

ALTER EXTENSION plprofiler DROP VIEW pl_profiler;
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_enable(bool);
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler_reset();
ALTER EXTENSION plprofiler DROP FUNCTION pl_profiler();

DROP VIEW pl_profiler;
DROP FUNCTION pl_profiler_enable(bool);
DROP FUNCTION pl_profiler_reset();
DROP FUNCTION pl_profiler();

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
LANGUAGE C ROWS 1000000;
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
LANGUAGE C ROWS 1000000;
GRANT EXECUTE ON FUNCTION pl_profiler_callgraph(bool) TO public;

CREATE FUNCTION pl_profiler_func_oids_current()
RETURNS oid[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_func_oids_current() TO public;

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

CREATE FUNCTION pl_profiler_collect_data()
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C;
GRANT EXECUTE ON FUNCTION pl_profiler_collect_data() TO public;

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
