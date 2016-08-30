plprofiler command reference
============================

```
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
```

Command run
-----------

```
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


```

Command monitor
---------------

```
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


```

Command reset
-------------

```
usage: plprofiler reset

    Deletes all data from the shared hashtables. This affects all databases
    in the cluster.

    This does NOT destroy any of the saved-datasets.


```
Command save
------------

```
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
```

Command list
------------

```
usage: plprofiler list

    Lists the available saved-datasets together with their TITLE.
```

Command edit
------------

```
usage: plprofiler edit [OPTIONS]

    Launches an editor with the metadata of the specified saved-dataset.
    This allows to change not only the metadata itself, but also the
    NAME of the saved-dataaset.

OPTIONS:

    --name=NAME     The name of the saved-dataset to edit.
```

Command report
--------------

```
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
```

Command delete
--------------

```
usage: plprofiler delete [OPTIONS]

    Delete the named saved-dataset.

OPTIONS:

    --name=NAME     The name of the saved-dataset to delete.
```

Command export
--------------

```
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
```

Command import
--------------

```
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
```
