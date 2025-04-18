#
# Copyright (c) 2014 Juniper Networks, Inc.
#

import re, setuptools


def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))

setuptools.setup(
    name='opencontrail-vrouter-netns',
    version='0.1.dev0',
    packages=setuptools.find_packages(),

    # metadata
    author="OpenContrail",
    author_email="dev@lists.opencontrail.org",
    license="Apache Software License",
    url="http://www.opencontrail.org/",
    long_description="Script to manage Linux network namespaces",

    install_requires=requirements('requirements.txt'),

    test_suite='opencontrail_vrouter_netns.tests',
    tests_require=requirements('test-requirements.txt'),

    entry_points = {
        'console_scripts': [
            'opencontrail-vrouter-netns = opencontrail_vrouter_netns.vrouter_netns:main',
            'opencontrail-vrouter-docker = opencontrail_vrouter_netns.vrouter_docker:main',
            'netns-daemon-start = opencontrail_vrouter_netns.daemon_start:daemon_start',
            'netns-daemon-stop = opencontrail_vrouter_netns.daemon_stop:daemon_stop'
        ],
    },
)
