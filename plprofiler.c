/*-------------------------------------------------------------------------
 *
 * plprofiler.c
 *
 *	  Profiling plugin for PL/pgSQL instrumentation
 *
 * Copyright (c) 2014-2016, BigSQL
 * Copyright (c) 2008-2014, PostgreSQL Global Development Group
 * Copyright 2006,2007 - EnterpriseDB, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

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

/* -------------------------------------------------------------------
 * stmt_stats
 * -------------------------------------------------------------------
 */
typedef struct
{
	int64			time_longest;	/* Slowest iteration of this stmt */
	int64			time_total;		/* Total time spent executing this stmt */
	PgStat_Counter	execCount;		/* Number of times we executed this stmt */
	instr_time		start_time;		/* Start time for this statement */
} stmt_stats;

typedef struct
{
	Oid				fn_oid;
	int				line_count;		/* Number of lines in this function */
	stmt_stats	   *stmtStats;		/* Performance counters for each line */
} profilerCtx;

typedef struct lineHashKey
{
	Oid				func_oid;
	int				line_number;
} lineHashKey;

typedef struct Counters
{
	int64			exec_count;
	int64			total_time;
	int64			time_longest;
	bool			isnew;
} Counters;

typedef struct lineEntry
{
	lineHashKey		key;			/* hash key of entry - MUST BE FIRST */
	Counters		counters;		/* the statistics for this line */
} lineEntry;

