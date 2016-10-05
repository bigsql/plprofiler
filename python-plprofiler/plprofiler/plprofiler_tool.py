#!/usr/bin/env python

import ConfigParser
import getopt
import json
import os
import StringIO
import subprocess
import sys
import tempfile
import time
import traceback

from plprofiler import plprofiler

__all__ = ['main']

def main():
    if len(sys.argv) == 1:
        usage()
        return 2

    if sys.argv[1] in ['-?', '--help', 'help']:
        if len(sys.argv) == 2:
            usage()
        else:
            if sys.argv[2] == 'save':
                help_save()
            elif sys.argv[2] == 'list':
                help_list()
            elif sys.argv[2] == 'edit':
                help_edit()
            elif sys.argv[2] == 'delete':
                help_delete()
            elif sys.argv[2] == 'reset':
                help_reset()
            elif sys.argv[2] == 'report':
                help_report()
            elif sys.argv[2] == 'export':
                help_export()
            elif sys.argv[2] == 'import':
                help_import()
            elif sys.argv[2] == 'run':
                help_run()
            elif sys.argv[2] == 'monitor':
                help_monitor()
            else:
                usage()
        return 0

    if sys.argv[1] == 'save':
        return save_command(sys.argv[2:])

    if sys.argv[1] == 'list':
        return list_command(sys.argv[2:])

    if sys.argv[1] == 'edit':
        return edit_command(sys.argv[2:])

    if sys.argv[1] == 'delete':
        return delete_command(sys.argv[2:])

    if sys.argv[1] == 'reset':
        return reset_command(sys.argv[2:])

    if sys.argv[1] == 'report':
        return report_command(sys.argv[2:])

    if sys.argv[1] == 'export':
        return export_command(sys.argv[2:])

    if sys.argv[1] == 'import':
        return import_command(sys.argv[2:])

    if sys.argv[1] == 'run':
        return run_command(sys.argv[2:])

    if sys.argv[1] == 'monitor':
        return monitor_command(sys.argv[2:])

    sys.stderr.write("ERROR: unknown command '%s'\n" %(sys.argv[1]))
    return 2

# ----
# save_data_command
# ----
def save_command(argv):
    connoptions = {}
    opt_name = None
    opt_title = None
    opt_desc = None
    opt_force = False
    need_edit = False

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # save command specific options
                'name=', 'title=', 'desc=', 'description=', 'force', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_save()
            return 0

        elif opt in ['--name']:
            opt_name = val
        elif opt in ['--title']:
            opt_title = val
        elif opt in ['--desc', '--description']:
            opt_desc = val
        elif opt in ['--force']:
            opt_force = True

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

    try:
        plp = plprofiler()
        plp.connect(connoptions)
        plp.save_dataset_from_shared(opt_name, config, opt_force)

    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        traceback.print_exc()
        return 1

def list_command(argv):
    connoptions = {}

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # list command specific options (none at the moment)
                ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_list()
            return 0

    # ----
    # Get the list of saved data sets.
    # ----
    try:
        plp = plprofiler()
        plp.connect(connoptions)
        rows = plp.get_dataset_list()
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    if len(rows) == 0:
        print "No saved data sets found"
    else:
        print ""
        max_name_len = 4
        for row in rows:
            if len(row[0]) > max_name_len:
                max_name_len = len(row[0])
        print 'Name' + ' '*(max_name_len - 4) + ' | Title'
        print '-'*max_name_len + '-+-' + '-'*(79 - 3 - max_name_len)
        for row in rows:
            config = json.loads(row[1])
            pad = max_name_len - len(row[0])
            print row[0] + ' '*pad + ' | ' + config.get('title', '')

        print ""
        print '(' + str(len(rows)) + ' data sets found)'
        print ""
    return 0

def edit_command(argv):
    connoptions = {}
    opt_name = None

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # edit command specific coptions
                'name=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_edit()
            return 0

        elif opt in ['--name']:
            opt_name = val

    if opt_name is None:
        sys.stderr.write("option --name must be given\n")
        return 2

    # ----
    # Get the current values and create a config with that.
    # ----
    try:
        plp = plprofiler()
        plp.connect(connoptions)
        config = plp.get_dataset_config(opt_name)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    # ----
    # Launch the editor for the user to edit the info.
    # ----
    try:
        edit_config_info(config)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2
    new_name = config['name']

    # ----
    # Update the dataset config
    # ----
    try:
        plp.update_dataset_config(opt_name, new_name, config)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    return 0

