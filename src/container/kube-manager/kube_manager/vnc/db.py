# vim: tabstop=4 shiftwidth=4 softtabstop=4
#
# Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
#

"""
Kube network manager DB.
"""

from cfgm_common.vnc_object_db import VncObjectDBClient


class KubeNetworkManagerDB(VncObjectDBClient):

    def __init__(self, args, logger):
        self._db_logger = logger

        cred = None
        if args.cassandra_user and args.cassandra_password:
            cred = {'username': args.cassandra_user,
                    'password': args.cassandra_password}

        super(KubeNetworkManagerDB, self).__init__(
            args.cassandra_server_list,
            args.cluster_id, None, None, self._db_logger.log,
            reset_config=False, credential=cred,
            ssl_enabled=args.cassandra_use_ssl,
            ca_certs=args.cassandra_ca_certs,
            cassandra_driver=args.cassandra_driver,
            zk_servers=args.zk_server_ip)
