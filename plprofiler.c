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

#include "plprofiler.h"

/**********************************************************************
 * PL executor callback function prototypes
 **********************************************************************/

static void profiler_func_init(PLpgSQL_execstate *estate,
						  PLpgSQL_function * func);
static void profiler_func_beg(PLpgSQL_execstate *estate,
							  PLpgSQL_function *func);
static void profiler_func_end(PLpgSQL_execstate *estate,
							  PLpgSQL_function *func);
static void profiler_stmt_beg(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt);
static void profiler_stmt_end(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt);

/**********************************************************************
 * Local function prototypes
 **********************************************************************/
static char *find_source(Oid oid, HeapTuple *tup, char **funcName);
static int count_source_lines(const char *src);
static void init_hash_tables(void);
static uint32 line_hash_fn(const void *key, Size keysize);
static int line_match_fn(const void *key1, const void *key2, Size keysize);
static uint32 callgraph_hash_fn(const void *key, Size keysize);
static int callgraph_match_fn(const void *key1, const void *key2, Size keysize);
static void callgraph_push(Oid func_oid);
static void callgraph_pop_one(Oid func_oid);
static void callgraph_pop(Oid func_oid);
static void callgraph_check(Oid func_oid);
static void callgraph_collect(uint64 us_elapsed, uint64 us_self,
							  uint64 us_children);
static void profiler_enabled_assign(bool newval, void *extra);
static void profiler_collect_data(void);
static Oid get_extension_schema(Oid ext_oid);

/**********************************************************************
 * Local variables
 **********************************************************************/

static MemoryContext	profiler_mcxt = NULL;
static HTAB			   *linestats_hash = NULL;
static HTAB			   *callgraph_hash = NULL;

static bool				profiler_enabled = false;
static int				profiler_enable_pid = 0;
static char			   *profiler_namespace = NULL;
static int				profiler_save_interval = 0;
static char			   *profiler_save_line_table = NULL;
static char			   *profiler_save_callgraph_table = NULL;

static callGraphKey		graph_stack;
static instr_time		graph_stack_entry[PL_MAX_STACK_DEPTH];
static uint64			graph_stack_child_time[PL_MAX_STACK_DEPTH];
static int				graph_stack_pt = 0;
static TransactionId	graph_current_xid = InvalidTransactionId;
static time_t			last_save_time = 0;

PLpgSQL_plugin		   *prev_plpgsql_plugin = NULL;
PLpgSQL_plugin		   *prev_pltsql_plugin = NULL;

static PLpgSQL_plugin	plugin_funcs = {
		profiler_func_init,
		profiler_func_beg,
		profiler_func_end,
		profiler_stmt_beg,
		profiler_stmt_end,
		NULL,
		NULL
	};

/**********************************************************************
 * Extension (de)initialization functions.
 **********************************************************************/

void
_PG_init(void)
{
	PLpgSQL_plugin **plugin_ptr;

	/* Register our custom configuration variables. */

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

	/* Link us into the PL/pgSQL executor. */
	plugin_ptr = (PLpgSQL_plugin **)find_rendezvous_variable("PLpgSQL_plugin");
	prev_plpgsql_plugin = *plugin_ptr;
	*plugin_ptr = &plugin_funcs;

	/* Link us into the PL/TSQL executor. */
	plugin_ptr = (PLpgSQL_plugin **)find_rendezvous_variable("PLTSQL_plugin");
	prev_pltsql_plugin = *plugin_ptr;
	*plugin_ptr = &plugin_funcs;

	/* Initialize local hash tables. */
	init_hash_tables();

	/* Set the database OID in the local call graph stack hash key. */
	graph_stack.db_oid = MyDatabaseId;
}

void
_PG_fini(void)
{
	PLpgSQL_plugin **plugin_ptr;

	plugin_ptr = (PLpgSQL_plugin **)find_rendezvous_variable("PLpgSQL_plugin");
	*plugin_ptr = prev_plpgsql_plugin;
	prev_plpgsql_plugin = NULL;

	plugin_ptr = (PLpgSQL_plugin **)find_rendezvous_variable("PLTSQL_plugin");
	*plugin_ptr = prev_pltsql_plugin;
	prev_pltsql_plugin = NULL;

	MemoryContextDelete(profiler_mcxt);
	profiler_mcxt = NULL;
	linestats_hash = NULL;
	callgraph_hash = NULL;
}

