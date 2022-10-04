from setuptools import setup

setup(
    name = 'plprofiler-client',
    description = 'PL/pgSQL Profiler module and command line tool',
    version = '4.2',
    author = 'Jan Wieck',
    author_email = 'jan@wi3ck.info',
    url = 'https://github.com/bigsql/plprofiler',
    license = 'Artistic License and CDDL',
    packages = ['plprofiler', ],
    long_description = """PL/pgSQL Profiler module and command line tool
==============================================

This is the Python module and command line tool to
control the PL/pgSQL Profiler extension for PostgreSQL.

Please visit https://github.com/bigsql/plprofiler for
the main project.""",
    long_description_content_type = 'text/markdown',
    package_data = {
        'plprofiler': [
            'lib/FlameGraph/README',
            'lib/FlameGraph/README-plprofiler',
            'lib/FlameGraph/flamegraph.pl',
        ],
    },
    install_requires = [
        'configparser',
    ],
    entry_points = {
        'console_scripts': [
            'plprofiler = plprofiler:main',
        ]
    },
)
