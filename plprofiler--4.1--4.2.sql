-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION plprofiler UPDATE TO '4.2'" to load this file. \quit

-- Replace pl_profiler_version()
CREATE OR REPLACE FUNCTION pl_profiler_version()
RETURNS integer
AS $$
BEGIN
	RETURN 40200;
END;
$$ LANGUAGE plpgsql;
ALTER FUNCTION pl_profiler_version() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_version() TO public;

CREATE OR REPLACE FUNCTION pl_profiler_versionstr()
RETURNS text
AS $$
BEGIN
	RETURN '4.2';
END;
$$ LANGUAGE plpgsql;
ALTER FUNCTION pl_profiler_versionstr() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_versionstr() TO public;

-- Declare all functions STRICT
ALTER FUNCTION pl_profiler_callgraph_local STRICT;
ALTER FUNCTION pl_profiler_callgraph_overflow STRICT;
ALTER FUNCTION pl_profiler_callgraph_shared STRICT;
ALTER FUNCTION pl_profiler_collect_data STRICT;
ALTER FUNCTION pl_profiler_func_oids_local STRICT;
ALTER FUNCTION pl_profiler_func_oids_shared STRICT;
ALTER FUNCTION pl_profiler_funcs_source STRICT;
ALTER FUNCTION pl_profiler_functions_overflow STRICT;
ALTER FUNCTION pl_profiler_get_collect_interval STRICT;
ALTER FUNCTION pl_profiler_get_enabled_global STRICT;
ALTER FUNCTION pl_profiler_get_enabled_local STRICT;
ALTER FUNCTION pl_profiler_get_enabled_pid STRICT;
ALTER FUNCTION pl_profiler_get_stack STRICT;
ALTER FUNCTION pl_profiler_lines_overflow STRICT;
ALTER FUNCTION pl_profiler_linestats_local STRICT;
ALTER FUNCTION pl_profiler_linestats_shared STRICT;
ALTER FUNCTION pl_profiler_reset_local STRICT;
ALTER FUNCTION pl_profiler_reset_shared STRICT;
ALTER FUNCTION pl_profiler_set_collect_interval STRICT;
ALTER FUNCTION pl_profiler_set_enabled_global STRICT;
ALTER FUNCTION pl_profiler_set_enabled_local STRICT;
ALTER FUNCTION pl_profiler_set_enabled_pid STRICT;
ALTER FUNCTION pl_profiler_version STRICT;
ALTER FUNCTION pl_profiler_versionstr STRICT;
