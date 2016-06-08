#!/bin/sh

PSQL="psql -qAt"
SET_NAME=""

while getopts "N:h:p:U:" OPT ; do
	case $OPT in
		N)		SET_NAME="$OPTARG"
				;;
		*)		PSQL="$PSQL -$OPT '$OPTARG'"
				;;
	esac
done
shift $(expr $OPTIND - 1)
PSQL="$PSQL $@"

if [ -z "$SET_NAME" ] ; then
	echo "ERROR: No data set name specified (use option -N string)" >&2
	exit 2
fi

function export_plprofiler_data() {
	PLP_SCHEMA=$(get_plprofiler_namespace)

	echo "-- ----"
	echo "-- plprofiler data export"
	echo "-- ----"
	echo "\\set ON_ERROR_STOP on"
	echo "SET work_mem TO '256MB';"
	echo "START TRANSACTION;"
	echo "SET search_path TO \"${PLP_SCHEMA}\";"
	echo ""

	export_plprofiler_functions
	export_plprofiler_source
	export_plprofiler_linestats
	export_plprofiler_callgraph
	create_import_functions
	generate_data_set
	drop_import_functions

	echo "COMMIT;"
}

function generate_data_set() {
	echo "-- ----"
	echo "-- Generate the new data set from the import data"
	echo "-- ----"
	echo "INSERT INTO pl_profiler_saved"
	echo "		(s_name, s_options)"
	echo "	VALUES ('$SET_NAME', '{\"name\": \"$SET_NAME\", \"title\": \"PL Profiler Report for $SET_NAME\", \"tabstop\": \"8\", \"svg_width\": \"1200\", \"table_width\": \"80%\", \"desc\": \"<h1>PL Profiler Report for $SET_NAME</h1>\n<p>\n<!-- description here -->\n</p>\"}');"
	echo ""
	echo "INSERT INTO pl_profiler_saved_functions"
	echo "		(f_s_id, f_funcoid, f_schema, f_funcname,"
	echo "		 f_funcresult, f_funcargs)"
	echo "	SELECT currval('pl_profiler_saved_s_id_seq') as s_id,"
	echo "		   F.func_oid, F.func_schema, F.func_name,"
	echo "		   F.func_result, F.func_arguments"
	echo "	FROM pl_profiler_import_functions F;"
	echo ""
	echo "INSERT INTO pl_profiler_saved_linestats"
	echo "		(l_s_id, l_funcoid,"
	echo "		 l_line_number, l_source, l_exec_count,"
	echo "		 l_total_time, l_longest_time)"
	echo "	SELECT currval('pl_profiler_saved_s_id_seq') as s_id,"
	echo "		   L.func_oid, L.line_number,"
	echo "		   coalesce(S.source, '-- SOURCE NOT FOUND'),"
	echo "		   sum(L.exec_count), sum(L.total_time),"
	echo "		   max(L.longest_time)"
	echo "	FROM pl_profiler_import_linestats L"
	echo "	LEFT JOIN pl_profiler_import_source S"
	echo "		ON S.func_oid = L.func_oid"
	echo "		AND S.line_number = L.line_number"
	echo "	GROUP BY s_id, L.func_oid, L.line_number, S.source"
	echo "	ORDER BY s_id, L.func_oid, L.line_number;"
	echo ""
	echo "INSERT INTO pl_profiler_saved_callgraph"
	echo "		(c_s_id, c_stack, c_call_count, c_us_total,"
	echo "		 c_us_children, c_us_self)"
	echo "	SELECT currval('pl_profiler_saved_s_id_seq') as s_id,"
	echo "		   pl_profiler_get_import_stack(stack) as stack,"
	echo "		   sum(call_count), sum(us_total),"
	echo "		   sum(us_children), sum(us_self)"
	echo "	FROM pl_profiler_import_callgraph"
	echo "	GROUP BY s_id, stack"
	echo "	ORDER BY s_id, stack;"
	echo ""
}

function get_plprofiler_namespace() {
	echo "SELECT N.nspname
			FROM pg_catalog.pg_extension E
			JOIN pg_catalog.pg_namespace N ON N.oid = E.extnamespace
			WHERE E.extname = 'plprofiler'
	" | $PSQL || exit 1
}

