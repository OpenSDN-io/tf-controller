#
# Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
#
import logging

from cfgm_common.exceptions import BadRequest
from cfgm_common.exceptions import RefsExistError
from cfgm_common.exceptions import ResourceExistsError
import gevent
import mock
from sandesh_common.vns import constants
from vnc_api.vnc_api import Tag, TagType

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


class TestTagType(TestTagBase):
    def test_pre_definied_tag_type_initialized(self):
        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            tag_types = {tag_type.name for tag_type
                         in self.api.tag_types_list(detail=True)}
        self.assertTrue(
            set(constants.TagTypeNameToId.keys()).issubset(tag_types))

    def test_tag_type_is_unique(self):
        name = 'tag-type-%s' % self.id()
        tag_type = TagType(name=name, tag_type_id='0x0050')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        new_tag_type = TagType(name=name, tag_type_id='0x0051')
        self.assertRaises(RefsExistError, self.api.tag_type_create,
                          new_tag_type)

    def test_tag_type_id_cannot_be_set(self):
        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id='0x00F')
        self.assertRaises(BadRequest, self.api.tag_type_create, tag_type)

    def test_ud_tag_type_id_can_be_set(self):
        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id='0x0018')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        self.assertEqual(tag_type.tag_type_id, "0x0018")
        self.api.tag_type_delete(id=tag_type_uuid)

    def test_tag_type_id_updated(self):
        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id='0x0060')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        tag_type.tag_type_id = '0x0015'
        self.api.tag_type_update(tag_type)
        tag_type_update = self.api.tag_type_read(id=tag_type_uuid)
        self.assertEqual(tag_type.tag_type_id,
                         tag_type_update.tag_type_id.lower())

    def test_ud_tag_type_updated(self):
        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id="0x0020")
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        tag_type.tag_type_id = '0x00A0'
        self.api.tag_type_update(tag_type)
        tag_type_update = self.api.tag_type_read(id=tag_type_uuid)
        self.assertEqual(tag_type.tag_type_id.lower(),
                         tag_type_update.tag_type_id.lower())

    def test_ud_tag_type_id_cannot_be_assigned_to_multiple_tag_types(self):
        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id="0x0012")
        tag_type_uuid = self.api.tag_type_create(tag_type)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)

        tag_type = TagType(name="fake-tag-type%s" % self.id(),
                           tag_type_id="0x0012")
        self.assertRaises(BadRequest, self.api.tag_type_create, tag_type)
        self.api.tag_type_delete(id=tag_type_uuid)

    def test_tag_type_display_name_cannot_be_set(self):
        name = 'tag-type-%s' % self.id()
        tag_type = TagType(name=name, display_name='fake_name',
                           tag_type_id='0x0061')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        self.assertEqual(tag_type.display_name, name)

    def test_tag_type_display_name_cannot_be_updated(self):
        tag_type = TagType(name='tag-type-%s' % self.id(),
                           tag_type_id='0x0062')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        tag_type.display_name = 'new_name'
        self.assertRaises(BadRequest, self.api.tag_type_update, tag_type)

    def test_allocate_tag_type_id(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id='0x0063')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)
        self.assertEqual(type_str, mock_zk.get_tag_type_from_id(zk_id))

    def test_allocate_ud_tag_type_id(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id="0x0013",)
        tag_type_uuid = self.api.tag_type_create(tag_type)

        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)
        self.assertEqual(type_str, mock_zk.get_tag_type_from_id(zk_id))
        self.api.tag_type_delete(id=tag_type_uuid)

    def test_deallocate_tag_type_id(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id='0x0064')
        tag_type_uuid = self.api.tag_type_create(tag_type)

        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)
        self.api.tag_type_delete(id=tag_type_uuid)
        self.assertNotEqual(mock_zk.get_tag_type_from_id(zk_id),
                            tag_type.fq_name[-1])

    def test_deallocate_ud_tag_type_id(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id="0x0014",)
        tag_type_uuid = self.api.tag_type_create(tag_type)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)
        self.api.tag_type_delete(id=tag_type_uuid)
        self.assertNotEqual(mock_zk.get_tag_type_from_id(zk_id),
                            tag_type.fq_name[-1])

    def test_not_deallocate_tag_type_id_if_value_does_not_correspond(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id='0x0065')
        tag_type_uuid = self.api.tag_type_create(tag_type)

        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)
        fake_tag_type = "fake tag type"

        mock_zk._ud_tag_type_id_allocator.delete(zk_id)
        mock_zk._ud_tag_type_id_allocator.reserve(zk_id, fake_tag_type)
        self.addCleanup(mock_zk._ud_tag_type_id_allocator.delete, zk_id)

        self.api.tag_type_delete(id=tag_type_uuid)
        self.assertIsNotNone(mock_zk.get_tag_type_from_id(zk_id))
        self.assertEqual(fake_tag_type, mock_zk.get_tag_type_from_id(zk_id))

    def test_not_deallocate_ud_tag_type_id_if_value_does_not_correspond(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id="0x0015")
        tag_type_uuid = self.api.tag_type_create(tag_type)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)
        fake_tag_type = "fake tag type"
        mock_zk._ud_tag_type_id_allocator.delete(zk_id)
        mock_zk._ud_tag_type_id_allocator.reserve(zk_id, fake_tag_type)
        self.addCleanup(mock_zk._ud_tag_type_id_allocator.delete, zk_id)
        self.api.tag_type_delete(id=tag_type_uuid)
        self.assertIsNotNone(mock_zk.get_tag_type_from_id(zk_id))
        self.assertEqual(fake_tag_type, mock_zk.get_tag_type_from_id(zk_id))
        mock_zk._ud_tag_type_id_allocator.delete(zk_id)

    def test_tag_type_duplicate_race_condition(self):
        name = 'tag-type-%s' % self.id()
        tag_type1 = TagType(name=name, tag_type_id='0x0080')
        uuid1 = self.api.tag_type_create(tag_type1)
        self.addCleanup(self.api.tag_type_delete, id=uuid1)

        tag_type2 = TagType(name=name, tag_type_id='0x0081')
        self.assertRaises(RefsExistError, self.api.tag_type_create, tag_type2)

    def test_concurrent_tag_type_create_race_condition(self):
        mock_zk = self._api_server._db_conn._zk_db
        allocator = mock_zk._ud_tag_type_id_allocator
        initial_alloc_count = allocator.get_alloc_count()

        name = 'tag-type-race-%s' % self.id()

        def create_tag_task():
            tag_type = TagType(name=name, tag_type_id='0x0070')
            try:
                self.api.tag_type_create(tag_type)
            except Exception:
                pass

        threads = [gevent.spawn(create_tag_task) for _ in range(5)]
        gevent.joinall(threads)

        tag_types = self.api.tag_types_list()
        created_tags = [
            t for t in tag_types.get('tag-types', [])
            if t.get('fq_name') == [name]
        ]
        self.assertEqual(len(created_tags), 1,
                         "Duplicated tag types were created")
        if created_tags:
            self.addCleanup(self.api.tag_type_delete,
                            id=created_tags[0]['uuid'])

        final_alloc_count = allocator.get_alloc_count()
        self.assertEqual(final_alloc_count, initial_alloc_count + 1,
                         "ZK have extra allocations")

    def test_ud_tag_type_lower_boundary(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-boundary-%s' % self.id()

        tag_type = TagType(name=type_str, tag_type_id="0x0041")
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)

        self.assertEqual(
            type_str,
            mock_zk.get_tag_type_from_id(zk_id),
            "Failed to read UD tag type at lower boundary (0x0041)")

    def test_ud_tag_type_upper_boundary(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tag-type-upper-%s' % self.id()

        tag_type = TagType(name=type_str, tag_type_id="0xFFFF")
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)

        self.assertEqual(
            type_str,
            mock_zk.get_tag_type_from_id(zk_id),
            "Failed to read UD tag type at upper boundary (0xFFFF)")

    def test_boundary_between_system_and_ud(self):
        mock_zk = self._api_server._db_conn._zk_db

        system_id = 0x000F
        system_value = mock_zk.get_tag_type_from_id(system_id)

        ud_type_str = 'tag-type-boundary-%s' % self.id()
        tag_type = TagType(name=ud_type_str, tag_type_id="0x0010")
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)

        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        ud_id = int(tag_type.tag_type_id, 0)
        ud_value = mock_zk.get_tag_type_from_id(ud_id)

        self.assertNotEqual(system_value, ud_value)
        self.assertEqual(ud_type_str, ud_value)

    def test_ud_tag_type_no_index_shift(self):
        mock_zk = self._api_server._db_conn._zk_db

        type_str = 'tag-type-shift-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id="0x0016")
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)

        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)

        real_value = mock_zk._ud_tag_type_id_allocator.read(zk_id)
        api_value = mock_zk.get_tag_type_from_id(zk_id)

        self.assertEqual(
            real_value,
            api_value,
            "Allocator and API returned different values (index shift bug)")

    def test_ud_tag_type_reuse_after_delete(self):
        mock_zk = self._api_server._db_conn._zk_db

        type_str = 'tag-type-reuse-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id="0x0022")
        tag_type_uuid = self.api.tag_type_create(tag_type)

        tag_type = self.api.tag_type_read(id=tag_type_uuid)
        zk_id = int(tag_type.tag_type_id, 0)

        self.api.tag_type_delete(id=tag_type_uuid)

        new_type_str = type_str + "-new"
        tag_type = TagType(name=new_type_str, tag_type_id="0x0022")
        new_tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=new_tag_type_uuid)

        value = mock_zk.get_tag_type_from_id(zk_id)

        self.assertEqual(new_type_str, value)

    def test_tag_type_update_to_zero_id_fails(self):
        name = 'tag-type-no-zero-%s' % self.id()
        tag_type = TagType(name=name, tag_type_id='0x0025')
        tag_type_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tag_type_uuid)
        tag_type = self.api.tag_type_read(id=tag_type_uuid)

        tag_type.tag_type_id = '0x0000'

        self.assertRaises(BadRequest, self.api.tag_type_update, tag_type)

        tag_type_check = self.api.tag_type_read(id=tag_type_uuid)
        self.assertEqual(tag_type_check.tag_type_id, '0x0025')

    def test_tag_ids_updated_when_tag_type_id_changes(self):
        tt_name = 'tt-change-%s' % self.id()
        tag_obj = Tag(tag_type_name=tt_name, tag_value='val1')
        tag_uuid = self.api.tag_create(tag_obj)
        self.addCleanup(self.api.tag_delete, id=tag_uuid)

        tag_read = self.api.tag_read(id=tag_uuid)
        tt_uuid = tag_read.tag_type_refs[0]['uuid']

        initial_type_id = (int(tag_read.tag_id, 16) >> 32) & 0xFFFF

        with mock.patch.object(self._api_server, 'is_admin_request',
                               return_value=True):
            tt_read = self.api.tag_type_read(id=tt_uuid)
            tt_read.tag_type_id = '0x0040'
            self.api.tag_type_update(tt_read)

        gevent.sleep(0.1)

        tag_final = self.api.tag_read(id=tag_uuid)
        final_type_id = (int(tag_final.tag_id, 16) >> 32) & 0xFFFF
        self.assertEqual(
            final_type_id, 0x0040,
            f"tag_id not updated: expected 0x0040, got {hex(final_type_id)}"
        )
        self.assertNotEqual(initial_type_id, final_type_id)

    def test_ud_tag_type_below_lower_boundary_rejected(self):
        tag_type = TagType(name='tag-type-below-%s' % self.id(),
                           tag_type_id='0x000F')
        self.assertRaises(BadRequest, self.api.tag_type_create, tag_type)

    def test_create_notification_idempotent(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tt-notify-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id='0x0090')
        tt_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tt_uuid)
        tt = self.api.tag_type_read(id=tt_uuid)
        zk_id = int(tt.tag_type_id, 0)

        count_before = mock_zk._ud_tag_type_id_allocator.get_alloc_count()
        mock_zk.alloc_tag_type_id(type_str, zk_id)
        count_after = mock_zk._ud_tag_type_id_allocator.get_alloc_count()

        self.assertEqual(count_before, count_after,
                         "Repeated notification caused extra allocation")

    def test_alloc_count_no_leak_on_failed_create(self):
        mock_zk = self._api_server._db_conn._zk_db
        allocator = mock_zk._ud_tag_type_id_allocator  # ← UD
        initial_count = allocator.get_alloc_count()

        with mock.patch.object(
                self._api_server._db_conn,
                'dbe_create',
                side_effect=Exception("db failure")
        ):
            try:
                self.api.tag_type_create(TagType(
                    name='leak-test-%s' % self.id(),
                    tag_type_id='0x0091')  # ← явный ID
                )
            except Exception:
                pass

        self.assertEqual(
            allocator.get_alloc_count(), initial_count,
            "ZK UD ID leaked after failed create")

    def test_update_tag_type_id_no_duplicate_in_zk(self):
        mock_zk = self._api_server._db_conn._zk_db

        tag_type = TagType(name='tt-update-nodup-%s' % self.id(),
                           tag_type_id='0x0031')
        tt_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tt_uuid)
        tt = self.api.tag_type_read(id=tt_uuid)
        old_id = int(tt.tag_type_id, 0)

        tt.tag_type_id = '0x0032'
        self.api.tag_type_update(tt)

        self.assertIsNone(
            mock_zk.get_tag_type_from_id(old_id),
            "Old tag type ID still allocated after update"
        )
        self.assertEqual(
            mock_zk.get_tag_type_from_id(0x0032),
            tt.fq_name[-1]
        )

    def test_update_tag_type_same_id_idempotent(self):
        mock_zk = self._api_server._db_conn._zk_db
        tag_type = TagType(name='tt-same-id-%s' % self.id(),
                           tag_type_id='0x0033')
        tt_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tt_uuid)
        count_before = mock_zk._ud_tag_type_id_allocator.get_alloc_count()

        tt = self.api.tag_type_read(id=tt_uuid)
        tt.tag_type_id = '0x0033'
        self.api.tag_type_update(tt)

        count_after = mock_zk._ud_tag_type_id_allocator.get_alloc_count()
        self.assertEqual(count_before, count_after,
                         "Update to same ID changed allocation count")

    def test_zk_values_path_cleaned_after_last_tag_delete(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'cleanup-type-%s' % self.id().lower()

        tag = Tag(tag_type_name=type_str, tag_value='val1')
        tag_uuid = self.api.tag_create(tag)

        allocator = mock_zk._tag_value_id_allocator.get(type_str)
        self.assertIsNotNone(
            allocator,
            "Tag value allocator should exist after tag creation")
        self.assertEqual(allocator.get_alloc_count(), 1,
                         "One value should be allocated")

        self.api.tag_delete(id=tag_uuid)

        allocator_after = mock_zk._tag_value_id_allocator.get(type_str)
        self.assertIsNone(
            allocator_after,
            "Tag value allocator should be removed after last tag deletion")

    def test_zk_values_path_persists_while_other_tags_exist(self):
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'persist-type-%s' % self.id().lower()

        tag1 = Tag(tag_type_name=type_str, tag_value='val1')
        tag1_uuid = self.api.tag_create(tag1)
        tag2 = Tag(tag_type_name=type_str, tag_value='val2')
        tag2_uuid = self.api.tag_create(tag2)

        allocator = mock_zk._tag_value_id_allocator.get(type_str)
        self.assertIsNotNone(allocator)
        self.assertEqual(allocator.get_alloc_count(), 2)

        self.api.tag_delete(id=tag1_uuid)

        allocator_after_first = mock_zk._tag_value_id_allocator.get(type_str)
        self.assertIsNotNone(
            allocator_after_first,
            "Allocator should persist while other tags with same type exist")
        self.assertEqual(allocator_after_first.get_alloc_count(), 1)

        self.api.tag_delete(id=tag2_uuid)

        allocator_final = mock_zk._tag_value_id_allocator.get(type_str)
        self.assertIsNone(
            allocator_final,
            "Allocator should be removed after last tag deletion")


class TestTagTypeNotificationIdempotency(TestTagBase):
    """Tests for dbe_create_notification() behaviour.

    when called more than once for the same tag-type.

    In a multi-node cluster two api-server instances can process the same
    create-notification concurrently.  The second call hits a ZK node that
    already exists and alloc_tag_type_id() raises ResourceExistsError.
    Because there is no try/except around the call, the exception propagates
    out of _dbe_create_notification() and is logged as SYS_ERR.
    """

    @property
    def _zk(self):
        return self._api_server._db_conn._zk_db

    def test_duplicate_notification_raises_resource_exists_error(self):
        from vnc_cfg_api_server.resources.tag_type import TagTypeServer

        type_str = 'tt-notify-dup-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id='0x0090')
        tt_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tt_uuid)
        tt = self.api.tag_type_read(id=tt_uuid)
        obj_dict = {'fq_name': [type_str], 'tag_type_id': tt.tag_type_id}

        with mock.patch.object(
                self._zk, 'alloc_tag_type_id',
                side_effect=ResourceExistsError('tag_type', type_str),
        ):
            ok, result = TagTypeServer.dbe_create_notification(
                self._api_server._db_conn, tt_uuid, obj_dict)

        self.assertTrue(ok)
        self.assertEqual(result, '')

    def test_notification_is_idempotent_when_type_already_registered(self):
        # Calling alloc_tag_type_id for a type that already has a ZK node
        # must not raise and must not change the allocation count.
        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tt-notify-idem-%s' % self.id()
        tag_type = TagType(name=type_str, tag_type_id='0x0091')
        tt_uuid = self.api.tag_type_create(tag_type)
        self.addCleanup(self.api.tag_type_delete, id=tt_uuid)
        tt = self.api.tag_type_read(id=tt_uuid)
        zk_id = int(tt.tag_type_id, 0)

        count_before = mock_zk._ud_tag_type_id_allocator.get_alloc_count()
        # Second registration of the same type — must be a no-op
        mock_zk.alloc_tag_type_id(type_str, zk_id)
        count_after = mock_zk._ud_tag_type_id_allocator.get_alloc_count()

        self.assertEqual(
            count_before, count_after,
            "Duplicate notification must not create an extra ZK node"
        )


