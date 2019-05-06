/* ----------------------------------------------------------------------
 *
 * plprofiler.h
 *
 *	  Declarations for profiling plugin for PL/pgSQL instrumentation
 *
 * Copyright (c) 2014-2019, BigSQL
 * Copyright (c) 2008-2014, PostgreSQL Global Development Group
 * Copyright 2006,2007 - EnterpriseDB, Inc.
 *
 * Major Change History:
 * 2012 - Removed from PostgreSQL plDebugger Extension
 * 2015 - Resurrected as standalone plProfiler by OpenSCG
 * 2016 - Rewritten as v2 to use shared hash tables, have lower overhead
 *			- v3 Major performance improvements, flame graph UI
 *
 * ----------------------------------------------------------------------
 */

#ifndef PLPROFILER_H
#define PLPROFILER_H

#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "access/hash.h"
#include "access/htup.h"

#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif

#include "access/sysattr.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "plpgsql.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

#define PL_PROFILE_COLS		5
#define PL_CALLGRAPH_COLS	5
#define PL_FUNCS_SRC_COLS	3

#define PL_MAX_STACK_DEPTH	200
#define PL_MIN_FUNCTIONS	2000
#define PL_MIN_CALLGRAPH	20000
#define PL_MIN_LINES		200000


#define PL_DBG_PRINT_STACK(_d, _s) do {	\
		int _i;						\
		printf("stack %s: db=%d bt=", _d, _s.db_oid); \
		for (_i = 0; _i < PL_MAX_STACK_DEPTH && _s.stack[_i] != InvalidOid; _i++) { \
		    printf("%d,", _s.stack[_i]); \
		} \
		printf("\n"); \
	} while(0);


/**********************************************************************
 * Type and structure definitions
 **********************************************************************/

/* ----
 * profilerLineInfo
 *
 * 	Per source code line stats kept in the profilerInfo below, which
 * 	is the data we put into the plugin_info of the executor state.
 * ----
 */
typedef struct
{
	int64				us_max;		/* Slowest iteration of this stmt */
	int64				us_total;	/* Total time spent executing this stmt */
	int64				exec_count;	/* Number of times we executed this stmt */
	instr_time			start_time;	/* Start time for this statement */
} profilerLineInfo;

/* ----
 * profilerInfo
 *
 * 	The information we keep in the estate->plugin_info.
 * ----
 */
typedef struct
{
	Oid					fn_oid;		/* The functions OID */
	int					line_count;	/* Number of lines in this function */
	profilerLineInfo   *line_info;	/* Performance counters for each line */
} profilerInfo;

/* ----
 * linestatsHashKey
 *
 * 	Hash key for the linestats hash tables (both local and shared).
 * ----
 */
typedef struct
{
	Oid					db_oid;		/* The OID of the database */
	Oid					fn_oid;		/* The OID of the function */
} linestatsHashKey;

/* ----
 * linestatsLineInfo
 *
 * 	Per source code line statistics kept in the linestats hash table.
 * ----
 */
typedef struct
{
	int64				us_max;		/* Maximum execution time of statement */
	int64				us_total;	/* Total sum of statement exec time */
	int64				exec_count;	/* Count of statement executions */
} linestatsLineInfo;

/* ----
 * linestatsEntry
 *
 * 	Per function data kept in the linestats hash table.
 * ----
 */
typedef struct
{
	linestatsHashKey	key;		/* hash key of entry */
	slock_t				mutex;		/* Spin lock for updating counters */
	int					line_count;	/* Number of lines in this function */
	linestatsLineInfo  *line_info;	/* Performance counters for each line */
} linestatsEntry;

typedef struct callGraphKey
{
	Oid				db_oid;
	Oid				stack[PL_MAX_STACK_DEPTH];
} callGraphKey;

typedef struct callGraphEntry
{
    callGraphKey	key;
	slock_t			mutex;
	PgStat_Counter	callCount;
	uint64			totalTime;
	uint64			childTime;
	uint64			selfTime;
} callGraphEntry;

