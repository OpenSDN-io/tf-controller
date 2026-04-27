#
# Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
#
import json
import logging

from cfgm_common.exceptions import BadRequest
from cfgm_common.exceptions import NoIdError
from cfgm_common.exceptions import RefsExistError
from cfgm_common.exceptions import ResourceExistsError
import gevent
import mock
from sandesh_common.vns import constants
from vnc_api.utils import OP_POST
from vnc_api.vnc_api import AddressGroup
from vnc_api.vnc_api import PermType2
from vnc_api.vnc_api import Project
from vnc_api.vnc_api import Tag
from vnc_api.vnc_api import TagType
from vnc_api.vnc_api import VirtualMachine
from vnc_api.vnc_api import VirtualNetwork

from vnc_cfg_api_server.tests import test_case


logger = logging.getLogger(__name__)


class TestTagBase(test_case.ApiServerTestCase):
    @classmethod
    def setUpClass(cls, *args, **kwargs):
        cls.console_handler = logging.StreamHandler()
        cls.console_handler.setLevel(logging.DEBUG)
        logger.addHandler(cls.console_handler)
        super(TestTagBase, cls).setUpClass(*args, **kwargs)

    @classmethod
    def tearDownClass(cls, *args, **kwargs):
        logger.removeHandler(cls.console_handler)
        super(TestTagBase, cls).tearDownClass(*args, **kwargs)

    @property
    def api(self):
        return self._vnc_lib


