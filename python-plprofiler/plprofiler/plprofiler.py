# ----------------------------------------------------------------------
# plprofiler_data
#
#   Class handling all the profiler data.
# ----------------------------------------------------------------------

import psycopg2
import json
import time

from plprofiler_report import plprofiler_report
from sql_split import sql_split

__all__ = ['plprofiler', ]

class plprofiler:
    def __init__(self):
        self.dbconn = None

    def connect(self, connoptions):
        # ----
        # Connect to the database and get the plprofiler schema name.
        # ----
        if len(connoptions) == 0:
            connoptions['dsn'] = ''
        self.dbconn = psycopg2.connect(**connoptions)
        self.profiler_namespace = self.get_profiler_namespace()

    def get_profiler_namespace(self):
        # ----
        # Find out the namespace of the plprofiler extension.
        # ----
        cur = self.dbconn.cursor()
        cur.execute("""
                SELECT N.nspname
                FROM pg_catalog.pg_extension E
                JOIN pg_catalog.pg_namespace N ON N.oid = E.extnamespace
                WHERE E.extname = 'plprofiler'
            """)
        row = cur.fetchone()
        if row is None:
            cur.execute("""SELECT pg_catalog.current_database()""")
            dbname = cur.fetchone()[0]
            cur.close()
            self.dbconn.rollback()
            raise Exception('ERROR: plprofiler extension not found in ' +
                            'database "%s"' %dbname)
        result = row[0]
        cur.close()
        self.dbconn.rollback()
        return result

    def save_dataset_from_local(self, opt_name, config, overwrite = False):
        # ----
        # Aggregate the existing data found in pl_profiler_linestats_local
        # and pl_profiler_callgraph_local into a new entry in *_saved.
        # ----
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s;""", (self.profiler_namespace, ))
        cur.execute("""SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;""")

        try:
            if overwrite:
                cur.execute("""DELETE FROM pl_profiler_saved
                                WHERE s_name = %s""", (opt_name, ))
            cur.execute("""INSERT INTO pl_profiler_saved
                                (s_name, s_options, s_callgraph_overflow,
                                 s_functions_overflow, s_lines_overflow)
                            VALUES (%s, %s, false, false, false)""",
                        (opt_name, json.dumps(config)))
        except psycopg2.IntegrityError as err:
            self.dbconn.rollback()
            raise err

        cur.execute("""INSERT INTO pl_profiler_saved_functions
                            (f_s_id, f_funcoid, f_schema, f_funcname,
                             f_funcresult, f_funcargs)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               P.oid, N.nspname, P.proname,
                               pg_catalog.pg_get_function_result(P.oid) as func_result,
                               pg_catalog.pg_get_function_arguments(P.oid) as func_args
                        FROM pg_catalog.pg_proc P
                        JOIN pg_catalog.pg_namespace N on N.oid = P.pronamespace
                        WHERE P.oid IN (SELECT * FROM unnest(pl_profiler_func_oids_local()))
                        GROUP BY s_id, p.oid, nspname, proname
                        ORDER BY s_id, p.oid, nspname, proname""")
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("No function data to save found")

        cur.execute("""INSERT INTO pl_profiler_saved_linestats
                            (l_s_id, l_funcoid,
                             l_line_number, l_source, l_exec_count,
                             l_total_time, l_longest_time)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               L.func_oid, L.line_number,
                               coalesce(S.source, '-- SOURCE NOT FOUND'),
                               sum(L.exec_count), sum(L.total_time),
                               max(L.longest_time)
                        FROM pl_profiler_linestats_local() L
                        JOIN pl_profiler_funcs_source(pl_profiler_func_oids_local()) S
                            ON S.func_oid = L.func_oid
                            AND S.line_number = L.line_number
                        GROUP BY s_id, L.func_oid, L.line_number, S.source
                        ORDER BY s_id, L.func_oid, L.line_number""")
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("No plprofiler data to save")

        cur.execute("""INSERT INTO pl_profiler_saved_callgraph
                            (c_s_id, c_stack, c_call_count, c_us_total,
                             c_us_children, c_us_self)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               pl_profiler_get_stack(stack),
                               sum(call_count), sum(us_total),
                               sum(us_children), sum(us_self)
                        FROM pl_profiler_callgraph_local()
                        GROUP BY s_id, stack
                        ORDER BY s_id, stack;""")

        cur.execute("""RESET search_path""")
        cur.close()
        self.dbconn.commit()

    def save_dataset_from_shared(self, opt_name, config, overwrite = False):
        # ----
        # Aggregate the existing data found in pl_profiler_linestats_shared
        # and pl_profiler_callgraph_shared into a new entry in *_saved.
        # ----
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s;""", (self.profiler_namespace, ))
        cur.execute("""SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;""")

        try:
            if overwrite:
                cur.execute("""DELETE FROM pl_profiler_saved
                                WHERE s_name = %s""", (opt_name, ))
            cur.execute("""INSERT INTO pl_profiler_saved
                                (s_name, s_options, s_callgraph_overflow,
                                 s_functions_overflow, s_lines_overflow)
                            VALUES (%s, %s, pl_profiler_callgraph_overflow(),
                                    pl_profiler_functions_overflow(),
                                    pl_profiler_lines_overflow())""",
                        (opt_name, json.dumps(config)))
        except psycopg2.IntegrityError as err:
            self.dbconn.rollback()
            raise err

        cur.execute("""INSERT INTO pl_profiler_saved_functions
                            (f_s_id, f_funcoid, f_schema, f_funcname,
                             f_funcresult, f_funcargs)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               P.oid, N.nspname, P.proname,
                               pg_catalog.pg_get_function_result(P.oid) as func_result,
                               pg_catalog.pg_get_function_arguments(P.oid) as func_args
                        FROM pg_catalog.pg_proc P
                        JOIN pg_catalog.pg_namespace N on N.oid = P.pronamespace
                        WHERE P.oid IN (SELECT * FROM unnest(pl_profiler_func_oids_shared()))
                        GROUP BY s_id, p.oid, nspname, proname
                        ORDER BY s_id, p.oid, nspname, proname""")
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("No function data to save found")

        cur.execute("""INSERT INTO pl_profiler_saved_linestats
                            (l_s_id, l_funcoid,
                             l_line_number, l_source, l_exec_count,
                             l_total_time, l_longest_time)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               L.func_oid, L.line_number,
                               coalesce(S.source, '-- SOURCE NOT FOUND'),
                               sum(L.exec_count), sum(L.total_time),
                               max(L.longest_time)
                        FROM pl_profiler_linestats_shared() L
                        JOIN pl_profiler_funcs_source(pl_profiler_func_oids_shared()) S
                            ON S.func_oid = L.func_oid
                            AND S.line_number = L.line_number
                        GROUP BY s_id, L.func_oid, L.line_number, S.source
                        ORDER BY s_id, L.func_oid, L.line_number""")
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("No plprofiler data to save")

        cur.execute("""INSERT INTO pl_profiler_saved_callgraph
                            (c_s_id, c_stack, c_call_count, c_us_total,
                             c_us_children, c_us_self)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               pl_profiler_get_stack(stack),
                               sum(call_count), sum(us_total),
                               sum(us_children), sum(us_self)
                        FROM pl_profiler_callgraph_shared()
                        GROUP BY s_id, stack
                        ORDER BY s_id, stack;""")

        cur.execute("""RESET search_path""")
        cur.close()
        self.dbconn.commit()

    def save_dataset_from_report(self, report_data, overwrite = False):
        # ----
        # Save a dataset from the output of a get_*_report_data function.
        # This is used by the "import" command.
        # ----
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s;""", (self.profiler_namespace, ))
        cur.execute("""SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;""")

        config = report_data['config']
        opt_name = config['name']

        # ----
        # Add defaults for missing attributes in previous version.
        # ----
        if 'callgraph_overflow' not in report_data:
            report_data['callgraph_overflow'] = False
            report_data['functions_overflow'] = False
            report_data['lines_overflow'] = False

        # ----
        # Load the pl_profiler_saved entry.
        # ----
        try:
            if overwrite:
                cur.execute("""DELETE FROM pl_profiler_saved
                                WHERE s_name = %s""", (opt_name, ))
            cur.execute("""INSERT INTO pl_profiler_saved
                                (s_name, s_options, s_callgraph_overflow,
                                 s_function_overflow, s_lines_overflow)
                            VALUES (%s, %s, %s, %s, %s)""",
                        (opt_name, json.dumps(config),
                         report_data['callgraph_overflow'],
                         report_data['functions_overflow'],
                         report_data['lines_overflow'],))
        except psycopg2.IntegrityError as err:
            self.dbconn.rollback()
            raise err

        # ----
        # From the funcdefs, load the pl_profiler_saved_functions
        # and the pl_profiler_saved_linestats.
        # ----
        for funcdef in report_data['func_defs']:
            cur.execute("""INSERT INTO pl_profiler_saved_functions
                                (f_s_id, f_funcoid, f_schema, f_funcname,
                                 f_funcresult, f_funcargs)
                            VALUES
                                (currval('pl_profiler_saved_s_id_seq'),
                                 %s, %s, %s, %s, %s)""",
                            (funcdef['funcoid'], funcdef['schema'],
                             funcdef['funcname'], funcdef['funcresult'],
                             funcdef['funcargs']))

            for src in funcdef['source']:
                cur.execute("""INSERT INTO pl_profiler_saved_linestats
                                    (l_s_id, l_funcoid,
                                     l_line_number, l_source, l_exec_count,
                                     l_total_time, l_longest_time)
                                VALUES
                                    (currval('pl_profiler_saved_s_id_seq'),
                                     %s, %s, %s, %s, %s, %s)""",
                                (funcdef['funcoid'], src['line_number'],
                                 src['source'], src['exec_count'],
                                 src['total_time'], src['longest_time'], ))

        # ----
        # Finally insert the callgraph data.
        # ----
        for row in report_data['callgraph']:
            cur.execute("""INSERT INTO pl_profiler_saved_callgraph
                                (c_s_id, c_stack, c_call_count, c_us_total,
                                 c_us_children, c_us_self)
                            VALUES
                                (currval('pl_profiler_saved_s_id_seq'),
                                 %s::text[], %s, %s, %s, %s)""", row)

        cur.execute("""RESET search_path""")
        cur.close()
        self.dbconn.commit()

    def get_dataset_list(self):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT s_name, s_options
                        FROM pl_profiler_saved
                        ORDER BY s_name""")
        result = cur.fetchall()
        cur.execute("""RESET search_path""")
        cur.close()
        self.dbconn.rollback()
        return result

    def get_dataset_config(self, opt_name):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT s_options
                        FROM pl_profiler_saved
                        WHERE s_name = %s""", (opt_name, ))
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("No saved data with name '" + opt_name + "' found")
        row = cur.fetchone()
        config = json.loads(row[0])
        config['name'] = opt_name
        cur.execute("""RESET search_path""")
        cur.close()
        self.dbconn.rollback()

        return config

    def update_dataset_config(self, opt_name, new_name, config):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""UPDATE pl_profiler_saved
                        SET s_name = %s,
                            s_options = %s
                        WHERE s_name = %s""",
                    (new_name, json.dumps(config), opt_name))
        if cur.rowcount != 1:
            self.dbconn.rollback()
            raise Exception("Data set with name '" + opt_name +
                             "' no longer exists")
        else:
            cur.execute("""RESET search_path""")
            self.dbconn.commit()
        cur.close()

    def delete_dataset(self, opt_name):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""DELETE FROM pl_profiler_saved
                        WHERE s_name = %s""",
                    (opt_name, ))
        if cur.rowcount != 1:
            self.dbconn.rollback()
            raise Exception("Data set with name '" + opt_name +
                             "' does not exists")
        else:
            cur.execute("""RESET search_path""")
            self.dbconn.commit()
        cur.close()

    def get_local_report_data(self, opt_name, opt_top, func_oids):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))

        # ----
        # Create a default config.
        # ----
        config = {
                'name': opt_name,
                'title': 'PL Profiler Report for %s' %(opt_name, ),
                'tabstop': '8',
                'svg_width': '1200',
                'table_width': '80%',
                'desc': '<h1>PL Profiler Report for %s</h1>\n' %(opt_name, ) +
                        '<p>\n<!-- description here -->\n</p>',
            }

        # ----
        # If not specified, find the top N functions by self time.
        # ----
        found_more_funcs = False
        if func_oids is None or len(func_oids) == 0:
            func_oids_by_user = False
            func_oids = []
            cur.execute("""SELECT stack[array_upper(stack, 1)] as func_oid,
                                sum(us_self) as us_self
                            FROM pl_profiler_callgraph_local() C
                            GROUP BY func_oid
                            ORDER BY us_self DESC
                            LIMIT %s""", (opt_top + 1, ))
            for row in cur:
                func_oids.append(int(row[0]))
            if len(func_oids) > opt_top:
                func_oids = func_oids[:-1]
                found_more_funcs = True
        else:
            func_oids_by_user = True
            func_oids = [int(x) for x in func_oids]

        if len(func_oids) == 0:
            raise Exception("No profiling data found")

        # ----
        # Get an alphabetically sorted list of the selected functions.
        # ----
        cur.execute("""SELECT P.oid, N.nspname, P.proname
                        FROM pg_catalog.pg_proc P
                        JOIN pg_catalog.pg_namespace N ON N.oid = P.pronamespace
                        WHERE P.oid IN (SELECT * FROM unnest(%s))
                        ORDER BY upper(nspname), nspname,
                                 upper(proname), proname""", (func_oids, ))

        func_list = []
        for row in cur:
            func_list.append({
                    'funcoid':  str(row[0]),
                    'schema': str(row[1]),
                    'funcname': str(row[2]),
                })

        # ----
        # The view for linestats is extremely inefficient. We select
        # all of it once and cache it in a hash table.
        # ----
        linestats = {}
        cur.execute("""SELECT L.func_oid, L.line_number,
                            sum(L.exec_count)::bigint AS exec_count,
                            sum(L.total_time)::bigint AS total_time,
                            max(L.longest_time)::bigint AS longest_time,
                            S.source
                        FROM pl_profiler_linestats_local() L
                        JOIN pl_profiler_funcs_source(pl_profiler_func_oids_local()) S
                            ON S.func_oid = L.func_oid
                            AND S.line_number = L.line_number
                        GROUP BY L.func_oid, L.line_number, S.source
                        ORDER BY L.func_oid, L.line_number""")
        for row in cur:
            if row[0] not in linestats:
                linestats[row[0]] = []
            linestats[row[0]].append(row)

        # ----
        # Build a list of function definitions in the order, specified
        # by the func_oids list. This is either the oids, requested by
        # the user or the oids determined above in descending order of
        # self_time.
        # ----
        func_defs = []
        for func_oid in func_oids:
            # ----
            # First get the function definition and overall stats.
            # ----
            cur.execute("""WITH SELF AS (SELECT
                                stack[array_upper(stack, 1)] as func_oid,
                                    sum(us_self) as us_self
                                FROM pl_profiler_callgraph_local()
                                GROUP BY func_oid)
                        SELECT P.oid, N.nspname, P.proname,
                            pg_catalog.pg_get_function_result(P.oid),
                            pg_catalog.pg_get_function_arguments(P.oid),
                            coalesce(SELF.us_self, 0) as self_time
                            FROM pg_catalog.pg_proc P
                            JOIN pg_catalog.pg_namespace N ON N.oid = P.pronamespace
                            LEFT JOIN SELF ON SELF.func_oid = P.oid
                            WHERE P.oid = %s""",
                        (func_oid, ))
            row = cur.fetchone()
            if row is None:
                raise Exception("function with Oid %d not found\n" %func_oid)

            # ----
            # With that we can start the definition.
            # ----
            func_def = {
                    'funcoid': func_oid,
                    'schema': row[1],
                    'funcname': row[2],
                    'funcresult': row[3],
                    'funcargs': row[4],
                    'total_time': linestats[func_oid][0][3],
                    'self_time': int(row[5]),
                    'source': [],
                }

            # ----
            # Add all the source code lines to that.
            # ----
            for row in linestats[func_oid]:
                func_def['source'].append({
                        'line_number': int(row[1]),
                        'source': row[5],
                        'exec_count': int(row[2]),
                        'total_time': int(row[3]),
                        'longest_time': int(row[4]),
                    })

            # ----
            # Add this function to the list of function definitions.
            # ----
            func_defs.append(func_def)

        # ----
        # Get the callgraph data.
        # ----
        cur.execute("""SELECT array_to_string(pl_profiler_get_stack(stack), ';'),
                            stack,
                            call_count, us_total, us_children, us_self
                        FROM pl_profiler_callgraph_local()""")
        flamedata = ""
        callgraph = []
        for row in cur:
            flamedata += str(row[0]) + " " + str(row[5]) + "\n"
            callgraph.append(row[1:])

        # ----
        # That is it. Reset things and return the report data.
        # ----
        cur.execute("""RESET search_path""");
        self.dbconn.rollback()

        return {
                'config': config,
                'callgraph_overflow': False,
                'functions_overflow': False,
                'lines_overflow': False,
                'func_list': func_list,
                'func_defs': func_defs,
                'flamedata': flamedata,
                'callgraph': callgraph,
                'func_oids_by_user': func_oids_by_user,
                'found_more_funcs': found_more_funcs,
            }

    def get_shared_report_data(self, opt_name, opt_top, func_oids):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))

        # ----
        # Create a default config.
        # ----
        config = {
                'name': opt_name,
                'title': 'PL Profiler Report for %s' %(opt_name, ),
                'tabstop': '8',
                'svg_width': '1200',
                'table_width': '80%',
                'desc': '<h1>PL Profiler Report for %s</h1>\n' %(opt_name, ) +
                        '<p>\n<!-- description here -->\n</p>',
            }

        # ----
        # If not specified, find the top N functions by self time.
        # ----
        found_more_funcs = False
        if func_oids is None or len(func_oids) == 0:
            func_oids_by_user = False
            func_oids = []
            cur.execute("""SELECT stack[array_upper(stack, 1)] as func_oid,
                                sum(us_self) as us_self
                            FROM pl_profiler_callgraph_shared() C
                            GROUP BY func_oid
                            ORDER BY us_self DESC
                            LIMIT %s""", (opt_top + 1, ))
            for row in cur:
                func_oids.append(int(row[0]))
            if len(func_oids) > opt_top:
                func_oids = func_oids[:-1]
                found_more_funcs = True
        else:
            func_oids_by_user = True
            func_oids = [int(x) for x in func_oids]

        if len(func_oids) == 0:
            raise Exception("No profiling data found")

        # ----
        # Get an alphabetically sorted list of the selected functions.
        # ----
        cur.execute("""SELECT P.oid, N.nspname, P.proname
                        FROM pg_catalog.pg_proc P
                        JOIN pg_catalog.pg_namespace N ON N.oid = P.pronamespace
                        WHERE P.oid IN (SELECT * FROM unnest(%s))
                        ORDER BY upper(nspname), nspname,
                                 upper(proname), proname""", (func_oids, ))

        func_list = []
        for row in cur:
            func_list.append({
                    'funcoid':  str(row[0]),
                    'schema': str(row[1]),
                    'funcname': str(row[2]),
                })

        # ----
        # The view for linestats is extremely inefficient. We select
        # all of it once and cache it in a hash table.
        # ----
        linestats = {}
        cur.execute("""SELECT L.func_oid, L.line_number,
                            sum(L.exec_count)::bigint AS exec_count,
                            sum(L.total_time)::bigint AS total_time,
                            max(L.longest_time)::bigint AS longest_time,
                            S.source
                        FROM pl_profiler_linestats_shared() L
                        JOIN pl_profiler_funcs_source(pl_profiler_func_oids_shared()) S
                            ON S.func_oid = L.func_oid
                            AND S.line_number = L.line_number
                        GROUP BY L.func_oid, L.line_number, S.source
                        ORDER BY L.func_oid, L.line_number""")
        for row in cur:
            if row[0] not in linestats:
                linestats[row[0]] = []
            linestats[row[0]].append(row)

        # ----
        # Build a list of function definitions in the order, specified
        # by the func_oids list. This is either the oids, requested by
        # the user or the oids determined above in descending order of
        # self_time.
        # ----
        func_defs = []
        for func_oid in func_oids:
            # ----
            # First get the function definition and overall stats.
            # ----
            cur.execute("""WITH SELF AS (SELECT
                                stack[array_upper(stack, 1)] as func_oid,
                                    sum(us_self) as us_self
                                FROM pl_profiler_callgraph_shared()
                                GROUP BY func_oid)
                        SELECT P.oid, N.nspname, P.proname,
                            pg_catalog.pg_get_function_result(P.oid),
                            pg_catalog.pg_get_function_arguments(P.oid),
                            coalesce(SELF.us_self, 0) as self_time
                            FROM pg_catalog.pg_proc P
                            JOIN pg_catalog.pg_namespace N ON N.oid = P.pronamespace
                            LEFT JOIN SELF ON SELF.func_oid = P.oid
                            WHERE P.oid = %s""",
                        (func_oid, ))
            row = cur.fetchone()
            if row is None:
                raise Exception("function with Oid %d not found\n" %func_oid)

            # ----
            # With that we can start the definition.
            # ----
            func_def = {
                    'funcoid': func_oid,
                    'schema': row[1],
                    'funcname': row[2],
                    'funcresult': row[3],
                    'funcargs': row[4],
                    'total_time': linestats[func_oid][0][3],
                    'self_time': int(row[5]),
                    'source': [],
                }

            # ----
            # Add all the source code lines to that.
            # ----
            for row in linestats[func_oid]:
                func_def['source'].append({
                        'line_number': int(row[1]),
                        'source': row[5],
                        'exec_count': int(row[2]),
                        'total_time': int(row[3]),
                        'longest_time': int(row[4]),
                    })

            # ----
            # Add this function to the list of function definitions.
            # ----
            func_defs.append(func_def)

        # ----
        # Get the callgraph data.
        # ----
        cur.execute("""SELECT array_to_string(pl_profiler_get_stack(stack), ';'),
                            stack,
                            call_count, us_total, us_children, us_self
                        FROM pl_profiler_callgraph_shared()""")
        flamedata = ""
        callgraph = []
        for row in cur:
            flamedata += str(row[0]) + " " + str(row[5]) + "\n"
            callgraph.append(row[1:])

        # ----
        # Get the status of the overflow flags.
        # ----
        cur.execute("""SELECT
                pl_profiler_callgraph_overflow(),
                pl_profiler_functions_overflow(),
                pl_profiler_lines_overflow()
            """)
        overflow_flags = cur.fetchone()

        # ----
        # That is it. Reset things and return the report data.
        # ----
        cur.execute("""RESET search_path""");
        self.dbconn.rollback()

        return {
                'config': config,
                'callgraph_overflow': overflow_flags[0],
                'functions_overflow': overflow_flags[1],
                'lines_overflow': overflow_flags[2],
                'func_list': func_list,
                'func_defs': func_defs,
                'flamedata': flamedata,
                'callgraph': callgraph,
                'func_oids_by_user': func_oids_by_user,
                'found_more_funcs': found_more_funcs,
            }

    def get_saved_report_data(self, opt_name, opt_top, func_oids):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))

        # ----
        # Get the config of the saved dataset.
        # ----
        cur.execute("""SELECT s_options
                        FROM pl_profiler_saved
                        WHERE s_name = %s""", (opt_name, ))
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("No saved data with name '" + opt_name + "' found")
        row = cur.fetchone()
        config = json.loads(row[0])
        config['name'] = opt_name

        # ----
        # If not specified, find the top N functions by self time.
        # ----
        found_more_funcs = False
        if func_oids is None or len(func_oids) == 0:
            func_oids_by_user = False
            func_oids = []
            cur.execute("""SELECT regexp_replace(c_stack[array_upper(c_stack, 1)],
                                  E'.* oid=\\([0-9]*\\)$', E'\\\\1') as func_oid,
                                sum(c_us_self) as us_self
                            FROM pl_profiler_saved S
                            JOIN pl_profiler_saved_callgraph C
                                ON C.c_s_id = S.s_id
                            WHERE S.s_name = %s
                            GROUP BY func_oid
                            ORDER BY us_self DESC
                            LIMIT %s""", (opt_name, opt_top + 1, ))
            for row in cur:
                func_oids.append(int(row[0]))
            if len(func_oids) > opt_top:
                func_oids = func_oids[:-1]
                found_more_funcs = True
        else:
            func_oids_by_user = True
            func_oids = [int(x) for x in func_oids]

        if len(func_oids) == 0:
            raise Exception("No profiling data found")

        # ----
        # Get an alphabetically sorted list of the selected functions.
        # ----
        cur.execute("""SELECT f_funcoid, f_schema, f_funcname
                        FROM pl_profiler_saved S
                        JOIN pl_profiler_saved_functions F
                            ON F.f_s_id = S.s_id
                        WHERE S.s_name = %s
                        AND F.f_funcoid IN (SELECT * FROM unnest(%s))
                        ORDER BY upper(f_schema), f_schema,
                                 upper(f_funcname), f_funcname""", (opt_name,
                                                                    func_oids, ))
        func_list = []
        for row in cur:
            func_list.append({
                    'funcoid':  str(row[0]),
                    'schema': str(row[1]),
                    'funcname': str(row[2]),
                })

        # ----
        # Build a list of function definitions in the order, specified
        # by the func_oids list. This is either the oids, requested by
        # the user or the oids determined above in descending order of
        # self_time.
        # ----
        func_defs = []
        for func_oid in func_oids:
            # ----
            # First get the function definition and overall stats.
            # ----
            cur.execute("""WITH SELF AS (
                            SELECT regexp_replace(c_stack[array_upper(c_stack, 1)],
                                      E'.* oid=\\([0-9]*\\)$', E'\\\\1') as func_oid,
                                    sum(c_us_self) as us_self
                                FROM pl_profiler_saved S
                                JOIN pl_profiler_saved_callgraph C
                                    ON C.c_s_id = S.s_id
                                WHERE S.s_name = %s
                                GROUP BY func_oid)
                        SELECT l_funcoid, f_schema, f_funcname,
                            f_funcresult, f_funcargs,
                            coalesce(l_total_time, 0) as total_time,
                            coalesce(SELF.us_self, 0) as self_time
                            FROM pl_profiler_saved S
                            LEFT JOIN pl_profiler_saved_linestats L ON l_s_id = s_id
                            JOIN pl_profiler_saved_functions F ON f_funcoid = l_funcoid
                            LEFT JOIN SELF ON SELF.func_oid::bigint = f_funcoid
                            WHERE S.s_name = %s
                              AND L.l_funcoid = %s
                              AND L.l_line_number = 0""",
                        (opt_name, opt_name, func_oid, ))
            row = cur.fetchone()
            if row is None:
                raise Exception("function with Oid %d not found\n" %func_oid)

            # ----
            # With that we can start the definition.
            # ----
            func_def = {
                    'funcoid': func_oid,
                    'schema': row[1],
                    'funcname': row[2],
                    'funcresult': row[3],
                    'funcargs': row[4],
                    'total_time': int(row[5]),
                    'self_time': int(row[6]),
                    'source': [],
                }

            # ----
            # Add all the source code lines to that.
            # ----
            cur.execute("""SELECT l_line_number, l_source, l_exec_count,
                            l_total_time, l_longest_time
                            FROM pl_profiler_saved S
                            JOIN pl_profiler_saved_linestats L ON L.l_s_id = S.s_id
                            WHERE S.s_name = %s
                              AND L.l_funcoid = %s
                            ORDER BY l_s_id, l_funcoid, l_line_number""",
                            (opt_name, func_oid, ))
            for row in cur:
                func_def['source'].append({
                        'line_number': int(row[0]),
                        'source': row[1],
                        'exec_count': int(row[2]),
                        'total_time': int(row[3]),
                        'longest_time': int(row[4]),
                    })

            # ----
            # Add this function to the list of function definitions.
            # ----
            func_defs.append(func_def)

        # ----
        # Get the callgraph data.
        # ----
        cur.execute("""SELECT array_to_string(c_stack, ';'),
                            c_stack,
                            c_call_count, c_us_total, c_us_children, c_us_self
                        FROM pl_profiler_saved S
                        JOIN pl_profiler_saved_callgraph C ON C.c_s_id = S.s_id
                        WHERE S.s_name = %s""",
                    (opt_name, ))
        flamedata = ""
        callgraph = []
        for row in cur:
            flamedata += str(row[0]) + " " + str(row[5]) + "\n"
            callgraph.append(row[1:])

        # ----
        # That is it. Reset things and return the report data.
        # ----
        cur.execute("""RESET search_path""");
        self.dbconn.rollback()

        return {
                'config': config,
                'func_list': func_list,
                'func_defs': func_defs,
                'flamedata': flamedata,
                'callgraph': callgraph,
                'func_oids_by_user': func_oids_by_user,
                'found_more_funcs': found_more_funcs,
            }

    def enable(self):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT pl_profiler_enable(true)""")
        cur.execute("""SET plprofiler.collect_interval TO 0""")
        cur.execute("""RESET search_path""")
        self.dbconn.commit()
        cur.close()

    def disable(self):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT pl_profiler_enable(false)""")
        cur.execute("""RESET plprofiler.collect_interval""")
        cur.execute("""RESET search_path""")
        self.dbconn.commit()
        cur.close()

    def enable_monitor(self, opt_pid = None, opt_interval = 10):
        self.dbconn.autocommit = True
        cur = self.dbconn.cursor()
        cur.execute("""
                SELECT setting
                  FROM pg_catalog.pg_settings
                 WHERE name = 'server_version_num'
            """)
        server_version_num = int(cur.fetchone()[0])
        if server_version_num < 90400:
            cur.execute("""
                    SELECT setting
                      FROM pg_catalog.pg_settings
                     WHERE name = 'server_version'
                """)
            server_version = cur.fetchone()[0]
            self.dbconn.autocommit = False
            raise Exception(("ERROR: monitor command not supported on " +
                            "server version %s. Perform monitoring manually " +
                            "via postgresql.conf changes and reloading " +
                            "the postmaster.") %server_version)

        if opt_pid is not None:
            cur.execute("""ALTER SYSTEM SET plprofiler.enable_pid TO %s""", (opt_pid, ))
        else:
            cur.execute("""ALTER SYSTEM SET plprofiler.enabled TO on""")
        cur.execute("""ALTER SYSTEM SET plprofiler.collect_interval TO %s""", (opt_interval, ))
        cur.execute("""SELECT pg_catalog.pg_reload_conf()""")
        self.dbconn.autocommit = False
        cur.close()

    def disable_monitor(self):
        self.dbconn.autocommit = True
        cur = self.dbconn.cursor()
        cur.execute("""ALTER SYSTEM RESET plprofiler.enable_pid""")
        cur.execute("""ALTER SYSTEM RESET plprofiler.enabled""")
        cur.execute("""ALTER SYSTEM RESET plprofiler.collect_interval""");
        cur.execute("""SELECT pg_catalog.pg_reload_conf()""")
        self.dbconn.autocommit = False
        cur.close()

    def reset_local(self):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT pl_profiler_reset_local()""")
        cur.execute("""RESET search_path""")
        self.dbconn.commit()
        cur.close()

    def reset_shared(self):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT pl_profiler_reset_shared()""")
        cur.execute("""RESET search_path""")
        self.dbconn.commit()
        cur.close()

    def save_collect_data(self):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT pl_profiler_collect_data()""")
        cur.execute("""RESET search_path""")
        self.dbconn.commit()
        cur.close()

    def execute_sql(self, sql, output = None):
        try:
            self.dbconn.autocommit = True
            cur = self.dbconn.cursor()
            for query in sql_split(sql).get_statements():
                if output is not None:
                    print >> output, query
                start_time = time.time()
                try:
                    cur.execute(query)
                    end_time = time.time()
                    if cur.description is not None:
                        if cur.rowcount == 0:
                            if output is not None:
                                print >> output, "(0 rows)"
                        else:
                            max_col_len = max([len(d.name) for d in cur.description])
                            cols = ['  ' + ' '*(max_col_len - len(d[0])) + d[0] + ':'
                                        for d in cur.description]
                            if output is not None:
                                for row in cur:
                                    print >> output, "-- row" + str(cur.rownumber) + ":"
                                    for col in range(0, len(cols)):
                                        print >> output, cols[col], str(row[col])
                                print >> output, "----"
                                print >> output, "(%d rows)" %(cur.rowcount, )
                except Exception as err:
                    end_time = time.time()
                    print >> output, "ERROR: " + str(err)
                latency = end_time - start_time
                if output is not None:
                    print >> output, cur.statusmessage, "(%.3f seconds)" %latency
                    print >> output, ""
            self.dbconn.autocommit = False
        except Exception as err:
            self.dbconn.autocommit = False
            raise err
        self.dbconn.rollback()

    def report(self, report_data, output_fd):
        report = plprofiler_report()
        report.generate(report_data, output_fd)