def delete_command(argv):
    connoptions = {}
    opt_name = None

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # edit command specific coptions
                'name=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_delete()
            return 0

        elif opt in ['--name']:
            opt_name = val

    if opt_name is None:
        sys.stderr.write("option --name must be given\n")
        return 2

    # ----
    # Delete the requested data set.
    # ----
    try:
        plp = plprofiler()
        plp.connect(connoptions)
        plp.delete_dataset(opt_name)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

def reset_command(argv):
    connoptions = {}

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # edit command specific coptions
                ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_reset()
            return 0

    # ----
    # Delete the collected data from the pl_profiler_linestats_shared
    # and pl_profiler_callgraph_shared hashtables.
    # ----
    try:
        plp = plprofiler()
        plp.connect(connoptions)
        plp.reset_shared()
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

def report_command(argv):
    connoptions = {}
    opt_name = None
    opt_title = None
    opt_desc = None
    opt_top = 10
    opt_output = None
    opt_from_shared = False
    need_edit = False

    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:o:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # report command specific options
                'name=', 'title=', 'desc=', 'description=',
                'output=', 'top=', 'from-shared', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_report()
            return 0

        elif opt in ['--name']:
            opt_name = val
        elif opt in ['--title']:
            opt_title = val
        elif opt in ['--desc', '--description']:
            opt_desc = val
        elif opt in ('-o', '--output', ):
            opt_output = val
        elif opt in ('--top', ):
            opt_top = int(val)
        elif opt in ('--from-shared', ):
            opt_from_shared = True

    if opt_name is None and not opt_from_shared:
        sys.stderr.write("option --name or --from-shared must be given\n")
        return 2
    if opt_name is None:
        opt_name = 'current_data'
    if opt_from_shared and (opt_name is None or opt_title is None or opt_desc is None):
        need_edit = True

    if opt_output is None:
        output_fd = sys.stdout
    else:
        output_fd = open(opt_output, 'w')

    try:
        plp = plprofiler()
        plp.connect(connoptions)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    # ----
    # Get the report data either from the collected *_data tables
    # or a saved dataset.
    # ----
    if opt_from_shared:
        report_data = plp.get_shared_report_data(opt_name, opt_top, args)
    else:
        report_data = plp.get_saved_report_data(opt_name, opt_top, args)

    config = report_data['config']
    if opt_title is not None:
        config['title'] = opt_title
    if opt_desc is not None:
        config['desc'] = opt_desc

    # ----
    # Invoke the editor on the config if need be.
    # ----
    if need_edit:
        try:
            edit_config_info(config)
        except Exception as err:
            sys.stderr.write(str(err) + '\n')
            traceback.print_exc()
            return 2
        opt_name = config['name']
        report_data['config'] = config

    plp.report(report_data, output_fd)

    if opt_output is not None:
        output_fd.close()

    return 0

