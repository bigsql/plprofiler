from setuptools import setup

setup(
    name = 'plprofiler',
    description = 'PL/pgSQL Profiler module and command line tool',
    version = '4.2',
    author = 'Jan Wieck',
    author_email = 'jan@wi3ck.info',
    url = 'https://github.com/bigsql/plprofiler',
    license = 'Artistic License and CDDL',
    packages = ['plprofiler', ],
    package_data = {
        'plprofiler': [
            'lib/FlameGraph/README',
            'lib/FlameGraph/README-plprofiler',
            'lib/FlameGraph/flamegraph.pl',
        ],
    },
    install_requires = [
        'psycopg2-binary',
        'configparser',
    ],
    entry_points = {
        'console_scripts': [
            'plprofiler = plprofiler:main',
        ]
    },
)