typedef struct callGraphKey
{
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

/*
 * Local variables
 */
static MemoryContext	profiler_mcxt = NULL;
static HTAB			   *line_stats = NULL;

static bool				profiler_enabled = false;
static int				profiler_enable_pid = 0;
static char			   *profiler_namespace = NULL;
static int				profiler_save_interval = 0;
static char			   *profiler_save_line_table = NULL;
static char			   *profiler_save_callgraph_table = NULL;

static HTAB			   *callGraph_stats = NULL;
static callGraphKey		graph_stack;
static instr_time		graph_stack_entry[PL_MAX_STACK_DEPTH];
static uint64			graph_stack_child_time[PL_MAX_STACK_DEPTH];
static int				graph_stack_pt = 0;
static TransactionId	graph_current_xid = InvalidTransactionId;
static time_t			last_save_time = 0;

PLpgSQL_plugin		  **plugin_ptr = NULL;

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

/**********************************************************************
 * Exported function prototypes
 **********************************************************************/

void load_plugin( PLpgSQL_plugin * hooks );

/**********************************************************************
 * Helper function prototypes
 **********************************************************************/
static void profiler_init(PLpgSQL_execstate *estate,
						  PLpgSQL_function * func);
static void profiler_func_beg(PLpgSQL_execstate *estate,
							  PLpgSQL_function *func);
static void profiler_func_end(PLpgSQL_execstate *estate,
							  PLpgSQL_function *func);
static void profiler_stmt_beg(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt);
static void profiler_stmt_end(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt);

static char *findSource(Oid oid, HeapTuple *tup, char **funcName);
static int scanSource(const char *src);
static void InitHashTable(void);
static uint32 line_hash_fn(const void *key, Size keysize);
static int line_match_fn(const void *key1, const void *key2, Size keysize);
static uint32 callGraph_hash_fn(const void *key, Size keysize);
static int callGraph_match_fn(const void *key1, const void *key2, Size keysize);
static lineEntry *entry_alloc(lineHashKey *key);
static void callGraph_push(Oid func_oid);
static void callGraph_pop_one(Oid func_oid);
static void callGraph_pop(Oid func_oid);
static void callGraph_check(Oid func_oid);
static void callGraph_collect(uint64 us_elapsed, uint64 us_self,
							  uint64 us_children);
static void profiler_enabled_assign(bool newval, void *extra);
static void profiler_collect_data(void);
static Oid get_extension_schema(Oid ext_oid);

/**********************************************************************
 * Exported Function definitions
 **********************************************************************/
static PLpgSQL_plugin plugin_funcs = {
		profiler_init,
		profiler_func_beg,
		profiler_func_end,
		profiler_stmt_beg,
		profiler_stmt_end
	};

void
_PG_init(void)
{
	DefineCustomBoolVariable("plprofiler.enabled",
							"Enable or disable plprofiler globally",
							NULL,
							&profiler_enabled,
							false,
							PGC_USERSET,
							0,
							NULL,
							profiler_enabled_assign,
							NULL);

	DefineCustomIntVariable("plprofiler.enable_pid",
							"Enable or disable plprofiler for a specific pid",
							NULL,
							&profiler_enable_pid,
							0,
							0,
							INT_MAX,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomIntVariable("plprofiler.save_interval",
							"Interval in seconds for saving profiler stats "
							"in the permanent tables.",
							NULL,
							&profiler_save_interval,
							0,
							0,
							3600,
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("plprofiler.save_linestats_table",
							"The table in which the pl_profiler_collect_data() "
							"function (and the interval saving) will insert "
							"per source line stats into",
							NULL,
							&profiler_save_line_table,
							"pl_profiler_linestats_data",
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	DefineCustomStringVariable("plprofiler.save_callgraph_table",
							"The table in which the pl_profiler_collect_data() "
							"function (and the interval saving) will insert "
							"call graph stats into",
							NULL,
							&profiler_save_callgraph_table,
							"pl_profiler_callgraph_data",
							PGC_USERSET,
							0,
							NULL,
							NULL,
							NULL);

	plugin_ptr = (PLpgSQL_plugin **)find_rendezvous_variable("PLpgSQL_plugin");

	*plugin_ptr = &plugin_funcs;
}

void
load_plugin(PLpgSQL_plugin *hooks)
{
	hooks->func_setup = profiler_init;
	hooks->func_beg	  = profiler_func_beg;
	hooks->func_end	  = profiler_func_end;
	hooks->stmt_beg	  = profiler_stmt_beg;
	hooks->stmt_end	  = profiler_stmt_end;
}

/**********************************************************************
 * Hook functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * profiler_init()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a
 *	new function is about to start.	 Specifically, this instrumentation
 *	hook is called after the stack frame has been created, but before
 *	values are assigned to the local variables.
 *
 *	'estate' points to the stack frame for this function, 'func'
 *	points to the definition of the function
 *
 *	We use this hook to load the source code for the function that's
 *	being invoked and to set up our context structures
 * -------------------------------------------------------------------
 */
static void
profiler_init(PLpgSQL_execstate *estate, PLpgSQL_function *func )
{
	HeapTuple		procTuple;
	char		   *funcName;
	profilerCtx	   *profilerInfo;
	char		   *procSrc;

	/*
	 * Anonymous code blocks do not have function source code
	 * that we can lookup in pg_proc. For now we ignore them.
	 */
	if (func->fn_oid == InvalidOid)
		return;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
	{
		/*
		 * The profiler can be enabled/disabled via changing postgresql.conf
		 * and reload (SIGHUP). The change becomes visible in backends the
		 * next time, the TCOP loop is ready for a new client query. This
		 * allows to enable the profiler for some time, have it save the
		 * stats in the permanent tables, then turn it off again. At that
		 * moment, we want to release all profiler resources.
		 */
		if (line_stats != NULL)
		{
			MemoryContextDelete(profiler_mcxt);
			profiler_mcxt = NULL;
			line_stats = NULL;
			callGraph_stats = NULL;
		}
		return;
	}

	/*
	 * The PL/pgSQL interpreter provides a void pointer (in each stack frame)
	 * that's reserved for plugins.	 We allocate a profilerCtx structure and
	 * record it's address in that pointer so we can keep some per-invocation
	 * information.
	 */
	profilerInfo = (profilerCtx *)palloc(sizeof(profilerCtx ));
	estate->plugin_info = profilerInfo;

	/* Allocate enough space to hold and offset for each line of source code */
	procSrc = findSource( func->fn_oid, &procTuple, &funcName );

	profilerInfo->fn_oid = func->fn_oid;
	profilerInfo->line_count = scanSource(procSrc);
	profilerInfo->stmtStats = palloc0((profilerInfo->line_count + 1) *
									  sizeof(stmt_stats));

	ReleaseSysCache(procTuple);
}

/* -------------------------------------------------------------------
 * profiler_func_beg()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a
 *	new function is starting.  Specifically, this instrumentation
 *	hook is called after values have been assigned to all local
 *	variables (and all function parameters).
 *
 *	'estate' points to the stack frame for this function, 'func'
 *	points to the definition of the function
 * -------------------------------------------------------------------
 */
static void
profiler_func_beg(PLpgSQL_execstate *estate, PLpgSQL_function *func)
{
	TransactionId	current_xid;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	/*
	 * At entry time of a function we push it onto the graph call stack
	 * and remember the entry time.
	 */
	current_xid = GetTopTransactionId();

	if (graph_current_xid != current_xid && graph_stack_pt > 0)
	{
		/*
		 * We have a call stack but it started in another transaction.
		 * This only happens when a transaction aborts and the call stack
		 * is not properly unwound down to zero depth. Unwind it here.
		 */
		elog(DEBUG1, "plprofiler: stale call stack reset");
		while (graph_stack_pt > 0)
			callGraph_pop_one(func->fn_oid);
	}
	graph_current_xid = current_xid;

	/*
	 * Push this function Oid onto the stack, remember the entry time and
	 * set the time spent in children to zero.
	 */
	callGraph_push(func->fn_oid);
}

/* -------------------------------------------------------------------
 * profiler_func_end()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a
 *	function runs to completion.
 * -------------------------------------------------------------------
 */
static void
profiler_func_end(PLpgSQL_execstate *estate, PLpgSQL_function *func)
{
	profilerCtx	   *profilerInfo;
	int				lineNo;
	lineHashKey		key;
	lineEntry	   *entry;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	if (!line_stats)
		InitHashTable();

	profilerInfo = (profilerCtx *) estate->plugin_info;

	/* Loop through each line of source code and update the stats */
	for(lineNo = 0; lineNo < profilerInfo->line_count; lineNo++)
	{
		stmt_stats *stats = profilerInfo->stmtStats + (lineNo + 1);

		/* Set up key for hashtable search */
		key.func_oid = func->fn_oid;
		key.line_number = (int32)(lineNo + 1);

		entry = (lineEntry *)hash_search(line_stats, &key, HASH_FIND, NULL);

		if (!entry)
		{
			entry = entry_alloc(&key);
			if (!entry)
			{
				elog(ERROR, "Unable to allocate more space for the profiler. ");
				return;
			}
		}

		entry->counters.exec_count += stats->execCount;
		entry->counters.total_time += stats->time_total;

		if (stats->time_longest > entry->counters.time_longest)
			entry->counters.time_longest = stats->time_longest;

		entry = NULL;
	}

	/*
	 * Pop the call stack. This also does the time accounting
	 * for call graphs.
	 */
	callGraph_pop(func->fn_oid);

	/*
	 * Finally if a plprofiler.save_interval is configured, save and reset
	 * the stats if the interval has elapsed.
	 */
	if ((profiler_enabled || MyProcPid == profiler_enable_pid)
			&& profiler_save_interval > 0)
	{
		time_t	now = time(NULL);

		if (now >= last_save_time + profiler_save_interval)
		{
		    profiler_collect_data();
			last_save_time = now;
		}
	}
}

/* -------------------------------------------------------------------
 * profiler_stmt_beg()
 *
 *	This hook function is called by the PL/pgSQL interpreter just before
 *	executing a statement (stmt).
 *
 *	Prior to executing each statement, we record the current time and
 *	the current values of all of the performance counters.
 * -------------------------------------------------------------------
 */
static void
profiler_stmt_beg(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt)
{
	stmt_stats	   *stats;
	profilerCtx	   *profilerInfo;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	profilerInfo = (profilerCtx *)estate->plugin_info;
	stats = profilerInfo->stmtStats + stmt->lineno;

	/* Set the start time of the statement */
	INSTR_TIME_SET_CURRENT(stats->start_time);

	/* Check the call graph stack. */
	callGraph_check(profilerInfo->fn_oid);
}

/* -------------------------------------------------------------------
 * profiler_stmt_end()
 *
 *	This hook function is called by the PL/pgSQL interpreter just after
 *	it executes a statement (stmt).
 *
 *	We use this hook to 'delta' the before and after performance counters
 *	and record the differences in the stmt_stats structure associated
 *	with this statement.
 * -------------------------------------------------------------------
 */
static void
profiler_stmt_end(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt)
{
	stmt_stats	   *stats;
	profilerCtx	   *profilerInfo;
	instr_time		end_time;
	uint64			elapsed;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	INSTR_TIME_SET_CURRENT(end_time);

	profilerInfo = (profilerCtx *)estate->plugin_info;
	stats = profilerInfo->stmtStats + stmt->lineno;

	INSTR_TIME_SUBTRACT(end_time, stats->start_time);

	elapsed = INSTR_TIME_GET_MICROSEC(end_time);

	if (elapsed > stats->time_longest)
		stats->time_longest = elapsed;

	stats->time_total += elapsed;

	stats->execCount++;
}

/**********************************************************************
 * Helper functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * findSource()
 *
 *	This function locates and returns a pointer to a null-terminated string
 *	that contains the source code for the given function.
 *
 *	In addition to returning a pointer to the requested source code, this
 *	function sets *tup to point to a HeapTuple (that you must release when
 *	you are finished with it) and sets *funcName to point to the name of
 *	the given function.
 * -------------------------------------------------------------------
 */
static char *
findSource(Oid oid, HeapTuple *tup, char **funcName)
{
	bool	isNull;

	*tup = SearchSysCache(PROCOID, ObjectIdGetDatum(oid), 0, 0, 0);

	if(!HeapTupleIsValid(*tup))
		elog(ERROR, "plprofiler: cache lookup for function %u failed", oid);

	if (funcName != NULL)
		*funcName = NameStr(((Form_pg_proc)GETSTRUCT(*tup))->proname);

	return DatumGetCString(DirectFunctionCall1(textout,
											   SysCacheGetAttr(PROCOID,
															   *tup,
															   Anum_pg_proc_prosrc,
															   &isNull)));
}

/* -------------------------------------------------------------------
 * scanSource()
 *
 *	This function scans through the source code for a given function
 *	and counts the number of lines of code present in the string.
 * -------------------------------------------------------------------
 */
static int
scanSource(const char *src)
{
	int			line_count = 1;
	const char *cp = src;

	while(cp != NULL)
	{
		line_count++;
		cp = strchr(cp, '\n');
		if (cp)
			cp++;
	}

	return line_count;
}

/* -------------------------------------------------------------------
 * InitHashTable()
 *
 * Initialize hash table
 * -------------------------------------------------------------------
 */
static void
InitHashTable(void)
{
	HASHCTL		hash_ctl;

	/* Create the memory context for our data */
	profiler_mcxt = AllocSetContextCreate(TopMemoryContext,
										  "PL/pgSQL profiler",
										  ALLOCSET_DEFAULT_MINSIZE,
										  ALLOCSET_DEFAULT_INITSIZE,
										  ALLOCSET_DEFAULT_MAXSIZE);

	/* Create the hash table for line stats */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = sizeof(lineHashKey);
	hash_ctl.entrysize = sizeof(lineEntry);
	hash_ctl.hash = line_hash_fn;
	hash_ctl.match = line_match_fn;
	hash_ctl.hcxt = profiler_mcxt;

	line_stats = hash_create("Function Lines",
				 10000,
				 &hash_ctl,
				 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	/* Create the hash table for call stats */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = sizeof(callGraphKey);
	hash_ctl.entrysize = sizeof(callGraphEntry);
	hash_ctl.hash = callGraph_hash_fn;
	hash_ctl.match = callGraph_match_fn;
	hash_ctl.hcxt = profiler_mcxt;

	callGraph_stats = hash_create("Function Call Graphs",
				 1000,
				 &hash_ctl,
				 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
}

static uint32
line_hash_fn(const void *key, Size keysize)
{
	const lineHashKey *k = (const lineHashKey *) key;

	return hash_uint32((uint32) k->func_oid) ^
		hash_uint32((uint32) k->line_number);
}

static int
line_match_fn(const void *key1, const void *key2, Size keysize)
{
	const lineHashKey  *k1 = (const lineHashKey *)key1;
	const lineHashKey  *k2 = (const lineHashKey *)key2;

	if (k1->func_oid == k2->func_oid &&
		k1->line_number == k2->line_number)
		return 0;
	else
		return 1;
}

static uint32
callGraph_hash_fn(const void *key, Size keysize)
{
	return hash_any(key, keysize);
}

static int
callGraph_match_fn(const void *key1, const void *key2, Size keysize)
{
	callGraphKey   *stack1 = (callGraphKey *)key1;
	callGraphKey   *stack2 = (callGraphKey *)key2;
	int				i;

	for (i = 0; i < PL_MAX_STACK_DEPTH && stack1->stack[i] != InvalidOid; i++)
		if (stack1->stack[i] != stack2->stack[i])
			return 1;
	return 0;
}

static void
callGraph_push(Oid func_oid)
{
	/*
	 * We only track function Oids in the call stack up to PL_MAX_STACK_DEPTH.
	 * Beyond that we just count the current stack depth.
	 */
	if (graph_stack_pt < PL_MAX_STACK_DEPTH)
	{
		/*
		 * Push this function Oid onto the stack, remember the entry time and
		 * set the time spent in children to zero.
		 */
		graph_stack.stack[graph_stack_pt] = func_oid;
		INSTR_TIME_SET_CURRENT(graph_stack_entry[graph_stack_pt]);
		graph_stack_child_time[graph_stack_pt] = 0;
	}
	graph_stack_pt++;
}

static void
callGraph_pop_one(Oid func_oid)
{
	instr_time		now;
	uint64			us_elapsed;
	uint64			us_self;
	lineHashKey		key;
	lineEntry	   *entry;

	/* Check for call stack underrun. */
    if (graph_stack_pt <= 0)
	{
		elog(DEBUG1, "plprofiler: call graph stack underrun");
		return;
	}

	/* Remove one level from the call stack. */
	graph_stack_pt--;

	/* Calculate the time spent in this function and record it. */
	INSTR_TIME_SET_CURRENT(now);
	INSTR_TIME_SUBTRACT(now, graph_stack_entry[graph_stack_pt]);
	us_elapsed = INSTR_TIME_GET_MICROSEC(now);
	us_self = us_elapsed - graph_stack_child_time[graph_stack_pt];
	callGraph_collect(us_elapsed, us_self,
					  graph_stack_child_time[graph_stack_pt]);

	/* If we have a caller, add our own time to the time of its children. */
	if (graph_stack_pt > 0)
		graph_stack_child_time[graph_stack_pt - 1] += us_elapsed;

	/* Zap the oid from the call stack. */
	graph_stack.stack[graph_stack_pt] = InvalidOid;

	/*
	 * We also collect per function global counts in the pseudo line number
	 * zero. The line stats are cumulative (for example a FOR ... LOOP
	 * statement has the entire execution time of all statements in its
	 * block), so this can't be derived from the actual per line data.
	 */
	key.func_oid = func_oid;
	key.line_number = 0;

	entry = (lineEntry *)hash_search(line_stats, &key, HASH_FIND, NULL);

	if (!entry)
	{
		entry = entry_alloc(&key);
		if (!entry)
		{
			elog(ERROR, "Unable to allocate more space for the profiler. ");
			return;
		}
	}

	entry->counters.exec_count += 1;
	entry->counters.total_time += us_elapsed;

	if (us_elapsed > entry->counters.time_longest)
		entry->counters.time_longest = us_elapsed;

	entry = NULL;
}

static void
callGraph_pop(Oid func_oid)
{
	callGraph_check(func_oid);
	callGraph_pop_one(func_oid);
}

static void
callGraph_check(Oid func_oid)
{
	/*
	 * Unwind the call stack until our own func_oid appears on the top.
	 *
	 * In case of an exception, the pl executor does not call the
	 * func_end callback, so we record now as the end of the function
	 * calls, that were left on the stack.
	 */
	while (graph_stack_pt > 0
		   && graph_stack.stack[graph_stack_pt - 1] != func_oid)
	{
		elog(DEBUG1, "plprofiler: unwinding excess call graph stack entry for %u in %u",
			 graph_stack.stack[graph_stack_pt - 1], func_oid);
		callGraph_pop_one(func_oid);
	}
}

static void
callGraph_collect(uint64 us_elapsed, uint64 us_self, uint64 us_children)
{
	callGraphEntry *entry;
	bool			found;

	entry = (callGraphEntry *)hash_search(callGraph_stats, &graph_stack,
										  HASH_ENTER, &found);

	if (!found)
	{
		entry->callCount = 1;
		entry->totalTime = us_elapsed;
		entry->childTime = us_children;
		entry->selfTime = us_self;
	}
	else
	{
		entry->callCount++;
		entry->totalTime = entry->totalTime + us_elapsed;
		entry->childTime = entry->childTime + us_children;
		entry->selfTime  = entry->selfTime + us_self;
	}
}

static lineEntry *
entry_alloc(lineHashKey *key)
{
	lineEntry	   *entry;
	bool			found;

	/* Find or create an entry with desired hash code */
	entry = (lineEntry *)hash_search(line_stats, key, HASH_ENTER, &found);

	if (!found)
	{
		/* New entry, initialize it */
		MemSet(&entry->counters, 0, sizeof(Counters));
		entry->counters.isnew = true;
	}

	return entry;
}

static void
profiler_enabled_assign(bool newval, void *extra)
{
	profiler_enabled = newval;
	if (newval && !line_stats)
		InitHashTable();
}

static void
profiler_collect_data(void)
{
	SPI_push();
	DirectFunctionCall1(pl_profiler_collect_data, (Datum)0);
	SPI_pop();
}

/* No idea why this one is static in extensions.c - Jan */
/*
 * get_extension_schema - given an extension OID, fetch its extnamespace
 *
 * Returns InvalidOid if no such extension.
 */
static Oid
get_extension_schema(Oid ext_oid)
{
	Oid			result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ext_oid));

#if PG_VERSION_NUM < 90400
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  SnapshotNow, 1, entry);
#else
	scandesc = systable_beginscan(rel, ExtensionOidIndexId, true,
								  NULL, 1, entry);
#endif

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = ((Form_pg_extension) GETSTRUCT(tuple))->extnamespace;
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return result;
}

/**********************************************************************
 * SQL callable functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * pl_profiler_get_stack(stack oid[])
 *
 *	Converts a stack in Oid[] format into a text[].
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_get_stack(PG_FUNCTION_ARGS)
{
	ArrayType	   *stack_in = PG_GETARG_ARRAYTYPE_P(0);
	Datum		   *stack_oid;
	bool		   *nulls;
	int				nelems;
	int				i;
	Datum		   *funcdefs;
	char			funcdef_buf[100 + NAMEDATALEN * 2];

	/* Take the array apart */
	deconstruct_array(stack_in, OIDOID,
					  sizeof(Oid), true, 'i',
					  &stack_oid, &nulls, &nelems);

	/* Allocate the Datum array for the individual function signatures. */
	funcdefs = palloc(sizeof(Datum) * nelems);

	/*
	 * Turn each of the function Oids, that are in the array, into
	 * a text that is "schema.funcname(funcargs) oid=funcoid".
	 */
	for (i = 0; i < nelems; i++)
	{
		char	   *funcname;
		char	   *nspname;

		funcname = get_func_name(DatumGetObjectId(stack_oid[i]));
		if (funcname != NULL)
		{
			nspname = get_namespace_name(get_func_namespace(DatumGetObjectId(stack_oid[i])));
			if (nspname == NULL)
				nspname = pstrdup("<unknown>");
		}
		else
		{
			nspname = pstrdup("<unknown>");
			funcname = pstrdup("<unknown>");
		}

		snprintf(funcdef_buf, sizeof(funcdef_buf),
				 "%s.%s() oid=%u", nspname, funcname,
				 DatumGetObjectId(stack_oid[i]));

		pfree(nspname);
		pfree(funcname);

		funcdefs[i] = PointerGetDatum(cstring_to_text(funcdef_buf));
	}

	/* Return the texts as a text[]. */
	PG_RETURN_ARRAYTYPE_P(PointerGetDatum(construct_array(funcdefs, nelems,
														  TEXTOID, -1,
														  false, 'i')));
}

/* -------------------------------------------------------------------
 * pl_profiler_linestats()
 *
 *	Returns the current content of the line stats hash table
 *	as a set of rows.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_linestats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	bool				filter_zero = PG_GETARG_BOOL(0);
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	HASH_SEQ_STATUS		hash_seq;
	lineEntry		   *entry;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context "
						"that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (line_stats != NULL)
	{
		hash_seq_init(&hash_seq, line_stats);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			Datum		values[PL_PROFILE_COLS];
			bool		nulls[PL_PROFILE_COLS];
			int			i = 0;

			/*
			 * If filter_zero is true we are called from
			 * pl_profiler_collect_data() to save the current hash table
			 * data into pl_profiler_linestats_data. We filter out
			 * rows that are not new and have a zero exec_count.
			 */
			if (filter_zero)
			{
				if (entry->counters.isnew)
				{
					entry->counters.isnew = false;
				}
				else
				{
					if (entry->counters.exec_count == 0 &&
						entry->counters.total_time == 0)
						continue;
				}
			}

			/* Include this entry in the result. */
			MemSet(values, 0, sizeof(values));
			MemSet(nulls, 0, sizeof(nulls));

			values[i++] = ObjectIdGetDatum(entry->key.func_oid);
			values[i++] = Int64GetDatumFast(entry->key.line_number);
			values[i++] = Int64GetDatumFast(entry->counters.exec_count);
			values[i++] = Int64GetDatumFast(entry->counters.total_time);
			values[i++] = Int64GetDatumFast(entry->counters.time_longest);

			Assert(i == PL_PROFILE_COLS);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum)0;
}

