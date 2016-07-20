/* ----------------------------------------------------------------------
 *
 * plprofiler.h
 *
 *	  Declarations for profiling plugin for PL/pgSQL instrumentation
 *
 * Copyright (c) 2014-2016, BigSQL
 * Copyright (c) 2008-2014, PostgreSQL Global Development Group
 * Copyright 2006,2007 - EnterpriseDB, Inc.
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
#if PG_VERSION_NUM < 90400
#include "access/transam.h"
#include "utils/tqual.h"
#endif

#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_extension.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "access/sysattr.h"
#include "commands/extension.h"
#include "plpgsql.h"
#include "pgstat.h"

PG_MODULE_MAGIC;

#define PL_PROFILE_COLS		5
#define PL_CALLGRAPH_COLS	5
#define PL_FUNCS_SRC_COLS	3
#define PL_MAX_STACK_DEPTH	100

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
	PgStat_Counter	callCount;
	uint64			totalTime;
	uint64			childTime;
	uint64			selfTime;
} callGraphEntry;

/**********************************************************************
 * Exported function prototypes
 **********************************************************************/

void    _PG_init(void);
void    _PG_fini(void);

Datum pl_profiler_get_stack(PG_FUNCTION_ARGS);
Datum pl_profiler_linestats(PG_FUNCTION_ARGS);
Datum pl_profiler_callgraph(PG_FUNCTION_ARGS);
Datum pl_profiler_func_oids_current(PG_FUNCTION_ARGS);
Datum pl_profiler_funcs_source(PG_FUNCTION_ARGS);
Datum pl_profiler_reset(PG_FUNCTION_ARGS);
Datum pl_profiler_enable(PG_FUNCTION_ARGS);
Datum pl_profiler_collect_data(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pl_profiler_get_stack);
PG_FUNCTION_INFO_V1(pl_profiler_linestats);
PG_FUNCTION_INFO_V1(pl_profiler_callgraph);
PG_FUNCTION_INFO_V1(pl_profiler_func_oids_current);
PG_FUNCTION_INFO_V1(pl_profiler_funcs_source);
PG_FUNCTION_INFO_V1(pl_profiler_reset);
PG_FUNCTION_INFO_V1(pl_profiler_enable);
PG_FUNCTION_INFO_V1(pl_profiler_collect_data);

#endif /* PLPROFILER_H */
