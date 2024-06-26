#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

from setuptools import setup

setup(
    name='Control-Node',
    version='0.1.dev0',
    packages=['control_node',
              'control_node.control_node', 
              'control_node.control_node.cpuinfo', 
              'control_node.control_node.ifmap_server_show',
              'control_node.control_node.process_info', 
             ],
    package_data={'':['*.html', '*.css', '*.xml']},
    zip_safe=False,
    long_description="Control Node Sandesh",
    install_requires=[
        'psutil>=0.6.0'
    ]
)
