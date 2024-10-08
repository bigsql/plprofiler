Installing PL Profiler
======================

The **plprofiler** consists of two parts.
* A backend side, which is provided by a PostgreSQL extension and
* a client part, which is a Python package with a command line wrapper.

Installing PL Profiler via the PGEDGE distribution
--------------------------------------------------

In the [PGEDGE](https://github.com/pgedge/pgedge/) distribution both parts of the **plprofiler** are installed with the command

```
./pgedge install plprofiler-pg14
```

Installing PL Profiler via PGDG RPMs
------------------------------------

If you installed PostgreSQL via the [PGDG yum repository](https://yum.postgresql.org/) it is highly recommended to install the **plprofiler** from the same. There are two packages and a meta-package pulling in both:
* `plprofiler_XX-server` - the backend PostgreSQL extension
* `plprofiler_XX-client` - the Python package with commandline wrapper
* `plprofiler_XX` - the meta-package installing both

The XX in the package name is the major version of PostgreSQL. On a PostgreSQL server you would normally install via the meta-package. The separate `plprofiler_XX-server` package is provided for installations where one intentionally does not provide debugging or testing utilities.

The `plprofiler_XX-client` package is intended for developer workstations that do not have the PostgreSQL server itself installed.

Installing the PL Profiler client via pip
-----------------------------------------

In environments where users cannot install RPM packages, the `plprofiler-client` can be installed via Python's `pip` utility. It is recommended to use [Python Virtual Environments](https://docs.python.org/3/library/venv.html) in this case.

```
cd
virtualenv --system-site-packages plprofiler-venv
source ~/plprofiler-venv/bin/activate
pip install plprofiler-client psycopg-binary
```

The `psycopg-binary` dependency needs to be specified manually since adding it to the `install_requires` list of setup.py would create an unsatisfied runtime dependency when installing `plprofiler-client` from RPM.

Building PL Profiler from Source
--------------------------------

**plprofiler** is implemented as a `PGXS` extension. This means it can be built either from within the PostgreSQL source tree, or outside of it when just the PostgreSQL devel RPM package is installed and `$PATH` includes the location of the correct `pg_config` utility.

To install **plprofiler** from within a PostgreSQL source tree, the [**plprofiler** git repository](https://github.com/bigsql/plprofiler.git) needs to be cloned into the `contrib` directory. The following commands then install the backend extension:

```
cd contrib/plprofiler
make install
```

If **plprofiler** was cloned outside of the contrib directory, use the following commands:

```
cd plprofiler
USE_PGXS=1 make install
```

The `plprofiler-client` part in both cases is then installed via `setup.py`. It is recommended to use a [Python Virtual Environments](https://docs.python.org/3/library/venv.html) for this.

```
cd python-plprofiler
python ./setup.py install
```

The client requires the psycopg database connector. Since the **plprofiler** client was installed via `pip` in this case, it is recommended to run

```
pip install psycopg-binary
```