/**********************************************************************
 * Hook functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * profiler_func_init()
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
profiler_func_init(PLpgSQL_execstate *estate, PLpgSQL_function *func )
{
	profilerInfo	   *profiler_info;
	linestatsHashKey	linestats_key;
	linestatsEntry	   *linestats_entry;
	bool				linestats_found;

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
		if (linestats_hash != NULL)
			init_hash_tables();
		return;
	}

	/*
	 * Anonymous code blocks do not have function source code
	 * that we can lookup in pg_proc. For now we ignore them.
	 */
	if (func->fn_oid == InvalidOid)
		return;


	/*
	 * Search for this function in our line stats hash table. Create the
	 * entry if it does not exist yet.
	 */
	linestats_key.db_oid = MyDatabaseId;
	linestats_key.fn_oid = func->fn_oid;

	linestats_entry = (linestatsEntry *)hash_search(linestats_hash,
													&linestats_key,
													HASH_ENTER,
													&linestats_found);
	if (linestats_entry == NULL)
		elog(ERROR, "plprofiler out of memory");
	if (!linestats_found)
	{
		/* New function, initialize entry. */
		MemoryContext	old_context;
		HeapTuple		proc_tuple;
		char		   *proc_src;
		char		   *func_name;

		proc_src = find_source( func->fn_oid, &proc_tuple, &func_name );
		linestats_entry->line_count = count_source_lines(proc_src);
		old_context = MemoryContextSwitchTo(profiler_mcxt);
		linestats_entry->line_info = palloc0((linestats_entry->line_count + 1) *
											 sizeof(linestatsLineInfo));
		MemoryContextSwitchTo(old_context);

		ReleaseSysCache(proc_tuple);
	}


	/*
	 * The PL/pgSQL interpreter provides a void pointer (in each stack frame)
	 * that's reserved for plugins.	 We allocate a profilerInfo structure and
	 * record it's address in that pointer so we can keep some per-invocation
	 * information.
	 */
	profiler_info = (profilerInfo *)palloc(sizeof(profilerInfo ));

	profiler_info->fn_oid = func->fn_oid;
	profiler_info->line_count = linestats_entry->line_count;
	profiler_info->line_info = palloc0((profiler_info->line_count + 1) *
									  sizeof(profilerLineInfo));

	estate->plugin_info = profiler_info;
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

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
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
			callgraph_pop_one(func->fn_oid);
	}
	graph_current_xid = current_xid;

	/*
	 * Push this function Oid onto the stack, remember the entry time and
	 * set the time spent in children to zero.
	 */
	callgraph_push(func->fn_oid);
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
	profilerInfo	   *profiler_info;
	linestatsHashKey	key;
	linestatsEntry	   *entry;
	int					i;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	profiler_info = (profilerInfo *) estate->plugin_info;

	/* Find the linestats hash table entry for this function. */
	key.db_oid = MyDatabaseId;
	key.fn_oid = func->fn_oid;
	entry = (linestatsEntry *)hash_search(linestats_hash, &key,
										  HASH_FIND, NULL);
	if (!entry)
		elog(ERROR, "plprofiler: local linestats entry for fn_oid %u "
					"not found", func->fn_oid);

	/* Loop through each line of source code and update the stats */
	for(i = 0; i <= profiler_info->line_count; i++)
	{
		entry->line_info[i].exec_count +=
				profiler_info->line_info[i].exec_count;
		entry->line_info[i].us_total +=
				profiler_info->line_info[i].us_total;

		if (profiler_info->line_info[i].us_max > entry->line_info[i].us_max)
			entry->line_info[i].us_max =
					profiler_info->line_info[i].us_max;
	}

	/*
	 * Pop the call stack. This also does the time accounting
	 * for call graphs.
	 */
	callgraph_pop(func->fn_oid);

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
	profilerLineInfo   *line_info;
	profilerInfo	   *profiler_info;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	/* Set the start time of the statement */
	profiler_info = (profilerInfo *)estate->plugin_info;
	if (stmt->lineno <= profiler_info->line_count)
	{
		line_info = profiler_info->line_info + stmt->lineno;
		INSTR_TIME_SET_CURRENT(line_info->start_time);
	}

	/* Check the call graph stack. */
	callgraph_check(profiler_info->fn_oid);
}

/* -------------------------------------------------------------------
 * profiler_stmt_end()
 *
 *	This hook function is called by the PL/pgSQL interpreter just after
 *	it executes a statement (stmt).
 *
 *	We use this hook to 'delta' the before and after performance counters
 *	and record the differences in the profilerStmtInfo structure associated
 *	with this statement.
 * -------------------------------------------------------------------
 */
