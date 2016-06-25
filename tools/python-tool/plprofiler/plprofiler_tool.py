#!/usr/bin/env python

import ConfigParser
import getopt
import json
import os
import subprocess
import sys
import tempfile
import traceback

from plprofiler_data import plprofiler_data
import plprofiler_report

def main():
    if len(sys.argv) == 1:
        usage()
        return 2

    if sys.argv[1] == 'save':
        return save_data_command(sys.argv[2:])

    if sys.argv[1] == 'list':
        return list_data_command(sys.argv[2:])

    if sys.argv[1] == 'edit':
        return edit_data_command(sys.argv[2:])

    if sys.argv[1] == 'delete':
        return delete_data_command(sys.argv[2:])

    if sys.argv[1] == 'report':
        return plprofiler_report.report(sys.argv[2:])

    sys.stderr.write("ERROR: unknown command '%s'\n" %(sys.argv[1]))
    return 2

def save_data_command(argv):
    opt_conninfo = ''
    opt_name = None
    opt_title = None
    opt_desc = None
    opt_force = False
    need_edit = False

    # ----
    # Parse command line
    # ----
    try:
        opts, args = getopt.getopt(argv, "c:d:fn:t:", [
                'conninfo=', 'name=', 'title=', 'force',
                'desc=', 'description=', ])
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
        elif opt in ['-f', '--force']:
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
        ppdata = plprofiler_data()
        ppdata.connect(opt_conninfo)
        ppdata.save_dataset_from_data(opt_name, config, opt_force)

    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        traceback.print_exc()
        return 1

def list_data_command(argv):
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
    # Get the list of saved data sets.
    # ----
    try:
        ppdata = plprofiler_data()
        ppdata.connect(opt_conninfo)
        rows = ppdata.get_dataset_list()
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

def edit_data_command(argv):
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
    # Get the current values and create a config with that.
    # ----
    try:
        ppdata = plprofiler_data()
        ppdata.connect(opt_conninfo)
        config = ppdata.get_dataset_config(opt_name)
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
        ppdata.update_dataset_config(opt_name, new_name, config)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    return 0

def delete_data_command(argv):
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
    # Delete the requested data set.
    # ----
    try:
        ppdata = plprofiler_data()
        ppdata.connect(opt_conninfo)
        ppdata.delete_dataset(opt_name)
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

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

def usage():
    print """
usage: plprofiler COMMAND [OPTIONS]

plprofiler is a command line tool to manage and create reports of data
sets, collected by the PostgreSQL plprofiler extension.

The typical usage is to run a test with the plprofiler extension enabled
and either calling pl_profiler_save_stats() in the relevant sessions or
also have plprofiler.save_interval configured, and then run

    plprofiler save --name="test1" --conninfo="dbname=mydb"

on the command line. This will save the collected data permanently into
the pl_profiler_saved* tables and clear out the pl_profiler_*_data tables.
After that the command

    plprofiler report --name="test1" --conninfo="dbname=mydb" \\
                      --output="test1.html"

will generate the file "test1.html" containing an analysis of the test
run.

COMMANDS:

    save        Save the data collected in the pl_profiler_*_data tables
                as a set in the pl_profiler_saved* tables. OPTIONS are

                --conninfo="CONNINFO"
                --name="SET_NAME"               # required
                --title="TITLE"
                --description="DESCRIPTION"

                If TITLE or DESCRIPTION are not specified, plprofiler
                will launch an editor (configured in the $EDITOR
                environment variable) to edit the data set information.

    list        List the available data sets. OPTIONS are

                --conninfo="CONNINFO"

    edit        Edit the data set information. This command will launch
                an editor to change the metadata for the data set, like
                the TITLE, DESCRIPTION and other details. OPTIONS are

                --conninfo="CONNINFO"
                --name="SET_NAME"               # required

    delete      Permanently delete the saved set SET_NAME. No going
                back from here ... got to re-run the test and collect
                the data again.

                --conninfo="CONNINFO"
                --name="SET_NAME"               # required

    report      Generates an HTML file containing an analysis of one
                test data set. The HTML will contain and inline .SVG
                visualizing call graphs and per source line statistics
                of PL/pgSQL functions. OPTIONS are

                --conninfo="CONNINFO"
                --name="SET_NAME"               # required
                --top=NUM                       # number of functions
                                                # to analyze (default=10)

                The "top" functions to analyze are determined by the
                sum of their execution time from the plprofiler func_beg
                to func_end callback.

                An optional list of OID values as command line arguments
                will override the --top option and generate a report
                with those functions detailed only. The functions OIDs
                of interest can be determined from the function call
                graph .SVG at the beginning of the report.

"""
