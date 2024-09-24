#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

import re, setuptools


def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))

setuptools.setup(
    name='vrouter',
    version='0.1.dev0',
    packages=['vrouter',
              'vrouter.vrouter',
              'vrouter.vrouter.cpuinfo',
              'vrouter.vrouter.process_info',
              'vrouter.vrouter.derived_stats_results',
              'vrouter.loadbalancer',
              'vrouter.sandesh',
              'vrouter.sandesh.virtual_machine',
              'vrouter.sandesh.virtual_machine.port_bmap',
              'vrouter.sandesh.virtual_network',
              'vrouter.sandesh.flow',
              'vrouter.sandesh.interface',
              'vrouter.sandesh.interface.port_bmap',
             ],
    package_data={'':['*.html', '*.css', '*.xml']},
    zip_safe=False,
    long_description="Vrouter Sandesh",
    install_requires=requirements('requirements.txt'),
)