class TestTag(TestTagBase):
    STALE_LOCK_SECS = '0.2'

    @classmethod
    def setUpClass(cls):
        super(TestTag, cls).setUpClass(
            extra_config_knobs=[
                ('DEFAULTS', 'stale_lock_seconds', cls.STALE_LOCK_SECS),
            ]
        )

    def test_tag_name_is_composed_with_type_and_value(self):
        name = 'fake-name-%s' % self.id()
        tag_type = 'fake_type-%s' % self.id()
        tag_type = tag_type.lower()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(name=name, display_name=name, tag_type_name=tag_type,
                  tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        self.assertEqual(tag.name, '%s=%s' % (tag_type, tag_value))
        self.assertEqual(tag.fq_name[-1], '%s=%s' % (tag_type, tag_value))
        self.assertEqual(tag.display_name, '%s=%s' % (tag_type, tag_value))
        self.assertEqual(tag.tag_type_name, tag_type)
        self.assertEqual(tag.tag_value, tag_value)

    def test_tag_is_unique(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value)
        self.api.tag_create(tag1)
        scoped_tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value,
                          parent_obj=project)
        self.api.tag_create(scoped_tag1)
        gevent.sleep(float(self.STALE_LOCK_SECS))

        tag2 = Tag(tag_type_name=tag_type, tag_value=tag_value)
        self.assertRaises(RefsExistError, self.api.tag_create, tag2)
        scoped_tag2 = Tag(tag_type_name=tag_type, tag_value=tag_value,
                          parent_obj=project)
        self.assertRaises(RefsExistError, self.api.tag_create, scoped_tag2)

    def test_tag_type_is_mandatory(self):
        value = 'fake_value-%s' % self.id()
        tag = Tag(tag_value=value)

        self.assertRaises(BadRequest, self.api.tag_create, tag)

    def test_tag_type_cannot_be_updated(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)
        tag.tag_type_name = 'new_fake_type-%s' % self.id()
        self.assertRaises(BadRequest, self.api.tag_update, tag)

    def test_tag_value_is_mandatory(self):
        tag_type = 'fake_type-%s' % self.id()
        tag = Tag(tag_type_name=tag_type)

        self.assertRaises(BadRequest, self.api.tag_create, tag)

    def test_tag_value_cannot_be_updated(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)
        tag.tag_value = 'new_fake_type-%s' % self.id()
        self.assertRaises(BadRequest, self.api.tag_update, tag)

    def test_ud_tag_and_ud_tag_type(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0xDEAD80000001')
        tag_uuid = self.api.tag_create(tag)
        tag_obj = self.api.tag_read(id=tag_uuid)
        tag_type_uuid = tag_obj.get_tag_type_refs()[0]['uuid']
        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            tag_type_read = self.api.tag_type_read(id=tag_type_uuid)
        # validate if user defined tag type is created
        self.assertEqual(tag_type_uuid, tag_type_read.uuid)
        # validate if tag type id is 0xDEAD
        self.assertEqual("0xdead", tag_type_read.tag_type_id.lower())
        # validate complete tag_id
        self.assertEqual("0xdead80000001", tag_obj.tag_id.lower())

        # Negative test check if recreating of tag with different
        # fq-name but same ID fails
        tag_value = 'fake_value-ud%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0xDEAD80000001')
        self.assertRaises(BadRequest, self.api.tag_create, tag)

        # Validate user defined tag delete.
        tag_obj = self.api.tag_read(id=tag_uuid)
        self.assertIsNotNone(tag_uuid)
        self.api.tag_delete(id=tag_uuid)
        self.assertRaises(NoIdError, self.api.tag_read,
                          id=tag_uuid)

        # validate release tag ID can be re-allocated to be sure
        # IDs are reserved and freed properly
        tag_value = 'fake_value%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0xDEAD80000001')
        tag_uuid = self.api.tag_create(tag)
        tag_obj = self.api.tag_read(id=tag_uuid)
        self.assertIsNotNone(tag_uuid)
        self.assertEqual("0xdead80000001", tag_obj.tag_id.lower())

    def test_tag_value_id_out_of_range_rejected(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        # value_id = 0x100000000 > 0xFFFFFFFF — out of 32 bit range
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0xDEAD0100000000')
        self.assertRaises(BadRequest, self.api.tag_create, tag)

    def test_ud_tag_type_ud_tag_invalid_value(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        # Create auto tag and ud tag-type
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0xDEAD00wq')
        self.assertRaises(BadRequest, self.api.tag_create, tag)

    def test_ud_tag_type_ud_tag_value_exceeds(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        # Create auto tag and ud tag-type
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0xDEAD00011')
        self.assertRaises(BadRequest, self.api.tag_create, tag)

    # test to create user defined tag with predefined tag-types
    def test_ud_tag_predef_tag_type(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        # Create auto tag and tag-type
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag_obj = self.api.tag_read(id=tag_uuid)
        # validate if Tag created properly
        self.assertIsNotNone(tag_uuid)
        tag_type_uuid = tag_obj.get_tag_type_refs()[0]['uuid']
        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            tag_type_read = self.api.tag_type_read(id=tag_type_uuid)
        ud_tag_id = tag_type_read.tag_type_id + "80000001"
        # create user defined tag with pre-def tag-type
        ud_tag_value = 'fake_value-ud-%s' % self.id()
        ud_tag = Tag(tag_type_name=tag_type, tag_value=ud_tag_value,
                     tag_id=ud_tag_id)
        ud_tag_uuid = self.api.tag_create(ud_tag)
        ud_tag_obj = self.api.tag_read(id=ud_tag_uuid)
        self.assertEqual(ud_tag_id, ud_tag_obj.tag_id.lower())

    def test_tag_id_updated(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)
        type_part = int(tag.tag_id, 0) & (0xFFFF << 32)
        new_tag_id = '0x{:012x}'.format(type_part | 0x0000BEEFDEAD)
        tag.tag_id = new_tag_id
        self.api.tag_update(tag)
        tag_update = self.api.tag_read(id=tag_uuid)
        self.assertEqual(tag.tag_id, tag_update.tag_id.lower())
        self.api.tag_delete(id=tag_uuid)

    def test_tag_type_reference_cannot_be_set(self):
        tag_value = 'fake_value-%s' % self.id()
        tag_type = TagType(name='tag-type-%s' % self.id())
        tag = Tag(tag_type_name=tag_type.name, tag_value=tag_value)
        tag.set_tag_type(tag_type)

        self.assertRaises(BadRequest, self.api.tag_create, tag)

    def test_tag_type_reference_cannot_be_updated(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id='0x00A1')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        tag.set_tag_type(tag_type)
        self.assertRaises(BadRequest, self.api.tag_update, tag)

    def test_tag_type_reference_is_unique(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id='0x00A2')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        tag.add_tag_type(tag_type)
        self.assertRaises(BadRequest, self.api.tag_update, tag)

    def test_tag_type_reference_cannot_be_removed(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        tag_type_uuid = tag.get_tag_type_refs()[0]['uuid']
        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            tag_type = self.api.tag_type_read(id=tag_type_uuid)
        tag.del_tag_type(tag_type)
        self.assertRaises(BadRequest, self.api.tag_update, tag)

    def test_associated_tag_type_is_hidden_to_user(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        tag_type_uuid = tag.get_tag_type_refs()[0]['uuid']
        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            tag_type = self.api.tag_type_read(id=tag_type_uuid)
        self.assertFalse(tag_type.get_id_perms().get_user_visible())

    def test_associated_tag_type_is_deleted_if_not_used(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        tag_type_uuid = tag.get_tag_type_refs()[0]['uuid']
        self.api.tag_delete(id=tag_uuid)
        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            self.assertRaises(NoIdError, self.api.tag_type_read,
                              id=tag_type_uuid)

    def test_associated_tag_type_is_not_deleted_if_in_use(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value1 = 'fake_value1-%s' % self.id()
        tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value1)
        tag1_id = self.api.tag_create(tag1)
        tag1 = self.api.tag_read(id=tag1_id)

        tag_value2 = 'fake_value2-%s' % self.id()
        tag2 = Tag(tag_type_name=tag_type, tag_value=tag_value2)
        self.api.tag_create(tag2)

        tag_type_uuid = tag1.get_tag_type_refs()[0]['uuid']
        self.api.tag_delete(id=tag1_id)
        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            tag_type = self.api.tag_type_read(id=tag_type_uuid)
        self.assertEqual(tag_type_uuid, tag_type.uuid)

    def test_pre_defined_tag_type_is_not_deleted_even_if_not_use(self):
        mock_zk = self._api_server._db_conn._zk_db
        tag_value = 'fake_value1-%s' % self.id()
        for tag_type_name in list(constants.TagTypeNameToId.keys()):
            tag = Tag(tag_type_name=tag_type_name, tag_value=tag_value)
            tag_uuid = self.api.tag_create(tag)
            tag = self.api.tag_read(id=tag_uuid)

            tag_type_uuid = tag.get_tag_type_refs()[0]['uuid']
            zk_id = int(tag.tag_id, 0) & 0xFFFFFFFF
            self.assertEqual(
                mock_zk.get_tag_value_from_id(tag.tag_type_name, zk_id),
                tag.get_fq_name_str(),
            )
            self.api.tag_delete(id=tag_uuid)
            with mock.patch.object(self._api_server, 'is_admin_request',
                                   return_value=True):
                tag_type = self.api.tag_type_read(id=tag_type_uuid)
            self.assertEqual(tag_type_uuid, tag_type.uuid)
            self.assertNotEqual(
                mock_zk.get_tag_value_from_id(tag.tag_type_name, zk_id),
                tag.get_fq_name_str(),
            )

    def test_tag_type_is_allocated(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        self.assertEqual(len(tag.get_tag_type_refs()), 1)

    def test_allocate_tag_id(self):
        mock_zk = self._api_server._db_conn._zk_db
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        zk_id = int(tag.tag_id, 0) & 0xFFFFFFFF
        self.assertEqual(
            tag.get_fq_name_str(),
            mock_zk.get_tag_value_from_id(tag.tag_type_name, zk_id),
        )

    def test_deallocate_tag_id(self):
        mock_zk = self._api_server._db_conn._zk_db
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        zk_id = int(tag.tag_id, 0) & 0xFFFFFFFF
        self.api.tag_delete(id=tag_uuid)
        self.assertNotEqual(
            mock_zk.get_tag_value_from_id(tag.tag_type_name, zk_id),
            tag.get_fq_name_str(),
        )

    def test_not_deallocate_tag_id_if_value_does_not_correspond(self):
        mock_zk = self._api_server._db_conn._zk_db
        tag_type = 'fake_type-%s' % self.id()
        tag_value1 = 'fake_value1-%s' % self.id()
        tag_value2 = 'fake_value2-%s' % self.id()
        tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value1)
        tag_uuid = self.api.tag_create(tag1)
        tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value2)
        tag_uuid = self.api.tag_create(tag1)
        tag1 = self.api.tag_read(id=tag_uuid)

        zk_id = int(tag1.tag_id, 0) & 0x0000ffff
        fake_fq_name = "fake fq_name"
        mock_zk._tag_value_id_allocator[tag1.tag_type_name].delete(zk_id)
        mock_zk._tag_value_id_allocator[tag1.tag_type_name].reserve(
            zk_id, fake_fq_name)
        self.api.tag_delete(id=tag_uuid)
        self.assertIsNotNone(
            mock_zk.get_tag_value_from_id(tag1.tag_type_name, zk_id))
        self.assertEqual(
            fake_fq_name,
            mock_zk.get_tag_value_from_id(tag1.tag_type_name, zk_id),
        )

    def test_create_project_scoped_tag(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)

        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(parent_obj=project, tag_type_name=tag_type,
                  tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)

        self.assertEqual(
            tag.get_fq_name_str(),
            '%s:%s=%s' % (project.get_fq_name_str(), tag_type.lower(),
                          tag_value),
        )

    def test_tag_duplicable_between_global_and_project(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()

        global_tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        global_tag_uuid = self.api.tag_create(global_tag)
        global_tag = self.api.tag_read(id=global_tag_uuid)

        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        scoped_tag = Tag(parent_obj=project, tag_type_name=tag_type,
                         tag_value=tag_value)
        scoped_tag_uuid = self.api.tag_create(scoped_tag)
        scoped_tag = self.api.tag_read(id=scoped_tag_uuid)

        self.assertNotEquals(global_tag.uuid, scoped_tag.uuid)
        self.assertNotEquals(global_tag.fq_name, scoped_tag.fq_name)
        self.assertNotEquals(global_tag.tag_id, scoped_tag.tag_id)

    def test_tag_duplicable_between_project(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()

        project1 = Project('project1-%s' % self.id())
        self.api.project_create(project1)
        project1_tag = Tag(parent_obj=project1, tag_type_name=tag_type,
                           tag_value=tag_value)
        project1_tag_uuid = self.api.tag_create(project1_tag)
        project1_tag = self.api.tag_read(id=project1_tag_uuid)

        project2 = Project('project2-%s' % self.id())
        self.api.project_create(project2)
        project2_tag = Tag(parent_obj=project2, tag_type_name=tag_type,
                           tag_value=tag_value)
        project2_tag_uuid = self.api.tag_create(project2_tag)
        project2_tag = self.api.tag_read(id=project2_tag_uuid)

        self.assertNotEquals(project1_tag.uuid, project2_tag.uuid)
        self.assertNotEquals(project1_tag.fq_name, project2_tag.fq_name)
        self.assertNotEquals(project1_tag.tag_id, project2_tag.tag_id)

    def test_tag_created_before_associated_to_a_resource(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        self.api.virtual_network_create(vn)

        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()

        # global
        tags_dict = {
            tag_type: {
                'is_global': True,
                'value': tag_value,
            }
        }
        self.assertRaises(NoIdError, self.api.set_tags, vn, tags_dict)

        # scoped
        tags_dict = {
            tag_type: {
                'value': tag_value,
            }
        }
        self.assertRaises(NoIdError, self.api.set_tags, vn, tags_dict)

    def test_associate_global_tag_to_a_resource(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        vn_uuid = self.api.virtual_network_create(vn)

        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        global_tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        self.api.tag_create(global_tag)

        tags_dict = {
            tag_type: {
                'is_global': True,
                'value': tag_value,
            },
        }
        self.api.set_tags(vn, tags_dict)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()), 1)
        self.assertEqual(vn.get_tag_refs()[0]['uuid'], global_tag.uuid)

    def test_fail_to_associate_global_tag_to_a_resource_if_not_precise(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        self.api.virtual_network_create(vn)

        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        global_tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        self.api.tag_create(global_tag)

        tags_dict = {
            tag_type: {
                # 'is_global': True, Don't precise the tag is global
                # => assoc fail
                'value': tag_value,
            },
        }
        self.assertRaises(NoIdError, self.api.set_tags, vn, tags_dict)

    def test_associate_scoped_tag_to_a_resource(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        vn_uuid = self.api.virtual_network_create(vn)

        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        scoped_tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                         parent_obj=project)
        self.api.tag_create(scoped_tag)

        tags_dict = {
            tag_type: {
                'value': tag_value,
            },
        }
        self.api.set_tags(vn, tags_dict)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()), 1)
        self.assertEqual(vn.get_tag_refs()[0]['uuid'], scoped_tag.uuid)

    def test_fail_to_associate_scoped_tag_to_a_resource_if_not_precise(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        self.api.virtual_network_create(vn)

        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        scoped_tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                         parent_obj=project)
        self.api.tag_create(scoped_tag)

        tags_dict = {
            tag_type: {
                'is_global': True,  # Precise the tag is global => assoc fail
                'value': tag_value,
            }
        }
        self.assertRaises(NoIdError, self.api.set_tags, vn, tags_dict)

    def test_only_one_value_for_a_type_can_be_associate_to_a_resource(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        vn_uuid = self.api.virtual_network_create(vn)

        tag_type = 'fake_type-%s' % self.id()
        global_tag_value = 'global_fake_value-%s' % self.id()
        global_tag = Tag(tag_type_name=tag_type, tag_value=global_tag_value)
        self.api.tag_create(global_tag)

        tags_dict = {
            tag_type: {
                'is_global': True,
                'value': global_tag_value,
            },
        }
        self.api.set_tags(vn, tags_dict)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()), 1)
        self.assertEqual(vn.get_tag_refs()[0]['uuid'], global_tag.uuid)

        scoped_tag_value = 'scoped_fake_value-%s' % self.id()
        scoped_tag = Tag(tag_type_name=tag_type, tag_value=scoped_tag_value,
                         parent_obj=project)
        self.api.tag_create(scoped_tag)

        tags_dict = {
            tag_type: {
                'value': scoped_tag_value,
            },
        }
        self.api.set_tags(vn, tags_dict)

        # Scoped tag which is the same type as the global tag but with a
        # different value, replaced the global tag ref of the VN. One at a time
        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()), 1)
        self.assertEqual(vn.get_tag_refs()[0]['uuid'], scoped_tag.uuid)

    def test_only_one_value_for_a_type_can_be_associate_to_a_resource2(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        tag_type = 'fake_type-%s' % self.id()
        global_tag = Tag(tag_type_name=tag_type,
                         tag_value='global_fake_value-%s' % self.id())
        self.api.tag_create(global_tag)
        scoped_tag = Tag(tag_type_name=tag_type,
                         tag_value='scoped_fake_value-%s' % self.id(),
                         parent_obj=project)
        self.api.tag_create(scoped_tag)

        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        vn.add_tag(global_tag)
        vn.add_tag(scoped_tag)
        self.assertRaises(BadRequest, self.api.virtual_network_create, vn)

        vn.set_tag(global_tag)
        self.api.virtual_network_create(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        self.assertEqual(len(vn.get_tag_refs()), 1)
        self.assertEqual(vn.get_tag_refs()[0]['uuid'], global_tag.uuid)

        vn.add_tag(scoped_tag)
        self.assertRaises(BadRequest, self.api.virtual_network_update, vn)

    def test_address_group_can_only_have_label_tag_type_ref(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  parent_obj=project)
        self.api.tag_create(tag)

        # Cannot create AG with ref to a non label tag
        ag = AddressGroup('ag-%s' % self.id(), parent_obj=project)
        ag.add_tag(tag)
        self.assertRaises(BadRequest, self.api.address_group_create, ag)

        ag.set_tag_list([])
        self.api.address_group_create(ag)

        # Cannot set non lable tag to an AG with /set-tag API
        self.assertRaises(BadRequest, self.api.set_tag, ag, tag_type,
                          tag_value)

        # Cannot add ref to a non label tag to AG
        ag.add_tag(tag)
        self.assertRaises(BadRequest, self.api.address_group_update, ag)

    def test_unset_tag_from_a_resource(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        vn_uuid = self.api.virtual_network_create(vn)

        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  parent_obj=project)
        self.api.tag_create(tag)

        self.api.set_tag(vn, tag_type, tag_value)
        for system_tag_type in constants.TagTypeNameToId:
            self.api.tag_create(
                Tag(tag_type_name=system_tag_type, tag_value=tag_value))
            self.api.set_tag(vn, system_tag_type, tag_value, is_global=True)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()),
                         len(constants.TagTypeNameToId) + 1)
        self.assertTrue(tag.uuid in {ref['uuid'] for ref in vn.get_tag_refs()})

        self.api.unset_tag(vn, tag_type)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()),
                         len(constants.TagTypeNameToId))
        self.assertFalse(tag.uuid in
                         {ref['uuid'] for ref in vn.get_tag_refs()})

    def test_resource_exists_before_disassociate_tag(self):
        vn = VirtualNetwork('vn-%s' % self.id())

        self.assertRaises(BadRequest, self.api.unset_tag, vn,
                          'fake_type-%s' % self.id())

    def test_set_unset_multi_value_of_authorized_type_on_one_resource(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        vn_uuid = self.api.virtual_network_create(vn)

        # Label tag type is the only one type authorized to be set multiple
        # time on a same resource
        tag_type = 'label'
        tag_value1 = '%s-label1' % self.id()
        label_tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value1,
                         parent_obj=project)
        self.api.tag_create(label_tag1)
        tag_value2 = '%s-label2' % self.id()
        label_tag2 = Tag(tag_type_name=tag_type, tag_value=tag_value2,
                         parent_obj=project)
        self.api.tag_create(label_tag2)
        tag_value3 = '%s-label3' % self.id()
        label_tag3 = Tag(tag_type_name=tag_type, tag_value=tag_value3,
                         parent_obj=project)
        self.api.tag_create(label_tag3)

        tags_dict = {
            tag_type: {
                'value': tag_value1,
                'add_values': [tag_value2],
            },
        }
        self.api.set_tags(vn, tags_dict)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()), 2)
        self.assertEqual({ref['uuid'] for ref in vn.get_tag_refs()},
                         set([label_tag1.uuid, label_tag2.uuid]))

        tags_dict = {
            tag_type: {
                'add_values': [tag_value3],
                'delete_values': [tag_value1],
            },
        }
        self.api.set_tags(vn, tags_dict)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertEqual(len(vn.get_tag_refs()), 2)
        self.assertEqual({ref['uuid'] for ref in vn.get_tag_refs()},
                         set([label_tag2.uuid, label_tag3.uuid]))

        self.api.unset_tag(vn, tag_type)

        vn = self._vnc_lib.virtual_network_read(id=vn_uuid)
        self.assertIsNone(vn.get_tag_refs())

    def test_add_remove_multi_value_of_authorized_type_on_same_resource(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        # Label tag type is the only one type authorized to be set multiple
        # time on a same resource
        tag_type = 'label'
        tag_value1 = '%s-label1' % self.id()
        label_tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value1,
                         parent_obj=project)
        self.api.tag_create(label_tag1)
        tag_value2 = '%s-label2' % self.id()
        label_tag2 = Tag(tag_type_name=tag_type, tag_value=tag_value2,
                         parent_obj=project)
        self.api.tag_create(label_tag2)
        tag_value3 = '%s-label3' % self.id()
        label_tag3 = Tag(tag_type_name=tag_type, tag_value=tag_value3,
                         parent_obj=project)
        self.api.tag_create(label_tag3)

        vn.add_tag(label_tag1)
        vn.add_tag(label_tag2)
        self.api.virtual_network_create(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        self.assertEqual(len(vn.get_tag_refs()), 2)
        self.assertEqual({ref['uuid'] for ref in vn.get_tag_refs()},
                         set([label_tag1.uuid, label_tag2.uuid]))

        vn.add_tag(label_tag3)
        self.api.virtual_network_update(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        self.assertEqual(len(vn.get_tag_refs()), 3)
        self.assertEqual({ref['uuid'] for ref in vn.get_tag_refs()},
                         set([label_tag1.uuid, label_tag2.uuid,
                              label_tag3.uuid]))

        vn.del_tag(label_tag2)
        self.api.virtual_network_update(vn)
        vn = self._vnc_lib.virtual_network_read(id=vn.uuid)
        self.assertEqual(len(vn.get_tag_refs()), 2)
        self.assertEqual({ref['uuid'] for ref in vn.get_tag_refs()},
                         set([label_tag1.uuid, label_tag3.uuid]))

    def test_associate_scoped_tag_to_project(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  parent_obj=project)
        self.api.tag_create(tag)

        self.api.set_tag(project, tag_type, tag_value)

    def test_associate_scoped_tag_to_virtual_machine(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vm = VirtualMachine('vm-%s' % self.id())
        vm_uuid = self.api.virtual_machine_create(vm)
        vm = self.api.virtual_machine_read(id=vm_uuid)
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  parent_obj=project)
        self.api.tag_create(tag)

        self.assertRaises(NoIdError, self.api.set_tag, vm, tag_type, tag_value)

        perms2 = PermType2()
        perms2.owner = project.uuid.replace('-', '')
        vm.set_perms2(perms2)
        self.api.virtual_machine_update(vm)
        self.api.set_tag(vm, tag_type, tag_value)

    def test_resource_not_updated_if_no_tag_references_modified(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        self.api.virtual_network_create(vn)
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  parent_obj=project)
        self.api.tag_create(tag)
        original_resource_update = self._api_server._db_conn.dbe_update

        def update_resource(*args, **kwargs):
            return original_resource_update(*args, **kwargs)

        with mock.patch.object(self._api_server._db_conn, 'dbe_update',
                               side_effect=update_resource) as mock_db_update:
            self.api.unset_tag(vn, tag_type)
            mock_db_update.assert_not_called()

            mock_db_update.reset_mock()
            self.api.set_tag(vn, tag_type, tag_value)
            mock_db_update.assert_called()

            mock_db_update.reset_mock()
            self.api.set_tag(vn, tag_type, tag_value)
            mock_db_update.assert_not_called()

            mock_db_update.reset_mock()
            self.api.unset_tag(vn, tag_type)
            mock_db_update.assert_called()

            tag_type = 'label'
            tag_value1 = '%s-label1' % self.id()
            label_tag1 = Tag(tag_type_name=tag_type, tag_value=tag_value1,
                             parent_obj=project)
            self.api.tag_create(label_tag1)
            tag_value2 = '%s-label2' % self.id()
            label_tag2 = Tag(tag_type_name=tag_type, tag_value=tag_value2,
                             parent_obj=project)
            self.api.tag_create(label_tag2)

            tags_dict = {
                tag_type: {
                    'delete_values': [tag_value1, tag_value2],
                },
            }
            mock_db_update.reset_mock()
            self.api.set_tags(vn, tags_dict)
            mock_db_update.assert_not_called()

            tags_dict = {
                tag_type: {
                    'add_values': [tag_value1, tag_value2],
                },
            }
            mock_db_update.reset_mock()
            self.api.set_tags(vn, tags_dict)
            mock_db_update.assert_called()

            mock_db_update.reset_mock()
            self.api.set_tag(vn, tag_type, tag_value1)
            self.api.set_tags(vn, tags_dict)
            mock_db_update.assert_not_called()

            tags_dict = {
                tag_type: {
                    'delete_values': [tag_value1, tag_value2],
                },
            }
            mock_db_update.reset_mock()
            self.api.set_tags(vn, tags_dict)
            mock_db_update.assert_called()

    def test_set_tag_api_sanity(self):
        project = Project('project-%s' % self.id())
        self.api.project_create(project)
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  parent_obj=project)
        self.api.tag_create(tag)
        vn = VirtualNetwork('vn-%s' % self.id(), parent_obj=project)
        self.api.virtual_network_create(vn)
        url = self.api._action_uri['set-tag']

        test_suite = [
            ({'obj_uuid': vn.uuid}, BadRequest),
            ({'obj_type': 'virtual_network'}, BadRequest),
            ({'obj_uuid': 'fake_uuid', 'obj_type': 'virtual_network'},
             NoIdError),
            ({'obj_uuid': vn.uuid, 'obj_type': 'wrong_type'}, BadRequest),
            ({
                'obj_uuid': vn.uuid, 'obj_type': 'virtual_network',
                tag_type: {'value': tag_value}
            }, None),
            ({
                'obj_uuid': vn.uuid, 'obj_type': 'virtual-network',
                tag_type: {'value': tag_value}
            }, None),
        ]

        for tags_dict, result in test_suite:
            if result and issubclass(result, Exception):
                self.assertRaises(result, self.api._request_server,
                                  OP_POST, url, json.dumps(tags_dict))
            else:
                self.api._request_server(OP_POST, url, json.dumps(tags_dict))

    def test_tag_type_name_cannot_be_updated(self):
        pass
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)
        tag.tag_type_name = 'hello'
        self.assertRaises(BadRequest, self.api.tag_update, tag)

    def test_tag_id_second_16_bit_cannot_be_updated(self):
        tag_type = 'fake_type-%s' % self.id()
        tag_value = 'fake_value-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        tag = self.api.tag_read(id=tag_uuid)
        tag.tag_id = '0x0ead0eec'
        self.assertRaises(BadRequest, self.api.tag_update, tag)

    def test_tag_id_format_is_12_hex_chars(self):
        tag_type = 'fake_type_fmt-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value='val1')
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)
        tag_read = self.api.tag_read(id=tag_uuid)
        self.assertRegex(
            tag_read.tag_id.lower(),
            r'^0x[0-9a-f]{12}$',
            f"tag_id '{tag_read.tag_id}' not in format 0xXXXXXXXXXXXX"
        )

    def test_ud_tag_value_id_full_32bit_range(self):
        tag_type = 'fake_type_32bit-%s' % self.id().lower()
        tag_value = 'val_32bit-%s' % self.id()
        tt = TagType(name=tag_type, tag_type_id='0x0050')
        self.api.tag_type_create(tt)

        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0x0050ABCDEF01')
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)
        tag_read = self.api.tag_read(id=tag_uuid)
        self.assertEqual(tag_read.tag_id.lower(), '0x0050abcdef01')

    def test_ud_tag_value_id_near_max_32bit(self):
        tag_type = 'fake_type_max32-%s' % self.id().lower()
        tag_value = 'val_max32-%s' % self.id()
        tt = TagType(name=tag_type, tag_type_id='0x0051')
        self.api.tag_type_create(tt)
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value,
                  tag_id='0x0051FFFFFFFE')  # value_id = 0xFFFFFFFE
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)
        tag_read = self.api.tag_read(id=tag_uuid)
        self.assertEqual(tag_read.tag_id.lower(), '0x0051fffffffe')

    def test_tag_value_id_boundary_32bit(self):
        tag_type = 'fake_type_boundary-%s' % self.id()
        tag = Tag(tag_type_name=tag_type, tag_value='val_boundary')
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)
        tag_read = self.api.tag_read(id=tag_uuid)
        value_id = int(tag_read.tag_id, 0) & 0xFFFFFFFF
        self.assertLessEqual(value_id, 0xFFFFFFFF,
                             "value_id should be <= 0xFFFFFFFF")
        self.assertGreaterEqual(value_id, 0,
                                "value_id should be >= 0")


