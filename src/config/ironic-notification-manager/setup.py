#
# Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
#

import re
from setuptools import setup, find_packages


def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))


setup(
    name='ironic-notification-manager',
    version='0.1.dev0',
    packages=find_packages(),
    zip_safe=False,
    long_description="Ironic Node Notification Management Daemon",
    install_requires=requirements('requirements.txt'),
    entry_points={
        'console_scripts': [
            'ironic-notification-manager = ironic_notification_manager.ironic_notification_manager:main',
        ],
    },
)