static void
profiler_stmt_end(PLpgSQL_execstate *estate, PLpgSQL_stmt *stmt)
{
	profilerLineInfo   *line_info;
	profilerInfo	   *profiler_info;
	instr_time			end_time;
	uint64				elapsed;

	if (!profiler_enabled && MyProcPid != profiler_enable_pid)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	profiler_info = (profilerInfo *)estate->plugin_info;

	/*
	 * Ignore out of bounds line numbers. Someone is apparently
	 * profiling while executing DDL ... not much use in that.
	 */
	if (stmt->lineno > profiler_info->line_count)
		return;

	line_info = profiler_info->line_info + stmt->lineno;

	INSTR_TIME_SET_CURRENT(end_time);
	INSTR_TIME_SUBTRACT(end_time, line_info->start_time);

	elapsed = INSTR_TIME_GET_MICROSEC(end_time);

	if (elapsed > line_info->us_max)
		line_info->us_max = elapsed;

	line_info->us_total += elapsed;
	line_info->exec_count++;
}

/**********************************************************************
 * Helper functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * find_source()
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
find_source(Oid oid, HeapTuple *tup, char **funcName)
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
 * count_source_lines()
 *
 *	This function scans through the source code for a given function
 *	and counts the number of lines of code present in the string.
 * -------------------------------------------------------------------
 */
static int
count_source_lines(const char *src)
{
	int			line_count = 0;
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
 * init_hash_tables()
 *
 * Initialize hash table
 * -------------------------------------------------------------------
 */
static void
init_hash_tables(void)
{
	HASHCTL		hash_ctl;

	/* Create the memory context for our data */
	if (profiler_mcxt != NULL)
	{
		if (profiler_mcxt->isReset)
			return;
		MemoryContextReset(profiler_mcxt);
	}
	else
	{
		profiler_mcxt = AllocSetContextCreate(TopMemoryContext,
											  "PL/pgSQL profiler",
											  ALLOCSET_DEFAULT_MINSIZE,
											  ALLOCSET_DEFAULT_INITSIZE,
											  ALLOCSET_DEFAULT_MAXSIZE);
	}

	/* Create the hash table for line stats */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = sizeof(linestatsHashKey);
	hash_ctl.entrysize = sizeof(linestatsEntry);
	hash_ctl.hash = line_hash_fn;
	hash_ctl.match = line_match_fn;
	hash_ctl.hcxt = profiler_mcxt;

	linestats_hash = hash_create("Function Lines",
				 10000,
				 &hash_ctl,
				 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	/* Create the hash table for call stats */
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));

	hash_ctl.keysize = sizeof(callGraphKey);
	hash_ctl.entrysize = sizeof(callGraphEntry);
	hash_ctl.hash = callgraph_hash_fn;
	hash_ctl.match = callgraph_match_fn;
	hash_ctl.hcxt = profiler_mcxt;

	callgraph_hash = hash_create("Function Call Graphs",
				 1000,
				 &hash_ctl,
				 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
}

static uint32
line_hash_fn(const void *key, Size keysize)
{
	const linestatsHashKey *k = (const linestatsHashKey *) key;

	return hash_uint32((uint32) k->fn_oid) ^
		hash_uint32((uint32) k->db_oid);
}

static int
line_match_fn(const void *key1, const void *key2, Size keysize)
{
	const linestatsHashKey  *k1 = (const linestatsHashKey *)key1;
	const linestatsHashKey  *k2 = (const linestatsHashKey *)key2;

	if (k1->fn_oid == k2->fn_oid &&
		k1->db_oid == k2->db_oid)
		return 0;
	else
		return 1;
}

static uint32
callgraph_hash_fn(const void *key, Size keysize)
{
	return hash_any(key, keysize);
}

static int
callgraph_match_fn(const void *key1, const void *key2, Size keysize)
{
	callGraphKey   *stack1 = (callGraphKey *)key1;
	callGraphKey   *stack2 = (callGraphKey *)key2;
	int				i;

	if (stack1->db_oid != stack2->db_oid)
		return 1;
	for (i = 0; i < PL_MAX_STACK_DEPTH && stack1->stack[i] != InvalidOid; i++)
		if (stack1->stack[i] != stack2->stack[i])
			return 1;
	return 0;
}

static void
callgraph_push(Oid func_oid)
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
callgraph_pop_one(Oid func_oid)
{
	instr_time			now;
	uint64				us_elapsed;
	uint64				us_self;
	linestatsHashKey	key;
	linestatsEntry	   *entry;

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
	callgraph_collect(us_elapsed, us_self,
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
	key.fn_oid = func_oid;
	key.db_oid = MyDatabaseId;

	entry = (linestatsEntry *)hash_search(linestats_hash, &key, HASH_FIND, NULL);

	if (!entry)
		elog(ERROR, "plprofiler: local linestats entry for fn_oid %u "
					"not found", func_oid);

	entry->line_info[0].exec_count += 1;
	entry->line_info[0].us_total += us_elapsed;

	if (us_elapsed > entry->line_info[0].us_max)
		entry->line_info[0].us_max = us_elapsed;
}

static void
callgraph_pop(Oid func_oid)
{
	callgraph_check(func_oid);
	callgraph_pop_one(func_oid);
}

static void
callgraph_check(Oid func_oid)
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
		callgraph_pop_one(func_oid);
	}
}

