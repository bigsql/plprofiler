-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION plprofiler UPDATE TO '3.5'" to load this file. \quit

-- Replace pl_profiler_version()
CREATE OR REPLACE FUNCTION pl_profiler_version()
RETURNS integer
AS $$
BEGIN
	RETURN 40000;
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

