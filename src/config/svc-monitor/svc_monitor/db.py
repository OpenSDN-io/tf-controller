# vim: tabstop=4 shiftwidth=4 softtabstop=4
#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#

"""
Service monitor DB to store VM, SI information
"""
import inspect

from cfgm_common import jsonutils as json
from cfgm_common.datastore.keyspace import ConfigKeyspaceMap
from cfgm_common.vnc_object_db import VncObjectDBClient
from sandesh_common.vns.constants import SVC_MONITOR_KEYSPACE_NAME

class ServiceMonitorDB(VncObjectDBClient):

    _KEYSPACE = SVC_MONITOR_KEYSPACE_NAME
    _SVC_SI_CF = ConfigKeyspaceMap.get_cf_name(_KEYSPACE, 'SVC_SI_CF')
    _POOL_CF = ConfigKeyspaceMap.get_cf_name(_KEYSPACE, 'POOL_CF')
    _LB_CF = ConfigKeyspaceMap.get_cf_name(_KEYSPACE, 'LB_CF')
    _HM_CF = ConfigKeyspaceMap.get_cf_name(_KEYSPACE, 'HM_CF')

    def __init__(self, args, logger):
        self._db_logger = logger

        keyspaces = {
            self._KEYSPACE: {
                self._SVC_SI_CF: {},
                self._POOL_CF: {},
                self._LB_CF: {},
                self._HM_CF: {},
            }
        }

        cred = None
        if (args.cassandra_user is not None and
            args.cassandra_password is not None):
            cred={'username':args.cassandra_user,
                  'password':args.cassandra_password}

        super(ServiceMonitorDB, self).__init__(args.cassandra_server_list,
                                               args.cluster_id,
                                               keyspaces,
                                               None,
                                               self._db_logger.log,
                                               reset_config=args.reset_config,
                                               credential=cred,
                                               ssl_enabled=args.cassandra_use_ssl,
                                               ca_certs=args.cassandra_ca_certs,
                                               cassandra_driver=args.cassandra_driver,
                                               num_workers=args.num_workers,
                                               num_groups=args.num_groups,
                                               zk_servers=args.zk_server_ip)

        self._svc_si_cf = self._cassandra_driver._cf_dict[self._SVC_SI_CF]
        self._pool_cf = self._cassandra_driver._cf_dict[self._POOL_CF]
        self._lb_cf = self._cassandra_driver._cf_dict[self._LB_CF]
        self._hm_cf = self._cassandra_driver._cf_dict[self._HM_CF]

    # db CRUD
    def _db_get(self, cf_name, key, column):
        try:
            entry = self._cassandra_driver.get_one_col(cf_name, key, column)
        except Exception:
            # TODO(ethuleau): VncError is raised if more than one row was
            #                 fetched from db with get_one_col method.
            #                 Probably need to be cleaned
            self._db_logger.log("DB: %s %s get failed" %
                             (inspect.stack()[1][3], key))
            return None

        try:
            return json.loads(entry)
        except (ValueError, TypeError) as e:
            return entry


    def _db_insert(self, table_name, key, entry):
        try:
            self._cassandra_driver.insert(key, entry,
                                          cf_name=table_name)
        except Exception:
            self._db_logger.log("DB: %s %s insert failed" %
                             (inspect.stack()[1][3], key))
            return False

        return True

    def _db_remove(self, table, key, columns=None):
        try:
            if columns:
                self._cassandra_driver.remove(table,
                                              key, columns=columns)
            else:
                self._cassandra_driver.remove(table, key)
        except Exception:
            self._db_logger.log("DB: %s %s remove failed" %
                             (inspect.stack()[1][3], key))
            return False

        return True

    def _db_list(self, table):
        try:
            entries = list(self._cassandra_driver.get_range(table))
        except Exception:
            self._db_logger.log("DB: %s list failed" %
                             (inspect.stack()[1][3]))
            return None

        return entries

    def get_vm_db_prefix(self, inst_count):
        return('vm' + str(inst_count) + '-')

    def remove_vm_info(self, si_fq_str, vm_uuid):
        si_info = self.service_instance_get(si_fq_str)
        if not si_info:
            return

        prefix = None
        for key, item in list(si_info.items()):
            if item == vm_uuid:
                prefix = key.split('-')[0]
                break
        if not prefix:
            return

        vm_column_list = []
        for key in list(si_info.keys()):
            if key.startswith(prefix):
                vm_column_list.append(key)
        self.service_instance_remove(si_fq_str, vm_column_list)

    # service instance CRUD
    def service_instance_get(self, si_fq_str):
        return self._db_get(self._SVC_SI_CF, si_fq_str)

    def service_instance_insert(self, si_fq_str, entry):
        return self._db_insert(self._SVC_SI_CF, si_fq_str, entry)

    def service_instance_remove(self, si_fq_str, columns=None):
        return self._db_remove(self._SVC_SI_CF, si_fq_str, columns)

    def service_instance_list(self):
        return self._db_list(self._SVC_SI_CF)

    def health_monitor_config_get(self, hm_id):
        return self._db_get(self._HM_CF, hm_id, 'config_info')

    def health_monitor_config_insert(self, hm_id, hm_obj):
        entry = json.dumps(hm_obj)
        return self._db_insert(self._HM_CF, hm_id, {'config_info': entry})

    def health_monitor_config_remove(self, hm_id):
        return self._db_remove(self._HM_CF, hm_id, 'config_info')

    def health_monitor_driver_info_get(self, hm_id):
        return self._db_get(self._HM_CF, hm_id, 'driver_info')

    def health_monitor_driver_info_insert(self, hm_id, hm_obj):
        entry = json.dumps(hm_obj)
        return self._db_insert(self._HM_CF, hm_id, {'driver_info': entry})

    def health_monitor_driver_info_remove(self, hm_id):
        return self._db_remove(self._HM_CF, hm_id, 'driver_info')

    def health_monitor_list(self):
        ret_list = []
        for each_entry_id, each_entry_data in self._db_list(self._HM_CF) or []:
            if len(each_entry_data) > 0:
                config_info_obj_dict = json.loads(each_entry_data['config_info'])
                driver_info_obj_dict = None
                if 'driver_info' in each_entry_data:
                    driver_info_obj_dict = json.loads(each_entry_data['driver_info'])
                ret_list.append((each_entry_id, config_info_obj_dict, driver_info_obj_dict))
        return ret_list

    def healthmonitor_remove(self, hm_id, columns=None):
        return self._db_remove(self._HM_CF, hm_id, columns)

    def loadbalancer_config_get(self, lb_id):
        return self._db_get(self._LB_CF, lb_id, 'config_info')

    def loadbalancer_driver_info_get(self, lb_id):
        return self._db_get(self._LB_CF, lb_id, 'driver_info')

    def loadbalancer_config_insert(self, lb_id, lb_obj):
        entry = json.dumps(lb_obj)
        return self._db_insert(self._LB_CF, lb_id, {'config_info': entry})

    def loadbalancer_driver_info_insert(self, lb_id, lb_obj):
        entry = json.dumps(lb_obj)
        return self._db_insert(self._LB_CF, lb_id, {'driver_info': entry})

    def loadbalancer_remove(self, lb_id, columns=None):
        return self._db_remove(self._LB_CF, lb_id, columns)

    def loadbalancer_list(self):
        ret_list = []
        for each_entry_id, each_entry_data in self._db_list(self._LB_CF) or []:
            if len(each_entry_data) > 0:
                config_info_obj_dict = json.loads(each_entry_data['config_info'])
                driver_info_obj_dict = None
                if 'driver_info' in each_entry_data:
                    driver_info_obj_dict = json.loads(each_entry_data['driver_info'])
                ret_list.append((each_entry_id, config_info_obj_dict, driver_info_obj_dict))
        return ret_list

    def pool_config_get(self, pool_id):
        return self._db_get(self._POOL_CF, pool_id, 'config_info')

    def pool_driver_info_get(self, pool_id):
        return self._db_get(self._POOL_CF, pool_id, 'driver_info')

    def pool_config_insert(self, pool_id, pool_obj):
        entry = json.dumps(pool_obj)
        return self._db_insert(self._POOL_CF, pool_id, {'config_info': entry})

    def pool_driver_info_insert(self, pool_id, pool_obj):
        entry = json.dumps(pool_obj)
        return self._db_insert(self._POOL_CF, pool_id, {'driver_info': entry})

    def pool_remove(self, pool_id, columns=None):
        return self._db_remove(self._POOL_CF, pool_id, columns)

    def pool_list(self):
        ret_list = []
        for each_entry_id, each_entry_data in self._db_list(self._POOL_CF) or []:
            if len(each_entry_data) > 0:
                config_info_obj_dict = json.loads(each_entry_data['config_info'])
                driver_info_obj_dict = None
                if 'driver_info' in each_entry_data:
                    driver_info_obj_dict = json.loads(each_entry_data['driver_info'])
                ret_list.append((each_entry_id, config_info_obj_dict, driver_info_obj_dict))
        return ret_list