class TestTagTypeIdCascadeUpdate(TestTagBase):
    """Tests for pre_dbe_update().

    Two problems in the cascade path:
    1. If internal_request_update() fails for any back-referenced tag, the
    ZK state is already mutated (new_id allocated, old_id freed) with no
    rollback.
    2. The tag_id strings written to the back-referenced tags use uppercase
    hex (f"0x{...:012X}") while every other code path uses lowercase,
    causing silent string-comparison mismatches in logs and client code.
    """

    def _make_ud_tag_type(self, name, type_id_hex):
        tt = TagType(name=name, tag_type_id=type_id_hex)
        uuid = self.api.tag_type_create(tt)
        self.addCleanup(self._try_delete_tt, uuid)
        return uuid

    def _try_delete_tt(self, uuid):
        try:
            self.api.tag_type_delete(id=uuid)
        except Exception:
            pass

    def test_zk_state_rolled_back_when_cascade_tag_update_fails(self):
        from vnc_cfg_api_server.resources.tag_type import TagTypeServer

        mock_zk = self._api_server._db_conn._zk_db
        type_str = 'tt-cascade-fail-%s' % self.id()
        tt_uuid = self._make_ud_tag_type(type_str, '0x00C0')
        old_id = 0x00C0
        new_id = 0x00C1

        fake_tag_uuid = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'
        fake_read_result = {
            'fq_name': [type_str],
            'tag_type_id': '0x00c0',
            'tag_back_refs': [{'uuid': fake_tag_uuid}],
        }
        fake_tag_db = {
            'tag_id': '0x00c080000001',
            'fq_name': [type_str + '=v1'],
            'uuid': fake_tag_uuid,
        }

        original_server = TagTypeServer.server

        class FailingServer:
            def __getattr__(self, name):
                return getattr(original_server, name)

            def internal_request_update(
                    self, resource_type, obj_uuid, obj_json
            ):
                raise Exception("simulated cascade failure")

        TagTypeServer.server = FailingServer()
        try:
            with mock.patch.object(TagTypeServer, 'dbe_read',
                                   return_value=(True, fake_read_result)):
                with mock.patch.object(
                        TagTypeServer.db_conn, 'dbe_read',
                        side_effect=lambda rt, uuid: (True, fake_tag_db)):
                    try:
                        TagTypeServer.pre_dbe_update(
                            tt_uuid, [type_str],
                            {'tag_type_id': '0x00c1'},
                            TagTypeServer.db_conn,
                        )
                    except Exception:
                        pass
        finally:
            TagTypeServer.server = original_server

        self.assertIsNone(
            mock_zk.get_tag_type_from_id(new_id),
            "New type-ID must not remain in ZK after a failed cascade update",
        )
        self.assertIsNotNone(
            mock_zk.get_tag_type_from_id(old_id),
            "Old type-ID must be restored in ZK after a failed cascade update",
        )

    def test_linked_tag_ids_use_lowercase_hex_after_type_update(self):
        # The cascade update writes tag_id strings via f"0x{...:012X}" which
        # produces uppercase.  All other paths use lowercase.  After the fix
        # the tag_id must be all-lowercase so that string comparisons are
        # consistent across the codebase.
        type_str = 'tt-hex-case-%s' % self.id()
        tt_uuid = self._make_ud_tag_type(type_str, '0x00D0')

        tag = Tag(tag_type_name=type_str, tag_value='v1')
        tag_uuid = self.api.tag_create(tag)
        self.addCleanup(lambda: self.api.tag_delete(id=tag_uuid))

        tt_obj = self.api.tag_type_read(id=tt_uuid)
        tt_obj.tag_type_id = '0x00D1'
        self.api.tag_type_update(tt_obj)

        updated_tag = self.api.tag_read(id=tag_uuid)
        self.assertEqual(
            updated_tag.tag_id,
            updated_tag.tag_id.lower(),
            "tag_id '%s' must be all-lowercase after a tag_type_id cascade "
            "update" % updated_tag.tag_id,
        )
