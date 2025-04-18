#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

import re
import sys

import setuptools


def requirements(filename):
    with open(filename) as f:
        lines = f.read().splitlines()
    c = re.compile(r'\s*#.*')
    return list(filter(bool, map(lambda y: c.sub('', y).strip(), lines)))

setuptools.setup(
    name='schema_transformer',
    version='0.1.dev0',
    packages=setuptools.find_packages(
        exclude=["*.test", "*.test.*", "test.*", "test"]),
    package_data={'': ['*.html', '*.css', '*.xml']},
    zip_safe=False,
    long_description="VNC Configuration Schema Transformer",
    entry_points={
        # Please update sandesh/common/vns.sandesh on process name change
        'console_scripts': [
            'contrail-schema = schema_transformer.to_bgp:server_main',
        ],
    },
    install_requires=requirements('requirements.txt'),
    tests_require=requirements('test-requirements.txt'),
    test_suite='schema_transformer.tests'
)
