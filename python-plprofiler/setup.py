from setuptools import setup

setup(
    name = 'plprofiler',
    description = 'PL/pgSQL Profiler command line tool',
    version = '3-BETA3',
    author = 'Jan Wieck',
    author_email = 'janw@openscg.com',
    url = 'https://bitbucket.org/openscg/plprofiler/overview',
    license = 'Artistic License',
    packages = ['plprofiler', ],
    install_requires = [
        'psycopg2',
    ],
    entry_points = {
        'console_scripts': [
            'plprofiler = plprofiler:main',
        ]
    },
)
