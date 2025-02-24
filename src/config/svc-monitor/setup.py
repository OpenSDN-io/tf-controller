#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

import setuptools, re

def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))

setuptools.setup(
    name='svc_monitor',
    version='0.1.dev0',
    packages=setuptools.find_packages(),
    package_data={'': ['*.html', '*.css', '*.xml', '*.yml']},
    install_requires=requirements('requirements.txt'),

    # metadata
    author="OpenContrail",
    author_email="dev@lists.opencontrail.org",
    license="Apache Software License",
    url="http://www.opencontrail.org/",

    long_description="VNC Service Monitor",

    test_suite='svc_monitor.tests',


    entry_points = {
        # Please update sandesh/common/vns.sandesh on process name change
        'console_scripts' : [
            'contrail-svc-monitor = svc_monitor.svc_monitor:server_main',
        ],
    },
)
