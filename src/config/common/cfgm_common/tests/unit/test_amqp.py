# -*- coding: utf-8 -*-

#
# Copyright (c) 2020 Juniper Networks, Inc. All rights reserved.
#

import os
import shutil
import tempfile
import unittest

import mock

from cfgm_common.vnc_amqp import VncAmqpHandle


class _Logger(object):
    """Logger with the ConfigServiceLogger API: warning(), but no warn()."""

    def __init__(self):
        self.calls = []

    def _record(self, level):
        def log(msg, *args, **kwargs):
            self.calls.append((level, msg))
        return log

    def __getattr__(self, name):
        if name in ('debug', 'info', 'notice', 'warning', 'error', 'log'):
            return self._record(name)
        raise AttributeError(name)

    def messages(self, level):
        return [msg for lvl, msg in self.calls if lvl == level]


class _DB(object):
    """db_cls stand-in; the tests add the object types they need."""

    _indexed_by_name = False
    _object_db = mock.MagicMock()

    def __init__(self, obj_type_map=None):
        self._obj_type_map = obj_type_map or {}

    def get_obj_type_map(self):
        return self._obj_type_map

    def set_meta(self, meta):
        pass


def _notification(obj_type='virtual-network', oper='UPDATE',
                  uuid='00000000-0000-0000-0000-000000000001'):
    return {'oper': oper, 'type': obj_type, 'uuid': uuid,
            'fq_name': ['default-domain', 'default-project', 'vn']}


class TestVncAmqp(unittest.TestCase):

    def setUp(self):
        super(TestVncAmqp, self).setUp()
        self.logger = _Logger()
        self.tmp_dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp_dir)

    def _handle(self, db=None, trace_file=None):
        handle = VncAmqpHandle(mock.MagicMock(), self.logger, db or _DB(),
                               {}, 'test', {}, '127.0.0.1',
                               trace_file=trace_file)
        handle._db_resync_done.set()
        return handle

    def _assert_state_reset(self, handle):
        for attr in ('oper_info', 'obj_type', 'obj_class', 'obj',
                     'dependency_tracker', 'msg_tracer'):
            self.assertIsNone(getattr(handle, attr), attr)

    def test_close(self):
        vnc = VncAmqpHandle(
            *(7 * [mock.MagicMock()]))

        # Should not raise anything
        vnc.close()

        # Pretends call of establish()
        vnc._vnc_kombu = mock.MagicMock()
        vnc.close()
        vnc._vnc_kombu.shutdown.assert_called_once_with()

    def test_untracked_type_first_after_start(self):
        # No message bus trace exists yet for an untracked type.
        handle = self._handle()

        handle._vnc_subscribe_callback(_notification('access-control-list'))

        self.assertEqual([], self.logger.messages('error'))
        self._assert_state_reset(handle)

    def test_untracked_type_does_not_emit_previous_trace(self):
        obj_class = mock.MagicMock()
        obj_class.get_by_uuid.return_value = None
        handle = self._handle(_DB({'virtual_network': obj_class}))

        with mock.patch('cfgm_common.vnc_amqp.MessageBusNotifyTrace') as trc:
            for obj_type in ('virtual-network', 'access-control-list'):
                handle._vnc_subscribe_callback(_notification(obj_type))

        # Only the tracked notification is traced, once.
        self.assertEqual(1, trc.return_value.trace_msg.call_count)
        self._assert_state_reset(handle)

    def test_error_with_unwritable_trace_file(self):
        trace_file = os.path.join(self.tmp_dir, 'missing-dir', 'trace.err')
        handle = self._handle(trace_file=trace_file)
        handle.vnc_subscribe_actions = mock.Mock(
            side_effect=ValueError('handler failure'))

        handle._vnc_subscribe_callback(_notification())
        handle._vnc_subscribe_callback(_notification())

        errors = self.logger.messages('error')
        self.assertEqual(2, len(errors))
        self.assertIn('handler failure', errors[0])
        # The trace file problem is reported once, with the supported API.
        warnings = [m for m in self.logger.messages('warning')
                    if 'trace file' in m]
        self.assertEqual(1, len(warnings))
        self._assert_state_reset(handle)

    def test_error_is_written_to_trace_file(self):
        trace_file = os.path.join(self.tmp_dir, 'trace.err')
        handle = self._handle(trace_file=trace_file)
        handle.vnc_subscribe_actions = mock.Mock(
            side_effect=ValueError('handler failure'))

        handle._vnc_subscribe_callback(_notification())

        with open(trace_file) as f:
            self.assertIn('handler failure', f.read())

    def test_error_before_notification_state_is_set(self):
        # Fails before obj_type & co are set.
        handle = self._handle()
        notification = _notification()
        del notification['type']

        handle._vnc_subscribe_callback(notification)

        self.assertEqual(1, len(self.logger.messages('error')))
        self._assert_state_reset(handle)

    def test_trace_failure_does_not_escape(self):
        obj_class = mock.MagicMock()
        obj_class.get_by_uuid.return_value = None
        handle = self._handle(_DB({'virtual_network': obj_class}))

        with mock.patch('cfgm_common.vnc_amqp.MessageBusNotifyTrace') as trc:
            trc.return_value.trace_msg.side_effect = RuntimeError('sandesh')
            handle._vnc_subscribe_callback(_notification())

        self.assertEqual(1, len([m for m in self.logger.messages('warning')
                                 if 'Error in _vnc_subscribe_callback' in m]))
        self._assert_state_reset(handle)