function export_plprofiler_functions() {
	echo "-- ----"
	echo "-- Function call signatures"
	echo "-- ----"
	echo "CREATE TEMP TABLE pl_profiler_import_functions ("
	echo "    func_oid			oid,"
	echo "    func_schema		text,"
	echo "    func_name			text,"
	echo "    func_result		text,"
	echo "    func_arguments	text"
	echo ");"

	echo "COPY pl_profiler_import_functions FROM STDIN;"
	echo "COPY (SELECT func_oid, func_schema, func_name, func_result,
					   func_arguments
				FROM pl_profiler_all_functions) TO STDOUT;" | $PSQL
	echo "\\."
	echo "CREATE UNIQUE INDEX pl_profiler_import_functions_idx1"
	echo "    ON pl_profiler_import_functions (func_oid);"
	echo ""
}

function export_plprofiler_source() {
	echo "-- ----"
	echo "-- Function source text"
	echo "-- ----"
	echo "CREATE TEMP TABLE pl_profiler_import_source ("
	echo "    func_oid			oid,"
	echo "    line_number		bigint,"
	echo "    source			text"
	echo ");"

	echo "COPY pl_profiler_import_source FROM STDIN;"
	echo "COPY (SELECT func_oid, line_number, source
				FROM pl_profiler_all_source) TO STDOUT;" | $PSQL
	echo "\\."
	echo "CREATE UNIQUE INDEX pl_profiler_import_source_idx1"
	echo "    ON pl_profiler_import_source (func_oid, line_number);"
	echo ""
}

function export_plprofiler_linestats() {
	echo "-- ----"
	echo "-- linestats"
	echo "-- ----"
	echo "CREATE TEMP TABLE pl_profiler_import_linestats ("
	echo "    func_oid			oid,"
	echo "    line_number		bigint,"
	echo "    exec_count		bigint,"
	echo "    total_time		bigint,"
	echo "    longest_time		bigint"
	echo ");"

	echo "COPY pl_profiler_import_linestats FROM STDIN;"
	echo "COPY (SELECT func_oid, line_number, exec_count,
					   total_time, longest_time
				FROM pl_profiler_linestats_data) TO STDOUT;" | $PSQL
	echo "\\."
	echo "CREATE INDEX pl_profiler_import_linestats_idx1"
	echo "    ON pl_profiler_import_linestats (func_oid, line_number);"
	echo ""
}

function export_plprofiler_callgraph() {
	echo "-- ----"
	echo "-- callgraph"
	echo "-- ----"
	echo "CREATE TEMP TABLE pl_profiler_import_callgraph ("
	echo "    stack				oid[],"
	echo "    call_count		bigint,"
	echo "    us_total			bigint,"
	echo "    us_children		bigint,"
	echo "    us_self			bigint"
	echo ");"

	echo "COPY pl_profiler_import_callgraph FROM STDIN;"
	echo "COPY (SELECT stack, call_count,
					   us_total, us_children, us_self
				FROM pl_profiler_callgraph_data) TO STDOUT;" | $PSQL
	echo "\\."
	echo "CREATE INDEX pl_profiler_import_callgraph_idx1"
	echo "    ON pl_profiler_import_callgraph (stack);"
	echo ""
}

function create_import_functions() {
	echo "-- ----"
	echo "-- Translate an Oid[] into a text[] with function names"
	echo "-- ----"
	echo "CREATE FUNCTION pl_profiler_get_import_stack(oid[])"
	echo "RETURNS text[]"
	echo "AS \$\$"
	echo "    SELECT ARRAY("
	echo "        SELECT func_schema || '.' || func_name ||"
	echo "				 '() oid=' || foid::text"
	echo "        FROM pl_profiler_import_functions F"
	echo "		  JOIN unnest(\$1) foid ON F.func_oid = foid"
	echo "    )"
	echo "\$\$ LANGUAGE sql;"
	echo ""
}

function drop_import_functions() {
	echo "-- ----"
	echo "-- Drop the import helper function(s)"
	echo "-- ----"
	echo "DROP FUNCTION pl_profiler_get_import_stack(oid[]);"
	echo ""
}

# ----
# Do the whole thing.
# ----
export_plprofiler_data

