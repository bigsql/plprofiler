
import sys
import getopt
import psycopg2
import tempfile
import os
import subprocess
import ConfigParser
import traceback
import json

__all__ = ['save_data', 'list_data', 'edit_data', 'delete_data',
           'get_profiler_namespace']

def save_data(argv):
    opt_conninfo = ''
    opt_name = None
    opt_title = None
    opt_desc = None
    need_edit = False

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv, "c:d:n:t:", [
                'conninfo=', 'name=', 'title=', 'desc=', 'description=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-c', '--conninfo']:
            opt_conninfo = val
        elif opt in ['-n', '--name']:
            opt_name = val
        elif opt in ['-t', '--title']:
            opt_title = val
        elif opt in ['-d', '--desc', '--description']:
            opt_desc = val

    if opt_name is None:
        sys.stderr.write("option --name must be given\n")
        return 2

    # ----
    # Set defaults if options not given.
    # ----
    if opt_title is None:
        need_edit = True
        opt_title = 'PL Profiler Report for ' + opt_name
    if opt_desc is None:
        need_edit = True
        opt_desc = '<h1>' + opt_title + '</h1>\n' + \
                   '<p>\n<!-- description here -->\n</p>\n'

    # ----
    # Create our config.
    # ----
    config = {
        'name':         opt_name,
        'title':        opt_title,
        'tabstop':      8,
        'svg_width':    1200,
        'table_width':  '80%',
        'desc':         opt_desc,
    }

    # ----
    # If we set defaults for config options, invoke an editor.
    # ----
    if need_edit:
        try:
            edit_config_info(config)
        except Exception as err:
            sys.stderr.write(str(err) + '\n')
            traceback.print_exc()
            return 2
        opt_name = config['name']
    
    # ----
    # Connect to the database and get the plprofiler schema name.
    # ----
    try:
        db = psycopg2.connect(opt_conninfo)
        profiler_namespace = get_profiler_namespace(db)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 3

    # ----
    # Aggregate the existing data into a new entry in *_saved.
    # ----
    db.rollback()
    cur = db.cursor()
    cur.execute("""SET work_mem TO '256MB'""", (profiler_namespace, ))
    cur.execute("""SET search_path TO %s""", (profiler_namespace, ))
    cur.execute("""SET TRANSACTION ISOLATION LEVEL REPEATABLE READ""")
    try:
        cur.execute("""INSERT INTO pl_profiler_saved
                            (s_name, s_options)
                        VALUES (%s, %s)""",
                    (opt_name, json.dumps(config)))
    except psycopg2.IntegrityError as err:
        sys.stderr.write(str(err) + '\n')
        db.rollback()
        return 1

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
        sys.stderr.write("ERROR: There is no plprofiler data to save\n")
        sys.stderr.write("HINT: Is the profiler enabled and save_interval configured?\n")
        db.rollback()
        return 1
    print cur.rowcount, "rows inserted into pl_profiler_saved_functions"

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
        sys.stderr.write("ERROR: There is no plprofiler data to save\n")
        sys.stderr.write("HINT: Is the profiler enabled and save_interval configured?\n")
        db.rollback()
        return 1
    print cur.rowcount, "rows inserted into pl_profiler_saved_linestats"

    # cur.execute("""DELETE FROM pl_profiler_linestats_data""")
    # print cur.rowcount, "rows deleted from pl_profiler_linestats_data"

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
    print cur.rowcount, "rows inserted into pl_profiler_saved_callgraph"

    # cur.execute("""DELETE FROM pl_profiler_callgraph_data""")
    # print cur.rowcount, "rows deleted from pl_profiler_callgraph_data"

    db.commit()
    cur.close()
    db.close()

def list_data(argv):
    opt_conninfo = ''

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv, "c:", [
                'conninfo=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-c', '--conninfo']:
            opt_conninfo = val

    # ----
    # Connect to the database and get the plprofiler schema name.
    # ----
    try:
        db = psycopg2.connect(opt_conninfo)
        profiler_namespace = get_profiler_namespace(db)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 3

    # ----
    # Get the list of saved data sets.
    # ----
    cur = db.cursor()
    cur.execute("""SET search_path TO %s""", (profiler_namespace, ))
    cur.execute("""SELECT s_name, s_options
                    FROM pl_profiler_saved
                    ORDER BY s_name""")
    if cur.rowcount == 0:
        print "No saved data sets found"
    else:
        print ""
        max_name_len = 4
        for row in cur:
            if len(row[0]) > max_name_len:
                max_name_len = len(row[0])
        cur.scroll(0, mode = 'absolute')
        print 'Name' + ' '*(max_name_len - 4) + ' | Title'
        print '-'*max_name_len + '-+-' + '-'*(79 - 3 - max_name_len)
        for row in cur:
            config = json.loads(row[1])
            pad = max_name_len - len(row[0])
            print row[0] + ' '*pad + ' | ' + config.get('title', '')

        print ""
        print '(' + str(cur.rowcount) + ' data sets found)'
        print ""
    cur.close()
    db.close()
    return 0