/* -------------------------------------------------------------------
 * pl_profiler_callgraph()
 *
 *	Returns the current content of the call graph hash table
 *	as a set of rows.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_callgraph(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	bool				filter_zero = PG_GETARG_BOOL(0);
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	HASH_SEQ_STATUS		hash_seq;
	callGraphEntry	   *entry;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context "
						"that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (callGraph_stats != NULL)
	{
		hash_seq_init(&hash_seq, callGraph_stats);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			Datum		values[PL_CALLGRAPH_COLS];
			bool		nulls[PL_CALLGRAPH_COLS];
			Datum		funcdefs[PL_MAX_STACK_DEPTH];

			int			i = 0;
			int			j = 0;

			/*
			 * Filter out call stacks that have not been seen since
			 * the last collect_data() call.
			 */
			if (filter_zero && entry->callCount == 0)
				continue;

			MemSet(values, 0, sizeof(values));
			MemSet(nulls, 0, sizeof(nulls));

			for (i = 0; i < PL_MAX_STACK_DEPTH && entry->key.stack[i] != InvalidOid; i++)
				funcdefs[i] = ObjectIdGetDatum(entry->key.stack[i]);

			values[j++] = PointerGetDatum(construct_array(funcdefs, i,
														  OIDOID, sizeof(Oid),
														  true, 'i'));
			values[j++] = Int64GetDatumFast(entry->callCount);
			values[j++] = Int64GetDatumFast(entry->totalTime);
			values[j++] = Int64GetDatumFast(entry->childTime);
			values[j++] = Int64GetDatumFast(entry->selfTime);

			Assert(j == PL_CALLGRAPH_COLS);

			tuplestore_putvalues(tupstore, tupdesc, values, nulls);
		}
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum)0;
}

