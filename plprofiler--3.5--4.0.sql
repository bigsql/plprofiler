-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION plprofiler UPDATE TO '4.0'" to load this file. \quit

-- Drop obsolete function
DROP FUNCTION pl_profiler_enable(bool);

-- Create new functions
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

