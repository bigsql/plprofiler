#!/usr/bin/env python

import sys

import plprofiler_data
import plprofiler_report

def main():
    if len(sys.argv) == 1:
        usage()
        return 2

    if sys.argv[1] == 'save':
        return plprofiler_data.save_data(sys.argv[2:])

    if sys.argv[1] == 'list':
        return plprofiler_data.list_data(sys.argv[2:])

    if sys.argv[1] == 'edit':
        return plprofiler_data.edit_data(sys.argv[2:])

    if sys.argv[1] == 'delete':
        return plprofiler_data.delete_data(sys.argv[2:])

    if sys.argv[1] == 'report':
        return plprofiler_report.report(sys.argv[2:])

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
