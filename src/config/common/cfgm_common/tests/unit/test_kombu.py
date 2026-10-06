# -*- coding: utf-8 -*-

#
# Copyright (c) 2026 OpenSDN Authors. All rights reserved.
#

import socket
import unittest

import gevent
import gevent.event
import gevent.queue
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


class TestVncKombuHeartbeat(unittest.TestCase):

    def _conn(self, negotiated=None):
        conn = mock.Mock()
        conn._connection.heartbeat = negotiated
        return conn

    def test_interval_in_effect(self):
        client = _client(_heartbeat_seconds=60)

        self.assertEqual(6, client._heartbeat_interval(self._conn(6)))
        self.assertEqual(60, client._heartbeat_interval(self._conn(120)))
        self.assertEqual(60, client._heartbeat_interval(self._conn(0)))
        self.assertEqual(60, client._heartbeat_interval(self._conn(None)))
        self.assertEqual(60, client._heartbeat_interval(object()))

    def test_heartbeat_greenlet_only_handles_drain(self):
        drain = self._conn(6)
        drain.heartbeat_check.side_effect = [IOError('missed'), None]
        publish = mock.Mock()
        client = _client(_heartbeat_seconds=60, _running=True,
                         _conn_drain=drain, _conn_publish=publish)
        sleeps = []

        def sleep(seconds):
            sleeps.append(seconds)
            if len(sleeps) == 2:
                client._running = False

        with mock.patch('cfgm_common.vnc_kombu.gevent.sleep', sleep):
            client._connection_heartbeat()

        self.assertEqual(2, drain.heartbeat_check.call_count)
        self.assertEqual([], publish.mock_calls)
        self.assertEqual([3.0, 3.0], sleeps)
        client._logger.assert_called_once_with(
            'Error in rabbitmq heartbeat greenlet for drain: missed',
            level=mock.ANY)

    def test_publish_heartbeat_reads_then_checks(self):
        publish = mock.Mock()
        publish.drain_events.side_effect = [None, None, socket.timeout()]
        client = _client(_conn_publish=publish)

        self.assertTrue(client._publish_heartbeat())

        self.assertEqual(3, publish.drain_events.call_count)
        publish.heartbeat_check.assert_called_once_with()

    def test_publish_heartbeat_reading_is_bounded(self):
        publish = mock.Mock()  # drain_events() never times out
        client = _client(_conn_publish=publish)

        self.assertTrue(client._publish_heartbeat())

        self.assertEqual(client._MAX_IDLE_PUBLISH_FRAMES,
                         publish.drain_events.call_count)
        publish.heartbeat_check.assert_called_once_with()

    def test_publish_heartbeat_failure(self):
        publish = mock.Mock()
        publish.drain_events.side_effect = socket.timeout()
        publish.heartbeat_check.side_effect = IOError('broker gone')
        client = _client(_conn_publish=publish)

        self.assertFalse(client._publish_heartbeat())


class TestVncKombuPublisher(unittest.TestCase):

    def _client(self, heartbeat_seconds=0):
        client = _client(_running=True, _heartbeat_seconds=heartbeat_seconds,
                         _publish_queue=gevent.queue.Queue())
        client._conn_publish = mock.Mock(connection_errors=(IOError,),
                                         channel_errors=(IOError,))
        client._conn_publish._connection.heartbeat = heartbeat_seconds
        client._conn_publish.drain_events.side_effect = socket.timeout()
        client._producer = mock.Mock()
        client._reconnect_publish = mock.Mock()
        return client

    def _run(self, client, seconds):
        publisher = gevent.spawn(client._publisher)
        gevent.sleep(seconds)
        client._running = False
        publisher.kill()

    def test_start_does_not_open_the_publish_connection(self):
        client = _client(_heartbeat_seconds=0)
        client._reconnect_drain = mock.Mock()
        client._reconnect_publish = mock.Mock()

        with mock.patch('cfgm_common.vnc_kombu.vnc_greenlets.VncGreenlet'):
            client._start('q')

        client._reconnect_drain.assert_called_once_with(delete_old_q=True)
        client._reconnect_publish.assert_not_called()

    def test_connects_on_first_message(self):
        client = self._client()

        self._run(client, 0.05)
        client._reconnect_publish.assert_not_called()

        client._running = True
        client.publish({'oper': 'CREATE'})
        self._run(client, 0.05)
        client._reconnect_publish.assert_called_once_with()
        client._producer.publish.assert_called_once_with({'oper': 'CREATE'})

    def test_idle_connection_is_serviced(self):
        client = self._client(heartbeat_seconds=0.1)
        client.publish({'oper': 'CREATE'})

        self._run(client, 0.35)

        client._reconnect_publish.assert_called_once_with()
        self.assertGreaterEqual(
            client._conn_publish.heartbeat_check.call_count, 2)

    def test_dead_connection_is_reestablished_on_next_message(self):
        client = self._client(heartbeat_seconds=0.1)
        client._conn_publish.heartbeat_check.side_effect = IOError('gone')
        client.publish({'oper': 'CREATE'})
        self._run(client, 0.12)

        client._running = True
        client.publish({'oper': 'UPDATE'})
        self._run(client, 0.12)

        self.assertEqual(2, client._reconnect_publish.call_count)
        self.assertEqual(2, client._producer.publish.call_count)

    def test_failed_publish_is_retried(self):
        client = self._client()
        client._producer.publish.side_effect = [IOError('closed'), None]
        client.publish({'oper': 'CREATE'})

        self._run(client, 0.05)

        self.assertEqual(2, client._reconnect_publish.call_count)
        self.assertEqual([mock.call({'oper': 'CREATE'})] * 2,
                         client._producer.publish.mock_calls)
