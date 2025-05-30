# vim: tabstop=4 shiftwidth=4 softtabstop=4

# Copyright 2013 OpenStack Foundation
# Copyright 2013 IBM Corp.
#
#    Licensed under the Apache License, Version 2.0 (the "License"); you may
#    not use this file except in compliance with the License. You may obtain
#    a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
#    WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
#    License for the specific language governing permissions and limitations
#    under the License.

"""Provides methods needed by installation script for OpenStack development
virtual environments.

Since this script is used to bootstrap a virtualenv from the system's Python
environment, it should be kept strictly compatible with Python 2.6.

Synced in from openstack-common
"""

import optparse
import os
import subprocess
import sys
import fnmatch
import re


def find_files(directory, file_pattern):
    for root, dirs, files in os.walk(directory):
        for afile in files:
            if fnmatch.fnmatch(afile, file_pattern):
                yield os.path.join(root, afile)
        for adir in dirs:
            find_files(adir, file_pattern)


class InstallVenv(object):

    def __init__(self, root, venv, requirements,
                 test_requirements, py_version,
                 project):
        self.root = root
        self.venv = venv
        self.requirements = requirements
        self.test_requirements = test_requirements
        self.py_version = py_version
        self.project = project

    def die(self, message, *args):
        print(message % args, file=sys.stderr)
        sys.exit(1)

    def check_python_version(self):
        pass

    def run_command_with_code(self, cmd, redirect_output=True,
                              check_exit_code=True):
        """Runs a command in an out-of-process shell.

        Returns the output of that command. Working directory is self.root.
        """
        if redirect_output:
            stdout = subprocess.PIPE
        else:
            stdout = None

        proc = subprocess.Popen(cmd, cwd=self.root, stdout=stdout)
        output = proc.communicate()[0]
        if check_exit_code and proc.returncode != 0:
            self.die('Command "%s" failed.\n%s', ' '.join(cmd), output)
        return (output, proc.returncode)

    def run_command(self, cmd, redirect_output=True, check_exit_code=True):
        return self.run_command_with_code(cmd, redirect_output,
                                          check_exit_code)[0]

    def get_distro(self):
        if (os.path.exists('/etc/fedora-release') or
                os.path.exists('/etc/redhat-release')):
            return Fedora(
                self.root, self.venv, self.requirements,
                self.test_requirements, self.py_version, self.project)
        else:
            return Distro(
                self.root, self.venv, self.requirements,
                self.test_requirements, self.py_version, self.project)

    def check_dependencies(self):
        self.get_distro().install_virtualenv()

    def create_virtualenv(self, no_site_packages=True):
        """Creates the virtual environment and installs PIP.

        Creates the virtual environment and installs PIP only into the
        virtual environment.
        """
        if not os.path.isdir(self.venv):
            print('Creating venv...', end=' ')
            if no_site_packages:
                self.run_command(['virtualenv', '-q', '--no-site-packages',
                                 self.venv])
            else:
                self.run_command(['virtualenv', '-q', self.venv])
            print('done.')
        else:
            print("venv already exists...")
            pass

    def pip_install(self, find_links, *args):
        find_links_str = ' '.join('--find-links file://'+x for x in find_links)
        cmd_array = ['%stools/with_venv.sh' %(os.environ.get('tools_path', '')),
                         'python', '.venv/bin/pip', 'install', 
                         '--upgrade']
        if not args[0].startswith('pip'):
            cmd_array.extend(['--no-cache-dir'])
        for link in find_links:
            cmd_array.extend(['--find-links', 'file://'+link])
        self.run_command(cmd_array + list(args),
                          redirect_output=False)

    def get_requirements(self):
        for req_file in [self.requirements, self.test_requirements]:
            with open(req_file, 'r+') as fd:
                reqs = [req.strip() for req in fd.readlines()]
        gteq_or_lteq_or_eqeq = re.compile('(.*)[>=<]+[>=<]+')
        gt_or_lt_or_eq = re.compile('(.*)[>=<]+')
        requirements = []
        for req in reqs:
            for regexp in [gteq_or_lteq_or_eqeq, gt_or_lt_or_eq]:
                match = regexp.match(req)
                if match:
                    requirements.append(match.group(1))
                    continue
            else:
                requirements.append(req)
        return requirements

    def pip_clear_cache(self, find_links):
        contrail_reqs = []
        cache_dir = os.path.expanduser('~/.cache/pip')
        for req in self.get_requirements():
            req_pattern = '*' + req + '*'
            for find_link in find_links:
                for contrail_req in find_files(find_link, req_pattern):
                    for cachefile in find_files(cache_dir, req_pattern):
                        os.remove(cachefile)

    def install_dependencies(self, find_links):
        print('Installing dependencies with pip (this can take a while)...')

        # First things first, make sure our venv has the latest pip and
        # setuptools and pbr
        self.pip_install(find_links, 'pip>=6.0')
        self.pip_install(find_links, 'setuptools')
        self.pip_install(find_links, 'pbr')

        self.pip_clear_cache(find_links)
        self.pip_install(find_links, '-r', self.requirements,
                         '-r', self.test_requirements, '--pre')

    def parse_args(self, argv):
        """Parses command-line arguments."""
        parser = optparse.OptionParser()
        parser.add_option('-n', '--no-site-packages',
                          action='store_true',
                          help="Do not inherit packages from global Python "
                               "install")
        parser.add_option('-f', '--find-links',
                          action='append',
                          help="Build generation directory ")
        return parser.parse_args(argv[1:])[0]


class Distro(InstallVenv):

    def check_cmd(self, cmd):
        return bool(self.run_command(['which', cmd],
                    check_exit_code=False).strip())

    def install_virtualenv(self):
        if self.check_cmd('virtualenv'):
            return

        if self.check_cmd('easy_install'):
            print('Installing virtualenv via easy_install...', end=' ')
            if self.run_command(['easy_install', 'virtualenv']):
                print('Succeeded')
                return
            else:
                print('Failed')

        self.die('ERROR: virtualenv not found.\n\n%s development'
                 ' requires virtualenv, please install it using your'
                 ' favorite package management tool' % self.project)


class Fedora(Distro):
    """This covers all Fedora-based distributions.

    Includes: Fedora, RHEL, CentOS, Scientific Linux
    """

    def check_pkg(self, pkg):
        return self.run_command_with_code(['rpm', '-q', pkg],
                                          check_exit_code=False)[1] == 0

    def install_virtualenv(self):
        if self.check_cmd('virtualenv'):
            return

        if not self.check_pkg('python-virtualenv'):
            self.die("Please install 'python-virtualenv'.")

        super(Fedora, self).install_virtualenv()
