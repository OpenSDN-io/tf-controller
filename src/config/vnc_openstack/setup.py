#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#
import re

from setuptools import find_packages, setup


def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))


setup(
    name='vnc_openstack',
    version='0.1.dev0',
    install_requires=requirements('requirements.txt'),
    packages=find_packages(),
    include_package_data=True,
    zip_safe=False,
    long_description="VNC openstack interface package",
    entry_points={
        'vnc_cfg_api.resync': [
            'xxx = vnc_openstack:OpenstackDriver',
        ],
        'vnc_cfg_api.resourceApi': [
            'xxx = vnc_openstack:ResourceApiDriver',
        ],
        'vnc_cfg_api.neutronApi': [
            'xxx = vnc_openstack:NeutronApiDriver',
        ],
    },
)
