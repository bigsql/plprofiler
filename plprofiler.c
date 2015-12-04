
/*-------------------------------------------------------------------------
 *
 * plprofiler.c
 *
 *	  Profiling plugin for PL/pgSQL instrumentation
 *
 * Copyright (c) 2014-2015, BigSQL
 * Copyright (c) 2008-2014, PostgreSQL Global Development Group
 * Copyright 2006,2007 - EnterpriseDB, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <stdio.h>
#include <sys/time.h>
#include "access/hash.h"
#include "access/htup.h"

#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif

#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "plpgsql.h"
#include "pgstat.h"

PG_MODULE_MAGIC;

#define PL_PROFILE_COLS 6

/**********************************************************************
 * Type and structure definitions
 **********************************************************************/

/* -------------------------------------------------------------------
 * stmt_stats
 */
typedef struct
{
	int64          time_longest; /* Slowest iteration of this stmt                 */
	int64          time_total;   /* Total amount of time spent executing this stmt */
	PgStat_Counter execCount;    /* Number of times we've executed this stmt       */
	instr_time     start_time;   /* Start time for this statement                  */
} stmt_stats;

typedef struct
{
	int           line_count;      /* Number of lines-of-code in this function              */
	const char    ** sourceLines; /* Pointers to null-terminated source code for each line */
	stmt_stats  * stmtStats;    /* Pointers to performance counters for each line        */
} profilerCtx;

typedef struct lineHashKey
{
        Oid     func_oid;
        int     line_number;
} lineHashKey;

typedef struct Counters
{
        int64   exec_count; 
        int64   total_time; 
        int64   time_longest;
} Counters;

typedef struct lineEntry
{
        lineHashKey key;       /* hash key of entry - MUST BE FIRST */
        Counters    counters;  /* the statistics for this line */
        int         line_len;  /* # of valid bytes in line string */
        char        line[1];   /* VARIABLE LENGTH ARRAY - MUST BE LAST */
} lineEntry;

static HTAB *line_stats = NULL;
static bool enabled     = false;

static int  plprofiler_max;    /* max # lines to track */
static int  plprofiler_len;    /* max lenght of a line to track */

PLpgSQL_plugin **plugin_ptr = NULL;

Datum pl_profiler(PG_FUNCTION_ARGS);
Datum pl_profiler_reset(PG_FUNCTION_ARGS);
Datum pl_profiler_enable(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pl_profiler);
PG_FUNCTION_INFO_V1(pl_profiler_reset);
PG_FUNCTION_INFO_V1(pl_profiler_enable);

/**********************************************************************
 * Exported function prototypes
 **********************************************************************/

void load_plugin( PLpgSQL_plugin * hooks );

/**********************************************************************
 * Helper function prototypes
 **********************************************************************/