/* -------------------------------------------------------------------
 * pl_profiler_func_oids_current()
 *
 *	Returns an array of all function Oids that we currently have
 *	linestat information for.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_func_oids_current(PG_FUNCTION_ARGS)
{
	int					i = 0;
	Datum			   *result;
	HASH_SEQ_STATUS		hash_seq;
	lineEntry		   *entry;

	/* First pass to count the number of Oids, we will return. */

	if (line_stats != NULL)
	{
		hash_seq_init(&hash_seq, line_stats);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			if (entry->key.line_number == 0)
				i++;
		}
	}

	/* Allocate Oid array for result. */
	if (i == 0)
	{
		result = palloc(sizeof(Datum));
	}
	else
	{
		result = palloc(sizeof(Datum) * i);
	}
	if (result == NULL)
		elog(ERROR, "out of memory in pl_profiler_func_oids_current()");

	/* Second pass to collect the Oids. */
	if (line_stats != NULL)
	{
		i = 0;
		hash_seq_init(&hash_seq, line_stats);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			if (entry->key.line_number == 0)
				result[i++] = ObjectIdGetDatum(entry->key.func_oid);
		}
	}

	/* Build and return the actual array. */
	PG_RETURN_ARRAYTYPE_P(PointerGetDatum(construct_array(result, i,
														  OIDOID, sizeof(Oid),
														  true, 'i')));
}