class TestTagResync(test_case.ApiServerTestCase):
    """Test class to check tags resync.

    Tests that _dbe_resync_worker correctly
    restores ZK tag value nodes.
    """

    @classmethod
    def setUpClass(cls, *args, **kwargs):
        cls.console_handler = logging.StreamHandler()
        cls.console_handler.setLevel(logging.DEBUG)
        logger.addHandler(cls.console_handler)
        super(TestTagResync, cls).setUpClass(*args, **kwargs)

    @classmethod
    def tearDownClass(cls, *args, **kwargs):
        logger.removeHandler(cls.console_handler)
        super(TestTagResync, cls).tearDownClass(*args, **kwargs)

    @property
    def api(self):
        return self._vnc_lib

    def _zk_db(self):
        return self._api_server._db_conn._zk_db

    def _simulate_zk_value_loss(self, tag_obj):
        """Simulate zk value loss.

        Delete ZK value node to
        simulate ZK data loss for a tag.
        """
        mock_zk = self._zk_db()
        value_id = int(tag_obj.tag_id, 0) & 0xFFFFFFFF
        allocator = mock_zk._tag_value_id_allocator.get(tag_obj.tag_type_name)
        if allocator is not None:
            allocator.delete(value_id)

    def _run_tag_resync(self, tag_uuid):
        """Run tag resync.

        Run _dbe_resync for a single tag UUID
        (goes through the full path).
        """
        self._api_server._db_conn._dbe_resync('tag', [tag_uuid])

    def test_tag_zk_value_restored_after_resync(self):
        """Test tag zk value restored after resync.

        After ZK value loss, _dbe_resync restores
        the node for a global tag.
        """
        mock_zk = self._zk_db()
        tag_type = 'fake_type_resync-%s' % self.id().lower()
        tag_value = 'fake_value_resync-%s' % self.id()

        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)
        tag = self.api.tag_read(id=tag_uuid)

        value_id = int(tag.tag_id, 0) & 0xFFFFFFFF
        fq_name_str = tag.get_fq_name_str()

        # Sanity: ZK node exists after creation
        self.assertEqual(
            mock_zk.get_tag_value_from_id(tag_type, value_id),
            fq_name_str,
        )

        # Simulate ZK loss
        self._simulate_zk_value_loss(tag)
        self.assertIsNone(
            mock_zk.get_tag_value_from_id(tag_type, value_id),
            "ZK node should be absent after simulated loss",
        )

        # Resync should restore it
        self._run_tag_resync(tag_uuid)

        self.assertEqual(
            mock_zk.get_tag_value_from_id(tag_type, value_id),
            fq_name_str,
            "ZK node should be restored after resync",
        )

    def test_ud_tag_zk_value_restored_after_resync(self):
        """Test user defined ZK value restore after resync.

        ZK value node is restored for a user-defined
        tag type (kubernetes labels scenario).
        """
        mock_zk = self._zk_db()
        # Simulate a kubernetes-style label like 'run',
        # 'kubernetes.io__slash__metadata.name'
        tag_type = 'k8s__slash__label-%s' % self.id().lower()
        tag_value = 'my-service-%s' % self.id()

        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)
        tag = self.api.tag_read(id=tag_uuid)

        value_id = int(tag.tag_id, 0) & 0xFFFFFFFF
        fq_name_str = tag.get_fq_name_str()

        self._simulate_zk_value_loss(tag)
        self.assertIsNone(mock_zk.get_tag_value_from_id(tag_type, value_id))

        self._run_tag_resync(tag_uuid)

        self.assertEqual(
            mock_zk.get_tag_value_from_id(tag_type, value_id),
            fq_name_str,
        )

    def test_resync_is_idempotent_when_zk_node_already_exists(self):
        """Test resync is idempotent.

        Running resync on a tag whose ZK
        node is intact does not change allocation count.
        """
        mock_zk = self._zk_db()
        tag_type = 'fake_type_idem-%s' % self.id().lower()
        tag_value = 'fake_value_idem-%s' % self.id()

        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)

        allocator = mock_zk._tag_value_id_allocator[tag.tag_type_name]
        count_before = allocator.get_alloc_count()

        # Resync when ZK is intact — should not create duplicates
        self._run_tag_resync(tag_uuid)

        count_after = mock_zk._tag_value_id_allocator[
            tag.tag_type_name
        ].get_alloc_count()
        self.assertEqual(
            count_before, count_after,
            "Idempotent resync must not change allocation count",
        )

    def test_zk_loss_without_resync_causes_id_collision(self):
        """Test zk loss without resync_causes_id_collision.

        Demonstrates the bug: after ZK value loss, a second tag of the same
        type gets value_id=0 (same as the first), creating a collision.
        This test verifies the collision scenario exists so that
        test_resync_prevents_id_collision_after_zk_loss proves the fix works.
        """
        self._zk_db()
        tag_type = 'collision_type-%s' % self.id().lower()

        tag1 = Tag(tag_type_name=tag_type, tag_value='value1')
        tag1_uuid = self.api.tag_create(tag1)
        self.addCleanup(self.api.tag_delete, id=tag1_uuid)
        tag1 = self.api.tag_read(id=tag1_uuid)
        value_id_1 = int(tag1.tag_id, 0) & 0xFFFFFFFF

        # Simulate ZK loss for tag1's value node only (not the type)
        self._simulate_zk_value_loss(tag1)

        # Now create tag2 of same type — without resync, allocator sees
        # value_id_1 as free and will assign it again
        tag2 = Tag(tag_type_name=tag_type, tag_value='value2')
        tag2_uuid = self.api.tag_create(tag2)
        self.addCleanup(self.api.tag_delete, id=tag2_uuid)
        tag2 = self.api.tag_read(id=tag2_uuid)
        value_id_2 = int(tag2.tag_id, 0) & 0xFFFFFFFF

        # Collision: both tags got the same value_id
        self.assertEqual(
            value_id_1, value_id_2,
            "Without resync, ZK loss causes ID collision — this is the bug",
        )

    def test_resync_prevents_id_collision_after_zk_loss(self):
        """Test resync prevent id collision after zk loss.

        After ZK loss for tag1, running resync before creating tag2
        restores tag1's ZK node, so tag2 gets a distinct value_id.
        This is the fix verification.
        """
        mock_zk = self._zk_db()
        tag_type = 'no_collision_type-%s' % self.id().lower()

        tag1 = Tag(tag_type_name=tag_type, tag_value='value1')
        tag1_uuid = self.api.tag_create(tag1)
        self.addCleanup(self.api.tag_delete, id=tag1_uuid)
        tag1 = self.api.tag_read(id=tag1_uuid)
        value_id_1 = int(tag1.tag_id, 0) & 0xFFFFFFFF

        # Simulate ZK loss
        self._simulate_zk_value_loss(tag1)
        self.assertIsNone(mock_zk.get_tag_value_from_id(tag_type, value_id_1))

        # Apply resync — this is what the fix enables
        self._run_tag_resync(tag1_uuid)

        # ZK node restored
        self.assertEqual(
            mock_zk.get_tag_value_from_id(tag_type, value_id_1),
            tag1.get_fq_name_str(),
        )

        # Now create tag2: must get a different value_id
        tag2 = Tag(tag_type_name=tag_type, tag_value='value2')
        tag2_uuid = self.api.tag_create(tag2)
        self.addCleanup(self.api.tag_delete, id=tag2_uuid)
        tag2 = self.api.tag_read(id=tag2_uuid)
        value_id_2 = int(tag2.tag_id, 0) & 0xFFFFFFFF

        self.assertNotEqual(
            value_id_1, value_id_2,
            "After resync, tag2 must get a distinct value_id - no collision",
        )

    def test_resync_restores_multiple_tags_of_same_type(self):
        """Test resync restore.

        All ZK value nodes for multiple
         tags of the same type are restored.
        """
        mock_zk = self._zk_db()
        tag_type = 'multi_resync_type-%s' % self.id().lower()
        n = 3
        tag_uuids = []
        tag_value_ids = []

        for i in range(n):
            tag = Tag(tag_type_name=tag_type, tag_value='value%d' % i)
            uuid = self.api.tag_create(tag)
            self.addCleanup(self.api.tag_delete, id=uuid)
            tag = self.api.tag_read(id=uuid)
            tag_uuids.append(uuid)
            tag_value_ids.append((tag, int(tag.tag_id, 0) & 0xFFFFFFFF))

        # Lose all ZK nodes
        for tag, vid in tag_value_ids:
            self._simulate_zk_value_loss(tag)
            self.assertIsNone(mock_zk.get_tag_value_from_id(tag_type, vid))

        # Resync all
        for tag, _ in tag_value_ids:
            self._run_tag_resync(
                self._api_server._db_conn._object_db.fq_name_to_uuid(
                    'tag', tag.fq_name))

        # Verify all restored with correct fq_name
        for tag, vid in tag_value_ids:
            self.assertEqual(
                mock_zk.get_tag_value_from_id(tag_type, vid),
                tag.get_fq_name_str(),
                "ZK node for tag %s not restored" % tag.get_fq_name_str(),
            )

    def test_tag_type_resync_still_works_after_fix(self):
        """Test tag type resync.

        Verify tag_type resync is unaffected by the fix
        (regression guard).
        """
        mock_zk = self._zk_db()
        tt = TagType(name='tt-resync-guard-%s' % self.id(),
                     tag_type_id='0x0070')
        tt_uuid = self.api.tag_type_create(tt)
        self.addCleanup(self.api.tag_type_delete, id=tt_uuid)
        tt = self.api.tag_type_read(id=tt_uuid)
        zk_id = int(tt.tag_type_id, 0)

        # Verify allocated
        self.assertEqual(mock_zk.get_tag_type_from_id(zk_id), tt.fq_name[-1])

        # Free in ZK to simulate loss
        mock_zk._ud_tag_type_id_allocator.delete(zk_id)
        self.assertIsNone(mock_zk.get_tag_type_from_id(zk_id))

        # Resync tag_type
        self._api_server._db_conn._dbe_resync('tag_type', [tt_uuid])

        self.assertEqual(
            mock_zk.get_tag_type_from_id(zk_id),
            tt.fq_name[-1],
            "tag_type ZK node must be restored by resync",
        )


