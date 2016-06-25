# ----------------------------------------------------------------------
# plprofiler_data
#
#   Class handling all the profiler data.
# ----------------------------------------------------------------------

import psycopg2
import json

__all__ = ['plprofiler_data', ]

class plprofiler_data:
    def __init__(self):
        self.dbconn = None

    def connect(self, conninfo):
        # ----
        # Connect to the database and get the plprofiler schema name.
        # ----
        self.dbconn = psycopg2.connect(conninfo)
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
            """);
        row = cur.next()
        cur.close()
        self.dbconn.rollback()
        if row is None:
            raise Exception("plprofiler extension not found")
        result = row[0]
        return result

    def save_dataset_from_data(self, opt_name, config, overwrite = False):
        # ----
        # Aggregate the existing data found in pl_profiler_linestats_data
        # and pl_profiler_callgraph_data into a new entry in *_saved.
        # ----
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s;""", (self.profiler_namespace, ))
        cur.execute("""SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;""")

        if overwrite:
            cur.execute("""DELETE FROM pl_profiler_saved
                            WHERE s_name = %s""", (opt_name, ))
        try:
            cur.execute("""INSERT INTO pl_profiler_saved
                                (s_name, s_options)
                            VALUES (%s, %s)""",
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
                        WHERE P.oid IN (SELECT DISTINCT func_oid
                                            FROM pl_profiler_linestats_data)
                        GROUP BY s_id, p.oid, nspname, proname
                        ORDER BY s_id, p.oid, nspname, proname""")
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("no data to save found")

        cur.execute("""INSERT INTO pl_profiler_saved_linestats
                            (l_s_id, l_funcoid,
                             l_line_number, l_source, l_exec_count,
                             l_total_time, l_longest_time)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               L.func_oid, L.line_number,
                               coalesce(S.source, '-- SOURCE NOT FOUND'),
                               sum(L.exec_count), sum(L.total_time),
                               max(L.longest_time)
                        FROM pl_profiler_linestats_data L
                        LEFT JOIN pl_profiler_all_source S
                            ON S.func_oid = L.func_oid
                            AND S.line_number = L.line_number
                        GROUP BY s_id, L.func_oid, L.line_number, S.source
                        ORDER BY s_id, L.func_oid, L.line_number""")
        if cur.rowcount == 0:
            self.dbconn.rollback()
            raise Exception("ERROR: There is no plprofiler data to save\n")

        cur.execute("""INSERT INTO pl_profiler_saved_callgraph
                            (c_s_id, c_stack, c_call_count, c_us_total,
                             c_us_children, c_us_self)
                        SELECT currval('pl_profiler_saved_s_id_seq') as s_id,
                               pl_profiler_get_stack(stack) as stack,
                               sum(call_count), sum(us_total),
                               sum(us_children), sum(us_self)
                        FROM pl_profiler_callgraph_data
                        GROUP BY s_id, stack
                        ORDER BY s_id, stack;""")

        cur.close()
        self.dbconn.commit()

    def get_dataset_list(self):
        cur = self.dbconn.cursor()
        cur.execute("""SET search_path TO %s""", (self.profiler_namespace, ))
        cur.execute("""SELECT s_name, s_options
                        FROM pl_profiler_saved
                        ORDER BY s_name""")
        result = cur.fetchall()
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
                             "' no longer exists\n")
        else:
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
                             "' does not exists\n")
        else:
            self.dbconn.commit()
        cur.close()

