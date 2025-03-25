#
# Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
# Place holder setup.py for fabric-ansible

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
    name='fabric_ansible',
    version='0.1.dev0',
    install_requires=requirements('requirements.txt'),
    packages=find_packages(exclude=["*.test", "*.test.*", "test.*", "test"]),
    package_data={'': ['*.html', '*.css', '*.xml']},
    zip_safe=False,
    long_description="Fabric Ansible",
    entry_points = {
         # Please update sandesh/common/vns.sandesh on process name change
         'console_scripts' : [
             'contrail-fabric-ansible = job_manager.job_manager:main',
         ],
    },
    cmdclass={
       'run_tests': RunTestsCommand,
    },
)
