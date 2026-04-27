#
# Copyright (c) 2018 Juniper Networks, Inc. All rights reserved.
#
from cfgm_common.exceptions import ResourceExistsError
from sandesh_common.vns.constants import TagTypeNameToId
from vnc_api.gen.resource_common import TagType

from vnc_cfg_api_server.context import get_context
from vnc_cfg_api_server.resources._resource_base import ResourceMixin


class TagTypeServer(ResourceMixin, TagType):

    @classmethod
    def pre_dbe_create(cls, tenant_name, obj_dict, db_conn):
        type_str = obj_dict['fq_name'][-1]
        obj_dict['name'] = type_str
        obj_dict['display_name'] = type_str
        tag_type_id = obj_dict.get('tag_type_id')
        if tag_type_id is not None:
            tag_type_id = int(tag_type_id, 16)
        # if tag-type set as input and its value is less than 0x7FFF
        # return error. Tag type set is only supported for user defined
        # tag types.
        if tag_type_id is not None and \
           not cls.vnc_zk_client.user_def_tag_type(tag_type_id):
            msg = "Tag type can be set only with user defined id" \
                  " in range 16-65535"
            return False, (400, msg)
        ok, result = cls.locate(fq_name=[type_str], create_it=False)
        if ok:
            return False, (400, "Tag type already exists")
        # Allocate ID for tag-type
        internal_request = obj_dict.get('internal_request', False)
        if tag_type_id is None and not internal_request:
            msg = ("Tag type must be created with an explicit id"
                   " in user-defined range 16-65535 (0x0010-0xFFFF)")
            return False, (400, msg)
        try:
            type_id = cls.vnc_zk_client.alloc_tag_type_id(type_str,
                                                          tag_type_id,
                                                          internal_request)
        except ResourceExistsError:
            return False, (400, "Tag Type with same Id already exists")

        def undo_type_id():
            cls.vnc_zk_client.free_tag_type_id(
                type_id, type_str
            )
            return True, ""
        get_context().push_undo(undo_type_id)

        # type_id is None for failure case and in range 0 to 65535 for success
        # case
        if type_id is None:
            return False, (400, f"Failed to allocate tag type id {tag_type_id}"
                                f" type string {type_str}")

        obj_dict['tag_type_id'] = "0x{:04x}".format(type_id)

        return True, ""

    @classmethod
    def pre_dbe_update(cls, id, fq_name, obj_dict, db_conn, **kwargs):
        # User display_name once created
        if obj_dict.get('display_name'):
            msg = "Tag Type value cannot be updated"
            return False, (400, msg)

        ok, read_result = cls.dbe_read(cls.db_conn, 'tag_type', id)
        if not ok:
            return False, (400, f"Tag type {id} not found")

        old_id_int = int(read_result['tag_type_id'], 0)
        new_tag_type_id = obj_dict.get('tag_type_id')

        if new_tag_type_id is not None:
            new_id_int = int(new_tag_type_id, 0)

            if new_id_int == old_id_int:
                return True, ""

            if not cls.vnc_zk_client.user_def_tag_type(new_id_int):
                msg = ("Tag type can be set only with"
                       " user defined id in range 16-65535")
                return False, (400, msg)
        else:
            return True, ""

        try:
            cls.vnc_zk_client.alloc_tag_type_id(
                read_result['fq_name'][-1],
                new_id_int,
            )
        except ResourceExistsError:
            return False, (400, f"Tag Type ID {new_id_int} already in use")
        # Free old value
        cls.vnc_zk_client.free_tag_type_id(
            old_id_int,
            read_result['fq_name'][-1],
        )
        try:
            if 'tag_back_refs' in read_result:
                for tag in read_result['tag_back_refs']:
                    ok, tag_db = cls.db_conn.dbe_read("tag", tag['uuid'])
                    if not ok:
                        continue

                    old_tag_id = int(tag_db['tag_id'], 16)
                    old_tag_value_id = old_tag_id & 0xFFFFFFFF
                    new_tag_id = (new_id_int << 32) | old_tag_value_id
                    new_tag_id_str = f"0x{new_tag_id:012x}"

                    tag_dict = {"tag_id": new_tag_id_str, 'force': 'yes'}
                    cls.server.internal_request_update(
                        "tag", tag_db['uuid'], tag_dict
                    )
        except Exception:
            cls.vnc_zk_client.alloc_tag_type_id(
                read_result['fq_name'][-1], old_id_int)
            cls.vnc_zk_client.free_tag_type_id(
                new_id_int, read_result['fq_name'][-1])
            raise
        return True, ""

    @classmethod
    def post_dbe_delete(cls, id, obj_dict, db_conn, **kwargs):
        # Deallocate tag-type ID
        cls.vnc_zk_client.free_tag_type_id(int(obj_dict['tag_type_id'], 0),
                                           obj_dict['fq_name'][-1])
        return True, ''

    @classmethod
    def dbe_create_notification(cls, db_conn, obj_id, obj_dict):
        try:
            cls.vnc_zk_client.alloc_tag_type_id(
                ':'.join(obj_dict['fq_name']), int(obj_dict['tag_type_id'], 0))
        except ResourceExistsError:
            pass  # already registered by another node — idempotent
        return True, ''

    @classmethod
    def dbe_delete_notification(cls, obj_id, obj_dict):
        # Deallocate in memory tag-type ID
        cls.vnc_zk_client.free_tag_type_id(int(obj_dict['tag_type_id'], 0),
                                           obj_dict['fq_name'][-1],
                                           notify=True)
        return True, ''

    @classmethod
    def get_tag_type_id(cls, type_str):
        if type_str in TagTypeNameToId:
            return True, TagTypeNameToId[type_str]

        ok, result = cls.locate(fq_name=[type_str], create_it=False,
                                fields=['tag_type_id'])
        if not ok:
            return False, result
        tag_type = result

        return True, int(tag_type['tag_type_id'], 0)