typedef struct
{
	LWLockId			lock;
	bool				profiler_enabled_global;
	int					profiler_enabled_pid;
	int					profiler_collect_interval;
	bool				callgraph_overflow;
	bool				functions_overflow;
	bool				lines_overflow;
	int					lines_used;
	linestatsLineInfo	line_info[1];
} profilerSharedState;

/**********************************************************************
 * Exported function prototypes
 **********************************************************************/

void    _PG_init(void);
void    _PG_fini(void);

Datum pl_profiler_get_stack(PG_FUNCTION_ARGS);
Datum pl_profiler_linestats_local(PG_FUNCTION_ARGS);
Datum pl_profiler_linestats_shared(PG_FUNCTION_ARGS);
Datum pl_profiler_callgraph_local(PG_FUNCTION_ARGS);
Datum pl_profiler_callgraph_shared(PG_FUNCTION_ARGS);
Datum pl_profiler_func_oids_local(PG_FUNCTION_ARGS);
Datum pl_profiler_func_oids_shared(PG_FUNCTION_ARGS);
Datum pl_profiler_funcs_source(PG_FUNCTION_ARGS);
Datum pl_profiler_reset_local(PG_FUNCTION_ARGS);
Datum pl_profiler_reset_shared(PG_FUNCTION_ARGS);
Datum pl_profiler_set_enabled_global(PG_FUNCTION_ARGS);
Datum pl_profiler_get_enabled_global(PG_FUNCTION_ARGS);
Datum pl_profiler_set_enabled_local(PG_FUNCTION_ARGS);
Datum pl_profiler_get_enabled_local(PG_FUNCTION_ARGS);
Datum pl_profiler_set_enabled_pid(PG_FUNCTION_ARGS);
Datum pl_profiler_get_enabled_pid(PG_FUNCTION_ARGS);
Datum pl_profiler_set_collect_interval(PG_FUNCTION_ARGS);
Datum pl_profiler_get_collect_interval(PG_FUNCTION_ARGS);
Datum pl_profiler_collect_data(PG_FUNCTION_ARGS);
Datum pl_profiler_callgraph_overflow(PG_FUNCTION_ARGS);
Datum pl_profiler_functions_overflow(PG_FUNCTION_ARGS);
Datum pl_profiler_lines_overflow(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pl_profiler_get_stack);
PG_FUNCTION_INFO_V1(pl_profiler_linestats_local);
PG_FUNCTION_INFO_V1(pl_profiler_linestats_shared);
PG_FUNCTION_INFO_V1(pl_profiler_callgraph_local);
PG_FUNCTION_INFO_V1(pl_profiler_callgraph_shared);
PG_FUNCTION_INFO_V1(pl_profiler_func_oids_local);
PG_FUNCTION_INFO_V1(pl_profiler_func_oids_shared);
PG_FUNCTION_INFO_V1(pl_profiler_funcs_source);
PG_FUNCTION_INFO_V1(pl_profiler_reset_local);
PG_FUNCTION_INFO_V1(pl_profiler_reset_shared);
PG_FUNCTION_INFO_V1(pl_profiler_set_enabled_global);
PG_FUNCTION_INFO_V1(pl_profiler_get_enabled_global);
PG_FUNCTION_INFO_V1(pl_profiler_set_enabled_local);
PG_FUNCTION_INFO_V1(pl_profiler_get_enabled_local);
PG_FUNCTION_INFO_V1(pl_profiler_set_enabled_pid);
PG_FUNCTION_INFO_V1(pl_profiler_get_enabled_pid);
PG_FUNCTION_INFO_V1(pl_profiler_set_collect_interval);
PG_FUNCTION_INFO_V1(pl_profiler_get_collect_interval);
PG_FUNCTION_INFO_V1(pl_profiler_collect_data);
PG_FUNCTION_INFO_V1(pl_profiler_callgraph_overflow);
PG_FUNCTION_INFO_V1(pl_profiler_functions_overflow);
PG_FUNCTION_INFO_V1(pl_profiler_lines_overflow);

#endif /* PLPROFILER_H */