/* -------------------------------------------------------------------
 * pl_profiler_funcs_source(func_oids oid[])
 *
 *	Return the source code of a number of functions specified by
 *	an input array of Oids.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_funcs_source(PG_FUNCTION_ARGS)
{
	ArrayType		   *func_oids_in = PG_GETARG_ARRAYTYPE_P(0);
	Datum			   *func_oids;
	bool			   *nulls;
	int					nelems;
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	int					fidx;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context "
						"that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not "
						"allowed in this context")));

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	/* Take the input array apart */
	deconstruct_array(func_oids_in, OIDOID,
					  sizeof(Oid), true, 'i',
					  &func_oids, &nulls, &nelems);

	/*
	 * Turn each of the function Oids, that are in the array, into
	 * a text that is "schema.funcname(funcargs) oid=funcoid".
	 */
	for (fidx = 0; fidx < nelems; fidx++)
	{
		HeapTuple	procTuple;
		char	   *procSrc;
		char	   *funcName;
		Datum		values[PL_FUNCS_SRC_COLS];
		bool		nulls[PL_FUNCS_SRC_COLS];
		int			i = 0;
		char	   *cp;
		char	   *linestart;
		int64		line_number = 1;

		/* Create the line-0 entry. */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[i++] = func_oids[fidx];
		values[i++] = (Datum)0;
		values[i++] = PointerGetDatum(cstring_to_text("-- Line 0"));

		Assert(i == PL_FUNCS_SRC_COLS);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		/* Find the source code and split it. */
		procSrc = findSource(func_oids[fidx], &procTuple, &funcName);
		if (procSrc == NULL)
		{
			ReleaseSysCache(procTuple);
			continue;
		}

		/*
		 * The returned procStr is palloc'd, so it is safe to scribble
		 * around in it.
		 */
		cp = procSrc;
		linestart = procSrc;
		while (cp != NULL)
		{
			cp = strchr(cp, '\n');
			if (cp != NULL)
				*cp++ = '\0';
			i = 0;
			values[i++] = func_oids[fidx];
			values[i++] = Int64GetDatumFast(line_number++);
			values[i++] = PointerGetDatum(cstring_to_text(linestart));

			Assert(i == PL_FUNCS_SRC_COLS);
			tuplestore_putvalues(tupstore, tupdesc, values, nulls);

			linestart = cp;
		}

		ReleaseSysCache(procTuple);
		pfree(procSrc);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum)0;
}

