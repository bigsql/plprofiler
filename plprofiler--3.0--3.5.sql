-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION plprofiler UPDATE TO '3.5'" to load this file. \quit

DO $$
BEGIN
	-- Create role plprofiler if it doesn't exist and grant it to rds_superuser
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

-- Create new function
CREATE FUNCTION pl_profiler_version()
RETURNS integer
AS $$
BEGIN
	RETURN 305;
END;
$$ LANGUAGE plpgsql;
ALTER FUNCTION pl_profiler_version() OWNER TO plprofiler;
GRANT EXECUTE ON FUNCTION pl_profiler_version() TO public;

-- Change object ownership

ALTER FUNCTION pl_profiler_linestats_local() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_linestats_shared() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_callgraph_local() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_callgraph_shared() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_func_oids_local() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_func_oids_shared() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_funcs_source(oid[]) OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_get_stack(oid[]) OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_reset_local() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_reset_shared() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_enable(bool) OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_collect_data() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_callgraph_overflow() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_functions_overflow() OWNER TO plprofiler;
ALTER FUNCTION pl_profiler_lines_overflow() OWNER TO plprofiler;
ALTER TABLE pl_profiler_saved OWNER TO plprofiler;
ALTER TABLE pl_profiler_saved_functions OWNER TO plprofiler;
ALTER TABLE pl_profiler_saved_linestats OWNER TO plprofiler;
ALTER TABLE pl_profiler_saved_callgraph OWNER TO plprofiler;