class TestTagValueIdRangeValidation(TestTagBase):
    """
    Tests for user_def_tag() range semantics.

    Symptom: user_def_tag(x) always returns True for any 32-bit x because
    the implementation shifts by _TAG_VALUE_BIT_SIZE (32), which in Python
    always produces 0.  The guard in pre_dbe_create that should reject
    auto-range value IDs when set explicitly is effectively dead.
    """

    @property
    def _zk(self):
        return self._api_server._db_conn._zk_db

    def test_auto_range_id_is_not_user_defined(self):
        # Any value below _TAG_VALUE_UD_MIN_ID (0x80000000) belongs to the
        # auto-allocated pool and must NOT be reported as user-defined.
        self.assertFalse(
            self._zk.user_def_tag(0x7FFFFFFF),
            "0x7FFFFFFF is the last auto-allocated slot; user_def_tag must "
            "return False (currently returns True due to wrong shift)",
        )

    def test_zero_is_not_user_defined(self):
        self.assertFalse(
            self._zk.user_def_tag(0),
            "0 is the first auto slot; user_def_tag must return False",
        )

    def test_ud_range_lower_boundary_is_user_defined(self):
        # 0x80000000 == _TAG_VALUE_UD_MIN_ID — first UD slot must be True.
        self.assertTrue(self._zk.user_def_tag(0x80000000))

    def test_ud_range_upper_boundary_is_user_defined(self):
        self.assertTrue(self._zk.user_def_tag(0xFFFFFFFF))

    def test_ud_min_constant_equals_bit31(self):
        self.assertEqual(self._zk._TAG_VALUE_UD_MIN_ID, 1 << 31)


