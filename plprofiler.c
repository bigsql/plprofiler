/*-------------------------------------------------------------------------
 *
 * plprofiler.c
 *
 *	  Profiling plugin for PL/pgSQL instrumentation
 *
 * Copyright (c) 2014-2022, OSCG-Partners
 * Copyright (c) 2008-2014, PostgreSQL Global Development Group
 * Copyright 2006,2007 - EnterpriseDB, Inc.
 *
 * Major Change History:
 * 2012 - Removed from PostgreSQL plDebugger Extension
 * 2015 - Resurrected as standalone plProfiler by OpenSCG
 * 2016 - Rewritten as v2 to use shared hash tables, have lower overhead
 *			- v3 Major performance improvements, flame graph UI
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
static Size profiler_shmem_size(void);
static void profiler_shmem_startup(void);
#if PG_VERSION_NUM >= 150000
static void profiler_shmem_request(void);
#endif
static void init_hash_tables(void);
static char *find_source(Oid oid, HeapTuple *tup, char **funcName);
static int count_source_lines(const char *src);
static uint32 line_hash_fn(const void *key, Size keysize);
static int line_match_fn(const void *key1, const void *key2, Size keysize);
static uint32 callgraph_hash_fn(const void *key, Size keysize);
static int callgraph_match_fn(const void *key1, const void *key2, Size keysize);
static void callgraph_push(Oid func_oid);
static void callgraph_pop_one(void);
static void callgraph_pop(Oid func_oid);
static void callgraph_check(Oid func_oid);
static void callgraph_collect(uint64 us_elapsed, uint64 us_self,
							  uint64 us_children);
static int32 profiler_collect_data(void);
static void profiler_xact_callback(XactEvent event, void *arg);

/**********************************************************************
 * Local variables
 **********************************************************************/

static MemoryContext	profiler_mcxt = NULL;
static HTAB			   *functions_hash = NULL;
static HTAB			   *callgraph_hash = NULL;
static profilerSharedState *profiler_shared_state = NULL;
static HTAB			   *functions_shared = NULL;
static HTAB			   *callgraph_shared = NULL;

static bool				profiler_first_call_in_xact = true;
static bool				profiler_active = false;
static bool				profiler_enabled_local = false;
static int				profiler_max_functions = PL_MIN_FUNCTIONS;
static int				profiler_max_lines = PL_MIN_LINES;
static int				profiler_max_callgraph = PL_MIN_CALLGRAPH;

static callGraphKey		graph_stack;
static instr_time		graph_stack_entry[PL_MAX_STACK_DEPTH];
static uint64			graph_stack_child_time[PL_MAX_STACK_DEPTH];
static int				graph_stack_pt = 0;
static time_t			last_collect_time = 0;
static bool				have_new_local_data = false;

static PLpgSQL_plugin  *prev_plpgsql_plugin = NULL;
static PLpgSQL_plugin  *prev_pltsql_plugin = NULL;
static shmem_startup_hook_type	prev_shmem_startup_hook = NULL;
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type	prev_shmem_request_hook = NULL;
#endif

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

	if (process_shared_preload_libraries_in_progress)
	{
		/*
		 * When loaded via shared_preload_libraries, we have to
		 * also hook into the shmem_startup call chain and register
		 * a callback at transaction end.
		 */
		prev_shmem_startup_hook = shmem_startup_hook;
		shmem_startup_hook = profiler_shmem_startup;
		#if PG_VERSION_NUM >= 150000
		prev_shmem_request_hook = shmem_request_hook;
		shmem_request_hook = profiler_shmem_request;
		#endif

		RegisterXactCallback(profiler_xact_callback, NULL);

		/*
		 * Additional config options only available if running via
		 * shared_preload_libraries. These all affect the amount of
		 * shared memory used by the extension, so they only make
		 * sense as PGC_POSTMASTER.
		 */
		DefineCustomIntVariable("plprofiler.max_functions",
								"Maximum number of functions that can be "
								"tracked in shared memory when using "
								"plprofiler.collect_in_shmem",
								NULL,
								&profiler_max_functions,
								PL_MIN_FUNCTIONS,
								PL_MIN_FUNCTIONS,
								INT_MAX,
								PGC_POSTMASTER,
								0,
								NULL,
								NULL,
								NULL);

		DefineCustomIntVariable("plprofiler.max_lines",
								"Maximum number of source lines that can be "
								"tracked in shared memory when using "
								"plprofiler.collect_in_shmem",
								NULL,
								&profiler_max_lines,
								PL_MIN_LINES,
								PL_MIN_LINES,
								INT_MAX,
								PGC_POSTMASTER,
								0,
								NULL,
								NULL,
								NULL);

		DefineCustomIntVariable("plprofiler.max_callgraphs",
								"Maximum number of call graphs that can be "
								"tracked in shared memory when using "
								"plprofiler.collect_in_shmem",
								NULL,
								&profiler_max_callgraph,
								PL_MIN_CALLGRAPH,
								PL_MIN_CALLGRAPH,
								INT_MAX,
								PGC_POSTMASTER,
								0,
								NULL,
								NULL,
								NULL);

		/* Request the additionl shared memory and LWLock needed. */
		#if PG_VERSION_NUM < 150000
		RequestAddinShmemSpace(profiler_shmem_size());
		#if PG_VERSION_NUM >= 90600
		RequestNamedLWLockTranche("plprofiler", 1);
		#else
		RequestAddinLWLocks(1);
		#endif
		#endif
	}
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
	functions_hash = NULL;
	callgraph_hash = NULL;

	if (prev_shmem_startup_hook != NULL)
	{
		shmem_startup_hook = prev_shmem_startup_hook;
		prev_shmem_startup_hook = NULL;

		UnregisterXactCallback(profiler_xact_callback, NULL);
	}
}