static void profiler_init( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void profiler_func_beg( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void profiler_func_end( PLpgSQL_execstate * estate, PLpgSQL_function * func );
static void profiler_stmt_beg( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );
static void profiler_stmt_end( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt );

static char *findSource( Oid oid, HeapTuple * tup, char ** funcName );
static char *copyLine( const char * src, size_t len );
static int   scanSource( const char * dst[], const char * src );
static void  InitHashTable(void);
static uint32 line_stats_fn(const void *key, Size keysize);
static int    line_match_fn(const void *key1, const void *key2, Size keysize);
static lineEntry *entry_alloc(lineHashKey *key, const char *line, int line_len);	

/**********************************************************************
 * Exported Function definitions
 **********************************************************************/

static PLpgSQL_plugin plugin_funcs = { profiler_init, profiler_func_beg, profiler_func_end, profiler_stmt_beg, profiler_stmt_end };

void _PG_init( void )
{
        DefineCustomIntVariable("plprofiler.max_lines",
          "Sets the maximum number of procedural lines of code tracked by plprofiler.",
                                                        NULL,
                                                        &plprofiler_max,
                                                        10000,
                                                        1000,
                                                        INT_MAX,
                                                        PGC_USERSET,
                                                        0,
                                                        NULL,
                                                        NULL,
                                                        NULL);

        DefineCustomIntVariable("plprofiler.line_length",
          "Sets the maximum length of each line of code tracked by plprofiler.",
                                                        NULL,
                                                        &plprofiler_len,
                                                        256,
                                                        24,
                                                        2048,
                                                        PGC_USERSET,
                                                        0,
                                                        NULL,
                                                        NULL,
                                                        NULL);


        plugin_ptr = (PLpgSQL_plugin **) find_rendezvous_variable("PLpgSQL_plugin");

	*plugin_ptr = &plugin_funcs;
}
        
void load_plugin( PLpgSQL_plugin * hooks )
{
	hooks->func_setup = profiler_init;
	hooks->func_beg   = profiler_func_beg;
	hooks->func_end   = profiler_func_end;
	hooks->stmt_beg   = profiler_stmt_beg;
	hooks->stmt_end   = profiler_stmt_end;
}

/**********************************************************************
 * Hook functions
 **********************************************************************/

/* -------------------------------------------------------------------
 * profiler_init()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a 
 *  new function is about to start.  Specifically, this instrumentation
 *  hook is called after the stack frame has been created, but before
 *  values are assigned to the local variables.
 *
 *	'estate' points to the stack frame for this function, 'func' 
 *	points to the definition of the function
 *
 *  We use this hook to load the source code for the function that's
 *  being invoked and to set up our context structures
 * -------------------------------------------------------------------
 */

static void profiler_init( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	HeapTuple	  procTuple;
	char        * funcName;
	profilerCtx * profilerInfo;
	char		* procSrc;

        if (!enabled)
            return;

	/*
	 * The PL/pgSQL interpreter provides a void pointer (in each stack frame) that's reserved
	 * for plugins.  We allocate a profilerCtx structure and record it's address in that
	 * pointer so we can keep some per-invocation information.
	 */
	profilerInfo = (profilerCtx *) palloc( sizeof( profilerCtx ));

	estate->plugin_info = profilerInfo;

	/* Allocate enough space to hold a pointer to each line of source code */
	procSrc = findSource( func->fn_oid, &procTuple, &funcName );

	profilerInfo->line_count   = scanSource( NULL, procSrc );
	profilerInfo->stmtStats   = palloc0( (profilerInfo->line_count + 1) * sizeof( stmt_stats ));
	profilerInfo->sourceLines = palloc( profilerInfo->line_count * sizeof( const char * ));
	
	/* Now scan through the source code for this function so we know where each line begins */

	scanSource( profilerInfo->sourceLines, procSrc );

	ReleaseSysCache( procTuple );
}

/* -------------------------------------------------------------------
 * profiler_func_beg()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a 
 *  new function is starting.  Specifically, this instrumentation
 *  hook is called after values have been assigned to all local 
 *	variables (and all function parameters).
 *
 *	'estate' points to the stack frame for this function, 'func' 
 *	points to the definition of the function
 * -------------------------------------------------------------------
 */

static void profiler_func_beg( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
}

/* -------------------------------------------------------------------
 * profiler_func_end()
 *
 *	This hook function is called by the PL/pgSQL interpreter when a 
 *  function runs to completion.
 * -------------------------------------------------------------------
 */

static void profiler_func_end( PLpgSQL_execstate * estate, PLpgSQL_function * func )
{
	profilerCtx * profilerInfo;
	int           lineNo;

        if (!enabled)
            return;

        if (!line_stats)
                InitHashTable();

	profilerInfo = (profilerCtx *) estate->plugin_info;

	/* Loop through each line of source code and update the stats */
	for(lineNo = 0; lineNo < profilerInfo->line_count; lineNo++)
	{
                lineHashKey   key;
                lineEntry   * entry;

                stmt_stats * stats = profilerInfo->stmtStats + (lineNo + 1);

                /* Set up key for hashtable search */
                key.func_oid = func->fn_oid;
                key.line_number = (int32) (lineNo + 1);

                entry = (lineEntry *) hash_search(line_stats, &key, HASH_FIND, NULL);

                if (!entry)
                {
                    int line_len = strlen(profilerInfo->sourceLines[lineNo]);
                    char line[plprofiler_len];

                    snprintf(line, plprofiler_len - 1, "%s", profilerInfo->sourceLines[lineNo]);

                    entry = entry_alloc(&key, line, line_len);
                    if (!entry) 
                    {
                          elog( ERROR, "Unable to allocate more space for the profiler. ");
                          return;
                    }
                }

                entry->counters.exec_count += stats->execCount;
                entry->counters.total_time += stats->time_total;

                if (stats->time_longest > entry->counters.time_longest)
                    entry->counters.time_longest = stats->time_longest;

                entry = NULL;

	}
}

/* -------------------------------------------------------------------
 * profiler_stmt_beg()
 *
 *	This hook function is called by the PL/pgSQL interpreter just before
 *	executing a statement (stmt).
 *
 *  Prior to executing each statement, we record the current time and 
 *  the current values of all of the performance counters.
 * -------------------------------------------------------------------
 */

static void profiler_stmt_beg( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
	stmt_stats     * stats;
        profilerCtx    * profilerInfo;

        if (!enabled)
            return;

	profilerInfo = (profilerCtx *) estate->plugin_info;
	stats = profilerInfo->stmtStats + stmt->lineno ;

        /* Set the start time of the statement */
        INSTR_TIME_SET_CURRENT(stats->start_time);
}

/* -------------------------------------------------------------------
 * profiler_stmt_end()
 *
 *	This hook function is called by the PL/pgSQL interpreter just after
 *	it executes a statement (stmt).
 *
 *	We use this hook to 'delta' the before and after performance counters
 *  and record the differences in the stmt_stats structure associated 
 *  with this statement.
 * -------------------------------------------------------------------
 */

static void profiler_stmt_end( PLpgSQL_execstate * estate, PLpgSQL_stmt * stmt )
{
	stmt_stats  * stats;
        profilerCtx * profilerInfo;
	instr_time    end_time;
        uint64        elapsed;

        if (!enabled)
            return;

        INSTR_TIME_SET_CURRENT(end_time);

	profilerInfo = (profilerCtx *) estate->plugin_info;
	stats = profilerInfo->stmtStats + stmt->lineno ;

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

static char * findSource( Oid oid, HeapTuple * tup, char ** funcName )
{
	bool	isNull;

	*tup = SearchSysCache( PROCOID, ObjectIdGetDatum( oid ), 0, 0, 0 );

	if(!HeapTupleIsValid( *tup ))
		elog( ERROR, "cache lookup for function %u failed", oid );

	*funcName = NameStr(((Form_pg_proc) GETSTRUCT( *tup ))->proname );

	return( DatumGetCString( DirectFunctionCall1( textout, SysCacheGetAttr( PROCOID, *tup, Anum_pg_proc_prosrc, &isNull ))));
}

/* -------------------------------------------------------------------
 * copyLine()
 *
 *	This function creates a null-terminated copy of the given string
 *	(presumably a line of source code).
 * -------------------------------------------------------------------
 */

char * copyLine( const char * src, size_t len )
{
	char * result = palloc(len+1);

	memcpy( result, src, len );
	result[len] = '\0';

	return( result );
}

/* -------------------------------------------------------------------
 * scanSource()
 *
 *	This function scans through the source code for a given function
 *	and counts the number of lines of code present in the string.  If
 *	the caller provides an array of char pointers (dst != NULL), we 
 *  copy each line of code into that array.
 *
 *  You would typically call this function twice:  the first time, you
 *  pass dst = NULL and scanSource() returns the number of lines of 
 *  code found in the string.  Once you know how many lines are present,
 *  you can allocate an array of char pointers and call scanSource()
 *  again - this time around, scanSource() will (non-destructively) split
 *  the source code into that array of char pointers.
 * -------------------------------------------------------------------
 */

int scanSource( const char * dst[], const char * src )
{
	int			 line_count = 0;
	const char * nl;
	
	while(( nl = strchr( src, '\n' )) != NULL )
	{
		/* src points to start of line, nl points to end of line */

		if( dst )
			dst[line_count] = copyLine( src, nl - src );

		line_count++;

		src = nl + 1;
	}

	return( line_count );
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
        HASHCTL         hash_ctl;

        MemSet(&hash_ctl, 0, sizeof(hash_ctl));

        hash_ctl.keysize = sizeof(lineHashKey);
        hash_ctl.entrysize = offsetof(lineEntry, line) + plprofiler_len;
        hash_ctl.hash = line_stats_fn;
        hash_ctl.match = line_match_fn;

        line_stats = hash_create("Function Lines", 
                                 plprofiler_max, 
                                 &hash_ctl, 
                                 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);
}

static uint32
line_stats_fn(const void *key, Size keysize)
{
        const lineHashKey *k = (const lineHashKey *) key;

        return hash_uint32((uint32) k->func_oid) ^
                hash_uint32((uint32) k->line_number);
}

static int
line_match_fn(const void *key1, const void *key2, Size keysize)
{
        const lineHashKey *k1 = (const lineHashKey *) key1;
        const lineHashKey *k2 = (const lineHashKey *) key2;

        if (k1->func_oid == k2->func_oid &&
                k1->line_number == k2->line_number)
                return 0;
        else
                return 1;
}

static lineEntry *
entry_alloc(lineHashKey *key, const char *line, int line_len)
{
        lineEntry  *entry;
        bool       found;
        int        entry_len;

        if (hash_get_num_entries(line_stats) >= plprofiler_max)
                return NULL;

        /* Find or create an entry with desired hash code */
        entry = (lineEntry *) hash_search(line_stats, key, HASH_ENTER, &found);

        if (!found)
        {
                /* New entry, initialize it */

                /* reset the statistics */
                memset(&entry->counters, 0, sizeof(Counters));

                /* ... and don't forget the line text */
                Assert(line_len >= 0);
                if (line_len > plprofiler_len)
                {
                    ereport(LOG, (errcode(ERRCODE_SUCCESSFUL_COMPLETION),
                                    errmsg("Procedural language function line length exceeds plprofiler.line_length: %d ",plprofiler_len),
                                        errhint("Set plprofiler.line_length to %d or greater", line_len)));
                    entry_len = plprofiler_len;
                }
                else 
                    entry_len = line_len;

                entry->line_len = entry_len;
                memcpy(entry->line, line, entry_len);
                entry->line[entry_len] = '\0';
        }

        return entry;
}

Datum
pl_profiler(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	HASH_SEQ_STATUS hash_seq;
	lineEntry  *entry;

	if (!line_stats)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("plprofiler must be loaded and enabled")));

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
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

	hash_seq_init(&hash_seq, line_stats);
	while ((entry = hash_seq_search(&hash_seq)) != NULL)
	{
		Datum		values[PL_PROFILE_COLS];
		bool		nulls[PL_PROFILE_COLS];
		int			i = 0;

		memset(values, 0, sizeof(values));
		memset(nulls, 0, sizeof(nulls));

		values[i++] = ObjectIdGetDatum(entry->key.func_oid);
		values[i++] = Int64GetDatumFast(entry->key.line_number);

		values[i++] = CStringGetTextDatum(entry->line);

		values[i++] = Int64GetDatumFast(entry->counters.exec_count);
		values[i++] = Int64GetDatumFast(entry->counters.total_time);
		values[i++] = Int64GetDatumFast(entry->counters.time_longest);

		Assert(i == PL_PROFILE_COLS);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

Datum
pl_profiler_reset(PG_FUNCTION_ARGS)
{
        HASH_SEQ_STATUS hash_seq;
        lineEntry  *entry;

        if (!line_stats)
                ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("pl_profiler must be loaded")));

        hash_seq_init(&hash_seq, line_stats);
        while ((entry = hash_seq_search(&hash_seq)) != NULL)
        {
                hash_search(line_stats, &entry->key, HASH_REMOVE, NULL);
        }

        PG_RETURN_VOID();
}

Datum
pl_profiler_enable(PG_FUNCTION_ARGS)
{
        if (PG_ARGISNULL(0))
                PG_RETURN_NULL();

        enabled = PG_GETARG_BOOL(0);

        if (enabled && !line_stats)
                InitHashTable();

        PG_RETURN_BOOL(enabled);
}