static void
callgraph_collect(uint64 us_elapsed, uint64 us_self, uint64 us_children)
{
	callGraphEntry *entry;
	bool			found;

	entry = (callGraphEntry *)hash_search(callgraph_hash, &graph_stack,
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

static void
profiler_enabled_assign(bool newval, void *extra)
{
	profiler_enabled = newval;
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
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	HASH_SEQ_STATUS		hash_seq;
	linestatsEntry	   *entry;

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

	if (linestats_hash != NULL)
	{
		hash_seq_init(&hash_seq, linestats_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			int		lno;

			for (lno = 0; lno <= entry->line_count; lno++)
			{
				Datum		values[PL_PROFILE_COLS];
				bool		nulls[PL_PROFILE_COLS];
				int			i = 0;

				/* Include this entry in the result. */
				MemSet(values, 0, sizeof(values));
				MemSet(nulls, 0, sizeof(nulls));

				values[i++] = ObjectIdGetDatum(entry->key.fn_oid);
				values[i++] = Int64GetDatumFast(lno);
				values[i++] = Int64GetDatumFast(entry->line_info[lno].exec_count);
				values[i++] = Int64GetDatumFast(entry->line_info[lno].us_total);
				values[i++] = Int64GetDatumFast(entry->line_info[lno].us_max);

				Assert(i == PL_PROFILE_COLS);

				tuplestore_putvalues(tupstore, tupdesc, values, nulls);
			}
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

	if (callgraph_hash != NULL)
	{
		hash_seq_init(&hash_seq, callgraph_hash);
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
	linestatsEntry	   *entry;

	/* First pass to count the number of Oids, we will return. */

	if (linestats_hash != NULL)
	{
		hash_seq_init(&hash_seq, linestats_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
			i++;
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
	if (linestats_hash != NULL)
	{
		i = 0;
		hash_seq_init(&hash_seq, linestats_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
			result[i++] = ObjectIdGetDatum(entry->key.fn_oid);
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
		procSrc = find_source(func_oids[fidx], &procTuple, &funcName);
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
	init_hash_tables();

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
	linestatsEntry	   *lineEnt;
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
	if (linestats_hash != NULL && profiler_save_line_table != NULL)
	{
		snprintf(query, sizeof(query),
				 "INSERT INTO \"%s\".\"%s\" "
				 "    SELECT func_oid, line_number, exec_count, total_time, "
				 "			 longest_time "
				 "    FROM \"%s\".pl_profiler_linestats(true)",
				 profiler_namespace, profiler_save_line_table,
				 profiler_namespace);
		SPI_exec(query, 0);

		hash_seq_init(&hash_seq, linestats_hash);
		while ((lineEnt = hash_seq_search(&hash_seq)) != NULL)
			MemSet(&(lineEnt->line_info), 0,
				   sizeof(linestatsLineInfo) * (lineEnt->line_count + 1));

		result += SPI_processed;
	}

	/* Same with call graph stats. */
	if (callgraph_hash != NULL && profiler_save_callgraph_table != NULL)
	{
		snprintf(query, sizeof(query),
				 "INSERT INTO \"%s\".\"%s\" "
				 "    SELECT stack, call_count, us_total, "
				 "           us_children, us_self "
				 "    FROM \"%s\".pl_profiler_callgraph(true)",
				 profiler_namespace, profiler_save_callgraph_table,
				 profiler_namespace);
		SPI_exec(query, 0);

		hash_seq_init(&hash_seq, callgraph_hash);
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

