#
# Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
#
from setuptools import setup, find_packages, Command
import os
import re

def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))

class RunTestsCommand(Command):
    description = "Test command to run testr in virtualenv"
    user_options = []
    boolean_options = []
    def initialize_options(self):
        self.cwd = None
    def finalize_options(self):
        self.cwd = os.getcwd()
    def run(self):
        logfname = 'test.log'
        args = '-V'
        rc_sig = os.system('./run_tests.sh %s' % args)
        if rc_sig >> 8:
            os._exit(rc_sig>>8)
        with open(logfname) as f:
            if not re.search('\nOK', ''.join(f.readlines())):
                os._exit(1)

setup(
    name='device_manager',
    version='0.1.dev0',
    packages=find_packages(exclude=["*.test", "*.test.*", "test.*", "test"]),
    package_data={'': ['*.html', '*.css', '*.xml']},
    zip_safe=False,
    long_description="VNC Configuration Physical Router Configuration Manager",
    install_requires=requirements('requirements.txt'),
    entry_points = {
         # Please update sandesh/common/vns.sandesh on process name change
         'console_scripts' : [
             'contrail-device-manager = device_manager.dm_server:server_main',
         ],
    },
    cmdclass={
       'run_tests': RunTestsCommand,
    },
)
