#
# Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
#
import uuid
import logging
import random
import netaddr

import unittest
from flexmock import flexmock
import json
import copy
import netaddr
import contextlib

from vnc_api.vnc_api import *
import vnc_api.gen.vnc_api_test_gen
from vnc_api.gen.resource_test import *
from cfgm_common import vnc_cgitb
vnc_cgitb.enable(format='text')

from cfgm_common.tests import test_common
from . import test_case

logger = logging.getLogger(__name__)
logger.setLevel(logging.DEBUG)


class TestDBAudit(test_case.ApiServerTestCase):
    @classmethod
    def setUpClass(cls, *args, **kwargs):
        cls.console_handler = logging.StreamHandler()
        cls.console_handler.setLevel(logging.DEBUG)
        logger.addHandler(cls.console_handler)
        super(TestDBAudit, cls).setUpClass(*args, **kwargs)
    # end setUpClass

    @classmethod
    def tearDownClass(cls, *args, **kwargs):
        logger.removeHandler(cls.console_handler)
        super(TestDBAudit, cls).tearDownClass(*args, **kwargs)
    # end tearDownClass

    @contextlib.contextmanager
    def audit_mocks(self):
        with test_common.patch_imports(
            [('schema_transformer.db',
              flexmock(db=flexmock(
                  SchemaTransformerDB=flexmock(get_db_info=lambda: [('to_bgp_keyspace', ['route_target_table'])]))))]):
            yield
    # end audit_mocks

    def _create_vn_subnet_ipam_iip(self, name):
        ipam_obj = vnc_api.NetworkIpam('vn-%s' % name)
        self._vnc_lib.network_ipam_create(ipam_obj)
        vn_obj = vnc_api.VirtualNetwork(name)
        vn_obj.add_network_ipam(ipam_obj,
            VnSubnetsType(
                [IpamSubnetType(SubnetType('1.1.1.0', 28))]))
        self._vnc_lib.virtual_network_create(vn_obj)
        iip_obj = vnc_api.InstanceIp('iip-%s' % name)
        iip_obj.add_virtual_network(vn_obj)
        self._vnc_lib.instance_ip_create(iip_obj)

        return vn_obj, ipam_obj, iip_obj
    # end _create_vn_subnet_ipam_iip

    def _create_security_group(self, name):
        sg_obj = vnc_api.SecurityGroup(name)
        self._vnc_lib.security_group_create(sg_obj)
        return sg_obj

    @unittest.skip("doesn't work with cassandra driver and called is not used")
    def test_checker(self):
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            test_obj = self._create_test_object()
            self.assertTill(self.vnc_db_has_ident, obj=test_obj)
            db_manage.db_check(*db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
    # end test_checker

    def test_checker_missing_mandatory_fields(self):
        # detect OBJ_UUID_TABLE entry missing required fields
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            test_obj = self._create_test_object()
            uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
            orig_col_val_ts = uuid_cf.get(test_obj.uuid,
                include_timestamp=True)
            omit_col_names = random.sample(set(
                ['type', 'fq_name', 'prop:id_perms']), 1)
            wrong_col_val_ts = dict((k,v) for k,v in list(orig_col_val_ts.items())
                if k not in omit_col_names)
            with uuid_cf.patch_row(
                test_obj.uuid, wrong_col_val_ts):
                db_checker = db_manage.DatabaseChecker(
                    *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
                errors = db_checker.check_obj_mandatory_fields()
                self.assertIn(db_manage.MandatoryFieldsMissingError,
                    [type(x) for x in errors])
    # end test_checker_missing_mandatory_fields

    def test_checker_fq_name_mismatch_index_to_object(self):
        # detect OBJ_UUID_TABLE and OBJ_FQ_NAME_TABLE inconsistency
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            test_obj = self._create_test_object()
            self.assert_vnc_db_has_ident(test_obj)

            uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
            orig_col_val_ts = uuid_cf.get(test_obj.uuid,
                include_timestamp=True)
            wrong_col_val_ts = copy.deepcopy(orig_col_val_ts)
            wrong_col_val_ts['fq_name'] = (json.dumps(['wrong-fq-name']),
                wrong_col_val_ts['fq_name'][1])
            with uuid_cf.patch_row(
                test_obj.uuid, wrong_col_val_ts):
                db_checker = db_manage.DatabaseChecker(
                    *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
                errors = db_checker.check_fq_name_uuid_match()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.FQNMismatchError, error_types)
                self.assertIn(db_manage.FQNStaleIndexError, error_types)
                self.assertIn(db_manage.FQNIndexMissingError, error_types)
    # end test_checker_fq_name_mismatch_index_to_object

    def test_checker_fq_name_index_stale(self):
        # fq_name table in cassandra has entry but obj_uuid table doesn't
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            test_obj = self._create_test_object()
            uuid_cf = self.get_cf('config_db_uuid','obj_uuid_table')
            with uuid_cf.patch_row(test_obj.uuid, new_columns=None):
                db_checker = db_manage.DatabaseChecker(
                    *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
                errors = db_checker.check_fq_name_uuid_match()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.FQNStaleIndexError, error_types)
    # test_checker_fq_name_mismatch_stale

    def test_checker_fq_name_index_missing(self):
        # obj_uuid table has entry but fq_name table in cassandra doesn't
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            test_obj = self._create_test_object()
            self.assert_vnc_db_has_ident(test_obj)
            uuid_cf = self.get_cf('config_db_uuid','obj_uuid_table')
            fq_name_cf = self.get_cf('config_db_uuid','obj_fq_name_table')
            test_obj_type = test_obj.get_type().replace('-', '_')
            orig_col_val_ts = fq_name_cf.get(test_obj_type,
                include_timestamp=True)
            # remove test obj in fq-name table
            wrong_col_val_ts = dict((k,v) for k,v in list(orig_col_val_ts.items())
                if ':'.join(test_obj.fq_name) not in k)
            with fq_name_cf.patch_row(test_obj_type, new_columns=wrong_col_val_ts):
                db_checker = db_manage.DatabaseChecker(
                    *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
                errors = db_checker.check_fq_name_uuid_match()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.FQNIndexMissingError, error_types)
    # test_checker_fq_name_mismatch_missing

    def test_checker_ifmap_identifier_extra(self):
        # ifmap has identifier but obj_uuid table in cassandra doesn't
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            test_obj = self._create_test_object()
            self.assert_vnc_db_has_ident(test_obj)

            uuid_cf = self.get_cf('config_db_uuid','obj_uuid_table')
            with uuid_cf.patch_row(test_obj.uuid, new_columns=None):
                db_checker = db_manage.DatabaseChecker(
                    *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
                errors = db_checker.check_fq_name_uuid_match()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.FQNStaleIndexError, error_types)
    # test_checker_ifmap_identifier_extra

    def test_checker_ifmap_identifier_missing(self):
        # ifmap doesn't have an identifier but obj_uuid table
        # in cassandra does
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            uuid_cf = self.get_cf('config_db_uuid','obj_uuid_table')
            with uuid_cf.patch_row(str(uuid.uuid4()),
                    new_columns={'type': json.dumps(''),
                                 'fq_name':json.dumps(''),
                                 'prop:id_perms':json.dumps('')}):
                db_checker = db_manage.DatabaseChecker(
                    *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
                errors = db_checker.check_fq_name_uuid_match()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.FQNIndexMissingError, error_types)
    # test_checker_ifmap_identifier_missing

    def test_checker_useragent_subnet_key_missing(self):
        pass # move to vnc_openstack test
    # test_checker_useragent_subnet_key_missing

    def test_checker_useragent_subnet_id_missing(self):
        pass # move to vnc_openstack test
    # test_checker_useragent_subnet_id_missing

    def test_checker_ipam_subnet_uuid_missing(self):
        pass # move to vnc_openstack test
    # test_checker_ipam_subnet_uuid_missing

    def test_checker_subnet_count_mismatch(self):
        pass # move to vnc_openstack test
    # test_checker_subnet_count_mismatch

    def test_checker_useragent_subnet_missing(self):
        pass # move to vnc_openstack test
    # test_checker_useragent_subnet_missing

    def test_checker_useragent_subnet_extra(self):
        pass # move to vnc_openstack test
    # test_checker_useragent_subnet_extra

    def test_checker_zk_vn_extra(self):
        vn_obj, _, _ = self._create_vn_subnet_ipam_iip(self.id())
        fq_name_cf = self.get_cf('config_db_uuid','obj_fq_name_table')
        orig_col_val_ts = fq_name_cf.get('virtual_network',
            include_timestamp=True)
        # remove test obj in fq-name table
        wrong_col_val_ts = dict((k,v) for k,v in list(orig_col_val_ts.items())
            if ':'.join(vn_obj.fq_name) not in k)
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
            # verify catch of extra ZK VN when name index is mocked
            with fq_name_cf.patch_row('virtual_network',
                new_columns=wrong_col_val_ts):
                errors = db_checker.check_subnet_addr_alloc()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.FQNIndexMissingError, error_types)
    # test_checker_zk_vn_extra

    def test_checker_zk_vn_missing(self):
        vn_obj, _, _ = self._create_vn_subnet_ipam_iip(self.id())
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))

            with db_checker._zk_client.patch_path(
                '%s%s/%s' %(self._cluster_id,
                          db_checker.BASE_SUBNET_ZK_PATH,
                          vn_obj.get_fq_name_str())):
                errors = db_checker.check_subnet_addr_alloc()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.ZkVNMissingError, error_types)
                self.assertIn(db_manage.ZkSubnetMissingError, error_types)
    # test_checker_zk_vn_missing

    def test_checker_zk_ip_extra(self):
        vn_obj, _, _ = self._create_vn_subnet_ipam_iip(self.id())
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))

            # verify catch of zk extra ip when iip is mocked absent
            iip_obj = vnc_api.InstanceIp(self.id())
            iip_obj.add_virtual_network(vn_obj)
            self._vnc_lib.instance_ip_create(iip_obj)
            uuid_cf = self.get_cf('config_db_uuid','obj_uuid_table')
            with uuid_cf.patch_row(iip_obj.uuid, None):
                errors = db_checker.check_subnet_addr_alloc()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.FQNStaleIndexError, error_types)
                self.assertIn(db_manage.ZkIpExtraError, error_types)
    # test_checker_zk_ip_extra

    def test_checker_zk_ip_missing(self):
        vn_obj, _, _ = self._create_vn_subnet_ipam_iip(self.id())
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))

            iip_obj = vnc_api.InstanceIp(self.id())
            iip_obj.add_virtual_network(vn_obj)
            self._vnc_lib.instance_ip_create(iip_obj)
            ip_addr = self._vnc_lib.instance_ip_read(
                id=iip_obj.uuid).instance_ip_address
            ip_str = "%(#)010d" % {'#': int(netaddr.IPAddress(ip_addr))}
            with db_checker._zk_client.patch_path(
                '%s%s/%s:1.1.1.0/28/%s' %(
                    self._cluster_id, db_checker.BASE_SUBNET_ZK_PATH,
                    vn_obj.get_fq_name_str(), ip_str)):
                errors = db_checker.check_subnet_addr_alloc()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.ZkIpMissingError, error_types)
    # test_checker_zk_ip_missing

    def test_checker_zk_route_target_extra(self):
        pass # move to schema transformer test
    # test_checker_zk_route_target_extra

    def test_checker_zk_route_target_range_wrong(self):
        pass # move to schema transformer test
    # test_checker_zk_route_target_range_wrong

    def test_checker_cass_route_target_range_wrong(self):
        pass # move to schema transformer test
    # test_checker_cass_route_target_range_wrong

    def test_checker_route_target_count_mismatch(self):
        # include user assigned route-targets here
        pass # move to schema transformer test
    # test_checker_route_target_count_mismatch

    def test_checker_zk_virtual_network_id_extra_and_missing(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        vn_obj, _, _ = self._create_vn_subnet_ipam_iip(self.id())

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
            with uuid_cf.patch_column(
                    vn_obj.uuid,
                    'prop:virtual_network_network_id',
                    json.dumps(42)):
                errors = db_checker.check_virtual_networks_id()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.ZkVNIdExtraError, error_types)
                self.assertIn(db_manage.ZkVNIdMissingError, error_types)
    # test_checker_zk_virtual_network_id_extra_and_missing

    def test_checker_zk_virtual_network_id_duplicate(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        vn1_obj, _, _ = self._create_vn_subnet_ipam_iip('vn1-%s' % self.id())
        vn1_obj = self._vnc_lib.virtual_network_read(id=vn1_obj.uuid)
        vn2_obj, _, _ = self._create_vn_subnet_ipam_iip('vn2-%s' % self.id())

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
            with uuid_cf.patch_column(
                    vn2_obj.uuid,
                    'prop:virtual_network_network_id',
                    json.dumps(vn1_obj.virtual_network_network_id)):
                errors = db_checker.check_virtual_networks_id()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.VNDuplicateIdError, error_types)
                self.assertIn(db_manage.ZkVNIdExtraError, error_types)
    # test_checker_zk_virtual_network_id_duplicate

    def test_checker_zk_security_group_id_extra_and_missing(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        sg_obj = self._create_security_group(self.id())

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
            with uuid_cf.patch_column(
                    sg_obj.uuid,
                    'prop:security_group_id',
                    json.dumps(8000042)):
                errors = db_checker.check_security_groups_id()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.ZkSGIdExtraError, error_types)
                self.assertIn(db_manage.ZkSGIdMissingError, error_types)
    # test_checker_zk_security_group_id_extra_and_missing

    def test_checker_zk_security_group_id_duplicate(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        sg1_obj = self._create_security_group('sg1-%s' % self.id())
        sg1_obj = self._vnc_lib.security_group_read(id=sg1_obj.uuid)
        sg2_obj = self._create_security_group('sg2-%s' % self.id())

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_checker = db_manage.DatabaseChecker(
                *db_manage._parse_args('check --cluster_id %s' %(self._cluster_id)))
            with uuid_cf.patch_column(
                    sg2_obj.uuid,
                    'prop:security_group_id',
                    json.dumps(sg1_obj.security_group_id)):
                errors = db_checker.check_security_groups_id()
                error_types = [type(x) for x in errors]
                self.assertIn(db_manage.SGDuplicateIdError, error_types)
                self.assertIn(db_manage.ZkSGIdExtraError, error_types)
    # test_checker_zk_security_group_id_duplicate

    def test_checker_security_group_0_missing(self):
        pass # move to schema transformer test
    # test_checker_security_group_0_missing

    def test_checker_route_targets_id_with_vn_rt_list_set_to_none(self):
        project = Project('project-%s' % self.id())
        self._vnc_lib.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        self._vnc_lib.virtual_network_create(vn)
        vn.set_route_target_list(None)
        self._vnc_lib.virtual_network_update(vn)

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            args = db_manage._parse_args(
                'check --cluster_id %s' % self._cluster_id)
            db_checker = db_manage.DatabaseChecker(*args)
            db_checker.audit_route_targets_id()

    def test_cleaner(self):
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_manage.db_clean(*db_manage._parse_args('clean --cluster_id %s' %(self._cluster_id)))
    # end test_cleaner

    def test_cleaner_zk_virtual_network_id(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        vn_obj, _, _ = self._create_vn_subnet_ipam_iip(self.id())
        vn_obj = self._vnc_lib.virtual_network_read(id=vn_obj.uuid)

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_cleaner = db_manage.DatabaseCleaner(
                *db_manage._parse_args('--execute clean --cluster_id %s' %(self._cluster_id)))
            fake_id = 42
            with uuid_cf.patch_column(
                    vn_obj.uuid,
                    'prop:virtual_network_network_id',
                    json.dumps(fake_id)):
                db_cleaner.clean_stale_virtual_network_id()
                zk_id_str = "%(#)010d" %\
                    {'#': vn_obj.virtual_network_network_id - 1}
                self.assertIsNone(
                    db_cleaner._zk_client.exists(
                        '%s%s/%s' % (
                            self._cluster_id, db_cleaner.BASE_VN_ID_ZK_PATH,
                            zk_id_str))
                )

    def test_healer_zk_virtual_network_id(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        vn_obj, _, _ = self._create_vn_subnet_ipam_iip(self.id())
        vn_obj = self._vnc_lib.virtual_network_read(id=vn_obj.uuid)

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_cleaner = db_manage.DatabaseHealer(
                    *db_manage._parse_args('--execute heal --cluster_id %s' % (
                        self._cluster_id)))
            fake_id = 42
            with uuid_cf.patch_column(
                    vn_obj.uuid,
                    'prop:virtual_network_network_id',
                    json.dumps(fake_id)):
                db_cleaner.heal_virtual_networks_id()
                zk_id_str = "%(#)010d" % {'#': fake_id - 1}
                self.assertIsNotNone(
                    db_cleaner._zk_client.exists(
                        '%s%s/%s' % (
                             self._cluster_id, db_cleaner.BASE_VN_ID_ZK_PATH,
                             zk_id_str))[0])

    def test_cleaner_zk_security_group_id(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        sg_obj = self._create_security_group(self.id())
        sg_obj = self._vnc_lib.security_group_read(id=sg_obj.uuid)

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_cleaner = db_manage.DatabaseCleaner(
                *db_manage._parse_args('--execute clean --cluster_id %s' %(self._cluster_id)))
            with uuid_cf.patch_column(
                    sg_obj.uuid,
                    'prop:security_group_id',
                    json.dumps(8000042)):
                db_cleaner.clean_stale_security_group_id()
                zk_id_str = "%(#)010d" % {'#': sg_obj.security_group_id}
                self.assertIsNone(
                    db_cleaner._zk_client.exists(
                        '%s%s/%s' % (
                            self._cluster_id, db_cleaner.BASE_VN_ID_ZK_PATH,
                            zk_id_str))
                )

    def test_healer_zk_security_group_id(self):
        uuid_cf = self.get_cf('config_db_uuid', 'obj_uuid_table')
        sg_obj = self._create_security_group(self.id())
        sg_obj = self._vnc_lib.security_group_read(id=sg_obj.uuid)

        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_cleaner = db_manage.DatabaseHealer(
                *db_manage._parse_args('--execute heal --cluster_id %s' %(self._cluster_id)))
            with uuid_cf.patch_column(
                    sg_obj.uuid,
                    'prop:security_group_id',
                    json.dumps(8000042)):
                db_cleaner.heal_security_groups_id()
                zk_id_str = "%(#)010d" % {'#': 42}
                self.assertIsNotNone(
                    db_cleaner._zk_client.exists(
                        '%s%s/%s' %
                        (self._cluster_id, db_cleaner.BASE_SG_ID_ZK_PATH,
                         zk_id_str))[0])

    def test_clean_obj_missing_mandatory_fields(self):
        pass
    # end test_clean_obj_missing_mandatory_fields

    def test_clean_dangling_fq_names(self):
        pass
    # end test_clean_dangling_fq_names()

    def test_clean_dangling_back_refs(self):
        pass
    # end test_clean_dangling_back_refs()

    def test_clean_dangling_children(self):
        pass
    # end test_clean_dangling_children

    def test_healer(self):
        with self.audit_mocks():
            from vnc_cfg_api_server import db_manage
            db_manage.db_heal(*db_manage._parse_args('heal --cluster_id %s' %(self._cluster_id)))
    # end test_healer

    def test_heal_fq_name_index(self):
        pass
    # end test_heal_fq_name_index

    def test_heal_back_ref_index(self):
        pass
    # end test_heal_back_ref_index

    def test_heal_children_index(self):
        pass
    # end test_heal_children_index

    def test_heal_useragent_subnet_uuid(self):
        pass
    # end test_heal_useragent_subnet_uuid
# end class TestDBAudit


if __name__ == '__main__':
    ch = logging.StreamHandler()
    ch.setLevel(logging.DEBUG)
    logger.addHandler(ch)

    # unittest.main(failfast=True)
    unittest.main()
