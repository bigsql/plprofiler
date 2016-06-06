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
    print """usage: plprofiler COMMAND [OPTIONS]

COMMANDS:

    save

    list

    edit

    delete
    
    report
"""