/* -------------------------------------------------------------------
 * profiler_shmem_size()
 *
 * 	Calculate the amount of shared memory the profiler needs to
 * 	keep functions, callgraphs and line statistics globally.
 * -------------------------------------------------------------------
 */
static Size
profiler_shmem_size(void)
{
	Size	num_bytes;

	num_bytes = offsetof(profilerSharedState, line_info);
	num_bytes = add_size(num_bytes,
						 sizeof(linestatsLineInfo) * profiler_max_lines);
	num_bytes = add_size(num_bytes,
						 hash_estimate_size(profiler_max_functions,
						 					sizeof(linestatsEntry)));
	num_bytes = add_size(num_bytes,
						 hash_estimate_size(profiler_max_callgraph,
						 					sizeof(callGraphEntry)));

	return num_bytes;
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

	/*
	 * On first call within a transaction we determine if the profiler
	 * is active or not. This means that starting/stopping to collect
	 * data will only happen on a transaction boundary.
	 */
	if (profiler_first_call_in_xact)
	{
		profiler_first_call_in_xact = false;

		if (profiler_shared_state != NULL)
		{
			profiler_active = (
				profiler_shared_state->profiler_enabled_global ||
				profiler_shared_state->profiler_enabled_pid == MyProcPid ||
				profiler_enabled_local);
		}
		else
		{
			profiler_active = profiler_enabled_local;
		}
	}

	if (!profiler_active)
	{
		/*
		 * The profiler can be enabled/disabled via changing postgresql.conf
		 * and reload (SIGHUP). The change becomes visible in backends the
		 * next time, the TCOP loop is ready for a new client query. This
		 * allows to enable the profiler for some time, have it save the
		 * stats in the permanent tables, then turn it off again. At that
		 * moment, we want to release all profiler resources.
		 */
		if (functions_hash != NULL)
			init_hash_tables();
		return;
	}

	/*
	 * Anonymous code blocks do not have function source code
	 * that we can lookup in pg_proc. For now we ignore them.
	 */
	if (func->fn_oid == InvalidOid)
		return;

	/* Tell collect_data() that new information has arrived locally. */
	have_new_local_data = true;

	/*
	 * Search for this function in our line stats hash table. Create the
	 * entry if it does not exist yet.
	 */
	linestats_key.db_oid = MyDatabaseId;
	linestats_key.fn_oid = func->fn_oid;

	linestats_entry = (linestatsEntry *)hash_search(functions_hash,
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
		linestats_entry->line_count = count_source_lines(proc_src) + 1;
		old_context = MemoryContextSwitchTo(profiler_mcxt);
		linestats_entry->line_info = palloc0(linestats_entry->line_count *
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
	profiler_info->line_info = palloc0(profiler_info->line_count *
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
	if (!profiler_active)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

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

	if (!profiler_active)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	/* Tell collect_data() that new information has arrived locally. */
	have_new_local_data = true;

	/* Find the linestats hash table entry for this function. */
	profiler_info = (profilerInfo *) estate->plugin_info;
	key.db_oid = MyDatabaseId;
	key.fn_oid = func->fn_oid;
	entry = (linestatsEntry *)hash_search(functions_hash, &key,
										  HASH_FIND, NULL);
	if (!entry)
	{
		elog(DEBUG1, "plprofiler: local linestats entry for fn_oid %u "
					"not found", func->fn_oid);
		return;
	}

	/* Loop through each line of source code and update the stats */
	for(i = 1; i < profiler_info->line_count; i++)
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
	 * Finally if a plprofiler.collect_interval is configured, save and reset
	 * the stats if the interval has elapsed.
	 */
	if (profiler_shared_state != NULL &&
		(profiler_shared_state->profiler_enabled_global ||
		 MyProcPid == profiler_shared_state->profiler_enabled_pid) &&
		profiler_shared_state->profiler_collect_interval > 0)
	{
		time_t	now = time(NULL);

		if (now >= last_collect_time +
				   profiler_shared_state->profiler_collect_interval)
		{
		    profiler_collect_data();
			last_collect_time = now;
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

	if (!profiler_active)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	/* Set the start time of the statement */
	profiler_info = (profilerInfo *)estate->plugin_info;
	if (stmt->lineno < profiler_info->line_count)
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

	if (!profiler_active)
		return;

	/* Ignore anonymous code block. */
	if (estate->plugin_info == NULL)
		return;

	profiler_info = (profilerInfo *)estate->plugin_info;

	/*
	 * Ignore out of bounds line numbers. Someone is apparently
	 * profiling while executing DDL ... not much use in that.
	 */
	if (stmt->lineno >= profiler_info->line_count)
		return;

	/* Tell collect_data() that new information has arrived locally. */
	have_new_local_data = true;

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

	functions_hash = hash_create("Function Lines",
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

#if PG_VERSION_NUM >= 150000
static void
profiler_shmem_request(void)
{
	RequestAddinShmemSpace(profiler_shmem_size());
	RequestNamedLWLockTranche("plprofiler", 1);
}
#endif

static void
profiler_shmem_startup(void)
{
	bool					found;
	profilerSharedState	   *plpss;
	Size					plpss_size = 0;
	HASHCTL					hash_ctl;

	if (prev_shmem_startup_hook)
	        prev_shmem_startup_hook();

	/* Reset in case of restart inside of the postmaster. */
	profiler_shared_state = NULL;
	functions_shared = NULL;
	callgraph_shared = NULL;

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	/* Create or attach to the shared state */
	plpss_size = add_size(plpss_size,
						  offsetof(profilerSharedState, line_info));
	plpss_size = add_size(plpss_size,
						  sizeof(linestatsLineInfo) * profiler_max_lines);
	profiler_shared_state = ShmemInitStruct("plprofiler state", plpss_size,
											&found);
	plpss = profiler_shared_state;
	if (!found)
	{
		memset(plpss, 0, offsetof(profilerSharedState, line_info) +
						 sizeof(linestatsLineInfo) * profiler_max_lines);

		#if PG_VERSION_NUM >= 90600
		plpss->lock = &(GetNamedLWLockTranche("plprofiler"))->lock;
		#else
		plpss->lock = LWLockAssign();
		#endif
	}

	/* (Re)Initialize local hash tables. */
	init_hash_tables();

	/* Create or attache to the shared functions hash table */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(linestatsHashKey);
	hash_ctl.entrysize = sizeof(linestatsEntry);
	hash_ctl.hash = line_hash_fn;
	hash_ctl.match = line_match_fn;
	functions_shared = ShmemInitHash("plprofiler functions",
									 profiler_max_functions,
									 profiler_max_functions,
									 &hash_ctl,
									 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	/* Create or attache to the shared callgraph hash table */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(callGraphKey);
	hash_ctl.entrysize = sizeof(callGraphEntry);
	hash_ctl.hash = callgraph_hash_fn;
	hash_ctl.match = callgraph_match_fn;
	callgraph_shared = ShmemInitHash("plprofiler callgraph",
									  profiler_max_callgraph,
									  profiler_max_callgraph,
									  &hash_ctl,
									  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	LWLockRelease(AddinShmemInitLock);
}

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
callgraph_pop_one(void)
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

	/*
	 * We also collect per function global counts in the pseudo line number
	 * zero. The line stats are cumulative (for example a FOR ... LOOP
	 * statement has the entire execution time of all statements in its
	 * block), so this can't be derived from the actual per line data.
	 */
	key.fn_oid = graph_stack.stack[graph_stack_pt];
	key.db_oid = MyDatabaseId;

	entry = (linestatsEntry *)hash_search(functions_hash, &key, HASH_FIND, NULL);

	if (entry)
	{
		entry->line_info[0].exec_count += 1;
		entry->line_info[0].us_total += us_elapsed;

		if (us_elapsed > entry->line_info[0].us_max)
			entry->line_info[0].us_max = us_elapsed;
	}
	else
	{
		elog(DEBUG1, "plprofiler: local linestats entry for fn_oid %u "
					"not found", graph_stack.stack[graph_stack_pt]);
	}

	/* Zap the oid from the call stack. */
	graph_stack.stack[graph_stack_pt] = InvalidOid;
}

static void
callgraph_pop(Oid func_oid)
{
	callgraph_check(func_oid);
	callgraph_pop_one();
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
		callgraph_pop_one();
	}
}

static void
callgraph_collect(uint64 us_elapsed, uint64 us_self, uint64 us_children)
{
	callGraphEntry *entry;
	bool			found;

	graph_stack.db_oid = MyDatabaseId;
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

static int32
profiler_collect_data(void)
{
	HASH_SEQ_STATUS			hash_seq;
	callGraphEntry		   *cge1;
	callGraphEntry		   *cge2;
	linestatsEntry		   *lse1;
	linestatsEntry		   *lse2;
	profilerSharedState	   *plpss = profiler_shared_state;
	bool					have_exclusive_lock = false;
	bool					found;
	int						i;

	/*
	 * Return without doing anything if the plprofiler extension
	 * was not loaded via shared_preload_libraries. We don't have
	 * any shared memory state in that case.
	 */
	if (plpss == NULL)
		return -1;

	/*
	 * Don't waste any time here if there was no new data recorded
	 * since the last collect_data() call.
	 */
	if (!have_new_local_data)
		return 0;
	have_new_local_data = false;

	/*
	 * Acquire a shared lock on the shared hash tables. We escalate
	 * to an exclusive lock later in case we need to add a new entry.
	 */
	LWLockAcquire(plpss->lock, LW_SHARED);

	/* Collect the callgraph data into shared memory. */
	hash_seq_init(&hash_seq, callgraph_hash);
	while ((cge1 = hash_seq_search(&hash_seq)) != NULL)
	{
		cge2 = hash_search(callgraph_shared, &(cge1->key),
						   HASH_FIND, NULL);
		if (cge2 == NULL)
		{
			/*
			 * This callgraph is not yet known in shared memory.
			 * Need to escalate the lock to exclusive.
			 */
			if (!have_exclusive_lock)
			{
				LWLockRelease(plpss->lock);
				LWLockAcquire(plpss->lock, LW_EXCLUSIVE);
				have_exclusive_lock = true;
			}

			cge2 = hash_search(callgraph_shared, &(cge1->key),
							   HASH_ENTER, &found);
			if (cge2 == NULL)
			{
				/*
				 * This means that we are out of shared memory for the
				 * callgraph_shared hash table. Nothing we can do
				 * here but complain.
				 */
				if (!plpss->callgraph_overflow)
				{
					elog(LOG,
						 "plprofiler: entry limit reached for "
						 "shared memory call graph data");
					plpss->callgraph_overflow = true;
				}
				break;
			}

			/*
			 * Since we released the lock above for lock escalation to
			 * exclusive, it is possible that someone else in the meantime
			 * created the entry for this call graph.
			 */
			if (!found)
			{
				/*
				 * We created a new entry for this call graph in the
				 * shared hash table. Initialize it.
				 */
				/* memcpy(&(cge2->key), &(cge1->key), sizeof(callGraphKey)); */
				SpinLockInit(&(cge2->mutex));
				cge2->callCount = 0;
				cge2->totalTime = 0;
				cge2->childTime = 0;
				cge2->selfTime = 0;
			}
		}

		/*
		 * At this point we have the local entry in cge1 and the shared
		 * entry in cge2. Since we may still only hold a shared lock on
		 * the shared state, use a spinlock on the shared entry while
		 * adding the counters. Then reset our local counters to zero.
		 */
		SpinLockAcquire(&(cge2->mutex));
		cge2->callCount += cge1->callCount;
		cge2->totalTime += cge1->totalTime;
		cge2->childTime += cge1->childTime;
		cge2->selfTime  += cge1->selfTime ;
		SpinLockRelease(&(cge2->mutex));

		cge1->callCount = 0;
		cge1->totalTime = 0;
		cge1->childTime = 0;
		cge1->selfTime = 0;
	}

	/* Collect the linestats data into shared memory. */
	hash_seq_init(&hash_seq, functions_hash);
	while ((lse1 = hash_seq_search(&hash_seq)) != NULL)
	{
		lse2 = hash_search(functions_shared, &(lse1->key),
						   HASH_FIND, NULL);
		if (lse2 == NULL)
		{
			/*
			 * This function is not yet known in shared memory.
			 * Need to escalate the lock to exclusive.
			 */
			if (!have_exclusive_lock)
			{
				LWLockRelease(plpss->lock);
				LWLockAcquire(plpss->lock, LW_EXCLUSIVE);
				have_exclusive_lock = true;
			}

			lse2 = hash_search(functions_shared, &(lse1->key),
							   HASH_ENTER, &found);
			if (lse2 == NULL)
			{
				/*
				 * This means that we are out of shared memory for the
				 * functions_shared hash table. Nothing we can do
				 * here but complain.
				 */
				if (!plpss->functions_overflow)
				{
					elog(LOG,
						 "plprofiler: entry limit reached for "
						 "shared memory functions data");
					plpss->functions_overflow = true;
				}
				break;
			}
			if (memcmp(&(lse2->key), &(lse1->key), sizeof(linestatsHashKey)) != 0)
			{
				elog(FATAL, "key of new hash entry doesn't match");
			}

			/*
			 * Since we released the lock above for lock escalation to
			 * exclusive, it is possible that someone else in the meantime
			 * created the entry for this function.
			 */
			if (!found)
			{
				/*
				 * We created a new entry for this function in the
				 * shared hash table. Initialize it. We also need to
				 * allocate the per line counters here. If we run out
				 * of per line counters in the shared state, we don't
				 * keep count for any lines of this function at all.
				 */
				SpinLockInit(&(lse2->mutex));
				if (lse1->line_count <= profiler_max_lines - plpss->lines_used)
				{
					lse2->line_count = lse1->line_count;
					lse2->line_info = &(plpss->line_info[plpss->lines_used]);
					plpss->lines_used += lse1->line_count;
					memset(lse2->line_info, 0,
						   sizeof(linestatsLineInfo) * lse1->line_count);
				}
				else
				{
					if (!plpss->lines_overflow)
					{
						elog(LOG,
							 "plprofiler: entry limit reached for "
							 "shared memory per source line data");
						plpss->lines_overflow = true;
					}
					lse2->line_count = 0;
					lse2->line_info = NULL;
				}
			}
		}

		/*
		 * At this point we have the local entry in lse1 and the shared
		 * entry in lse2. Since we may still only hold a shared lock on
		 * the shared state, use a spinlock on the shared entry while
		 * adding the counters.
		 */
		SpinLockAcquire(&(lse2->mutex));
		for (i = 0; i < lse1->line_count && i < lse2->line_count; i++)
		{
			if (lse1->line_info[i].us_max > lse2->line_info[i].us_max)
				lse2->line_info[i].us_max = lse1->line_info[i].us_max;
			lse2->line_info[i].us_total += lse1->line_info[i].us_total;
			lse2->line_info[i].exec_count += lse1->line_info[i].exec_count;
		}
		SpinLockRelease(&(lse2->mutex));

		memset(lse1->line_info, 0,
			   sizeof(linestatsLineInfo) * lse1->line_count);
	}

	/* All done, release the lock. */
	LWLockRelease(plpss->lock);

	return 0;
}

static void
profiler_xact_callback(XactEvent event, void *arg)
{
	Assert(profiler_shared_state != NULL);

	/* Collect the statistics if needed. */
	if (profiler_active &&
		profiler_shared_state->profiler_collect_interval > 0)
	{
		switch (event)
		{
			case XACT_EVENT_COMMIT:
			case XACT_EVENT_ABORT:
			#if PG_VERSION_NUM >= 90500
			case XACT_EVENT_PARALLEL_COMMIT:
			case XACT_EVENT_PARALLEL_ABORT:
			#endif
				profiler_collect_data();
				break;

			default:
				break;
		}
	}

	/* Tell func_init that we need to evaluate the new active state. */
	profiler_first_call_in_xact = true;

	/* We can also unwind the callstack here in case of abort. */
	callgraph_check(InvalidOid);
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
 * pl_profiler_linestats_local()
 *
 *	Returns the content of the local line stats hash table
 *	as a set of rows.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_linestats_local(PG_FUNCTION_ARGS)
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

	if (functions_hash != NULL)
	{
		hash_seq_init(&hash_seq, functions_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
		{
			int64	lno;

			for (lno = 0; lno < entry->line_count; lno++)
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

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_linestats_shared()
 *
 *	Returns the content of the shared line stats hash table
 *	as a set of rows.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_linestats_shared(PG_FUNCTION_ARGS)
{
	ReturnSetInfo		   *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	TupleDesc				tupdesc;
	Tuplestorestate		   *tupstore;
	MemoryContext			per_query_ctx;
	MemoryContext			oldcontext;
	HASH_SEQ_STATUS			hash_seq;
	linestatsEntry		   *entry;
	profilerSharedState	   *plpss = profiler_shared_state;

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

	/* Check that plprofiler was loaded via shared_preload_libraries */
	if (plpss == NULL)
		elog(ERROR, "plprofiler was not loaded via shared_preload_libraries");

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

	/* Place a shared lock on the shared memory data. */
	LWLockAcquire(plpss->lock, LW_SHARED);

	hash_seq_init(&hash_seq, functions_shared);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		int64	lno;

		if (entry->key.db_oid != MyDatabaseId)
			continue;

		/* Guard agains concurrent updates of the counters. */
		SpinLockAcquire(&(entry->mutex));

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

		/* Done with the counter access. */
		SpinLockRelease(&(entry->mutex));
	}

	/* Release the shared lock on the shared memory data. */
	LWLockRelease(plpss->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_callgraph_local()
 *
 *	Returns the content of the local call graph hash table
 *	as a set of rows.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_callgraph_local(PG_FUNCTION_ARGS)
{
	ReturnSetInfo	   *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;
	MemoryContext		per_query_ctx;
	MemoryContext		oldcontext;
	HASH_SEQ_STATUS		hash_seq;
	callGraphEntry	   *entry;

	/* Check to see if caller supports us returning a tuplestore */
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

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_callgraph_shared()
 *
 *	Returns the content of the shared call graph hash table
 *	as a set of rows.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_callgraph_shared(PG_FUNCTION_ARGS)
{
	ReturnSetInfo		   *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	TupleDesc				tupdesc;
	Tuplestorestate		   *tupstore;
	MemoryContext			per_query_ctx;
	MemoryContext			oldcontext;
	HASH_SEQ_STATUS			hash_seq;
	callGraphEntry		   *entry;
	profilerSharedState	   *plpss = profiler_shared_state;

	/* Check to see if caller supports us returning a tuplestore */
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

	/* Check that plprofiler was loaded via shared_preload_libraries */
	if (plpss == NULL)
		elog(ERROR, "plprofiler was not loaded via shared_preload_libraries");

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

	/* Place a shared lock on the shared memory data. */
	LWLockAcquire(plpss->lock, LW_SHARED);

	hash_seq_init(&hash_seq, callgraph_shared);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PL_CALLGRAPH_COLS];
		bool		nulls[PL_CALLGRAPH_COLS];
		Datum		funcdefs[PL_MAX_STACK_DEPTH];

		int			i = 0;
		int			j = 0;

		/* Only entries of the local database are visible. */
		if (entry->key.db_oid != MyDatabaseId)
			continue;

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		for (i = 0; i < PL_MAX_STACK_DEPTH && entry->key.stack[i] != InvalidOid; i++)
			funcdefs[i] = ObjectIdGetDatum(entry->key.stack[i]);

		values[j++] = PointerGetDatum(construct_array(funcdefs, i,
													  OIDOID, sizeof(Oid),
													  true, 'i'));

		/* Guard agains concurrent updates of the counters. */
		SpinLockAcquire(&(entry->mutex));

		values[j++] = Int64GetDatumFast(entry->callCount);
		values[j++] = Int64GetDatumFast(entry->totalTime);
		values[j++] = Int64GetDatumFast(entry->childTime);
		values[j++] = Int64GetDatumFast(entry->selfTime);

		/* Done with the counter access. */
		SpinLockRelease(&(entry->mutex));

		Assert(j == PL_CALLGRAPH_COLS);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* Release the shared lock on the shared memory data. */
	LWLockRelease(plpss->lock);

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_func_oids_local()
 *
 *	Returns an array of all function Oids that we have
 *	linestat information for in the local hash table.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_func_oids_local(PG_FUNCTION_ARGS)
{
	int					i = 0;
	Datum			   *result;
	HASH_SEQ_STATUS		hash_seq;
	linestatsEntry	   *entry;

	/* First pass to count the number of Oids, we will return. */

	if (functions_hash != NULL)
	{
		hash_seq_init(&hash_seq, functions_hash);
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
		elog(ERROR, "out of memory in pl_profiler_func_oids_local()");

	/* Second pass to collect the Oids. */
	if (functions_hash != NULL)
	{
		i = 0;
		hash_seq_init(&hash_seq, functions_hash);
		while ((entry = hash_seq_search(&hash_seq)) != NULL)
			result[i++] = ObjectIdGetDatum(entry->key.fn_oid);
	}

	/* Build and return the actual array. */
	PG_RETURN_ARRAYTYPE_P(PointerGetDatum(construct_array(result, i,
														  OIDOID, sizeof(Oid),
														  true, 'i')));
}

/* -------------------------------------------------------------------
 * pl_profiler_func_oids_shared()
 *
 *	Returns an array of all function Oids that we have
 *	linestat information for in the shared hash table.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_func_oids_shared(PG_FUNCTION_ARGS)
{
	int						i = 0;
	Datum				   *result;
	HASH_SEQ_STATUS			hash_seq;
	linestatsEntry		   *entry;
	profilerSharedState	   *plpss = profiler_shared_state;

	/* Check that plprofiler was loaded via shared_preload_libraries */
	if (plpss == NULL)
		elog(ERROR, "plprofiler was not loaded via shared_preload_libraries");

	/* Place a shared lock on the shared memory data. */
	LWLockAcquire(plpss->lock, LW_SHARED);

	/* First pass to count the number of Oids, we will return. */

	hash_seq_init(&hash_seq, functions_shared);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (entry->key.db_oid == MyDatabaseId)
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
		elog(ERROR, "out of memory in pl_profiler_func_oids_shared()");

	/* Second pass to collect the Oids. */
	i = 0;
	hash_seq_init(&hash_seq, functions_shared);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		if (entry->key.db_oid == MyDatabaseId)
			result[i++] = ObjectIdGetDatum(entry->key.fn_oid);
	}

	/* Release the shared lock on the shared memory data. */
	LWLockRelease(plpss->lock);

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
		int64		line_number = 0;

		/* Create the line-0 entry. */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));

		values[i++] = func_oids[fidx];
		values[i++] = Int64GetDatumFast(line_number);
		values[i++] = PointerGetDatum(cstring_to_text("-- Line 0"));

		Assert(i == PL_FUNCS_SRC_COLS);
		tuplestore_putvalues(tupstore, tupdesc, values, nulls);

		line_number++;

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
			values[i++] = Int64GetDatumFast(line_number);
			values[i++] = PointerGetDatum(cstring_to_text(linestart));

			Assert(i == PL_FUNCS_SRC_COLS);
			tuplestore_putvalues(tupstore, tupdesc, values, nulls);

			linestart = cp;
			line_number++;
		}

		ReleaseSysCache(procTuple);
		pfree(procSrc);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_reset_local()
 *
 *	Drop all data collected in the local hash tables.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_reset_local(PG_FUNCTION_ARGS)
{
	init_hash_tables();

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_reset_shared()
 *
 *	Drop all data collected in the shared hash tables and the
 *	shared state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_reset_shared(PG_FUNCTION_ARGS)
{
	HASH_SEQ_STATUS			hash_seq;
	callGraphEntry		   *lsent;
	linestatsEntry		   *cgent;
	profilerSharedState	   *plpss = profiler_shared_state;

	/* Check that plprofiler was loaded via shared_preload_libraries */
	if (plpss == NULL)
		elog(ERROR, "plprofiler was not loaded via shared_preload_libraries");

	LWLockAcquire(plpss->lock, LW_EXCLUSIVE);

	/* Reset the shared state. */
	plpss->callgraph_overflow = false;
	plpss->functions_overflow = false;
	plpss->lines_overflow = false;
	plpss->lines_used = 0;

	/* Delete all entries from the callgraph hash table. */
	hash_seq_init(&hash_seq, callgraph_shared);
	while ((cgent = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(callgraph_shared, &(cgent->key), HASH_REMOVE, NULL);
	}

	/* Delete all entries from the functions hash table. */
	hash_seq_init(&hash_seq, functions_shared);
	while ((lsent = hash_seq_search(&hash_seq)) != NULL)
	{
		hash_search(functions_shared, &(lsent->key), HASH_REMOVE, NULL);
	}

	LWLockRelease(plpss->lock);

	PG_RETURN_VOID();
}

/* -------------------------------------------------------------------
 * pl_profiler_set_enabled_global()
 *
 *	Turn global profiling on or off.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_set_enabled_global(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	if (profiler_shared_state == NULL)
		elog(ERROR, "plprofiler not loaded via shared_preload_libraries");
	else
		profiler_shared_state->profiler_enabled_global = PG_GETARG_BOOL(0);

	PG_RETURN_BOOL(profiler_shared_state->profiler_enabled_global);
}

/* -------------------------------------------------------------------
 * pl_profiler_get_enabled_global()
 *
 *	Report global profiling state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_get_enabled_global(PG_FUNCTION_ARGS)
{
	if (profiler_shared_state == NULL)
		elog(ERROR, "plprofiler not loaded via shared_preload_libraries");

	PG_RETURN_BOOL(profiler_shared_state->profiler_enabled_global);
}

/* -------------------------------------------------------------------
 * pl_profiler_set_enabled_local()
 *
 *	Turn local profiling on or off.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_set_enabled_local(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	profiler_enabled_local = PG_GETARG_BOOL(0);

	PG_RETURN_BOOL(profiler_enabled_local);
}

/* -------------------------------------------------------------------
 * pl_profiler_get_enabled_local()
 *
 *	Report local profiling state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_get_enabled_local(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(profiler_enabled_local);
}

/* -------------------------------------------------------------------
 * pl_profiler_set_enabled_pid()
 *
 *	Turn pid profiling on or off.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_set_enabled_pid(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	if (profiler_shared_state == NULL)
		elog(ERROR, "plprofiler not loaded via shared_preload_libraries");
	else
		profiler_shared_state->profiler_enabled_pid = PG_GETARG_INT32(0);

	PG_RETURN_INT32(profiler_shared_state->profiler_enabled_pid);
}

/* -------------------------------------------------------------------
 * pl_profiler_get_enabled_pid()
 *
 *	Report pid profiling state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_get_enabled_pid(PG_FUNCTION_ARGS)
{
	if (profiler_shared_state == NULL)
		elog(ERROR, "plprofiler not loaded via shared_preload_libraries");

	PG_RETURN_INT32(profiler_shared_state->profiler_enabled_pid);
}

/* -------------------------------------------------------------------
 * pl_profiler_set_collect_interval()
 *
 *	Turn pid profiling on or off.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_set_collect_interval(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	if (profiler_shared_state == NULL)
		PG_RETURN_INT32(-1);
	else
		profiler_shared_state->profiler_collect_interval = PG_GETARG_INT32(0);

	PG_RETURN_INT32(profiler_shared_state->profiler_collect_interval);
}

/* -------------------------------------------------------------------
 * pl_profiler_get_collect_interval()
 *
 *	Report pid profiling state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_get_collect_interval(PG_FUNCTION_ARGS)
{
	if (profiler_shared_state == NULL)
		elog(ERROR, "plprofiler not loaded via shared_preload_libraries");

	PG_RETURN_INT32(profiler_shared_state->profiler_collect_interval);
}

/* -------------------------------------------------------------------
 * pl_profiler_collect_data()
 *
 *	SQL level callable function to collect profiling data from the
 *	local tables into the shared hash tables.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_collect_data(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(profiler_collect_data());
}

/* -------------------------------------------------------------------
 * pl_profiler_callgraph_overflow()
 *
 *	Return the flag callgraph_overflow from the shared state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_callgraph_overflow(PG_FUNCTION_ARGS)
{
	profilerSharedState	   *plpss = profiler_shared_state;

	/* Check that plprofiler was loaded via shared_preload_libraries */
	if (plpss == NULL)
		elog(ERROR, "plprofiler was not loaded via shared_preload_libraries");

	PG_RETURN_BOOL(plpss->callgraph_overflow);
}

/* -------------------------------------------------------------------
 * pl_profiler_functions_overflow()
 *
 *	Return the flag functions_overflow from the shared state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_functions_overflow(PG_FUNCTION_ARGS)
{
	profilerSharedState	   *plpss = profiler_shared_state;

	/* Check that plprofiler was loaded via shared_preload_libraries */
	if (plpss == NULL)
		elog(ERROR, "plprofiler was not loaded via shared_preload_libraries");

	PG_RETURN_BOOL(plpss->functions_overflow);
}

/* -------------------------------------------------------------------
 * pl_profiler_lines_overflow()
 *
 *	Return the flag lines_overflow from the shared state.
 * -------------------------------------------------------------------
 */
Datum
pl_profiler_lines_overflow(PG_FUNCTION_ARGS)
{
	profilerSharedState	   *plpss = profiler_shared_state;

	/* Check that plprofiler was loaded via shared_preload_libraries */
	if (plpss == NULL)
		elog(ERROR, "plprofiler was not loaded via shared_preload_libraries");

	PG_RETURN_BOOL(plpss->lines_overflow);
}