def export_command(argv):
    connoptions = {}
    opt_all = False
    opt_name = None
    opt_title = None
    opt_desc = None
    opt_top = pow(2, 31)
    opt_output = None
    opt_from_shared = False
    opt_edit = False

    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:o:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # report command specific options
                'all', 'name=', 'title=', 'desc=', 'description=',
                'edit', 'output=', 'from-shared', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_export()
            return 0

        elif opt in ['--all']:
            opt_all = True
        elif opt in ['--edit']:
            opt_edit = True
        elif opt in ['--name']:
            opt_name = val
        elif opt in ['--title']:
            opt_title = val
        elif opt in ['--desc', '--description']:
            opt_desc = val
        elif opt in ('-o', '--output', ):
            opt_output = val
        elif opt in ('--from-shared', ):
            opt_from_shared = True

    if not opt_all and opt_name is None and not opt_from_shared:
        sys.stderr.write("option --all, --name or --from-shared must be given\n")
        return 2

    if opt_output is None:
        output_fd = sys.stdout
    else:
        output_fd = open(opt_output, 'w')

    try:
        plp = plprofiler()
        plp.connect(connoptions)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    if opt_all:
        export_names = [row[0] for row in plp.get_dataset_list()]
    else:
        if opt_from_shared:
            export_names = ['collected_data']
        else:
            export_names = [opt_name]

    # ----
    # Build the export data set.
    # ----
    export_set = []
    for name in export_names:
        # ----
        # Get the report data either from the collected *_data tables
        # or a saved dataset.
        # ----
        if opt_from_shared:
            report_data = plp.get_shared_report_data(name, opt_top, args)
        else:
            report_data = plp.get_saved_report_data(name, opt_top, args)

        config = report_data['config']
        if opt_title is not None:
            config['title'] = opt_title
        if opt_desc is not None:
            config['desc'] = opt_desc

        # ----
        # Launch an editor if we are asked to edit configs.
        # ----
        if opt_edit:
            try:
                edit_config_info(config)
            except Exception as err:
                sys.stderr.write(str(err) + '\n')
                traceback.print_exc()
                return 2
            report_data['config'] = config

        export_set.append(report_data)

    # ----
    # Write the whole thing out.
    # ----
    output_fd.write(json.dumps(export_set, indent = 2, sort_keys = True) + "\n")

    if opt_output is not None:
        output_fd.close()

    return 0

def import_command(argv):
    connoptions = {}
    opt_file = None
    opt_edit = False
    opt_force = False

    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:f:h:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # report command specific options
                'file=', 'edit', 'force', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_import()
            return 0

        elif opt in ['-f', '--file']:
            opt_file = val
        elif opt in ['--edit']:
            opt_edit = True
        elif opt in ['--force']:
            opt_force = True

    if opt_file is None:
        sys.stderr.write("option --file must be given\n")
        return 2

    try:
        plp = plprofiler()
        plp.connect(connoptions)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    # ----
    # Read the export data set and process it.
    # ----
    with open(opt_file, 'r') as fd:
        import_set = json.loads(fd.read())

    for report_data in import_set:
        # ----
        # Launch an editor if we are asked to edit configs.
        # ----
        if opt_edit:
            try:
                config = report_data['config']
                edit_config_info(config)
            except Exception as err:
                sys.stderr.write(str(err) + '\n')
                traceback.print_exc()
                return 2
            report_data['config'] = config

        # ----
        # Try to save this report as a saved set.
        # ----
        plp.save_dataset_from_report(report_data, opt_force)

    return 0

def run_command(argv):
    connoptions = {}
    opt_name = None
    opt_title = None
    opt_desc = None
    opt_sql_file = None
    opt_query = None
    opt_top = 10
    opt_output = None
    opt_save = False
    opt_force = False
    need_edit = False

    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "c:d:f:h:o:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # run command specific options
                'name=', 'title=', 'desc=', 'description=',
                'command=', 'file=',
                'save', 'force', 'output=', 'top=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_run()
            return 0

        elif opt in ['--name']:
            opt_name = val
        elif opt in ('-T', '--title', ):
            opt_title = val
        elif opt in ('-D', '--desc', '--description', ):
            opt_desc = val
        elif opt in ('-c', '--command', ):
            opt_query = val
        elif opt in ('-f', '--file', ):
            opt_sql_file = val
        elif opt in ('--top', ):
            opt_top = int(val)
        elif opt in ('-o', '--output', ):
            opt_output = val
        elif opt in ('-s', '--save', ):
            opt_save = True
        elif opt in ('-f', '--force', ):
            opt_force = True

    if opt_name is None:
        need_edit = True
        opt_name = 'current'

    if opt_title is None:
        need_edit = True
        opt_title = "PL Profiler Report for %s" %(opt_name, )

    if opt_desc is None:
        need_edit = True
        opt_desc = ("<h1>PL Profiler Report for %s</h1>\n" +
                    "<p>\n<!-- description here -->\n</p>") %(opt_name, )

    if opt_sql_file is not None and opt_query is not None:
        sys.stderr.write("--query and --sql-file are mutually exclusive\n")
        return 2
    if opt_sql_file is None and opt_query is None:
        sys.stderr.write("One of --query or --sql-file must be given\n")
        return 2
    if opt_query is None:
        with open(opt_sql_file, 'r') as fd:
            opt_query = fd.read()

    try:
        plp = plprofiler()
        plp.connect(connoptions)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    plp.enable()
    plp.reset_local()
    plp.execute_sql(opt_query, sys.stdout)

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

    if opt_save:
        try:
            plp.save_dataset_from_local(opt_name, config, opt_force)
        except Exception as err:
            sys.stderr.write(str(err) + "\n")
            return 1

    if opt_output is not None:
        with open(opt_output, 'w') as output_fd:
            report_data = plp.get_local_report_data(opt_name, opt_top, args)
            report_data['config'] = config
            plp.report(report_data, output_fd)
            output_fd.close()

    return 0