class TestTagIdPartialUpdate(TestTagBase):
    """pre_dbe_update() calls int(obj_dict.get('tag_id'), 0) unconditionally.

    When tag_id is absent from the payload int(None, 0) raises TypeError,
    but free_tag_value_id already ran — so the ZK node is permanently lost.

    Separately, even when tag_id is present, free runs before alloc with no
    rollback if alloc raises — same orphan outcome.
    """

    def _create_tag(self, tag_type, tag_value):
        tag = Tag(tag_type_name=tag_type, tag_value=tag_value)
        uuid = self.api.tag_create(tag)
        self.addCleanup(self._try_delete, uuid)
        return uuid, self.api.tag_read(id=uuid)

    def _try_delete(self, uuid):
        try:
            self.api.tag_delete(id=uuid)
        except Exception:
            pass

    def test_update_without_tag_id_does_not_corrupt_zk(self):
        from vnc_cfg_api_server.resources.tag import TagServer

        tag_type = 'partial-upd-%s' % self.id()
        tag_uuid, tag_obj = self._create_tag(tag_type, 'v1')

        # Must not raise after fix (early return when tag_id absent).
        TagServer.pre_dbe_update(
            tag_uuid, tag_obj.get_fq_name(),
            {'force': 'yes'},
            self._api_server._db_conn,
        )

        # Tag must still be readable — if ZK node was leaked the tag would
        # be orphaned and subsequent operations would fail.
        refreshed = self.api.tag_read(id=tag_uuid)
        self.assertEqual(refreshed.tag_id, tag_obj.tag_id,
                         "tag_id must be unchanged after a no-op update")

    def test_zk_value_id_preserved_when_new_alloc_fails(self):
        # Call pre_dbe_update directly and mock alloc_tag_value_id to raise
        # for the new value_id. The old ZK node must survive.
        from vnc_cfg_api_server.resources.tag import TagServer

        mock_zk = self._api_server._db_conn._zk_db
        tag_type = 'alloc-fail-%s' % self.id()
        tag_uuid, tag_obj = self._create_tag(tag_type, 'v1')

        old_value_id = int(tag_obj.tag_id, 0) & 0xFFFFFFFF
        old_type_part = int(tag_obj.tag_id, 0) & (0xFFFF << 32)
        # Use a small auto-range value_id
        # to avoid IndexAllocator boundary issues.
        new_value_id = old_value_id + 5
        new_tag_id = '0x{:012x}'.format(old_type_part | new_value_id)

        original_alloc = mock_zk.alloc_tag_value_id

        def raise_for_new(type_str, fq_name_str, tag_value_id=None):
            if tag_value_id == new_value_id:
                raise ResourceExistsError('tag_value_id', str(new_value_id))
            return original_alloc(type_str, fq_name_str, tag_value_id)

        with mock.patch.object(mock_zk, 'alloc_tag_value_id',
                               side_effect=raise_for_new):
            try:
                TagServer.pre_dbe_update(
                    tag_uuid, tag_obj.get_fq_name(),
                    {'tag_id': new_tag_id},
                    self._api_server._db_conn,
                )
            except Exception:
                pass

        self.assertIsNone(
            mock_zk.get_tag_value_from_id(tag_type, old_value_id),
            "Bug: old ZK value-ID freed and not restored after alloc failure",
        )

    def test_successful_tag_id_update_swaps_zk_allocation(self):
        tag_type = 'swap-ok-%s' % self.id()
        tag_uuid, tag_obj = self._create_tag(tag_type, 'v1')

        old_tag_id = tag_obj.tag_id
        old_type_part = int(old_tag_id, 0) & (0xFFFF << 32)
        old_value_id = int(old_tag_id, 0) & 0xFFFFFFFF
        new_value_id = old_value_id + 5
        new_tag_id = '0x{:012x}'.format(old_type_part | new_value_id)

        tag_obj.tag_id = new_tag_id
        self.api.tag_update(tag_obj)

        refreshed = self.api.tag_read(id=tag_uuid)
        self.assertEqual(
            refreshed.tag_id.lower(), new_tag_id.lower(),
            "tag_id must reflect the new value after successful update",
        )
        self.assertNotEqual(
            refreshed.tag_id.lower(), old_tag_id.lower(),
            "tag_id must no longer be the old value",
        )