/* -------------------------------------------------------------------
 * pl_profiler_reset()
 *
 *	Drop all current data collected in the hash tables.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_reset(PG_FUNCTION_ARGS)
{
	if (!line_stats || !profiler_mcxt)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("pl_profiler must be loaded")));

	MemoryContextDelete(profiler_mcxt);
	profiler_mcxt = NULL;
	line_stats = NULL;
	callGraph_stats = NULL;

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_enable()
 *
 *	Turn profiling on or off.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_enable(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	profiler_enabled = PG_GETARG_BOOL(0);

	if (profiler_enabled && !line_stats)
		InitHashTable();

	PG_RETURN_BOOL(profiler_enabled);
}

/* -------------------------------------------------------------------
 * pl_profiler_collect_data()
 *
 *	Save the current content of the hash tables into the configured
 *	permanent tables and reset the counters.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_collect_data(PG_FUNCTION_ARGS)
{
	HASH_SEQ_STATUS		hash_seq;
	lineEntry		   *lineEnt;
	callGraphEntry	   *callGraphEnt;
	char				query[256 + NAMEDATALEN * 2];
	int32				result = 0;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect() failed in pl_profiler_collect_data()");

	/*
	 * If not done yet, figure out which namespace the plprofiler
	 * extension is installed in.
	 */
	if (profiler_namespace == NULL)
	{
		MemoryContext	oldcxt;

		/* Figure out our extension installation schema. */
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		profiler_namespace = get_namespace_name(get_extension_schema(
							 get_extension_oid("plprofiler", false)));
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * If we have the hash table for line stats and the table name
	 * to save them into is configured, do that and reset all counters
	 * (but don't just zap the hash table as rebuilding that is rather
	 * costly).
	 */
	if (line_stats != NULL && profiler_save_line_table != NULL)
	{
		snprintf(query, sizeof(query),
				 "INSERT INTO \"%s\".\"%s\" "
				 "    SELECT func_oid, line_number, exec_count, total_time, "
				 "			 longest_time "
				 "    FROM \"%s\".pl_profiler_linestats(true)",
				 profiler_namespace, profiler_save_line_table,
				 profiler_namespace);
		SPI_exec(query, 0);

		hash_seq_init(&hash_seq, line_stats);
		while ((lineEnt = hash_seq_search(&hash_seq)) != NULL)
			MemSet(&(lineEnt->counters), 0, sizeof(Counters));

		result += SPI_processed;
	}

	/* Same with call graph stats. */
	if (callGraph_stats != NULL && profiler_save_callgraph_table != NULL)
	{
		snprintf(query, sizeof(query),
				 "INSERT INTO \"%s\".\"%s\" "
				 "    SELECT stack, call_count, us_total, "
				 "           us_children, us_self "
				 "    FROM \"%s\".pl_profiler_callgraph(true)",
				 profiler_namespace, profiler_save_callgraph_table,
				 profiler_namespace);
		SPI_exec(query, 0);

		hash_seq_init(&hash_seq, callGraph_stats);
		while ((callGraphEnt = hash_seq_search(&hash_seq)) != NULL)
		{
			callGraphEnt->callCount = 0;
			callGraphEnt->totalTime = 0;
			callGraphEnt->childTime = 0;
			callGraphEnt->selfTime = 0;
		}

		result += SPI_processed;
	}

	SPI_finish();

	PG_RETURN_INT32(result);
}

