from setuptools import setup

setup(
    name = 'plprofiler',
    description = 'PL/pgSQL Profiler command line tool',
    version = '4-DEV',
    author = 'Jan Wieck',
    author_email = 'janw@openscg.com',
    url = 'https://bitbucket.org/openscg/plprofiler/overview',
    license = 'Artistic License',
    packages = ['plprofiler', ],
    package_data = {
        'plprofiler': [
            'lib/FlameGraph/README',
            'lib/FlameGraph/*.pl',
            'lib/FlameGraph/*.txt',
            'lib/FlameGraph/*.svg',
            'lib/FlameGraph/*.awk',
            'lib/FlameGraph/demos/*',
            'lib/FlameGraph/dev/*',
            'lib/FlameGraph/docs/*',
        ],
    },
    install_requires = [
        'psycopg2',
    ],
    entry_points = {
        'console_scripts': [
            'plprofiler = plprofiler:main',
        ]
    },
)
