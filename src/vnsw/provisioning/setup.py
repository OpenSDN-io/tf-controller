#
# Copyright (c) 2017 Juniper Networks, Inc.
#

import re, setuptools


def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))

setuptools.setup(
    name='contrail-vrouter-provisioning',
    version='0.1.dev0',
    install_requires=requirements('requirements.txt'),
    packages=setuptools.find_packages(),

    # metadata
    author="OpenContrail",
    author_email="dev@lists.opencontrail.org",
    license="Apache Software License",
    url="http://www.opencontrail.org/",
    long_description="Contrail compute provisioning module",
    entry_points={
        'console_scripts': [
            'contrail-compute-setup = contrail_vrouter_provisioning.setup:main',
            'contrail-toragent-setup = contrail_vrouter_provisioning.toragent.setup:main',
            'contrail-toragent-cleanup = contrail_vrouter_provisioning.toragent.cleanup:main',
            ],
    },
)
