from setuptools import setup

setup(
    name = 'plprofiler',
    description = 'PL/pgSQL Profiler command line tool',
    version = '2.0',
    author = 'Jan Wieck',
    author_email = 'janw@openscg.com',
    url = 'https://bitbucket.org/openscg/plprofiler/overview',
    license = 'Artistic License',
    packages = ['plprofiler_tool', ],
    entry_points = {
        'console_scripts': [
            'plprofiler = plprofiler_tool:main',
        ]
    }
)
