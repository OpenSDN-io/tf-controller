# -*- coding: utf-8 -*-

#
# Copyright (c) 2020 Juniper Networks, Inc. All rights reserved.
#

import os
import shutil
import tempfile
import unittest

import gevent
import mock
from requests.exceptions import ConnectionError

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


class TestVncAmqpApiServerLoss(unittest.TestCase):
    """The api-server cannot be reached while a notification is handled."""

    def setUp(self):
        super(TestVncAmqpApiServerLoss, self).setUp()
        self.logger = _Logger()
        self.handle = VncAmqpHandle(mock.MagicMock(), self.logger, _DB(), {},
                                    'test', {}, '127.0.0.1')
        self.handle._db_resync_done.set()
        self.handle._vnc_kombu = mock.Mock()

    def test_exits_without_closing_from_the_consumer(self):
        self.handle.vnc_subscribe_actions = mock.Mock(
            side_effect=ConnectionError('api-server down'))

        with self.assertRaises(SystemExit) as ctx:
            self.handle._vnc_subscribe_callback(_notification())

        self.assertEqual(2, ctx.exception.code)
        # close() would kill the consumer greenlet before SystemExit.
        self.handle._vnc_kombu.shutdown.assert_not_called()
        self.assertEqual(2, self.handle.vnc_subscribe_actions.call_count)
        self.assertIn('Api-server connection lost. Exiting',
                      self.logger.messages('error'))
        self.assertIsNone(self.handle.oper_info)

    def test_single_failure_is_retried(self):
        self.handle.vnc_subscribe_actions = mock.Mock(
            side_effect=[ConnectionError('api-server blip'), None])

        self.handle._vnc_subscribe_callback(_notification())

        self.assertEqual(2, self.handle.vnc_subscribe_actions.call_count)
        self.assertEqual([], self.logger.messages('error'))

    def test_system_exit_from_consumer_greenlet_reaches_main(self):
        self.handle.vnc_subscribe_actions = mock.Mock(
            side_effect=ConnectionError('api-server down'))
        consumer = gevent.spawn(self.handle._vnc_subscribe_callback,
                                _notification())

        with self.assertRaises(SystemExit) as ctx:
            gevent.joinall([consumer, gevent.spawn(gevent.sleep, 5)])
        self.assertEqual(2, ctx.exception.code)


class TestVncAmqpConsumerExit(unittest.TestCase):
    """The consumer greenlet ends while the client is running."""

    def setUp(self):
        super(TestVncAmqpConsumerExit, self).setUp()
        self.logger = _Logger()
        self.handle = VncAmqpHandle(mock.MagicMock(), self.logger, _DB(), {},
                                    'test', {}, '127.0.0.1')

    @mock.patch('cfgm_common.vnc_amqp.gevent.get_hub')
    def test_unexpected_exit_stops_the_process(self, get_hub):
        get_hub.return_value.SYSTEM_ERROR = gevent.hub.Hub.SYSTEM_ERROR
        consumer = gevent.spawn(gevent.sleep, 0)
        consumer.join()

        self.handle._consumer_exited(consumer)

        throw = get_hub.return_value.parent.throw
        self.assertEqual(1, throw.call_count)
        exc = throw.call_args[0][0]
        self.assertIsInstance(exc, SystemExit)
        self.assertEqual(2, exc.code)
        self.assertEqual(1, len(self.logger.messages('error')))

    @mock.patch('cfgm_common.vnc_amqp.gevent.get_hub')
    def test_system_errors_are_left_to_gevent(self, get_hub):
        get_hub.return_value.SYSTEM_ERROR = gevent.hub.Hub.SYSTEM_ERROR
        for exc in (SystemExit(2), KeyboardInterrupt()):
            self.handle._consumer_exited(mock.Mock(exception=exc))

        get_hub.return_value.parent.throw.assert_not_called()

    @mock.patch('cfgm_common.vnc_amqp.VncKombuClient')
    def test_resync_done_opens_the_consume_gate(self, kombu_client):
        self.handle._rabbitmq_cfg = dict.fromkeys((
            'servers', 'port', 'user', 'password', 'vhost', 'ha_mode',
            'use_ssl', 'ssl_version', 'ssl_keyfile', 'ssl_certfile',
            'ssl_ca_certs'))
        self.handle.establish()
        gate = kombu_client.call_args[1]['consume_gate']
        self.assertFalse(gate.is_set())

        self.handle.resync_done()

        self.assertTrue(gate.is_set())

    @mock.patch('cfgm_common.vnc_amqp.VncKombuClient')
    def test_establish_watches_the_consumer(self, kombu_client):
        cfg = dict.fromkeys(('servers', 'port', 'user', 'password', 'vhost',
                             'ha_mode', 'use_ssl', 'ssl_version',
                             'ssl_keyfile', 'ssl_certfile', 'ssl_ca_certs'))
        self.handle._rabbitmq_cfg = cfg

        self.handle.establish()

        kombu_client.return_value.link_consumer_exit.assert_called_once_with(
            self.handle._consumer_exited)
        # Notifications are consumed once the resync is done.
        self.assertIs(self.handle._db_resync_done,
                      kombu_client.call_args[1]['consume_gate'])