def edit_data(argv):
    opt_conninfo = ''
    opt_name = None

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv, "c:n:", [
                'conninfo=', 'name=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-c', '--conninfo']:
            opt_conninfo = val
        elif opt in ['-n', '--name']:
            opt_name = val

    if opt_name is None:
        sys.stderr.write("option --name must be given\n")
        return 2

    # ----
    # Connect to the database and get the plprofiler schema name.
    # ----
    try:
        db = psycopg2.connect(opt_conninfo)
        profiler_namespace = get_profiler_namespace(db)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 3

    # ----
    # Get the current values and create a config with that.
    # ----
    cur = db.cursor()
    cur.execute("""SET search_path TO %s""", (profiler_namespace, ))
    cur.execute("""SELECT s_options
                    FROM pl_profiler_saved
                    WHERE s_name = %s""", (opt_name, ))
    if cur.rowcount == 0:
        print "No saved data with name '" + opt_name + "' found"
        db.rollback()
        return 1
    row = cur.fetchone()
    config = json.loads(row[0])
    config['name'] = opt_name

    # ----
    # Do not let the DB sit idle in transaction.
    # ----
    db.rollback()

    # ----
    # Launch the editor for the user to edit the info.
    # ----
    try:
        edit_config_info(config)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2
    new_name = config['name']

    cur.execute("""SET search_path TO %s""", (profiler_namespace, ))
    cur.execute("""UPDATE pl_profiler_saved
                    SET s_name = %s,
                        s_options = %s
                    WHERE s_name = %s""",
                (new_name, json.dumps(config), opt_name))
    if cur.rowcount != 1:
        sys.stderr.write("ERROR: data set with name '" + opt_name + 
                         "' no longer exists\n")
        db.rollback()
    else:
        db.commit()
    cur.close()
    db.close()
    return 0

def delete_data(argv):
    opt_conninfo = ''
    opt_name = None

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv, "c:n:", [
                'conninfo=', 'name=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-c', '--conninfo']:
            opt_conninfo = val
        elif opt in ['-n', '--name']:
            opt_name = val

    if opt_name is None:
        sys.stderr.write("option --name must be given\n")
        return 2

    # ----
    # Connect to the database and get the plprofiler schema name.
    # ----
    try:
        db = psycopg2.connect(opt_conninfo)
        profiler_namespace = get_profiler_namespace(db)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 3

    # ----
    # Delete the requested data set.
    # ----
    cur = db.cursor()
    cur.execute("""SET search_path TO %s""", (profiler_namespace, ))
    cur.execute("""DELETE FROM pl_profiler_saved WHERE s_name = %s""",
                (opt_name, ))
    if cur.rowcount == 0:
        print "No saved data with name '" + opt_name + "' found"
    db.commit()
    cur.close()
    db.close()

def get_profiler_namespace(db):
    cur = db.cursor()
    cur.execute("""
            SELECT N.nspname
            FROM pg_catalog.pg_extension E
            JOIN pg_catalog.pg_namespace N ON N.oid = E.extnamespace
            WHERE E.extname = 'plprofiler'
        """);
    row = cur.next()
    if row is None:
        cur.close()
        raise Exception("plprofiler extension not found")
    result = row[0]
    cur.close()
    return result
    
def edit_config_info(config):
    EDITOR = os.environ.get('EDITOR','vim') #that easy!
    opts = ['title', 'tabstop', 'svg_width', 'table_width', 'desc', ]

    name = config['name']
    tmp_config = ConfigParser.RawConfigParser()
    tmp_config.add_section(name)
    for opt in opts:
        tmp_config.set(name, opt, str(config[opt]))

    with tempfile.NamedTemporaryFile(suffix=".tmp.conf") as tf:
        tmp_config.write(tf)
        tf.flush()
        subprocess.call([EDITOR, tf.name])

        for s in tmp_config.sections():
            tmp_config.remove_section(s)

        tf.seek(0)
        tmp_config.readfp(tf)

    if len(tmp_config.sections()) != 1:
        raise Exception("config must have exactly one section")
    name = tmp_config.sections()[0]
    config['name'] = name
    for opt in opts:
        if tmp_config.has_option(name, opt):
            config[opt] = str(tmp_config.get(name, opt))