def monitor_command(argv):
    connoptions = {}
    opt_duration = 60
    opt_interval = 10
    opt_pid = None

    try:
        opts, args = getopt.getopt(argv,
                # Standard connection related options
                "d:h:p:U:", [
                'dbname=', 'host=', 'port=', 'user=', 'help',
                # monitor command specific options
                'pid=', 'interval=', 'duration=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2

    for opt, val in opts:
        if opt in ['-d', '--dbname']:
            if val.find('=') < 0:
                connoptions['database'] = val
            else:
                connoptions['dsn'] = val
        elif opt in ['-h', '--host']:
            connoptions['host'] = val
        elif opt in ['-p', '--port']:
            connoptions['port'] = int(val)
        elif opt in ['-U', '--user']:
            connoptions['user'] = val
        elif opt in ['--help']:
            help_monitor()
            return 0

        elif opt in ('-p', '--pid', ):
            opt_pid = val
        elif opt in ('-i', '--interval', ):
            opt_interval = val
        elif opt in ('-d', '--duration', ):
            opt_duration = val

    try:
        plp = plprofiler()
        plp.connect(connoptions)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    try:
        plp.enable_monitor(opt_pid, opt_interval)
    except Exception as err:
        print str(err)
        return 1
    print "monitoring for %d seconds ..." %(int(opt_duration))
    try:
        time.sleep(int(opt_duration))
    finally:
        plp.disable_monitor()
    print "done."

    return 0

def edit_config_info(config):
    if os.name == 'posix':
        default_editor = 'vi'
    elif os.name == 'nt':
        default_editor = 'notepad'
    else:
        raise Exception("unsupported OS type %s" %os.name)

    EDITOR = os.environ.get('EDITOR', default_editor)
    opts = ['title', 'tabstop', 'svg_width', 'table_width', 'desc', ]

    # ----
    # Create a ConfigParser that contains relevant sections of the config.
    # ----
    name = config['name']
    tmp_config = ConfigParser.RawConfigParser()
    tmp_config.add_section(name)
    for opt in opts:
        tmp_config.set(name, opt, str(config[opt]))

    # ----
    # We need the temp file to edit to have the correct, OS specific
    # line endings. So we create a StringIO buffer first to get the
    # file content from the ConfigParser, then change '\n' into 
    # os.linesep when creating the temp file.
    # ----
    buf = StringIO.StringIO()
    tmp_config.write(buf)
    tf = tempfile.NamedTemporaryFile(suffix=".tmp.conf", delete = False)
    tf_name = tf.name
    tf.write(buf.getvalue().replace('\n', os.linesep))
    tf.close()

    # ----
    # Call the editor.
    # ----
    subprocess.call([EDITOR, tf_name])

    # ----
    # Remove all sections from the ConfigParser object, read back
    # the temp file and extract the one expected section.
    # ----
    for s in tmp_config.sections():
        tmp_config.remove_section(s)

    tf = open(tf_name, 'r')
    tmp_config.readfp(tf)
    tf.close()
    os.remove(tf_name)

    if len(tmp_config.sections()) != 1:
        raise Exception("config must have exactly one section")
    name = tmp_config.sections()[0]
    config['name'] = name
    for opt in opts:
        if tmp_config.has_option(name, opt):
            config[opt] = str(tmp_config.get(name, opt))

def usage():
    print """
usage: plprofiler COMMAND [OPTIONS]

    plprofiler is a command line tool to control the plprofiler extension
    for PostgreSQL.

    The input of this utility are the call and execution statistics, the
    plprofiler extension collects. The final output is an HTML report of
    the statistics gathered. There are several ways to collect the data,
    save the data permanently and even transport it from a production
    system to a lab system for offline analysis.

    Use

        plprofiler COMMAND --help

    for detailed information about one of the commands below.

GENERAL OPTIONS:

    All commands implement the following command line options to specify
    the target database:

        -h, --host=HOST     The host name of the database server.

        -p, --port=PORT     The PostgreSQL port number.

        -U, --user=USER     The PostgreSQL user name to connect as.

        -d, --dbname=DB     The PostgreSQL database name or the DSN.
                            plprofiler currently uses psycopg2 to connect
                            to the target database. Since that is based
                            on libpq, all the above parameters can also
                            be specified in this option with the usual
                            conninfo string or URI formats.

        --help              Print the command specific help information
                            and exit.

TERMS:

    The following terms are used in the text below and the help output of
    individual commands:

    local-data      By default the plprofiler extension collects run-time
                    data in per-backend hashtables (in-memory). This data is
                    only accessible in the current session and is lost when
                    the session ends or the hash tables are explicitly reset.

    shared-data     The plprofiler extension can copy the local-data
                    into shared hashtables, to make the statistics available
                    to other sessions. See the "monitor" command for
                    details. This data still relies on the local database's
                    system catalog to resolve Oid values into object
                    definitions.

    saved-dataset   The local-data as well as the shared-data can
                    be turned into a named, saved dataset. These sets
                    can be exported and imported onto other machines.
                    The saved datasets are independent of the system
                    catalog, so a report can be generated again later,
                    even even on a different system.


COMMANDS:

    run             Runs one or more SQL statements with the plprofiler
                    extension enabled and creates a saved-dataset and/or
                    an HTML report from the local-data.

    monitor         Monitors a running application for a requested time
                    and creates a saved-dataset and/or an HTML report from
                    the resulting shared-data.

    reset           Deletes the data from shared hash tables.

    save            Saves the current shared-data as a saved-dataset.

    list            Lists the available saved-datasets.

    edit            Edits the metadata of one saved-dataset. The metadata
                    is used in the generation of the HTML reports.

    report          Generates an HTML report from either a saved-dataset
                    or the shared-data.

    delete          Deletes a saved-dataset.

    export          Exports one or all saved-datasets into a JSON file.

    import          Imports the saved-datasets from a JSON file, created
                    with the export command.

"""

def help_run():
    print """
usage: plprofiler run [OPTIONS]

    Runs one or more SQL commands (hopefully invoking one or more PL/pgSQL
    functions and/or triggers), then turns the local-data into an HTML
    report and/or a saved-dataset.

OPTIONS:

    --name=NAME     The name of the data set to use in the HTML report or
                    saved-dataset.

    --title=TITLE   Ditto.

    --desc=DESC     Ditto.

    -c, --command=CMD   The SQL string to execute. Can be multiple SQL
                    commands, separated by semicolon.

    -f, --file=FILE Read SQL commands to execute from FILE.

    --save          Create a saved-dataset.

    --force         Overwrite an existing saved-dataset of the same NAME.

    --output=FILE   Save an HTML report in FILE.

    --top=N         Include up to N function detail descriptions in the
                    report (default=10).

"""

def help_monitor():
    print """
usage: plprofiler monitor [OPTIONS]

    Turns profile data capturing and periodic saving on for either all
    database backends, or a single one (specified by PID), waits for a
    specified amount of time, then turns it back off. If during that
    time the application (or specific backend) is executing queries, that
    invoke PL/pgSQL functions, profile statistics will be collected into
    shared-data at the specified interval as well as every transaction
    end (commit or rollback).

    The resulting saved-data can be used with the "save" and "report"
    commands and cleared with "reset".

NOTES:

    The change in configuration options will become visible to running
    backends when they go through the PostgreSQL TCOP loop. That is, when
    they receive the next "client" command, like a query or prepared
    statement execution request. They will not start/stop collecting
    data while they are in the middle of a long-running query.

REQUIREMENTS:

    This command uses PostgreSQL features, that are only available in
    version 9.4 and higher.

    The plprofiler extension must be loaded via the configuration option
    "shared_preload_libraries" in the postgresql.conf file.

OPTIONS:

    --pid=PID       The PID of the backend, to monitor. If not given, the
                    entire PostgreSQL instance will be suspect to monitoring.

    --interval=SEC  Interval in seconds at which the monitored backend(s)
                    will copy the local-data to shared-data and then
                    reset their local-data.

    --duration=SEC  Duration of the monitoring run in seconds.

"""

def help_reset():
    print """
usage: plprofiler reset

    Deletes all data from the shared hashtables. This affects all databases
    in the cluster.

    This does NOT destroy any of the saved-datasets.

"""

def help_save():
    print """
usage: plprofiler save [OPTIONS]

    The save command is used to create a saved-dataset from shared-data.
    Saved datasets are independent from the system catalog, since all their
    Oid based information has been resolved into textual object descriptions.
    Their reports can be recreated later or even on another system (after
    transport via export/import).

OPTIONS:

    --name=NAME     The name of the saved-dataset. Must be unique.

    --title=TITLE   The title used by the report command in the <title>
                    tag of the generated HTML output.

    --desc=DESC     An HTML formatted paragraph (or more) that describes
                    the profiling report.

    --force         Overwite an existing saved-dataset with the same NAME.

NOTES:

    If the options for TITLE and DESC are not specified on the command line,
    the save command will launch an editor, allowing to edit the default
    report configuration. This metadata can later be changed with the
    "edit" command.

"""

def help_list():
    print """
usage: plprofiler list

    Lists the available saved-datasets together with their TITLE.

"""

def help_edit():
    print """
usage: plprofiler edit [OPTIONS]

    Launches an editor with the metadata of the specified saved-dataset.
    This allows to change not only the metadata itself, but also the
    NAME of the saved-dataaset.

OPTIONS:

    --name=NAME     The name of the saved-dataset to edit.

"""

def help_report():
    print """
usage: plprofiler report [OPTIONS]

    Create an HTML report from either shared-data or a saved-dataset.

OPTIONS:

    --from-shared   Use the shared-data rather than a saved-dataset.

    --name=NAME     The name of the saved-dataset to load or the NAME
                    to use with --from-shared.

    --title=TITLE   Override the TITLE found in the saved-dataset's
                    metadata, or the TITLE to use with --from-shared.

    --desc=DESC     Override the DESC found in the saved-dataset's
                    metadata, or the DESC to use with --from-shared.

    --output=FILE   Destination for the HTML report (default=stdout).

    --top=N         Include up to N function detail descriptions in the
                    report (default=10).

"""

def help_delete():
    print """
usage: plprofiler delete [OPTIONS]

    Delete the named saved-dataset.

OPTIONS:

    --name=NAME     The name of the saved-dataset to delete.

"""

def help_export():
    print """
usage: plprofiler export [OPTIONS]

    Export the shared-data or one or more saved-datasets as a JSON
    document.

OPTIONS:

    --all           Export all saved-datasets.

    --from-shared   Export the shared-data instead of a saved-dataset.

    --name=NAME     The NAME of the dataset to save.

    --title=TITLE   The TITLE of the dataset in the export.

    --desc=DESC     The DESC of the dataset in the export.

    --edit          Launches the config editor for each dataset, included
                    in the export.

    --output=FILE   Save the JSON export data in FILE (default=stdout).

"""

def help_import():
    print """
usage: plprofiler import [OPTIONS]

    Imports one or more datasets from an export file.

OPTIONS:

    -f, --file=FILE Read the profile data from FILE. This should be the
                    output of a previous "export" command.

    --edit          Edit each dataset's metadata before storing it as
                    a saved-dataset.

    --force         Overwrite any existing saved-datasets with the same
                    NAMEs, as they appear in the input file (or after
                    editing).

"""