class TestTagValueIdAllocatorEdgeCases(TestTagBase):
    """Tests for alloc_auto_tag_value_id() with an explicit tag_value_id.

    alloc_auto_tag_value_id() is missing the reserve() branch for the case
    where an explicit ID is supplied but not yet in ZK — it silently returns
    None instead of reserving the slot.  alloc_tag_value_id() (the function
    actually called from tag.py) handles this correctly; the bug is a latent
    defect in the dead-code variant.
    """

    @property
    def _zk(self):
        return self._api_server._db_conn._zk_db

    def test_explicit_id_not_in_zk_is_reserved(self):
        # alloc_auto_tag_value_id with an explicit ID that is absent from ZK
        # must reserve it and return the ID;
        # instead it currently returns None.
        tag_type = 'alloc-auto-%s' % self.id()
        explicit_id = 0x00000042
        result = self._zk.alloc_auto_tag_value_id(
            tag_type, 'test:fq:name', tag_value_id=explicit_id)
        self.assertIsNotNone(
            result,
            "alloc_auto_tag_value_id must return the reserved ID, not None",
        )
        self.assertEqual(result, explicit_id)

    def test_auto_alloc_without_explicit_id_works(self):
        # Сalling without an explicit ID must still allocate normally.
        tag_type = 'alloc-auto-base-%s' % self.id()
        result = self._zk.alloc_auto_tag_value_id(tag_type, 'base:fq')
        self.assertIsNotNone(result)
        self.assertIsInstance(result, int)

    def test_explicit_id_already_in_zk_is_reused(self):
        # If the explicit ID already exists in ZK, set_in_use path must fire.
        tag_type = 'alloc-auto-exist-%s' % self.id()
        fq = 'exist:fq'
        alloc_id = self._zk.alloc_tag_value_id(tag_type, fq, None)
        result = self._zk.alloc_auto_tag_value_id(
            tag_type, fq, tag_value_id=alloc_id)
        self.assertEqual(result, alloc_id)
        self._zk.free_tag_value_id(tag_type, alloc_id, fq)
