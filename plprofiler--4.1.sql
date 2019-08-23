-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION plprofiler" to load this file. \quit

DO $$
BEGIN
	-- Create role plprofiler if it doesn't exist
    IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_authid WHERE rolname = 'plprofiler') THEN
	    CREATE ROLE plprofiler WITH NOLOGIN;
	END IF;
	-- AWS RDS specific:
	-- End users in RDS don't have access to a real postgres superuser.
	-- Instead they need this role granted to the rds_superuser role.
    IF EXISTS (SELECT 1 FROM pg_catalog.pg_authid WHERE rolname = 'rds_superuser') THEN
		GRANT plprofiler TO rds_superuser WITH ADMIN OPTION;
	END IF;
END;
$$ LANGUAGE plpgsql;

-- Register functions.

CREATE OR REPLACE FUNCTION pl_profiler_version()
RETURNS integer
AS $$
BEGIN
	RETURN 40100;
END;
$$ LANGUAGE plpgsql;
ALTER FUNCTION pl_profiler_version() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_version() TO public;

CREATE OR REPLACE FUNCTION pl_profiler_versionstr()
RETURNS text
AS $$
BEGIN
	RETURN '4.1';
END;
$$ LANGUAGE plpgsql;
ALTER FUNCTION pl_profiler_versionstr() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_versionstr() TO public;

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
ALTER FUNCTION pl_profiler_linestats_local() OWNER TO plprofiler;
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
ALTER FUNCTION pl_profiler_linestats_shared() OWNER TO plprofiler;

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
ALTER FUNCTION pl_profiler_callgraph_local() OWNER TO plprofiler;
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
ALTER FUNCTION pl_profiler_callgraph_shared() OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_func_oids_local()
RETURNS oid[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_func_oids_local() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_func_oids_local() TO public;

CREATE FUNCTION pl_profiler_func_oids_shared()
RETURNS oid[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_func_oids_shared() OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_funcs_source(
	IN  func_oids oid[],
	OUT func_oid oid,
	OUT line_number int8,
	OUT source text
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C ROWS 1000000;
ALTER FUNCTION pl_profiler_funcs_source(oid[]) OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_funcs_source(oid[]) TO public;

CREATE FUNCTION pl_profiler_get_stack(stack oid[])
RETURNS text[]
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_get_stack(oid[]) OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_get_stack(oid[]) TO public;

CREATE FUNCTION pl_profiler_reset_local()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_reset_local() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_reset_local() TO public;

CREATE FUNCTION pl_profiler_reset_shared()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_reset_shared() OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_set_enabled_global(enabled bool)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_set_enabled_global(bool) OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_get_enabled_global()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_get_enabled_global() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_get_enabled_global() TO public;

CREATE FUNCTION pl_profiler_set_enabled_local(enabled bool)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_set_enabled_local(bool) OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_set_enabled_local(bool) TO public;

CREATE FUNCTION pl_profiler_get_enabled_local()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_get_enabled_local() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_get_enabled_local() TO public;

CREATE FUNCTION pl_profiler_set_enabled_pid(pid int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_set_enabled_pid(int4) OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_get_enabled_pid()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_get_enabled_pid() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_get_enabled_pid() TO public;

CREATE FUNCTION pl_profiler_set_collect_interval(seconds int4)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_set_collect_interval(int4) OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_get_collect_interval()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_get_collect_interval() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_get_collect_interval() TO public;

CREATE FUNCTION pl_profiler_collect_data()
RETURNS int4
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_collect_data() OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_callgraph_overflow()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_callgraph_overflow() OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_functions_overflow()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_functions_overflow() OWNER TO plprofiler;

CREATE FUNCTION pl_profiler_lines_overflow()
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C;
ALTER FUNCTION pl_profiler_lines_overflow() OWNER TO plprofiler;

CREATE TABLE pl_profiler_saved (
	s_id			serial						PRIMARY KEY,
    s_name			text						NOT NULL UNIQUE,
	s_options		text						NOT NULL DEFAULT '',
	s_callgraph_overflow	bool,
	s_functions_overflow	bool,
	s_lines_overflow		bool
);
ALTER TABLE pl_profiler_saved OWNER TO plprofiler;

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
ALTER TABLE pl_profiler_saved_functions OWNER TO plprofiler;

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
ALTER TABLE pl_profiler_saved_linestats OWNER TO plprofiler;

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
ALTER TABLE pl_profiler_saved_callgraph OWNER TO plprofiler;
