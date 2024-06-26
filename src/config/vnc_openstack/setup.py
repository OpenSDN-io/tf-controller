#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#
from setuptools import find_packages, setup


setup(
    name='vnc_openstack',
    version='0.1.dev0',
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
    install_requires=[
        'future',
    ],
)
