# -*- coding: utf-8 -*-

#
# Copyright (c) 2026 OpenSDN Authors. All rights reserved.
#

import socket
import unittest

import gevent
import gevent.event
import mock

from cfgm_common.vnc_kombu import VncKombuClient


def _client(**attrs):
    # client without connections or greenlets
    client = VncKombuClient.__new__(VncKombuClient)
    client._logger = mock.Mock()
    client.__dict__.update(attrs)
    return client


class TestVncKombuSubscribe(unittest.TestCase):

    def test_callback_error_is_logged_and_acked(self):
        client = _client(_subscribe_cb=mock.Mock(side_effect=ValueError('x')))
        message = mock.Mock()

        client._subscribe({'oper': 'UPDATE'}, message)

        message.ack.assert_called_once_with()
        client._logger.assert_called_once_with(
            'Error in rabbitmq subscribe callback: x', level=mock.ANY)

    def test_system_exit_is_not_swallowed(self):
        client = _client(_subscribe_cb=mock.Mock(side_effect=SystemExit(2)))
        message = mock.Mock()

        self.assertRaises(SystemExit, client._subscribe, {}, message)
        message.ack.assert_called_once_with()


class TestVncKombuCloseDrain(unittest.TestCase):
    """Deliveries dispatched by py-amqp while it closes the connection."""

    def setUp(self):
        super(TestVncKombuCloseDrain, self).setUp()
        self.callback = mock.Mock()
        self.client = _client(_subscribe_cb=self.callback)
        self.buffered = mock.Mock()

    def _dispatch_buffered(self, *args, **kwargs):
        # What py-amqp does while it waits for Connection.CloseOk
        self.client._subscribe({'oper': 'UPDATE'}, self.buffered)

    def _assert_buffered_left_to_broker(self):
        self.callback.assert_not_called()
        self.buffered.ack.assert_not_called()
        self.assertFalse(self.client._drain_closing)

    def test_close_does_not_process_buffered_deliveries(self):
        self.client._conn_drain = mock.Mock()
        self.client._conn_drain.close.side_effect = self._dispatch_buffered

        self.client._close_drain_connection()

        self._assert_buffered_left_to_broker()
        # Deliveries on a live connection are processed again.
        live = mock.Mock()
        self.client._subscribe({'oper': 'UPDATE'}, live)
        self.callback.assert_called_once_with({'oper': 'UPDATE'})
        live.ack.assert_called_once_with()

    def test_queue_deletion_does_not_process_buffered_deliveries(self):
        self.client._conn_drain = mock.Mock()
        self.client._delete_queue = mock.Mock(
            side_effect=self._dispatch_buffered)

        self.client._close_drain_connection(delete_queue=True)

        self.client._delete_queue.assert_called_once_with()
        self._assert_buffered_left_to_broker()

    def test_reconnect_does_not_process_buffered_deliveries(self):
        self.client._conn_drain = mock.Mock()
        self.client._conn_drain.close.side_effect = self._dispatch_buffered
        self.client._server_addrs = []
        self.client._update_queue_obj = mock.Mock()

        with mock.patch('cfgm_common.vnc_kombu.kombu.Consumer'):
            self.client._reconnect_drain()

        self._assert_buffered_left_to_broker()
        self.client._conn_drain.ensure_connection.assert_called_once_with()

    def test_flag_is_cleared_when_close_fails(self):
        self.client._conn_drain = mock.Mock()
        self.client._conn_drain.close.side_effect = IOError('broken pipe')

        self.assertRaises(IOError, self.client._close_drain_connection)
        self.assertFalse(self.client._drain_closing)


class TestVncKombuConsumerExit(unittest.TestCase):

    def _consumer(self):
        return gevent.spawn(gevent.sleep, 0)

    def test_callback_on_exit_while_running(self):
        client = _client(_running=True,
                         _connection_monitor_greenlet=self._consumer())
        callback = mock.Mock()

        client.link_consumer_exit(callback)
        client._connection_monitor_greenlet.join()
        gevent.sleep(0)  # links run from the hub

        callback.assert_called_once_with(client._connection_monitor_greenlet)

    def test_no_callback_after_shutdown(self):
        client = _client(_running=True,
                         _connection_monitor_greenlet=self._consumer())
        callback = mock.Mock()

        client.link_consumer_exit(callback)
        client._running = False  # what shutdown() does first
        client._connection_monitor_greenlet.join()
        gevent.sleep(0)

        callback.assert_not_called()

    def test_client_without_consumer(self):
        # e.g. a client whose __init__ was mocked out by a test
        _client().link_consumer_exit(mock.Mock())


class TestVncKombuConsumeGate(unittest.TestCase):
    """Messages are consumed only once the consume gate is set."""

    def _watch(self, gate, set_gate_after, stop_after):
        consumer = mock.Mock()
        client = _client(_running=True, _consume_gate=gate,
                         _consumer=consumer)
        timeouts = []

        def drain_events(timeout):
            timeouts.append(timeout)
            if len(timeouts) == set_gate_after:
                gate.set()
            if len(timeouts) == stop_after:
                client._running = False
            raise socket.timeout()

        client._conn_drain = mock.Mock(connection_errors=(IOError,),
                                       channel_errors=(IOError,))
        client._conn_drain.drain_events.side_effect = drain_events
        client._connection_watch(connected=True)
        return consumer, timeouts

    def test_consumes_only_once_gate_is_set(self):
        consumer, timeouts = self._watch(gevent.event.Event(),
                                         set_gate_after=2, stop_after=4)

        # read all along, with a short timeout until the gate is set
        self.assertEqual([1, 1, 3, 3], timeouts)
        self.assertEqual(2, consumer.consume.call_count)

    def test_gate_not_set(self):
        consumer, timeouts = self._watch(gevent.event.Event(),
                                         set_gate_after=None, stop_after=3)

        consumer.consume.assert_not_called()
        self.assertEqual([1, 1, 1], timeouts)

    def test_no_gate(self):
        consumer = mock.Mock()
        client = _client(_running=True, _consumer=consumer)
        client._conn_drain = mock.Mock(connection_errors=(IOError,),
                                       channel_errors=(IOError,))

        def drain_events(timeout):
            client._running = False
            raise socket.timeout()
        client._conn_drain.drain_events.side_effect = drain_events

        client._connection_watch(connected=True)

        consumer.consume.assert_called_once_with()
