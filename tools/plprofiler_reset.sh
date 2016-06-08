#!/bin/sh

PSQL="psql -qAt"

while getopts "h:p:U:" OPT ; do
	case $OPT in
		h|p|U)	PSQL="$PSQL -$OPT '$OPTARG'"
				;;
	    *)		echo "ERROR: Unknown option -$OPT" >&2
				exit 2
				;;
	esac
done
shift $(expr $OPTIND - 1)
PSQL="$PSQL $@"

function reset_plprofiler_data() {
	PLP_SCHEMA=$(get_plprofiler_namespace)

	echo "-- ----"
	echo "-- plprofiler data export"
	echo "-- ----"
	echo "\\set ON_ERROR_STOP on"
	echo "SET work_mem TO '256MB';"
	echo "START TRANSACTION;"
	echo "SET search_path TO \"${PLP_SCHEMA}\";"
	echo ""
	echo "DELETE FROM pl_profiler_linestats_data;"
	echo "DELETE FROM pl_profiler_callgraph_data;"
	echo ""
	echo "COMMIT;"
}

function get_plprofiler_namespace() {
	echo "SELECT N.nspname
			FROM pg_catalog.pg_extension E
			JOIN pg_catalog.pg_namespace N ON N.oid = E.extnamespace
			WHERE E.extname = 'plprofiler'
	" | $PSQL || exit 1
}

# ----
# Do the whole thing.
# ----
reset_plprofiler_data | $PSQL

